CC = gcc
CFLAGS = -Wall -Wextra -Wshadow -Wconversion -O2
LDFLAGS = -lncurses

proclens: proclens.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f proclens

run: proclens
	./proclens

.PHONY: clean run
