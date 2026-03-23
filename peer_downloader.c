/*
 * Peer File Downloader (C)
 * ========================
 * [CS-4390.002 — Computer Networks]
 * 
 * Dividing file into segments: The downloading file is divided into many segments, and
the size of each segment could be defined in any number in advance.
 * Segment selection: The to-be-downloaded segment(s) is (are) chosen sequentially.
 * Peer selection: The peer which has the newest timestamp is selected to be connected to
download the corresponding segment. 
 * 4) Update: Only after downloading a complete segment does a peer update its record file
and sends information to tell the tracker server that it has this part of the file.
 * 
 * Downloads files from peers using the tracker's  track.c file.
 *
 *  track.c file header (written by handle_createtracker_req):
 *   Filename: <filename>
 *   Filesize: <filesize>
 *   Description: <description>
 *   MD5: <md5>
 *   #list of peers follows next
 *
 * Peer lines (written by handle_updatetracker_req):
 *   <ip>:<port>:<start_byte>:<end_byte>:<timestamp>
 *
 * Usage:
 *   ./peer_downloader <tracker_host> <tracker_port> <track_file> <my_ip> <my_port>
 *
 * Compile:
 *   gcc -std=c11 -pthread -Wall -Wextra -o peer_downloader peer_downloader.c -lpthread
 *   Makfile made
 * 
 * Error handling, Constants, and Thread Handling assisted by Claud.ai 
 *  [MUTEX NOT DESTROYED ON ERROR -> CAUSES RESOURCE LEAKS] TBD
 */

#define _POSIX_C_SOURCE 200112L            

#include <arpa/inet.h>
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

 // Constants & limits
#define MAX_CHUNK_SIZE   1024
#define MAX_PEERS        256
#define MAX_SEGMENTS     256
#define MAX_CHUNKS       4096
#define MAX_FILENAME     256
#define MAX_IP           64

/* ----------------------------------------------------------------------
 * Data structures (PeerSegment, Segment, ChunkResult, ThreadArgs, TrackInfo)
 * ---------------------------------------------------------------------- */
typedef struct {
    char peer_ip[MAX_IP];
    int peer_port;
    long byteStart;
    long byteEnd;
    double timestamp;           // higher = more recent
} 

PeerSegment;
typedef struct {
    long byteStart;
    long byteEnd;
    PeerSegment peers[MAX_PEERS];
    int peer_count;            //peer count
} 

Segment;
//Holds one chunk 
typedef struct {
    long   byteStart;
    char  *data;
    long   size;
} ChunkResult;
typedef struct {
    Segment segment;             
    ChunkResult *results;                             
    int *resultCount;            // counter                            
    pthread_mutex_t *lock;        // LOCK
} ThreadArgs;

// Parsed track.c file
typedef struct {
    char filename[MAX_FILENAME];
    long filesize;
    Segment segments[MAX_SEGMENTS];
    int segmentCount;
} 
TrackInfo;

/* ----------------------------------------------------------------------
 * Helpers (return newest, comparator, Chunk Comparator)
 * ---------------------------------------------------------------------- */

// Return the newest peer


static int tcpConnect(const char *host, int port, int timeoutSec)
{
    struct addrinfo hints, *res= NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype= SOCK_STREAM;

    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port);

    if (getaddrinfo(host, portStr, &hints, &res) != 0||!res)
        return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd< 0) { 
        freeaddrinfo(res); return -1; }

    struct timeval tv = { timeoutSec, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return fd;
}
// Send all length bytes. Returns 0 on success, -1 on error. 
static int sendAll(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent< len) {
        ssize_t n = send(fd, buf+ sent, len-sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}


    static int cmpSegments(const void *a, const void *b)       // Segment comparator for sort
    {
        const Segment *sa= (const Segment *)a;
        const Segment *sb= (const Segment *)b;
        if (sa->byteStart< sb->byteStart) 
            return -1;
        if (sa->byteStart > sb->byteStart) 
            return 1;
        return 0;
    }

/* ----------------------------------------------------------------------
 *  track.c file parser
 * ---------------------------------------------------------------------- */ 
static int parse_track_file(const char *path, TrackInfo *info)
{
    // Returns 0 on success, -1 on failure.
    memset(info, 0, sizeof(*info));

    FILE *fh= fopen(path, "r");
    if (!fh) { perror("fopen"); return -1; }

    char line[512];

    // Filename  
    if (!fgets(line, sizeof(line), fh)) goto fail;
    if (sscanf(line, "Filename: %255s", info->filename) != 1) goto fail;
    // Size
    if (!fgets(line, sizeof(line), fh)) goto fail;
    if (sscanf(line, "Filesize: %ld", &info->filesize) != 1) goto fail;
    // Description  
    if (!fgets(line, sizeof(line), fh)) goto fail;
    // MD5
    if (!fgets(line, sizeof(line), fh)) goto fail;
    if (!fgets(line, sizeof(line), fh)) goto fail;  // skip #comment line


    // Peer lines: ip:port:start:end:timestamp
    while (fgets(line, sizeof(line), fh)) {
        line[strcspn(line, "\r\n")]= '\0';
        if (line[0] == '\0' ||line[0] == '#') continue;

        char ip[MAX_IP];
        int port;
        long start, end;
        double ts;

        if (sscanf(line, "%63[^:]:%d:%ld:%ld:%lf",
                   ip, &port, &start, &end, &ts) != 5) {
            fprintf(stderr, "[WARN] Skipping malformed peer line: %s\n", line);
            continue;
        }

        // Find/create a segment
        int segIdx = -1;
        for (int i = 0; i< info->segmentCount; i++) {
            if (info->segments[i].byteStart == start &&
                info->segments[i].byteEnd == end) {
                segIdx = i;
                break;
            }
        }
        if (segIdx == -1) {
            if (info->segmentCount >= MAX_SEGMENTS) {
                fprintf(stderr, "[WARN] Too many segments — increase MAX_SEGMENTS\n");
                continue;
            }
            segIdx = info->segmentCount++;
            info->segments[segIdx].byteStart= start;
            info->segments[segIdx].byteEnd = end;
            info->segments[segIdx].peer_count= 0;
        }

        Segment *seg = &info->segments[segIdx];
        if (seg->peer_count< MAX_PEERS) {
            PeerSegment *ps = &seg->peers[seg->peer_count++];
            strncpy(ps->peer_ip, ip, MAX_IP-1);
            ps->peer_ip[MAX_IP- 1]= '\0';
            ps->peer_port= port;
            ps->byteStart= start;
            ps->byteEnd= end;
            ps->timestamp= ts;
        }
    }
    fclose(fh);

    if (info->filename[0]== '\0') {
        fprintf(stderr, "[ERROR] Track file is empty or invalid.\n");
        return -1;
    }

        // Sort segments by byteStart for sequential download
    qsort(info->segments, (size_t)info->segmentCount,
          sizeof(Segment), cmpSegments);
    return 0;

fail:
    fclose(fh);
    fprintf(stderr, "[ERROR] Track file is truncated.\n");
    return -1;
}

    // Receive exactly len bytes. Returns bytes received (< len = peer closed).  
    static size_t recvExact(int fd, char *buf, size_t len)
    {
        size_t got = 0;
        while (got< len) {
            ssize_t n= recv(fd, buf+got, len-got, 0);
            if (n<= 0) break;
            got += (size_t)n;
        }
        return got;
    }

/* ----------------------------------------------------------------------
 * TCP Chunk Download
 * ---------------------------------------------------------------------- */

static char *downloadChunk(const PeerSegment *peer,
                             long chunkStart, long chunkEnd,
                             long *outSize)
{
    long chunkSize = chunkEnd-chunkStart+1;
    *outSize = 0;

    if (chunkSize > MAX_CHUNK_SIZE) {
        int fd = tcpConnect(peer->peer_ip, peer->peer_port, 5);
        if (fd >= 0) {
            sendAll(fd, "<GET invalid>\n", 14);
            close(fd);
        }
        fprintf(stderr, "[ERROR] Chunk %ld-%ld exceeds %d bytes — skipped.\n",
                chunkStart, chunkEnd, MAX_CHUNK_SIZE);
        return NULL;
    }

    int fd = tcpConnect(peer->peer_ip, peer->peer_port, 10);
    if (fd< 0) {
        fprintf(stderr, "Dead peer %s:%d — connect failed\n",
                peer->peer_ip, peer->peer_port);
        return NULL;
    }

    char request[128];
    int reqLen = snprintf(request, sizeof(request),
                            "GET %ld %ld\n", chunkStart, chunkEnd);
    if (sendAll(fd, request, (size_t)reqLen)< 0) {
        fprintf(stderr, "Send failed to %s:%d\n",
                peer->peer_ip, peer->peer_port);
        close(fd);
        return NULL;
    }

    char *data = malloc((size_t)chunkSize);
    if (!data) { close(fd); return NULL; }


    size_t got = recvExact(fd, data, (size_t)chunkSize);
    close(fd);

    if ((long)got != chunkSize) {
        fprintf(stderr, "Expected %ld bytes from %s:%d, got %zu\n",
                chunkSize, peer->peer_ip, peer->peer_port, got);
        free(data);
        return NULL;
    }

    *outSize = chunkSize;
    return data;
}

    static const PeerSegment * bestPeer(const Segment * seg)
    {
        if (seg-> peer_count == 0) return NULL;
        int best= 0;
        for (int i= 1; i< seg->peer_count; i++) {
            if (seg->peers[i].timestamp > seg->peers[best].timestamp)
                best= i;
        }
        return &seg->peers[best];
    }

/* ----------------------------------------------------------------------
 * Thread function
 * ---------------------------------------------------------------------- */

static void *thread_download_segment(void *arg)
{
    ThreadArgs *targs = (ThreadArgs *)arg;
    Segment *seg = &targs->segment;


    const PeerSegment *peer = bestPeer(seg);
    if (!peer) {
        fprintf(stderr, "[ERROR] No peers for segment %ld-%ld\n",
                seg->byteStart, seg->byteEnd);
        return NULL;
    }

    long pos= seg->byteStart;
    while (pos<= seg->byteEnd) {
        long chunkEnd= pos+MAX_CHUNK_SIZE-1;
        if (chunkEnd>seg->byteEnd) chunkEnd= seg->byteEnd;

        long dataSize= 0;
        char *data= downloadChunk(peer, pos, chunkEnd, &dataSize);

        if (!data) {
            fprintf(stderr, "Skipping chunk %ld-%ld\n",
                    pos, chunkEnd);
        } else {
            pthread_mutex_lock(targs->lock);
            int idx = *targs->resultCount;
            targs->results[idx].byteStart= pos;
            targs->results[idx].data = data;
            targs->results[idx].size = dataSize;
            (*targs->resultCount)++;
            pthread_mutex_unlock(targs->lock);
        }
        pos= chunkEnd+1;
    }
    return NULL;
}

/* ----------------------------------------------------------------------
 * Tracker update
 * ---------------------------------------------------------------------- */

static void send_update_tracker(const char *tracker_host, int tracker_port,
                                const char *filename, const Segment *segment,
                                const char *my_ip, int my_port)
{
    char msg[512];
    snprintf(msg, sizeof(msg),
             "UPDATETRACKER %s %ld %ld %s %d\n",
             filename,
             segment->byteStart,
             segment->byteEnd,
             my_ip,
             my_port);

    int fd = tcpConnect(tracker_host, tracker_port, 5);
    if (fd< 0) {
        fprintf(stderr, "Could not reach tracker!\n");
        return;
    }

    sendAll(fd, msg, strlen(msg));

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = recv(fd, resp, sizeof(resp)-1, 0);
    close(fd);

    if (n > 0) {
        resp[strcspn(resp, "\r\n")] = '\0';
        printf("TRACKER: Segment %ld-%ld → %s\n",
               segment->byteStart, segment->byteEnd, resp);
    }
}

/* ----------------------------------------------------------------------
 * Main Download 
 * ---------------------------------------------------------------------- */

    static int cmpChunks(const void *a, const void *b)          // Chunk comparator for sort
    {
        const ChunkResult *ca = (const ChunkResult *)a;
        const ChunkResult *cb = (const ChunkResult *)b;
        if (ca->byteStart< cb->byteStart) 
            return -1;
        if (ca->byteStart > cb->byteStart) 
            return 1;
        return 0;
    }


static int downloadFile(const char *tracker_host, int tracker_port,
                          const char *track_path,
                          const char *my_ip, int my_port)
{
    TrackInfo info;
    printf("Parsing track file: %s\n", track_path);
    if (parse_track_file(track_path, &info)< 0) return -1;
    printf("File: %s Size: %ld bytes Segments: %d\n",
           info.filename, info.filesize, info.segmentCount);

    ChunkResult *results= calloc(MAX_CHUNKS, sizeof(ChunkResult));
    int resultCount = 0;
    pthread_mutex_t lock;
    pthread_mutex_init(&lock, NULL);

    if (!results) { perror("calloc"); return -1; }

    for (int si = 0; si< info.segmentCount; si++) {
        Segment *seg = &info.segments[si];
        printf("\n[SEGMENT %d/%d] bytes %ld-%ld\n",
               si+1, info.segmentCount, seg->byteStart, seg->byteEnd);

        // Build chunk boundaries for this segment  
        long chunkStarts[MAX_CHUNKS / MAX_SEGMENTS+2];
        long chunkEnds [MAX_CHUNKS / MAX_SEGMENTS+2];
        int chunkCount = 0;

        for (long pos = seg->byteStart; pos<= seg->byteEnd; ) {
            long ce = pos+MAX_CHUNK_SIZE-1;
            if (ce > seg->byteEnd) ce = seg->byteEnd;
            chunkStarts[chunkCount] = pos;
            chunkEnds [chunkCount] = ce;
            chunkCount++;
            pos = ce+1;
        }

        // Spawn one thread per chunk (mutually exclusive byte ranges)  
        pthread_t *threads= malloc((size_t)chunkCount * sizeof(pthread_t));
        ThreadArgs *targs = malloc((size_t)chunkCount * sizeof(ThreadArgs));
        if (!threads|| !targs) { 
            perror("malloc"); free(results); return -1; }

        for (int c = 0; c< chunkCount; c++) {
            targs[c].segment= *seg;
            targs[c].segment.byteStart= chunkStarts[c];
            targs[c].segment.byteEnd= chunkEnds[c];
            targs[c].results = results;
            targs[c].resultCount= &resultCount;
            targs[c].lock = &lock;
            pthread_create(&threads[c], NULL, thread_download_segment, &targs[c]);
        }
        for (int c = 0; c< chunkCount; c++)
            pthread_join(threads[c], NULL);

        free(threads);
        free(targs);

        // Notify tracker after each complete segment  
        send_update_tracker(tracker_host, tracker_port,
                            info.filename, seg, my_ip, my_port);
    }

    /* ---------------------------------------------------------------
     * Sort and Merge Chunks
     * ---------------------------------------------------------------- */

    qsort(results, (size_t)resultCount, sizeof(ChunkResult), cmpChunks);

    printf("\n[INFO] Merging %d chunks -> %s\n", resultCount, info.filename);

    FILE *out = fopen(info.filename, "wb");
    if (!out) { 
        perror("fopen output"); free(results); return -1; }

    // allocate file size  
    if (info.filesize > 0) {
        fseek(out, info.filesize-1, SEEK_SET);
        fputc('\0', out);
        fseek(out, 0, SEEK_SET);
    }

    for (int i = 0; i< resultCount; i++) {
        fseek(out, results[i].byteStart, SEEK_SET);
        fwrite(results[i].data, 1, (size_t)results[i].size, out);
        free(results[i].data);
    }
    fclose(out);
    free(results);
    pthread_mutex_destroy(&lock);

    struct stat st;
    stat(info.filename, &st);
    printf("[INFO] Written %ld bytes to %s\n", (long)st.st_size, info.filename);

    // Delete  track.c file from direcotory
    if (remove(track_path) == 0)
        printf("[INFO] Deleted track file: %s\n", track_path);
    else
        fprintf(stderr, "[WARN] Could not delete track file: %s\n", strerror(errno));

    printf("\n[DONE] Download complete.\n");
    return 0;
}

/* ----------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc !=6) {
        fprintf(stderr,
                "Usage: %s <tracker_host> <tracker_port> <track_file> <my_ip> <my_port>\n",
                argv[0]);
        return 1;
    }

    const char *tracker_host= argv[1];
    int tracker_port= atoi(argv[2]);
    const char *track_path= argv[3];
    const char *my_ip = argv[4];
    int my_port= atoi(argv[5]);

    struct stat st;
    if (stat(track_path, &st) !=0|| !S_ISREG(st.st_mode)) {
        fprintf(stderr, "[ERROR] Track file not found: %s\n", track_path);
        return 1;
    }

    return downloadFile(tracker_host, tracker_port,
                         track_path, my_ip, my_port) == 0 ? 0 : 1;
}