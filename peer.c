/* ============================================================
   peer.c — CS4390 Project
   Peer Program: handles tracker communication and file sharing
   ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define MAXLINE 512
#define CHUNK_SIZE 1024



// Holds tracker server connection info (read from config)
typedef struct {
    char ip[256];
    int  port;
    int  update_interval; // how often to send updatetracker (seconds)
} TrackerConfig;

// Holds info about a file this peer is sharing or downloading
typedef struct {
    char filename[256];
    char filesize[256];
    char description[256];
    char md5[64];
    char ip[256];
    char port[16];
} FileInfo;


TrackerConfig tracker;  // loaded once from config file at startup



// --- Config ---
void load_config();                         // reads clientThreadConfig.cfg

// --- Tracker Connection ---
int  connect_to_tracker();                  // opens a socket to the tracker, returns sockid

// --- Commands sent to Tracker ---
void send_list();                           // sends REQ LIST, prints response
void send_get(char *filename);              // sends GET filename.track, saves response
void send_createtracker(FileInfo *f);       // sends createtracker message
void send_updatetracker(char *filename,     // sends updatetracker message
                        char *start_byte,
                        char *end_byte);

// --- Periodic Update Thread ---
void *update_thread(void *arg);             // runs in background, calls updatetracker every N seconds


int main(int argc, char *argv[]) {

    // 1. Load config file
    load_config();
    printf("Connecting to tracker at %s:%d\n", tracker.ip, tracker.port);

    // 2. TODO: start server thread (Person 4's job)

    // 3. TODO: start periodic update thread
     pthread_t update_tid;
     pthread_create(&update_tid, NULL, update_thread, NULL);

    // 4. Main loop — accept user commands for manual testing
    char command[MAXLINE];
    while(1) {
        printf("\nEnter command (list / get / createtracker / updatetracker / quit): ");
        fgets(command, sizeof(command), stdin);
        command[strcspn(command, "\n")] = '\0';  // strip newline

        if(strcmp(command, "list") == 0) {
            send_list();

        } else if(strncmp(command, "get", 3) == 0) {
            char filename[256];
            sscanf(command, "get %s", filename);
            send_get(filename);

        } else if(strncmp(command, "createtracker", 13) == 0) {
            // Expected format: createtracker filename filesize description md5 ip port
            char filename[256], filesize[256], description[256], md5[64], ip[256], port[16];
            sscanf(command, "createtracker %s %s %s %s %s %s",
                   filename, filesize, description, md5, ip, port);
            FileInfo f;
            strcpy(f.filename, filename);
            strcpy(f.filesize, filesize);
            strcpy(f.description, description);
            strcpy(f.md5, md5);
            strcpy(f.ip, ip);
            strcpy(f.port, port);
            send_createtracker(&f);

        } else if(strncmp(command, "updatetracker", 13) == 0) {
            char filename[256], start_byte[256], end_byte[256];
            sscanf(command, "updatetracker %s %s %s", filename, start_byte, end_byte);
            send_updatetracker(filename, start_byte, end_byte);

        } else if(strcmp(command, "quit") == 0) {
            printf("Exiting...\n");
            break;

        } else {
            printf("Unknown command.\n");
        }
    }

    return 0;
}

/* ============================================================
   CONFIG
   ============================================================ */
void load_config() {
    FILE *fptr = fopen("clientThreadConfig.cfg", "r");
    if(fptr == NULL) {
        printf("Cannot open clientThreadConfig.cfg\n");
        exit(0);
    }

    char port_str[16];
    char interval_str[16];

    fgets(port_str,       sizeof(port_str),       fptr);  // line 1: port
    fgets(tracker.ip,     sizeof(tracker.ip),      fptr);  // line 2: tracker IP
    fgets(interval_str,   sizeof(interval_str),    fptr);  // line 3: update interval

    fclose(fptr);

    // Strip newlines
    tracker.ip[strcspn(tracker.ip, "\n")]     = '\0';
    port_str[strcspn(port_str, "\n")]         = '\0';
    interval_str[strcspn(interval_str, "\n")] = '\0';

    tracker.port            = atoi(port_str);
    tracker.update_interval = atoi(interval_str);
}

/* ============================================================
   TRACKER CONNECTION
   ============================================================ */
int connect_to_tracker() {
    int sockid;
    struct sockaddr_in server_addr;

    if((sockid = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        printf("Socket creation failed\n");
        exit(0);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(tracker.port);
    inet_aton(tracker.ip, &server_addr.sin_addr);
    memset(server_addr.sin_zero, '\0', sizeof(server_addr.sin_zero));

    if(connect(sockid, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1) {
        printf("Cannot connect to tracker\n");
        close(sockid);
        return -1;
    }

    return sockid;
}

/* ============================================================
   COMMANDS
   ============================================================ */
void send_list() {
    int sockid = connect_to_tracker();
    if(sockid == -1) return;

    // Send the command
    send(sockid, "REQ LIST\n", 9, 0);

    // Read and print the response
    char buffer[MAXLINE];
    int bytes;
    printf("\n--- Available Files ---\n");
    while((bytes = recv(sockid, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes] = '\0';
        printf("%s", buffer);
        if(strstr(buffer, "<REP LIST END>")) break;  // stop when we see the end marker
    }

    close(sockid);
}

void send_get(char *filename) {
    int sockid = connect_to_tracker();
    if(sockid == -1) return;

    // Send the command
    char msg[MAXLINE];
    sprintf(msg, "GET %s\n", filename);
    send(sockid, msg, strlen(msg), 0);

    // Read response and save to local cache folder
    char buffer[MAXLINE];
    int bytes;
    char cache_file[512];
    sprintf(cache_file, "cache/%s", filename);
    FILE *cache_fptr = fopen(cache_file, "w");

    printf("\n--- Tracker File: %s ---\n", filename);
    while((bytes = recv(sockid, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes] = '\0';
        printf("%s", buffer);
        if(cache_fptr) fprintf(cache_fptr, "%s", buffer);
        if(strstr(buffer, "<REP GET END")) break;
    }

    if(cache_fptr) fclose(cache_fptr);
    close(sockid);
}

void send_createtracker(FileInfo *f) {
    int sockid = connect_to_tracker();
    if(sockid == -1) return;

    char msg[MAXLINE];
    sprintf(msg, "createtracker %s %s %s %s %s %s\n",
            f->filename, f->filesize, f->description,
            f->md5, f->ip, f->port);
    send(sockid, msg, strlen(msg), 0);

    char buffer[MAXLINE];
    int bytes = recv(sockid, buffer, sizeof(buffer) - 1, 0);
    if(bytes > 0) {
        buffer[bytes] = '\0';
        printf("Tracker response: %s\n", buffer);
    }

    close(sockid);
}

void send_updatetracker(char *filename, char *start_byte, char *end_byte) {
    // Load peer's own IP and port from serverThreadConfig.cfg
    FILE *server_cfg = fopen("serverThreadConfig.cfg", "r");
    char peer_ip[256] = "127.0.0.1";
    char peer_port[16] = "8000";
    
    if(server_cfg) {
        fgets(peer_port, sizeof(peer_port), server_cfg);
        fgets(peer_ip, sizeof(peer_ip), server_cfg);
        fclose(server_cfg);
        peer_ip[strcspn(peer_ip, "\n")] = '\0';
        peer_port[strcspn(peer_port, "\n")] = '\0';
    }
    
    int sockid = connect_to_tracker();
    if(sockid == -1) return;

    char msg[MAXLINE];
    sprintf(msg, "updatetracker %s %s %s %s %s\n",
            filename, start_byte, end_byte, peer_ip, peer_port);
    send(sockid, msg, strlen(msg), 0);

    char buffer[MAXLINE];
    int bytes = recv(sockid, buffer, sizeof(buffer) - 1, 0);
    if(bytes > 0) {
        buffer[bytes] = '\0';
        printf("Tracker response: %s\n", buffer);
    }

    close(sockid);
}

/* ============================================================
   PERIODIC UPDATE THREAD
   ============================================================ */
void *update_thread(void *arg) {
    while(1) {
        sleep(tracker.update_interval);
        printf("[Update Thread] Sending periodic updatetracker...\n");
        // TODO: loop through shared files and call send_updatetracker() for each
        // For now, just a placeholder that shows the thread is running
        printf("[Update Thread] Cycle complete\n");
    }
    return NULL;
}