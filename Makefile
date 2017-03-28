.POSIX:
CC     = cc -std=c99
CFLAGS = -Wall -Wextra -O3 -g3
LDLIBS = -lm

connect4 : connect4.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ connect4.c $(LDLIBS)

clean :
	rm -f connect4
