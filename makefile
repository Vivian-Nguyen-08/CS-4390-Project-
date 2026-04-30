CC     = gcc
CFLAGS = -std=c11 -pthread -Wall -Wextra

# Auto-detect OpenSSL location (Homebrew on Mac, default path on Linux)
OPENSSL_PREFIX := $(shell brew --prefix openssl 2>/dev/null || echo /usr)
SSL_INC        := -I$(OPENSSL_PREFIX)/include
SSL_LIB        := -L$(OPENSSL_PREFIX)/lib

all: tracker 

tracker: tracker.c
	$(CC) $(CFLAGS) $(SSL_INC) $(SSL_LIB) -o tracker tracker.c -lpthread -lssl -lcrypto

clean:
	rm -f tracker 