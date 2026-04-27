#!/usr/bin/env bash
# =============================================================================
# demo.sh — CS4390 P2P File Sharing Demo (Single Machine, 4 Terminals)
# HOW IT WORKS:
#   This script opens 14 terminal windows automatically:
#     Terminal 1 — Tracker
#     Terminal 2 — Peer1 (seeds small file)
#     Terminal 3 — Peer2 (seeds large file)
#     Terminal 4-14 — Peer3-Peer13 (downloader)
#
#   Commands are sent to each peer through a named pipe (FIFO).
#   Each peer's output appears in its own terminal window.
#
# HOW TO RUN:
#   1. Put work.txt and demo.mp4 in the same folder as this script
#   2. Run:  bash demo.sh
#   4 terminal windows will open automatically.
#
# SUPPORTED TERMINALS:
#   Mac  — Terminal.app (built in, no install needed)
#   Linux — gnome-terminal or xterm
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
# CONFIG
# =============================================================================
SMALL_FILE="work.txt"
LARGE_FILE="demo.mp4"
TRACKER_SHARED_DIR="torrents"
LOG_DIR="logs"
BASE_DIR="$(pwd)"

PEER_PORTS=(0 8001 8002 8003 8004 8005 8006 8007 8008 8009 8010 8011 8012 8013)
TRACKER_IP="127.0.0.1"

T_WAVE1=30            # seconds: Peer3 starts
T_WAVE2=90            # seconds: Peer1 and Peer2 stop
DOWNLOAD_TIMEOUT=300  # seconds: wait for Peer3 before verifying

# Read from config files
TRACKER_PORT=$(sed -n '1p' clientThreadConfig.cfg | tr -d '[:space:]')
UPDATE_INTERVAL=$(sed -n '3p' clientThreadConfig.cfg | tr -d '[:space:]')
MAX_PEERS=$(sed -n '5p'       clientThreadConfig.cfg | tr -d '[:space:]')
MAX_SEGMENTS=$(sed -n '6p'    clientThreadConfig.cfg | tr -d '[:space:]')
CACHE_DIR=$(sed -n '7p'       clientThreadConfig.cfg | tr -d '[:space:]')
SHARED_FOLDER=$(sed -n '2p'   serverThreadConfig.cfg | tr -d '[:space:]')
SEGMENT_SIZE=$(sed -n '3p'    serverThreadConfig.cfg | tr -d '[:space:]')

[ -z "$TRACKER_PORT" ]    && TRACKER_PORT=3490
[ -z "$UPDATE_INTERVAL" ] && UPDATE_INTERVAL=900
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

# elapsed_t: seconds elapsed since DEMO_START
elapsed_t() { echo $(( $(date +%s) - DEMO_START )); }

# sleep_until: sleep until N seconds have elapsed from DEMO_START
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

# open_terminal: open a new terminal window running a command
# Supports macOS Terminal.app, gnome-terminal, and xterm
open_terminal() {
    local title=$1
    local cmd=$2

    if [[ "$OSTYPE" == "darwin"* ]]; then
        osascript <<APPLESCRIPT
tell application "Terminal"
    do script "printf '\\\\033]0;${title}\\\\007'; ${cmd}"
    activate
end tell
APPLESCRIPT
    elif command -v gnome-terminal >/dev/null 2>&1; then
        gnome-terminal --title="$title" -- bash -c "$cmd; exec bash" &
    elif command -v xterm >/dev/null 2>&1; then
        xterm -title "$title" -e bash -c "$cmd; exec bash" &
    else
        die "No terminal emulator found. Need Terminal.app (Mac), gnome-terminal, or xterm."
    fi
    sleep 1
}

# =============================================================================
# PEER CONTROL via named FIFOs — no SSH needed
# =============================================================================
PEER_PIDS=()
PEER_FDS=()
FD_NEXT=10
TRACKER_PID=""

# setup_peer_dir: create directories and write config files for Peer N
setup_peer_dir() {
    local n=$1 upload_port=$2
    local dir="${BASE_DIR}/peer${n}"

    mkdir -p "${dir}/${SHARED_FOLDER}" "${dir}/${CACHE_DIR}" "${BASE_DIR}/${LOG_DIR}"

    # Clean any stale .track files from a previous run
    rm -f "${dir}/${CACHE_DIR}"/*.track 2>/dev/null || true

    # Write clientThreadConfig.cfg with all 7 lines
    printf '%s\n%s\n%s\n%s\n%s\n%s\n%s\n' \
        "$TRACKER_PORT" "$TRACKER_IP" "$UPDATE_INTERVAL" \
        "$MAX_PEERS" "$MAX_SEGMENTS" "$CACHE_DIR" \
        > "${dir}/clientThreadConfig.cfg"

    # Write serverThreadConfig.cfg with port, shared folder, segment size
    printf '%s\n%s\n%s\n' \
        "$upload_port" "$SHARED_FOLDER" "$SEGMENT_SIZE" \
        > "${dir}/serverThreadConfig.cfg"

    cp peer "${dir}/peer"
}

# start_peer: open terminal window first, then create FIFO once terminal is ready
start_peer() {
    local n=$1
    local dir="${BASE_DIR}/peer${n}"
    local fifo="${dir}/stdin.fifo"
    local logf="${BASE_DIR}/${LOG_DIR}/peer${n}.log"

    # Create fresh FIFO
    [ -p "$fifo" ] && rm -f "$fifo"
    mkfifo "$fifo"

    # Open peer in a new terminal window — reads from FIFO, logs to file
    # Use quotes around paths to handle spaces in directory names
    open_terminal "Peer${n}" \
        "cd '${dir}' && ./peer Peer${n} < '${fifo}' 2>&1 | tee '${logf}'"

    # Give terminal time to open and start reading from the FIFO
    # before we open the write end — otherwise exec blocks indefinitely
    sleep 2

    # Now open write end of FIFO — terminal is ready to read
    local fd=$FD_NEXT
    FD_NEXT=$((FD_NEXT + 1))
    eval "exec ${fd}>'${fifo}'"
    PEER_FDS[$n]=$fd

    log "Peer${n} started  port=${n}"
}

# send_cmd: write one command into the peer's FIFO
send_cmd() {
    local n=$1 cmd=$2
    local fd=${PEER_FDS[$n]}
    echo "${cmd}" >&${fd}
    log "Peer${n}: ${cmd}"
}

# stop_peer: send quit, close FIFO, wait for process
stop_peer() {
    local n=$1
    local fd=${PEER_FDS[$n]}
    log "Stopping Peer${n}..."
    send_cmd "$n" "quit"
    eval "exec ${fd}>&-"
    sleep 1
    echo "Peer${n} terminated"
    PEER_PIDS[$n]=""
    PEER_FDS[$n]=""
}

# cleanup: runs on Ctrl+C or script exit
cleanup() {
    log "Shutting down..."
    for n in 1 2 3 4 5 6 7 8 9 10 11 12 13; do
        [ -n "${PEER_FDS[$n]}" ] && eval "exec ${PEER_FDS[$n]}>&-" 2>/dev/null || true
        [ -n "${PEER_PIDS[$n]}" ] && kill "${PEER_PIDS[$n]}" 2>/dev/null || true
    done
    [ -n "$TRACKER_PID" ] && kill "$TRACKER_PID" 2>/dev/null || true
    pkill -f "./peer Peer"   2>/dev/null || true
    pkill -f "./tracker"     2>/dev/null || true
}
trap cleanup EXIT INT TERM

# =============================================================================
# STEP 0 — Compile
# =============================================================================
log "Compiling..."
mkdir -p "$LOG_DIR"

SSL_FLAGS=$(openssl_flags)

gcc -std=c11 -pthread -Wall -Wextra -o peer peer.c -lpthread \
    || die "peer.c failed to compile"

# shellcheck disable=SC2086
gcc -std=c11 -Wall -Wextra -o tracker tracker.c \
    $SSL_FLAGS -lpthread -lssl -lcrypto \
    || die "tracker.c failed to compile"

log "Compilation successful"

# =============================================================================
# STEP 1 — Check files exist and read their info
# =============================================================================
log "Reading file info..."
[ -f "$SMALL_FILE" ] || die "$SMALL_FILE not found in $(pwd)"
[ -f "$LARGE_FILE" ] || die "$LARGE_FILE not found in $(pwd)"

SMALL_SIZE=$(wc -c < "$SMALL_FILE" | tr -d ' ')
LARGE_SIZE=$(wc -c < "$LARGE_FILE"  | tr -d ' ')
SMALL_MD5=$(md5_file "$SMALL_FILE")
LARGE_MD5=$(md5_file "$LARGE_FILE")

log "Small: $SMALL_FILE  size=${SMALL_SIZE}  md5=${SMALL_MD5}"
log "Large: $LARGE_FILE  size=${LARGE_SIZE}  md5=${LARGE_MD5}"

# =============================================================================
# STEP 2 — Clean tracker directory (spec: no pre-existing .track files)
# =============================================================================
log "Cleaning tracker directory: ${TRACKER_SHARED_DIR}"
rm -rf "$TRACKER_SHARED_DIR"
mkdir -p "$TRACKER_SHARED_DIR"
printf '%s\n%s\n' "$TRACKER_PORT" "$TRACKER_SHARED_DIR" > sconfig

# =============================================================================
# STEP 3 — Start tracker in its own terminal (t=0)
# =============================================================================
log "=== t=0s: Starting tracker on port ${TRACKER_PORT} ==="
DEMO_START=$(date +%s)
open_terminal "Tracker" \
    "cd '${BASE_DIR}' && ./tracker 2>&1 | tee '${LOG_DIR}/tracker.log'"
sleep 1
log "Tracker running"

# =============================================================================
# STEP 4 — Set up and start Peer1 and Peer2 (t=0)
# =============================================================================
log "=== t=0s: Starting Peer1 and Peer2 ==="

setup_peer_dir 1 "${PEER_PORTS[1]}"
setup_peer_dir 2 "${PEER_PORTS[2]}"

cp "$SMALL_FILE" "peer1/${SHARED_FOLDER}/${SMALL_FILE}"
cp "$LARGE_FILE"  "peer2/${SHARED_FOLDER}/${LARGE_FILE}"

start_peer 1
start_peer 2
sleep 2

# =============================================================================
# STEP 5 — Peer1 and Peer2 send createtracker (t=0)
# =============================================================================
log "Peer1 and Peer2 sending createtracker..."

send_cmd 1 "createtracker ${SMALL_FILE} ${SMALL_SIZE} small_test_file ${SMALL_MD5} ${TRACKER_IP} ${PEER_PORTS[1]}"
send_cmd 2 "createtracker ${LARGE_FILE} ${LARGE_SIZE} large_test_file ${LARGE_MD5} ${TRACKER_IP} ${PEER_PORTS[2]}"

# Wait for tracker to write .track files
sleep 5

# Send updatetracker so .track files have peer entries before Peer3 starts
# end_byte must be filesize-1 (last valid byte offset, not filesize)
SMALL_END=$((SMALL_SIZE - 1))
LARGE_END=$((LARGE_SIZE - 1))
send_cmd 1 "updatetracker ${SMALL_FILE} 0 ${SMALL_END}"
send_cmd 2 "updatetracker ${LARGE_FILE} 0 ${LARGE_END}"
sleep 3

# =============================================================================
# STEP 6 — t=30s: Start Peer3 to Peer8 list then download both files
# =============================================================================
sleep_until "$T_WAVE1"
log "=== t=$(elapsed_t)s: Starting Peers3-8 ==="
for n in 3 4 5 6 7 8; do
    setup_peer_dir "$n" "${PEER_PORTS[$n]}"
    start_peer "$n"
    sleep 1
    send_cmd "$n" "list"
    sleep 1
    send_cmd "$n" "get ${SMALL_FILE}.track"
    sleep 1
    send_cmd "$n" "get ${LARGE_FILE}.track"
done
# =============================================================================
# STEP 7 — t=90s: Start Peer9 to Peer13. Stop Peer1 and Peer2 
# =============================================================================
sleep_until "$T_WAVE2"
log "=== t=$(elapsed_t)s: Starting Peers9–13 AND stopping Peers 1 & 2 ==="
for n in 9 10 11 12 13; do
    setup_peer_dir "$n" "${PEER_PORTS[$n]}"
    start_peer "$n"
    sleep 1
    send_cmd "$n" "list"
    sleep 1
    send_cmd "$n" "get ${SMALL_FILE}.track"
    sleep 1
    send_cmd "$n" "get ${LARGE_FILE}.track"
done
stop_peer 1
stop_peer 2

# =============================================================================
# STEP 8 — Wait for Peer3 to finish then stop it
# Polls every 5 seconds — checks that both files have reached their
# expected sizes before stopping, not just that they exist
# =============================================================================
log "Waiting up to ${DOWNLOAD_TIMEOUT}s for Peer3-13 to complete downloads..."

elapsed=0
while [ $elapsed -lt $DOWNLOAD_TIMEOUT ]; do
   all_done=true
    for n in 3 4 5 6 7 8 9 10 11 12 13; do
        small_path="peer${n}/${SHARED_FOLDER}/${SMALL_FILE}"
        large_path="peer${n}/${SHARED_FOLDER}/${LARGE_FILE}"
        small_ok=false
        large_ok=false
        if [ -f "$small_path" ]; then
            sz=$(wc -c < "$small_path" | tr -d ' ')
            [ "$sz" -ge "$SMALL_SIZE" ] && small_ok=true
        fi
        if [ -f "$large_path" ]; then
            sz=$(wc -c < "$large_path" | tr -d ' ')
            [ "$sz" -ge "$LARGE_SIZE" ] && large_ok=true
        fi
        if ! $small_ok || ! $large_ok; then
            all_done=false
            break
        fi
    done
    if $all_done; then
        log "All Peers 3–13 completed downloads at ${elapsed}s"
        break
    fi

    sleep 5
    elapsed=$((elapsed + 5))
done

# Send quit to Peer3 immediately once downloads are done
log "Stopping Peers 3-13..."
for n in 3 4 5 6 7 8 9 10 11 12 13; do
    stop_peer "$n"
done
sleep 1

# =============================================================================
# STEP 9 — Verify MD5 of downloaded files
# =============================================================================
sleep 3   # give OS time to fully flush file writes
log "=== Verifying downloads ==="

all_pass=true
for n in 3 4 5 6 7 8 9 10 11 12 13; do
    small_path="peer${n}/${SHARED_FOLDER}/${SMALL_FILE}"
    large_path="peer${n}/${SHARED_FOLDER}/${LARGE_FILE}"

    log "Checking: ${small_path}"
    log "Checking: ${large_path}"

    small_ok="FAIL"; large_ok="FAIL"
    actual_small=""; actual_large=""

    # Check small file
    if [ ! -f "$small_path" ]; then
        log "MISSING: ${small_path} does not exist"
    else
        actual_small_size=$(wc -c < "$small_path" | tr -d ' ')
        actual_small=$(md5_file "$small_path")
        log "Peer${n} found ${SMALL_FILE}: size=${actual_small_size} (expected=${SMALL_SIZE}) md5=${actual_small}"
        [ "$actual_small" = "$SMALL_MD5" ] && small_ok="OK"
    fi

    # Check large file
    if [ ! -f "$large_path" ]; then
        log "MISSING: ${large_path} does not exist"
    else
        actual_large_size=$(wc -c < "$large_path" | tr -d ' ')
        actual_large=$(md5_file "$large_path")
        log "Peer${n} found ${LARGE_FILE}: size=${actual_large_size} (expected=${LARGE_SIZE}) md5=${actual_large}"
        [ "$actual_large" = "$LARGE_MD5" ] && large_ok="OK"
    fi

    echo "Peer${n}: ${SMALL_FILE} [${small_ok}]  ${LARGE_FILE} [${large_ok}]"

    if [ "$small_ok" != "OK" ] || [ "$large_ok" != "OK" ]; then
        all_pass=false
        log "WARNING: Peer${n} failed MD5 check"
        log "Expected  small md5: ${SMALL_MD5}"
        log "Got       small md5: ${actual_small}"
        log "Expected  large md5: ${LARGE_MD5}"
        log "Got       large md5: ${actual_large}"
    fi
done

if $all_pass; then
    log "All downloads verified successfully across all 11 downloader peers"
else
    log "WARNING: some downloads failed MD5 check — check logs in ${LOG_DIR}/"
fi

log "=== Demo complete. Logs in ${LOG_DIR}/ ==="
