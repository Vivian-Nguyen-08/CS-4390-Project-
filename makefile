CC = gcc
CFLAGS = -lpthread -lssl -lcrypto

all: tracker peers

tracker: tracker.c
	$(CC) $(CFLAGS) -o tracker tracker.c

peers: peer.c
	$(CC) $(CFLAGS) -o peer peer.c
	mkdir -p peer1 peer2 peer3
	cp peer peer1/
	cp peer peer2/
	cp peer peer3/
	rm -f peer

clean:
	rm -f tracker
	rm -f peer1/peer peer2/peer peer3/peer
