/* peer.c — CS4390 Project
   Combined Peer: tracker communication, upload server, downloader

   Usage: ./peer <PeerID>

   clientThreadConfig.cfg   line1=tracker_port  line2=tracker_ip  line3=update_interval_secs
   serverThreadConfig.cfg   line1=upload_port   line2=shared_folder_name
*/

#define _POSIX_C_SOURCE 200112L

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAXLINE          512
#define MAX_CHUNK_SIZE   1024   /* max bytes per chunk request — enforced on both sides */
#define MAX_PEERS        256
#define MAX_SEGMENTS     256
#define MAX_CHUNKS       16384
#define MAX_REQUESTS     20
#define MAX_SHARED_FILES 32
#define DEFAULT_INTERVAL 900    /* 15 minutes — default updatetracker period per spec */

/* Peer ID prefix on every log line, set from argv[1] at startup */
static char peer_id[64] = "Peer";
#define plog(fmt, ...) printf("%s: " fmt "\n", peer_id, ##__VA_ARGS__)

/* ---- Structs ---- */

/* Tracker server connection info, loaded from clientThreadConfig.cfg */
typedef struct { char ip[256]; int port; int update_interval; } TrackerConfig;

/* Metadata for a file this peer is sharing, used in createtracker/updatetracker */
typedef struct {
    char filename[256], filesize[256], description[256];
    char md5[64], ip[256], port[16];
} FileInfo;

/* One peer entry parsed from a .track file — who has which byte range */
typedef struct {
    char peer_ip[64]; int peer_port;
    long byteStart, byteEnd; double timestamp;
} PeerSegment;

/* A byte range with all peers that have it */
typedef struct {
    long byteStart, byteEnd;
    PeerSegment peers[MAX_PEERS]; int peer_count;
} Segment;

/* One downloaded chunk held in memory before the final file merge */
typedef struct { long byteStart; char *data; long size; } ChunkResult;

/* Arguments passed to each per-chunk download thread */
typedef struct {
    Segment segment; ChunkResult *results;
    int *resultCount; int maxResults;
    pthread_mutex_t *lock; char filename[256];
} ThreadArgs;

/* Full parsed contents of a .track file */
typedef struct {
    char filename[256]; long filesize; char md5[64];
    Segment segments[MAX_SEGMENTS]; int segmentCount;
} TrackInfo;

/* ---- Globals ---- */
static TrackerConfig   tracker;
static int             upload_port       = 0;
static char            upload_ip[256]    = "127.0.0.1";
static char            shared_dir[256]   = "shared";
static FileInfo        shared_files[MAX_SHARED_FILES];   /* files registered via createtracker */
static int             shared_file_count = 0;
static pthread_mutex_t shared_files_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- Forward Declarations ---- */

/* Config */
static void load_config(void);           /* reads clientThreadConfig.cfg and serverThreadConfig.cfg */
static void resume_from_cache(void);     /* scans cache/ on startup and resumes incomplete downloads */

/* TCP helpers */
static int    tcpConnect(const char *host, int port, int timeoutSec); /* open TCP connection with timeout */
static int    connect_to_tracker(void);                               /* tcpConnect to the tracker server */
static int    sendAll(int fd, const char *buf, size_t len);           /* send all bytes, handles short writes */
static size_t recvExact(int fd, char *buf, size_t len);               /* receive exactly len bytes */
static int    recvLine(int fd, char *buf, size_t maxlen);             /* receive one complete line ending in \n */

/* Tracker commands — peer to tracker protocol */
static void send_list(void);                                    /* REQ LIST   — get all available .track files */
static void send_get(char *filename);                           /* GET        — fetch a specific .track file */
static void send_createtracker(FileInfo *f);                    /* createtracker — register a file on tracker */
static void send_updatetracker(char *filename,                  /* updatetracker — report bytes we now have */
                                char *start_byte, char *end_byte);

/* Background threads */
static void *update_thread(void *arg);        /* periodically sends updatetracker for all shared files */
static void *start_upload_server(void *arg);  /* TCP server — accepts chunk requests from other peers */
static void *handle_upload_client(void *arg); /* handles one incoming chunk request per thread */

/* Downloader */
static int               cmpSegments(const void *a, const void *b);  /* qsort comparator for Segment */
static int               cmpChunks(const void *a, const void *b);    /* qsort comparator for ChunkResult */
static int               parse_track_file(const char *path, TrackInfo *info); /* parse a .track file */
static const PeerSegment *bestPeer(const Segment *seg);               /* pick peer with newest timestamp */
static char              *downloadChunk(const PeerSegment *peer,      /* fetch one 1024-byte chunk from a peer */
                                        long chunkStart, long chunkEnd, long *outSize);
static void              *thread_download_segment(void *arg);         /* thread: download one segment */
static int                downloadFile(const char *track_path);       /* orchestrate full file download */

/* ---- Config ---- */

/* load_config: reads both config files.
   clientThreadConfig.cfg supplies the tracker address and update interval.
   serverThreadConfig.cfg supplies the upload port and shared folder name.
   The peer's own IP is detected automatically via gethostname. */
static void load_config(void)
{
    FILE *f = fopen("clientThreadConfig.cfg", "r");
    if (!f) { fprintf(stderr, "%s: Cannot open clientThreadConfig.cfg\n", peer_id); exit(1); }
    char port_str[16], interval_str[16];
    if (!fgets(port_str, sizeof(port_str), f) ||
        !fgets(tracker.ip, sizeof(tracker.ip), f) ||
        !fgets(interval_str, sizeof(interval_str), f)) {
        fprintf(stderr, "%s: clientThreadConfig.cfg incomplete\n", peer_id);
        fclose(f); exit(1);
    }
    fclose(f);
    tracker.ip[strcspn(tracker.ip, "\n")]     = '\0';
    port_str[strcspn(port_str, "\n")]         = '\0';
    interval_str[strcspn(interval_str, "\n")] = '\0';
    tracker.port            = atoi(port_str);
    tracker.update_interval = atoi(interval_str);
    if (tracker.port <= 0) { fprintf(stderr, "%s: Invalid tracker port\n", peer_id); exit(1); }
    if (tracker.update_interval <= 0) tracker.update_interval = DEFAULT_INTERVAL;

    FILE *sf = fopen("serverThreadConfig.cfg", "r");
    if (!sf) { fprintf(stderr, "%s: serverThreadConfig.cfg missing — upload disabled\n", peer_id); return; }
    char s_port[32];
    if (!fgets(s_port, sizeof(s_port), sf) || !fgets(shared_dir, sizeof(shared_dir), sf)) {
        fprintf(stderr, "%s: serverThreadConfig.cfg incomplete\n", peer_id);
        fclose(sf); return;
    }
    fclose(sf);
    s_port[strcspn(s_port, "\n")]         = '\0';
    shared_dir[strcspn(shared_dir, "\n")] = '\0';
    upload_port = atoi(s_port);
    if (upload_port <= 0) { fprintf(stderr, "%s: Invalid upload port\n", peer_id); upload_port = 0; return; }

    /* Detect local IP using UDP socket trick — more reliable than gethostname
       which often resolves to 127.0.0.1 on Mac. We connect a UDP socket to
       the tracker address (no data sent) and read the local address the OS
       chose, which is the correct outbound interface IP. */
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp >= 0) {
        struct sockaddr_in remote;
        memset(&remote, 0, sizeof(remote));
        remote.sin_family = AF_INET;
        remote.sin_port   = htons((uint16_t)tracker.port);
        inet_pton(AF_INET,tracker.ip, &remote.sin_addr);
        if (connect(udp, (struct sockaddr *)&remote, sizeof(remote)) == 0) {
            struct sockaddr_in local;
            socklen_t local_len = sizeof(local);
            if (getsockname(udp, (struct sockaddr *)&local, &local_len) == 0) {
                const char *detected = inet_ntoa(local.sin_addr);
                /* Only use if it is not a loopback address */
                if (strncmp(detected, "127.", 4) != 0)
                    strncpy(upload_ip, detected, sizeof(upload_ip) - 1);
            }
        }
        close(udp);
    }
}

/* ---- TCP Helpers ---- */

/* tcpConnect: opens a TCP socket to host:port with a send/recv timeout.
   Returns the socket fd on success, -1 on failure. */
static int tcpConnect(const char *host, int port, int timeoutSec)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char ps[16]; snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    struct timeval tv = { timeoutSec, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res); return fd;
}

/* connect_to_tracker: convenience wrapper that always connects to the
   global tracker address loaded from config. */
static int connect_to_tracker(void)
{
    int fd = tcpConnect(tracker.ip, tracker.port, 10);
    if (fd < 0) fprintf(stderr, "%s: Cannot connect to tracker\n", peer_id);
    return fd;
}

/* sendAll: keeps sending until all len bytes are written.
   Handles short writes that can occur on busy sockets. */
static int sendAll(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* recvExact: keeps receiving until exactly len bytes arrive or the
   connection closes. Used when we know the expected payload size. */
static size_t recvExact(int fd, char *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
    }
    return got;
}

/* recvLine: reads one character at a time until \n or connection closes.
   This guarantees each call returns exactly one complete line regardless
   of how TCP segments the data — fixes the bug where recv returns multiple
   lines in one call and strncmp only checks the start of the buffer,
   causing entire blocks of content to be silently skipped. */
static int recvLine(int fd, char *buf, size_t maxlen)
{
    size_t pos = 0;
    while (pos < maxlen - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) break;
        buf[pos++] = c;
        if (c == '\n') break;
    }
    buf[pos] = '\0';
    return (int)pos;
}

/* ---- Tracker Commands ---- */

/* send_list: sends <REQ LIST> to the tracker and prints the response.
   Uses recvLine so each line is processed individually — fixes the bug
   where a single recv call returns multiple lines and the end marker
   check only tests the start of the buffer. */
static void send_list(void)
{
    int fd = connect_to_tracker(); if (fd < 0) return;
    sendAll(fd, "<REQ LIST>\n", 11);
    char line[MAXLINE];
    printf("\n--- Available Files ---\n");
    while (recvLine(fd, line, sizeof(line)) > 0) {
        printf("%s", line);
        if (strstr(line, "<REP LIST END>")) break;
    }
    close(fd);
}

/* send_get: sends <GET filename.track> to the tracker and saves the
   response to cache/filename.
   Uses recvLine to process one line at a time — fixes the critical bug
   where recv returns multiple lines in one call (e.g. <REP GET BEGIN>
   plus Filename: in the same buffer), causing the entire content to be
   skipped by the strncmp check and writing an empty cache file.
   Verifies MD5 from <REP GET END md5> against MD5 in the tracker file. */
static void send_get(char *filename)
{
    int fd = connect_to_tracker(); if (fd < 0) return;
    char msg[MAXLINE];
    snprintf(msg, sizeof(msg), "<GET %s>\n", filename);
    sendAll(fd, msg, strlen(msg));

    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "cache/%s", filename);
    FILE *cf = fopen(cache_path, "w");
    if (!cf) fprintf(stderr, "%s: Cannot write cache file %s\n", peer_id, cache_path);

    char rep_md5[64] = {0}, file_md5[64] = {0};
    char line[MAXLINE];
    plog("Receiving tracker file: %s", filename);

    while (recvLine(fd, line, sizeof(line)) > 0) {
        /* Skip the protocol header line — do not write to cache */
        if (strncmp(line, "<REP GET BEGIN>", 15) == 0) continue;
        /* End marker — extract MD5 and stop */
        if (strncmp(line, "<REP GET END", 12) == 0) {
            sscanf(line, "<REP GET END %63s>", rep_md5);
            rep_md5[strcspn(rep_md5, ">")] = '\0';
            break;
        }
        /* Capture MD5 field from inside the tracker file */
        if (strncmp(line, "MD5:", 4) == 0) sscanf(line, "MD5: %63s", file_md5);
        /* Write clean content to cache */
        if (cf) fprintf(cf, "%s", line);
    }
    if (cf) fclose(cf);
    close(fd);

    /* MD5 verification — discard cache file if checksums do not match */
    if (rep_md5[0] && file_md5[0]) {
        if (strcmp(rep_md5, file_md5) == 0)
            plog("MD5 verified OK for %s", filename);
        else {
            plog("MD5 MISMATCH for %s — discarding", filename);
            remove(cache_path);
        }
    }
}

/* send_createtracker: sends <createtracker filename filesize description
   md5 ip port> to the tracker.  The tracker creates a .track file on
   disk.  Only peers that hold the complete file should call this.
   After a success response the file is added to shared_files[] so the
   periodic update thread keeps it fresh. */
static void send_createtracker(FileInfo *f)
{
    int fd = connect_to_tracker(); if (fd < 0) return;
    char msg[MAXLINE];
    snprintf(msg, sizeof(msg), "<createtracker %s %s %s %s %s %s>\n",
             f->filename, f->filesize, f->description, f->md5, f->ip, f->port);
    sendAll(fd, msg, strlen(msg));
    char buf[MAXLINE];
    int n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) { buf[n] = '\0'; buf[strcspn(buf, "\r\n")] = '\0'; plog("Tracker: %s", buf); }
    close(fd);
}

/* send_updatetracker: sends <updatetracker filename start end ip port>
   to the tracker to report which byte range this peer currently holds.
   Called manually, by the periodic update thread, and automatically
   after each segment is successfully downloaded. */
static void send_updatetracker(char *filename, char *start_byte, char *end_byte)
{
    int fd = connect_to_tracker(); if (fd < 0) return;
    char msg[MAXLINE];
    snprintf(msg, sizeof(msg), "<updatetracker %s %s %s %s %d>\n",
             filename, start_byte, end_byte, upload_ip, upload_port);
    sendAll(fd, msg, strlen(msg));
    char buf[MAXLINE];
    int n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) { buf[n] = '\0'; buf[strcspn(buf, "\r\n")] = '\0'; plog("Tracker: %s", buf); }
    close(fd);
}

/* ---- Background Threads ---- */

/* update_thread: runs continuously in the background.
   Every update_interval seconds it sends updatetracker for every file
   in shared_files[], keeping this peer's entries alive in the tracker.
   A snapshot of shared_files[] is taken under the lock so network calls
   are made without holding the mutex. */
static void *update_thread(void *arg)
{
    (void)arg;
    while (1) {
        sleep((unsigned int)tracker.update_interval);
        pthread_mutex_lock(&shared_files_lock);
        int count = shared_file_count;
        FileInfo snap[MAX_SHARED_FILES];
        memcpy(snap, shared_files, (size_t)count * sizeof(FileInfo));
        pthread_mutex_unlock(&shared_files_lock);
        if (count == 0) continue;
        plog("Sending updatetracker for %d file(s)", count);
        for (int i = 0; i < count; i++)
            send_updatetracker(snap[i].filename, "0", snap[i].filesize);
    }
    return NULL;
}

/* handle_upload_client: handles one incoming chunk request from another peer.
   Protocol: peer sends "GET <start_byte> <end_byte>\n", we respond with
   exactly (end - start + 1) raw bytes read from the shared file.
   Chunk size is capped at MAX_CHUNK_SIZE (1024 bytes) per spec — requests
   above this limit receive <GET invalid> and the connection is closed. */
static void *handle_upload_client(void *arg)
{
    int sock = *(int *)arg; free(arg);

    /* Read request line byte-by-byte to avoid consuming file payload */
    char request[MAXLINE]; memset(request, 0, sizeof(request));
    size_t pos = 0;
    while (pos < sizeof(request) - 1) {
        ssize_t n = recv(sock, request + pos, 1, 0);
        if (n <= 0) goto cleanup;
        if (request[pos] == '\n') { request[pos] = '\0'; break; }
        pos++;
    }

    if (strncmp(request, "<GET invalid>", 13) == 0) goto cleanup;

    long byte_start = -1, byte_end = -1;
    if (sscanf(request, "GET %ld %ld", &byte_start, &byte_end) != 2
            || byte_start < 0 || byte_end < byte_start) goto cleanup;

    long chunk_size = byte_end - byte_start + 1;
    if (chunk_size > MAX_CHUNK_SIZE) {
        sendAll(sock, "<GET invalid>\n", 14); goto cleanup;
    }

    /* Snapshot file paths outside the lock then stat without holding it */
    pthread_mutex_lock(&shared_files_lock);
    int nfiles = shared_file_count;
    char candidates[MAX_SHARED_FILES][512];
    for (int i = 0; i < nfiles; i++)
        snprintf(candidates[i], sizeof(candidates[i]),
                 "%s/%s", shared_dir, shared_files[i].filename);
    pthread_mutex_unlock(&shared_files_lock);

    /* Find the first registered file whose size covers the requested range */
    char filepath[512] = {0};
    for (int i = 0; i < nfiles; i++) {
        struct stat st;
        if (stat(candidates[i], &st) == 0 && byte_end < st.st_size) {
            snprintf(filepath, sizeof(filepath), "%s", candidates[i]); break;
        }
    }
    if (filepath[0] == '\0') goto cleanup;

    FILE *fptr = fopen(filepath, "rb");
    if (!fptr) goto cleanup;
    if (fseeko(fptr, (off_t)byte_start, SEEK_SET) != 0) { fclose(fptr); goto cleanup; }

    char *buf = malloc((size_t)chunk_size);
    if (!buf) { fclose(fptr); goto cleanup; }
    size_t got = fread(buf, 1, (size_t)chunk_size, fptr);
    fclose(fptr);

    if ((long)got == chunk_size && sendAll(sock, buf, (size_t)chunk_size) == 0)
        plog("[Upload] Served bytes %ld-%ld from %s", byte_start, byte_end, filepath);
    free(buf);

cleanup:
    close(sock); return NULL;
}

/* start_upload_server: binds a TCP socket on upload_port and loops
   accepting connections.  Each connection is handed to a new detached
   handle_upload_client thread so multiple peers can download from us
   simultaneously. */
static void *start_upload_server(void *arg)
{
    (void)arg;
    if (upload_port <= 0) return NULL;

    int sockid = socket(AF_INET, SOCK_STREAM, 0);
    if (sockid < 0) { perror("upload socket"); return NULL; }
    int yes = 1; setsockopt(sockid, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET; addr.sin_port = htons(upload_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    memset(addr.sin_zero, '\0', sizeof(addr.sin_zero));

    if (bind(sockid, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(sockid, MAX_REQUESTS) < 0) {
        perror("upload bind/listen"); close(sockid); return NULL;
    }
    plog("[Upload] Listening on port %d", upload_port);

    struct sockaddr_in ca; socklen_t cl = sizeof(ca);
    while (1) {
        int child = accept(sockid, (struct sockaddr *)&ca, &cl);
        if (child < 0) { perror("upload accept"); continue; }
        int *sp = malloc(sizeof(int));
        if (!sp) { close(child); continue; }
        *sp = child;
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_upload_client, sp) != 0) {
            free(sp); close(child); continue;
        }
        pthread_detach(tid);
    }
    close(sockid); return NULL;
}

/* ---- Downloader ---- */

/* cmpSegments / cmpChunks: qsort comparators that sort by byteStart
   so segments and chunks are processed in sequential byte order. */
static int cmpSegments(const void *a, const void *b)
{
    const Segment *sa = a, *sb = b;
    return (sa->byteStart > sb->byteStart) - (sa->byteStart < sb->byteStart);
}

static int cmpChunks(const void *a, const void *b)
{
    const ChunkResult *ca = a, *cb = b;
    return (ca->byteStart > cb->byteStart) - (ca->byteStart < cb->byteStart);
}

/* parse_track_file: reads a .track file and populates a TrackInfo struct.
   Skips <REP GET BEGIN> and <REP GET END> protocol lines so it works
   whether the file was saved from a GET response or written directly.
   Each peer line (ip:port:start:end:timestamp) is grouped into Segments
   by byte range so multiple peers sharing the same range are collected. */
static int parse_track_file(const char *path, TrackInfo *info)
{
    memset(info, 0, sizeof(*info));
    FILE *fh = fopen(path, "r");
    if (!fh) { perror("parse_track_file"); return -1; }
    char line[512];

    /* Scan past any protocol preamble until we find the Filename: field */
    while (fgets(line, sizeof(line), fh))
        if (sscanf(line, "Filename: %255s", info->filename) == 1) break;
    if (info->filename[0] == '\0') goto fail;

    if (!fgets(line, sizeof(line), fh)) goto fail;
    if (sscanf(line, "Filesize: %ld", &info->filesize) != 1) goto fail;
    if (!fgets(line, sizeof(line), fh)) goto fail;      /* description — skip */
    if (!fgets(line, sizeof(line), fh)) goto fail;      /* MD5 */
    sscanf(line, "MD5: %63s", info->md5);

    /* Parse peer entries, skipping blank lines, comments, and protocol tags */
    while (fgets(line, sizeof(line), fh)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#' || line[0] == '<') continue;
        char ip[64]; int port; long start, end; double ts;
        if (sscanf(line, "%63[^:]:%d:%ld:%ld:%lf", ip, &port, &start, &end, &ts) != 5) continue;

        /* Find or create a segment matching this byte range */
        int si = -1;
        for (int i = 0; i < info->segmentCount; i++)
            if (info->segments[i].byteStart == start && info->segments[i].byteEnd == end)
                { si = i; break; }
        if (si == -1) {
            if (info->segmentCount >= MAX_SEGMENTS) continue;
            si = info->segmentCount++;
            info->segments[si].byteStart  = start;
            info->segments[si].byteEnd    = end;
            info->segments[si].peer_count = 0;
        }
        Segment *seg = &info->segments[si];
        if (seg->peer_count < MAX_PEERS) {
            PeerSegment *ps = &seg->peers[seg->peer_count++];
            strncpy(ps->peer_ip, ip, 64 - 1); ps->peer_ip[64-1] = '\0';
            ps->peer_port = port; ps->byteStart = start;
            ps->byteEnd   = end;  ps->timestamp  = ts;
        }
    }
    fclose(fh);
    if (info->segmentCount == 0) {
        fprintf(stderr, "%s: No peer entries in %s\n", peer_id, path); return -1;
    }
    qsort(info->segments, (size_t)info->segmentCount, sizeof(Segment), cmpSegments);
    return 0;
fail:
    fclose(fh);
    fprintf(stderr, "%s: Track file invalid: %s\n", peer_id, path);
    return -1;
}

/* bestPeer: from all peers that have a segment, returns the one with the
   most recent timestamp — per spec peer selection strategy. */
static const PeerSegment *bestPeer(const Segment *seg)
{
    if (seg->peer_count == 0) return NULL;
    int best = 0;
    for (int i = 1; i < seg->peer_count; i++)
        if (seg->peers[i].timestamp > seg->peers[best].timestamp) best = i;
    return &seg->peers[best];
}

/* downloadChunk: connects to a peer and requests bytes [chunkStart, chunkEnd].
   Sends "GET <start> <end>\n" and reads back exactly chunkSize raw bytes.
   Returns a malloc'd buffer on success, NULL on any failure (dead peer,
   short read, oversized chunk).  Caller must free the returned buffer. */
static char *downloadChunk(const PeerSegment *peer,
                            long chunkStart, long chunkEnd, long *outSize)
{
    long chunkSize = chunkEnd - chunkStart + 1;
    *outSize = 0;
    if (chunkSize > MAX_CHUNK_SIZE) {
        int fd = tcpConnect(peer->peer_ip, peer->peer_port, 5);
        if (fd >= 0) { sendAll(fd, "<GET invalid>\n", 14); close(fd); }
        return NULL;
    }
    int fd = tcpConnect(peer->peer_ip, peer->peer_port, 10);
    if (fd < 0) { fprintf(stderr, "%s: Dead peer %s:%d\n", peer_id, peer->peer_ip, peer->peer_port); return NULL; }

    char req[128];
    int rl = snprintf(req, sizeof(req), "GET %ld %ld\n", chunkStart, chunkEnd);
    if (sendAll(fd, req, (size_t)rl) < 0) { close(fd); return NULL; }

    char *data = malloc((size_t)chunkSize);
    if (!data) { close(fd); return NULL; }
    size_t got = recvExact(fd, data, (size_t)chunkSize);
    close(fd);
    if ((long)got != chunkSize) { free(data); return NULL; }
    *outSize = chunkSize;
    return data;
}

/* thread_download_segment: thread entry point for downloading one segment.
   Splits the segment into MAX_CHUNK_SIZE chunks, downloads each in sequence
   from the best peer, and stores results in the shared ChunkResult array
   protected by a mutex.  Logs each chunk download as the spec requires. */
static void *thread_download_segment(void *arg)
{
    ThreadArgs *ta = (ThreadArgs *)arg;
    Segment    *seg = &ta->segment;
    const PeerSegment *peer = bestPeer(seg);
    if (!peer) { fprintf(stderr, "%s: No peers for segment %ld-%ld\n", peer_id, seg->byteStart, seg->byteEnd); return NULL; }

    for (long pos = seg->byteStart; pos <= seg->byteEnd; ) {
        long ce = pos + MAX_CHUNK_SIZE - 1;
        if (ce > seg->byteEnd) ce = seg->byteEnd;

        plog("downloading %ld to %ld bytes of %s from %s %d",
             pos, ce, ta->filename, peer->peer_ip, peer->peer_port);

        long dataSize = 0;
        char *data = downloadChunk(peer, pos, ce, &dataSize);
        if (data) {
            pthread_mutex_lock(ta->lock);
            if (*ta->resultCount < ta->maxResults) {
                int idx = (*ta->resultCount)++;
                ta->results[idx].byteStart = pos;
                ta->results[idx].data      = data;
                ta->results[idx].size      = dataSize;
            } else { free(data); }
            pthread_mutex_unlock(ta->lock);
        }
        pos = ce + 1;
    }
    return NULL;
}

/* downloadFile: orchestrates downloading a complete file described by a
   .track file.  For each segment it spawns one thread per chunk, joins
   all threads, then sends updatetracker to report the completed segment.
   Once all segments are done, chunks are sorted and merged into the final
   output file in shared_dir/.  The .track cache file is deleted on success. */
static int downloadFile(const char *track_path)
{
    TrackInfo info;
    plog("Parsing: %s", track_path);
    if (parse_track_file(track_path, &info) < 0) return -1;
    plog("Downloading %s  size=%ld  segments=%d", info.filename, info.filesize, info.segmentCount);

    ChunkResult *results = calloc(MAX_CHUNKS, sizeof(ChunkResult));
    if (!results) { perror("calloc"); return -1; }
    int resultCount = 0;
    pthread_mutex_t lock; pthread_mutex_init(&lock, NULL);

    for (int si = 0; si < info.segmentCount; si++) {
        Segment *seg = &info.segments[si];
        plog("[Segment %d/%d] bytes %ld-%ld", si+1, info.segmentCount, seg->byteStart, seg->byteEnd);

        int maxCPS = MAX_CHUNKS / (info.segmentCount > 0 ? info.segmentCount : 1) + 2;
        long *cs = malloc((size_t)maxCPS * sizeof(long));
        long *ce = malloc((size_t)maxCPS * sizeof(long));
        if (!cs || !ce) { free(cs); free(ce); free(results); pthread_mutex_destroy(&lock); return -1; }

        int cc = 0;
        for (long pos = seg->byteStart; pos <= seg->byteEnd && cc < maxCPS; ) {
            long end = pos + MAX_CHUNK_SIZE - 1;
            if (end > seg->byteEnd) end = seg->byteEnd;
            cs[cc] = pos; ce[cc] = end; cc++; pos = end + 1;
        }

        pthread_t  *threads = malloc((size_t)cc * sizeof(pthread_t));
        ThreadArgs *targs   = malloc((size_t)cc * sizeof(ThreadArgs));
        if (!threads || !targs) {
            free(threads); free(targs); free(cs); free(ce);
            free(results); pthread_mutex_destroy(&lock); return -1;
        }

        int created = 0;
        for (int c = 0; c < cc; c++) {
            targs[c].segment           = *seg;
            targs[c].segment.byteStart = cs[c];
            targs[c].segment.byteEnd   = ce[c];
            targs[c].results           = results;
            targs[c].resultCount       = &resultCount;
            targs[c].maxResults        = MAX_CHUNKS;
            targs[c].lock              = &lock;
            snprintf(targs[c].filename, 256, "%s", info.filename);
            if (pthread_create(&threads[c], NULL, thread_download_segment, &targs[c]) != 0) break;
            created++;
        }
        for (int c = 0; c < created; c++) pthread_join(threads[c], NULL);
        free(threads); free(targs); free(cs); free(ce);

        /* Notify tracker that we now hold this segment */
        char start[32], end[32];
        snprintf(start, sizeof(start), "%ld", seg->byteStart);
        snprintf(end,   sizeof(end),   "%ld", seg->byteEnd);
        send_updatetracker(info.filename, start, end);
    }

    /* Sort chunks by offset then write sequentially into the output file */
    qsort(results, (size_t)resultCount, sizeof(ChunkResult), cmpChunks);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/%s", shared_dir, info.filename);
    FILE *out = fopen(outpath, "wb");
    if (!out) {
        fprintf(stderr, "%s: Cannot open %s: %s\n", peer_id, outpath, strerror(errno));
        free(results); pthread_mutex_destroy(&lock); return -1;
    }
    if (info.filesize > 0) { fseeko(out, (off_t)(info.filesize-1), SEEK_SET); fputc('\0', out); fseeko(out, 0, SEEK_SET); }
    for (int i = 0; i < resultCount; i++) {
        fseeko(out, (off_t)results[i].byteStart, SEEK_SET);
        fwrite(results[i].data, 1, (size_t)results[i].size, out);
        free(results[i].data);
    }
    fclose(out); free(results); pthread_mutex_destroy(&lock);

    struct stat st; stat(outpath, &st);
    plog("Download complete: %s (%ld bytes)", outpath, (long)st.st_size);
    if (remove(track_path) == 0) plog("Deleted cache entry: %s", track_path);
    return 0;
}

/* resume_from_cache: called at startup to handle incomplete downloads from
   a previous session.  Scans cache/ for any .track files still present,
   checks if the corresponding file in shared_dir/ is smaller than the
   expected filesize, and resumes the download if so. */
static void resume_from_cache(void)
{
    mkdir("cache", 0755); mkdir(shared_dir, 0755);
    DIR *d = opendir("cache");
    if (!d) return;
    plog("Checking cache for incomplete downloads...");
    int found = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strstr(de->d_name, ".track")) continue;
        char track_path[512];
        snprintf(track_path, sizeof(track_path), "cache/%s", de->d_name);
        TrackInfo info;
        if (parse_track_file(track_path, &info) < 0) continue;
        char dest[512]; snprintf(dest, sizeof(dest), "%s/%s", shared_dir, info.filename);
        struct stat st;
        if (stat(dest, &st) == 0 && st.st_size >= info.filesize) continue;
        plog("Resuming: %s", info.filename);
        downloadFile(track_path); found++;
    }
    closedir(d);
    if (found > 0) plog("Resumed %d download(s)", found);
    else           plog("No incomplete downloads");
}

/* ---- main ---- */
int main(int argc, char *argv[])
{
    if (argc >= 2) snprintf(peer_id, sizeof(peer_id), "%s", argv[1]);

    load_config();
    plog("Tracker %s:%d  upload_port=%d  shared=%s",
         tracker.ip, tracker.port, upload_port, shared_dir);

    resume_from_cache();

    /* Start upload server and periodic update threads in the background */
    pthread_t tid;
    if (pthread_create(&tid, NULL, start_upload_server, NULL) == 0) pthread_detach(tid);
    if (pthread_create(&tid, NULL, update_thread, NULL) == 0) pthread_detach(tid);

    /* Command loop — accepts manual and scripted commands from stdin */
    char command[MAXLINE];
    while (1) {
        printf("\n%s> ", peer_id); fflush(stdout);
        if (fgets(command, sizeof(command), stdin) == NULL) break;
        command[strcspn(command, "\n")] = '\0';

        if (strcmp(command, "list") == 0) {
            /* list: request the tracker's list of available .track files */
            plog("List"); send_list();

        } else if (strncmp(command, "get ", 4) == 0) {
            /* get <filename.track>: fetch the .track file from the tracker
               then immediately start downloading the actual file from peers */
            char filename[256] = {0};
            sscanf(command, "get %255s", filename);
            if (!filename[0]) { printf("Usage: get <filename.track>\n"); continue; }
            plog("Get %s", filename);
            send_get(filename);
            char track_path[512];
            snprintf(track_path, sizeof(track_path), "cache/%s", filename);
            struct stat st;
            if (stat(track_path, &st) != 0 || !S_ISREG(st.st_mode)) {
                fprintf(stderr, "%s: Track file not found after GET\n", peer_id); continue;
            }
            downloadFile(track_path);

        } else if (strncmp(command, "createtracker ", 14) == 0) {
            /* createtracker <filename> <filesize> <description> <md5> <ip> <port>:
               register a file this peer holds with the tracker so others can find it */
            char filename[256], filesize[256], description[256], md5[64], ip[256], port[16];
            if (sscanf(command, "createtracker %255s %255s %255s %63s %255s %15s",
                       filename, filesize, description, md5, ip, port) != 6) {
                printf("Usage: createtracker <filename> <filesize> <description> <md5> <ip> <port>\n"); continue;
            }
            plog("createtracker %s %s %s %s %s %s", filename, filesize, description, md5, ip, port);
            FileInfo f;
            snprintf(f.filename,    sizeof(f.filename),    "%s", filename);
            snprintf(f.filesize,    sizeof(f.filesize),    "%s", filesize);
            snprintf(f.description, sizeof(f.description), "%s", description);
            snprintf(f.md5,         sizeof(f.md5),         "%s", md5);
            snprintf(f.ip,          sizeof(f.ip),          "%s", ip);
            snprintf(f.port,        sizeof(f.port),        "%s", port);
            send_createtracker(&f);
            pthread_mutex_lock(&shared_files_lock);
            if (shared_file_count < MAX_SHARED_FILES) shared_files[shared_file_count++] = f;
            pthread_mutex_unlock(&shared_files_lock);

        } else if (strncmp(command, "updatetracker ", 14) == 0) {
            /* updatetracker <filename> <start_byte> <end_byte>:
               manually tell the tracker which byte range we currently hold */
            char filename[256], start[256], end[256];
            sscanf(command, "updatetracker %255s %255s %255s", filename, start, end);
            send_updatetracker(filename, start, end);

        } else if (strcmp(command, "quit") == 0) {
            plog("Exiting"); break;

        } else if (command[0] != '\0') {
            printf("Unknown command.\n");
        }
    }
    return 0;
}