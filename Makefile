PREFIX = /usr/local
MANDIR = $(PREFIX)/share/man
CONFDIR = /etc
CFLAGS = -std=c99 -D_DEFAULT_SOURCE -Wall -Wpedantic -Wextra

all: hwfan

hwfan: hwfan.c
	$(CC) $(CFLAGS) -o hwfan $<

clean:
	rm -f hwfan

install: hwfan
	install -Dm755 hwfan $(PREFIX)/bin/hwfan
	install -Dm644 hwfan.1 $(MANDIR)/man1/hwfan.1
	install -Dm644 hwfan.conf $(CONFDIR)/hwfan.conf

# Not removing config on uninstall is intentional.
uninstall:
	rm -f ${PREFIX}/bin/hwfan $(MANDIR)/man1/hwfan.1

.PHONY: clean install uninstall
