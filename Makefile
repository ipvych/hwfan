.POSIX:
CFLAGS = -std=c99 -D_DEFAULT_SOURCE -Og -g -Wall -Wpedantic -Wextra

hwfan: hwfan.c
	$(CC) $(CFLAGS) -o hwfan $<

clean:
	rm -f hwfan

.PHONY: clean
