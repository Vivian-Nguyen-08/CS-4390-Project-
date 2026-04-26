#!/usr/bin/env bash
# =============================================================================
# demo.sh — CS4390 P2P File Sharing Demo
# =============================================================================
#
# SETUP INSTRUCTIONS — READ BEFORE RUNNING
# =========================================
#
# MACHINE LAYOUT:
#   Machine 1 — runs the tracker AND this script
#   Machine 2 — runs all peers (Peer1 through Peer13)
#
# STEP 1 — Clone the project on both machines
#   On each machine run:
#     git clone <your-github-url> ~/project
#   Make sure work.txt and demo.mp4 are in ~/project on Machine 2
#
# STEP 2 — Set the machine IPs below
#   Find each machine's IP with:   ip addr   (Linux)  or   ifconfig   (Mac)
#   Edit the MACHINE IP lines in the CONFIG section below.
#
# STEP 3 — Set the SSH username below
#   Both machines must have the same username, or edit SSH_USER per machine.
#
# STEP 4 — Set up passwordless SSH from Machine 1 to Machine 2
#   Run these commands ONCE on Machine 1:
#     ssh-keygen -t rsa -b 4096        (press Enter for all prompts)
#     ssh-copy-id <SSH_USER>@<PEERS_MACHINE>
#   Test it works (should print "ok" with no password prompt):
#     ssh <SSH_USER>@<PEERS_MACHINE> echo ok
#
# STEP 5 — Install OpenSSL on all machines if not present (tracker compile needs it)
#   Linux:   sudo apt install libssl-dev
#   Mac:     brew install openssl
#
# STEP 6 — Run the demo from Machine 1:
#     chmod +x demo.sh
#     bash demo.sh
#
# TIMELINE:
#   t=0s    Tracker starts, Peer1 and Peer2 start and send createtracker
#   t=30s   Peers 3-8 start, send list, download both files
#   t=90s   Peers 9-13 start, send list, download both files
#             Peer1 and Peer2 terminate
#   t=90s + DOWNLOAD_TIMEOUT   All peers terminate, MD5 verified
# =============================================================================

if [ "${BASH_VERSINFO[0]}" -lt 3 ]; then
    echo "[DEMO ERROR] bash 3.2 or higher required." >&2; exit 1
fi

# =============================================================================
# MACHINE CONFIG — edit these before running
# =============================================================================
SSH_USER="student"            # SSH username on all machines
PROJECT_DIR="$HOME/project"   # absolute path to project on all machines

TRACKER_MACHINE="192.168.1.1" # Machine 1 — tracker (also runs this script)
PEER1_MACHINE="192.168.1.2"   # Machine 2 — seeds small file
PEER2_MACHINE="192.168.1.2"   # Machine 2 — seeds large file
PEERS_MACHINE="192.168.1.2"   # Machine 2 — all peers 3-13 run here

# =============================================================================
# DEMO CONFIG — files and timing
# =============================================================================
SMALL_FILE="work.txt"
LARGE_FILE="demo.mp4"
TRACKER_SHARED_DIR="torrents"
LOG_DIR="logs"

PEER_PORTS=(0 8001 8002 8003 8004 8005 8006 8007 8008 8009 8010 8011 8012 8013)

T_WAVE1=30            # seconds until Peers 3-8 start
T_WAVE2=90            # seconds until Peers 9-13 start AND Peers 1-2 stop
DOWNLOAD_TIMEOUT=300  # seconds to wait for all downloaders after T_WAVE2

# =============================================================================
# READ PEER SETTINGS FROM LOCAL CONFIG FILES
# =============================================================================
TRACKER_PORT=$(sed -n '1p' clientThreadConfig.cfg | tr -d '[:space:]')
UPDATE_INTERVAL=$(sed -n '3p' clientThreadConfig.cfg | tr -d '[:space:]')
MAX_CHUNKS=$(sed -n '4p'      clientThreadConfig.cfg | tr -d '[:space:]')
MAX_PEERS=$(sed -n '5p'       clientThreadConfig.cfg | tr -d '[:space:]')
MAX_SEGMENTS=$(sed -n '6p'    clientThreadConfig.cfg | tr -d '[:space:]')
CACHE_DIR=$(sed -n '7p'       clientThreadConfig.cfg | tr -d '[:space:]')
SHARED_FOLDER=$(sed -n '2p'   serverThreadConfig.cfg | tr -d '[:space:]')
SEGMENT_SIZE=$(sed -n '3p'    serverThreadConfig.cfg | tr -d '[:space:]')

[ -z "$TRACKER_PORT" ]    && TRACKER_PORT=3490
[ -z "$UPDATE_INTERVAL" ] && UPDATE_INTERVAL=900
[ -z "$MAX_CHUNKS" ]      && MAX_CHUNKS=16384
[ -z "$MAX_PEERS" ]       && MAX_PEERS=256
[ -z "$MAX_SEGMENTS" ]    && MAX_SEGMENTS=256
[ -z "$CACHE_DIR" ]       && CACHE_DIR="cache"
[ -z "$SHARED_FOLDER" ]   && SHARED_FOLDER="shared"
[ -z "$SEGMENT_SIZE" ]    && SEGMENT_SIZE=1048576

# =============================================================================
# HELPERS
# =============================================================================
ts()  { date +%H:%M:%S; }
log() { echo "[DEMO $(ts)] $*"; }
die() { echo "[DEMO ERROR] $*" >&2; exit 1; }

DEMO_START=""

elapsed_t() { echo $(( $(date +%s) - DEMO_START )); }

sleep_until() {
    local target=$1
    local remaining=$(( target - $(elapsed_t) ))
    if [ $remaining -gt 0 ]; then
        log "Sleeping ${remaining}s (target t=${target}s, elapsed=$(elapsed_t)s)"
        sleep "$remaining"
    fi
}

md5_file() {
    if command -v md5sum >/dev/null 2>&1; then
        md5sum "$1" | awk '{print $1}'
    else
        md5 -q "$1"
    fi
}

openssl_flags() {
    if command -v brew >/dev/null 2>&1; then
        local prefix
        prefix=$(brew --prefix openssl 2>/dev/null)
        if [ -n "$prefix" ] && [ -d "$prefix" ]; then
            echo "-I${prefix}/include -L${prefix}/lib"; return
        fi
    fi
    echo ""
}

run_remote() {
    local host=$1; shift
    ssh -o StrictHostKeyChecking=no \
        -o ConnectTimeout=10 \
        "${SSH_USER}@${host}" "$@"
}

remote_md5() {
    local host=$1 filepath=$2
    run_remote "$host" \
        "if command -v md5sum >/dev/null 2>&1; \
         then md5sum ${filepath} | awk '{print \$1}'; \
         else md5 -q ${filepath}; fi"
}

remote_filesize() {
    local host=$1 filepath=$2
    run_remote "$host" "wc -c < ${filepath} | tr -d ' '"
}

is_local() {
    [[ "$1" == "127.0.0.1" || "$1" == "localhost" ]]
}

# =============================================================================
# PEER CONTROL
# =============================================================================
PEER_PIDS=()
PEER_FDS=()
FD_NEXT=10
TRACKER_PID=""

setup_peer_dir() {
    local n=$1 host=$2 upload_port=$3
    local dir="${PROJECT_DIR}/peer${n}"
    log "Setting up Peer${n} on ${host}..."

    if is_local "$host"; then
        mkdir -p "${dir}/${SHARED_FOLDER}" "${dir}/${CACHE_DIR}" "${PROJECT_DIR}/${LOG_DIR}"
        rm -f "${dir}/${CACHE_DIR}"/*.track 2>/dev/null || true
        printf '%s\n%s\n%s\n%s\n%s\n%s\n%s\n' \
            "$TRACKER_PORT" "$TRACKER_MACHINE" "$UPDATE_INTERVAL" \
            "$MAX_CHUNKS" "$MAX_PEERS" "$MAX_SEGMENTS" "$CACHE_DIR" \
            > "${dir}/clientThreadConfig.cfg"
        printf '%s\n%s\n%s\n' \
            "$upload_port" "$SHARED_FOLDER" "$SEGMENT_SIZE" \
            > "${dir}/serverThreadConfig.cfg"
        cp peer "${dir}/peer"
    else
        run_remote "$host" "
            mkdir -p ${dir}/${SHARED_FOLDER} ${dir}/${CACHE_DIR} ${PROJECT_DIR}/${LOG_DIR}
            rm -f ${dir}/${CACHE_DIR}/*.track 2>/dev/null || true
            printf '%s\n%s\n%s\n%s\n%s\n%s\n%s\n' \
                '${TRACKER_PORT}' '${TRACKER_MACHINE}' '${UPDATE_INTERVAL}' \
                '${MAX_CHUNKS}' '${MAX_PEERS}' '${MAX_SEGMENTS}' '${CACHE_DIR}' \
                > ${dir}/clientThreadConfig.cfg
            printf '%s\n%s\n%s\n' \
                '${upload_port}' '${SHARED_FOLDER}' '${SEGMENT_SIZE}' \
                > ${dir}/serverThreadConfig.cfg
        "
    fi
}

start_peer() {
    local n=$1 host=$2
    local dir="${PROJECT_DIR}/peer${n}"
    local fifo="${dir}/stdin.fifo"
    local logf="${PROJECT_DIR}/${LOG_DIR}/peer${n}.log"

    if is_local "$host"; then
        [ -p "$fifo" ] && rm -f "$fifo"
        mkfifo "$fifo"

        local fd=$FD_NEXT
        FD_NEXT=$((FD_NEXT + 1))
        eval "exec ${fd}>${fifo}"
        PEER_FDS[$n]=$fd

        (cd "${dir}" && ./peer "Peer${n}") < "$fifo" 2>&1 | tee "$logf" &
        PEER_PIDS[$n]=$!
    else
        run_remote "$host" "
            [ -p ${fifo} ] && rm -f ${fifo}
            mkfifo ${fifo}
            cd ${dir} && nohup ./peer Peer${n} < ${fifo} >> ${logf} 2>&1 &
        "
    fi

    sleep 1
    log "Peer${n} started on ${host}"
}

send_cmd() {
    local n=$1 host=$2 cmd=$3

    if is_local "$host"; then
        local fd=${PEER_FDS[$n]}
        echo "${cmd}" >&${fd}
    else
        local fifo="${PROJECT_DIR}/peer${n}/stdin.fifo"
        run_remote "$host" "echo '${cmd}' > ${fifo}"
    fi

    log "Peer${n}: ${cmd}"
}

stop_peer() {
    local n=$1 host=$2
    local fd=${PEER_FDS[$n]}
    [ -z "$fd" ] && return
    log "Stopping Peer${n} on ${host}..."
    send_cmd "$n" "$host" "quit"
    sleep 2

    if is_local "$host"; then
        [ -n "$fd" ] && eval "exec ${fd}>&-" 2>/dev/null || true
        [ -n "${PEER_PIDS[$n]}" ] && wait "${PEER_PIDS[$n]}" 2>/dev/null || true
    else
        run_remote "$host" "pkill -f 'peer Peer${n}' 2>/dev/null || true"
    fi

    echo "Peer${n} terminated"
    PEER_PIDS[$n]=""
    PEER_FDS[$n]=""
}

cleanup() {
    log "Shutting down..."
    for n in 1 2 3 4 5 6 7 8 9 10 11 12 13; do
        [ -n "${PEER_FDS[$n]}" ] && eval "exec ${PEER_FDS[$n]}>&-" 2>/dev/null || true
        [ -n "${PEER_PIDS[$n]}" ] && kill "${PEER_PIDS[$n]}" 2>/dev/null || true
    done
    for host in "$PEER1_MACHINE" "$PEER2_MACHINE" "$PEERS_MACHINE"; do
        is_local "$host" && continue
        run_remote "$host" "pkill -f 'peer Peer' 2>/dev/null || true" 2>/dev/null || true
    done
    [ -n "$TRACKER_PID" ] && kill "$TRACKER_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# =============================================================================
# STEP 0 — Verify SSH connectivity to all peer machines
# =============================================================================
log "Verifying SSH connectivity..."
for host in "$PEER1_MACHINE" "$PEER2_MACHINE" "$PEERS_MACHINE"; do
    is_local "$host" && continue
    run_remote "$host" "echo ok" > /dev/null 2>&1 \
        || die "Cannot SSH to ${SSH_USER}@${host}. Check SSH keys (see instructions at top of script)."
    log "SSH OK: ${host}"
done

# =============================================================================
# STEP 1 — Compile on all machines
# =============================================================================
log "Compiling on all machines..."
mkdir -p "$LOG_DIR"

SSL_FLAGS=$(openssl_flags)

gcc -std=c11 -pthread -Wall -Wextra -o peer peer.c -lpthread \
    || die "peer.c failed to compile"
# shellcheck disable=SC2086
gcc -std=c11 -Wall -Wextra -o tracker tracker.c \
    $SSL_FLAGS -lpthread -lssl -lcrypto \
    || die "tracker.c failed to compile"

for host in "$PEER1_MACHINE" "$PEER2_MACHINE" "$PEERS_MACHINE"; do
    is_local "$host" && continue
    log "Compiling on ${host}..."
    run_remote "$host" "
        cd ${PROJECT_DIR} && git pull --quiet
        gcc -std=c11 -pthread -Wall -Wextra -o peer peer.c -lpthread
    " || die "Compilation failed on ${host}"
done

log "Compilation successful on all machines"

# =============================================================================
# STEP 2 — Read file info from owning machines
# =============================================================================
log "Reading file info..."

if is_local "$PEER1_MACHINE"; then
    SMALL_SIZE=$(wc -c < "${PROJECT_DIR}/${SMALL_FILE}" | tr -d ' ')
    SMALL_MD5=$(md5_file "${PROJECT_DIR}/${SMALL_FILE}")
else
    SMALL_SIZE=$(remote_filesize "$PEER1_MACHINE" "${PROJECT_DIR}/${SMALL_FILE}")
    SMALL_MD5=$(remote_md5      "$PEER1_MACHINE" "${PROJECT_DIR}/${SMALL_FILE}")
fi

if is_local "$PEER2_MACHINE"; then
    LARGE_SIZE=$(wc -c < "${PROJECT_DIR}/${LARGE_FILE}" | tr -d ' ')
    LARGE_MD5=$(md5_file "${PROJECT_DIR}/${LARGE_FILE}")
else
    LARGE_SIZE=$(remote_filesize "$PEER2_MACHINE" "${PROJECT_DIR}/${LARGE_FILE}")
    LARGE_MD5=$(remote_md5      "$PEER2_MACHINE" "${PROJECT_DIR}/${LARGE_FILE}")
fi

log "Small: $SMALL_FILE  size=${SMALL_SIZE}  md5=${SMALL_MD5}"
log "Large: $LARGE_FILE  size=${LARGE_SIZE}  md5=${LARGE_MD5}"

# =============================================================================
# STEP 3 — Clean tracker directory
# =============================================================================
log "Cleaning tracker directory: ${TRACKER_SHARED_DIR}"
rm -rf "$TRACKER_SHARED_DIR"
mkdir -p "$TRACKER_SHARED_DIR"
printf '%s\n%s\n' "$TRACKER_PORT" "$TRACKER_SHARED_DIR" > sconfig

# =============================================================================
# STEP 4 — Start tracker (t=0)
# =============================================================================
log "=== t=0s: Starting tracker on port ${TRACKER_PORT} ==="
DEMO_START=$(date +%s)
./tracker 2>&1 | tee "${LOG_DIR}/tracker.log" &
TRACKER_PID=$!
sleep 1
log "Tracker running  PID=${TRACKER_PID}"

# =============================================================================
# STEP 5 — Set up and start Peer1 and Peer2 (t=0)
# =============================================================================
log "=== t=0s: Starting Peer1 and Peer2 ==="

if is_local "$PEER1_MACHINE"; then
    mkdir -p "${PROJECT_DIR}/peer1/${SHARED_FOLDER}"
    cp "${PROJECT_DIR}/${SMALL_FILE}" "${PROJECT_DIR}/peer1/${SHARED_FOLDER}/${SMALL_FILE}"
else
    run_remote "$PEER1_MACHINE" \
        "mkdir -p ${PROJECT_DIR}/peer1/${SHARED_FOLDER} && \
         cp ${PROJECT_DIR}/${SMALL_FILE} ${PROJECT_DIR}/peer1/${SHARED_FOLDER}/${SMALL_FILE}"
fi

if is_local "$PEER2_MACHINE"; then
    mkdir -p "${PROJECT_DIR}/peer2/${SHARED_FOLDER}"
    cp "${PROJECT_DIR}/${LARGE_FILE}" "${PROJECT_DIR}/peer2/${SHARED_FOLDER}/${LARGE_FILE}"
else
    run_remote "$PEER2_MACHINE" \
        "mkdir -p ${PROJECT_DIR}/peer2/${SHARED_FOLDER} && \
         cp ${PROJECT_DIR}/${LARGE_FILE} ${PROJECT_DIR}/peer2/${SHARED_FOLDER}/${LARGE_FILE}"
fi

setup_peer_dir 1 "$PEER1_MACHINE" "${PEER_PORTS[1]}"
setup_peer_dir 2 "$PEER2_MACHINE" "${PEER_PORTS[2]}"
start_peer 1 "$PEER1_MACHINE"
start_peer 2 "$PEER2_MACHINE"
sleep 2

# =============================================================================
# STEP 6 — Peer1 and Peer2 send createtracker (t=0)
# =============================================================================
log "Peer1 and Peer2 sending createtracker..."
send_cmd 1 "$PEER1_MACHINE" \
    "createtracker ${SMALL_FILE} ${SMALL_SIZE} small_test_file ${SMALL_MD5} ${PEER1_MACHINE} ${PEER_PORTS[1]}"
send_cmd 2 "$PEER2_MACHINE" \
    "createtracker ${LARGE_FILE} ${LARGE_SIZE} large_test_file ${LARGE_MD5} ${PEER2_MACHINE} ${PEER_PORTS[2]}"

sleep 5

SMALL_END=$((SMALL_SIZE - 1))
LARGE_END=$((LARGE_SIZE  - 1))
send_cmd 1 "$PEER1_MACHINE" "updatetracker ${SMALL_FILE} 0 ${SMALL_END}"
send_cmd 2 "$PEER2_MACHINE" "updatetracker ${LARGE_FILE} 0 ${LARGE_END}"
sleep 3

# =============================================================================
# STEP 7 — t=30s: Start Peers 3-8, list then download both files
# =============================================================================
sleep_until "$T_WAVE1"
log "=== t=$(elapsed_t)s: Starting Peers 3-8 ==="

for n in 3 4 5 6 7 8; do
    setup_peer_dir "$n" "$PEERS_MACHINE" "${PEER_PORTS[$n]}"
    start_peer "$n" "$PEERS_MACHINE"
    sleep 3
    send_cmd "$n" "$PEERS_MACHINE" "list"
    sleep 1
    send_cmd "$n" "$PEERS_MACHINE" "get ${SMALL_FILE}.track"
    sleep 2
    send_cmd "$n" "$PEERS_MACHINE" "get ${LARGE_FILE}.track"
done

# =============================================================================
# STEP 8 — t=90s: Start Peers 9-13 AND stop Peers 1 & 2
# =============================================================================
sleep_until "$T_WAVE2"
log "=== t=$(elapsed_t)s: Starting Peers 9-13 AND stopping Peers 1 & 2 ==="

for n in 9 10 11 12 13; do
    setup_peer_dir "$n" "$PEERS_MACHINE" "${PEER_PORTS[$n]}"
    start_peer "$n" "$PEERS_MACHINE"
    sleep 3
    send_cmd "$n" "$PEERS_MACHINE" "list"
    sleep 1
    send_cmd "$n" "$PEERS_MACHINE" "get ${SMALL_FILE}.track"
    sleep 2
    send_cmd "$n" "$PEERS_MACHINE" "get ${LARGE_FILE}.track"
done

stop_peer 1 "$PEER1_MACHINE"
stop_peer 2 "$PEER2_MACHINE"

# =============================================================================
# STEP 9 — Wait for all downloaders to finish then stop them
# =============================================================================
log "Waiting ${DOWNLOAD_TIMEOUT}s for Peers 3-13 to complete..."
sleep "$DOWNLOAD_TIMEOUT"

for n in 3 4 5 6 7 8 9 10 11 12 13; do
    stop_peer "$n" "$PEERS_MACHINE"
done
sleep 3

# =============================================================================
# STEP 10 — Verify MD5 of downloaded files
# =============================================================================
log "=== Verifying downloads ==="

all_pass=true
for n in 3 4 5 6 7 8 9 10 11 12 13; do
    small_ok="FAIL"; large_ok="FAIL"

    if is_local "$PEERS_MACHINE"; then
        actual_small=$(md5_file "${PROJECT_DIR}/peer${n}/${SHARED_FOLDER}/${SMALL_FILE}" 2>/dev/null)
        actual_large=$(md5_file "${PROJECT_DIR}/peer${n}/${SHARED_FOLDER}/${LARGE_FILE}"  2>/dev/null)
    else
        actual_small=$(remote_md5 "$PEERS_MACHINE" "${PROJECT_DIR}/peer${n}/${SHARED_FOLDER}/${SMALL_FILE}")
        actual_large=$(remote_md5 "$PEERS_MACHINE" "${PROJECT_DIR}/peer${n}/${SHARED_FOLDER}/${LARGE_FILE}")
    fi

    [ "$actual_small" = "$SMALL_MD5" ] && small_ok="OK"
    [ "$actual_large" = "$LARGE_MD5" ] && large_ok="OK"
    echo "Peer${n}: ${SMALL_FILE} [${small_ok}]  ${LARGE_FILE} [${large_ok}]"

    if [ "$small_ok" != "OK" ] || [ "$large_ok" != "OK" ]; then
        all_pass=false
    fi
done

if $all_pass; then
    log "All downloads verified successfully across all 11 downloader peers"
else
    log "WARNING: some downloads failed MD5 check"
fi

log "=== Demo complete. Logs in ${LOG_DIR}/ ==="
