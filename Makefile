CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lncurses

proclens: proclens.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f proclens

.PHONY: clean
