.POSIX:
CFLAGS = -std=c99 -D_DEFAULT_SOURCE -Wall -Wpedantic -Wextra

hwfan: hwfan.c
	$(CC) $(CFLAGS) -o hwfan $<

clean:
	rm -f hwfan

.PHONY: clean
