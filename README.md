# CS-4390-Project-

  Peer File Downloader (C)
  ========================
  [CS-4390.002 — Computer Networks]
  
  Dividing file into segments: The downloading file is divided into many segments, and
the size of each segment could be defined in any number in advance.
  Segment selection: The to-be-downloaded segment(s) is (are) chosen sequentially.
  Peer selection: The peer which has the newest timestamp is selected to be connected to
dwnload the corresponding segment. 
  4) Update: Only after downloading a complete segment does a peer update its record file
and sends information to tell the tracker server that it has this part of the file.
  
  Downloads files from peers using the tracker's  track.c file.
 
   track.c file header (written by handle_createtracker_req):
    Filename: <filename>
    Filesize: <filesize>
    Description: <description>
    MD5: <md5>
    #list of peers follows next
 
  Peer lines (written by handle_updatetracker_req):
    <ip>:<port>:<start_byte>:<end_byte>:<timestamp>
 
  Usage:
    ./peer_downloader <tracker_host> <tracker_port> <track_file> <my_ip> <my_port>
 
  Compile:
    gcc -std=c11 -pthread -Wall -Wextra -o peer_downloader peer_downloader.c -lpthread
    Makfile made

    ### Example Command ./peer_downloader <tracker_host> <tracker_port> <track_file> <my_ip> <my_port>

* Structs
* Constants
* Data Structures (PeerSegment, Segment, ChunkResult, ThreadArgs, TrackInfo)
* Helpers (Connection Check, recieveExact)
* track.c file parser
* TCP Chunk Download
* Thread Section
* Tracker Update
* Download Orchestrator
* Sort and Merge Chunks
* Main 
