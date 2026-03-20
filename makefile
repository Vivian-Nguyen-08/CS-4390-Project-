all: tracker

tracker: tracker.c
#gcc -o tracker tracker.c -lpthread 
	gcc -o tracker tracker.c -lpthread -lssl -lcrypto

clean:
	rm -f tracker