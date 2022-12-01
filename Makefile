CC ?= gcc

advent22: advent22.c
	$(CC) -march=native -mtune=native -g3 -O3 -std=c2x -Wall -Werror -Wpedantic -Wno-unused-function advent22.c -lcurl -o advent22
