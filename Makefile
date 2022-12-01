CC ?= gcc
CFLAGS_EXTRA ?= -O3
CFLAGS := -march=native -mtune=native -g3 -std=c2x -Wall -Werror -Wpedantic -Wno-unused-function $(CFLAGS_EXTRA)

advent22: advent22.c
	$(CC) $(CFLAGS) advent22.c -lcurl -o advent22

clean:
	rm advent22
