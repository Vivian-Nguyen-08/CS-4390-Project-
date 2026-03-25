CC = gcc
CFLAGS = -pthread

all: tracker peer1/peer peer2/peer peer3/peer

tracker: tracker.c
#gcc -o tracker tracker.c -lpthread 
	gcc -o tracker tracker.c -lpthread -lssl -lcrypto

clean:
	rm -f tracker

peer1/peer: peer.c
	mkdir -p peer1
	$(CC) $(CFLAGS) -o peer1/peer peer.c

peer2/peer: peer.c
	mkdir -p peer2
	$(CC) $(CFLAGS) -o peer2/peer peer.c

peer3/peer: peer.c
	mkdir -p peer3
	$(CC) $(CFLAGS) -o peer3/peer peer.c


