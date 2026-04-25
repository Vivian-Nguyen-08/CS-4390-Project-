CC     = gcc
CFLAGS = -std=c11 -pthread -Wall -Wextra

# Auto-detect OpenSSL location (Homebrew on Mac, default path on Linux)
OPENSSL_PREFIX := $(shell brew --prefix openssl 2>/dev/null || echo /usr)
SSL_INC        := -I$(OPENSSL_PREFIX)/include
SSL_LIB        := -L$(OPENSSL_PREFIX)/lib

all: tracker peer1/peer peer2/peer peer3/peer

# ------- Tracker -------
tracker: tracker.c
	$(CC) $(CFLAGS) $(SSL_INC) $(SSL_LIB) -o tracker tracker.c -lpthread -lssl -lcrypto

# ------- Peer instances -------
# Each peer gets its own directory with its own config files.

peer1/peer: peer.c
	mkdir -p peer1/shared peer1/cache
	$(CC) $(CFLAGS) -o peer1/peer peer.c -lpthread
	@if [ ! -f peer1/clientThreadConfig.cfg ]; then cp clientThreadConfig.cfg peer1/; fi
	@if [ ! -f peer1/serverThreadConfig.cfg ]; then printf '8001\nshared\n' > peer1/serverThreadConfig.cfg; fi

peer2/peer: peer.c
	mkdir -p peer2/shared peer2/cache
	$(CC) $(CFLAGS) -o peer2/peer peer.c -lpthread
	@if [ ! -f peer2/clientThreadConfig.cfg ]; then cp clientThreadConfig.cfg peer2/; fi
	@if [ ! -f peer2/serverThreadConfig.cfg ]; then printf '8002\nshared\n' > peer2/serverThreadConfig.cfg; fi

peer3/peer: peer.c
	mkdir -p peer3/shared peer3/cache
	$(CC) $(CFLAGS) -o peer3/peer peer.c -lpthread
	@if [ ! -f peer3/clientThreadConfig.cfg ]; then cp clientThreadConfig.cfg peer3/; fi
	@if [ ! -f peer3/serverThreadConfig.cfg ]; then printf '8003\nshared\n' > peer3/serverThreadConfig.cfg; fi

# ------- Clean -------
clean:
	rm -f tracker peer
	rm -f peer1/peer peer2/peer peer3/peer