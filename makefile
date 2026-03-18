all: tracker

tracker: tracker.c
	gcc -o tracker tracker.c -lpthread 

clean:
	rm -f tracker