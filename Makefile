CC = gcc
CFLAGS = -Wall -Wextra -Wshadow -Wconversion -O2
LDFLAGS = -lncurses
PREFIX = /usr/local

proclens: proclens.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f proclens

install: proclens
	install -Dm755 proclens $(DESTDIR)$(PREFIX)/bin/proclens

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/proclens

run: proclens
	./proclens

.PHONY: clean install uninstall run
