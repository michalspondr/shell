CFLAGS=-g -Wall -pedantic -ansi

all: xspond00.c
	gcc $(CFLAGS) -o xspond00 xspond00.c -lpthread
clean:
	rm -f *.o xspond00 *~ core*

