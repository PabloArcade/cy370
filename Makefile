# SPDX-License-Identifier: GPL-3.0-or-later
# cy370dump: Linux binary with gcc, Windows binary with mingw-w64 (32 MB stack; no link time stamp, so that the
# same source and compiler give the same file).
CC      ?= gcc
WINCC   ?= x86_64-w64-mingw32-gcc
CFLAGS  ?= -O2
WARN     = -std=c99 -Wall -Wextra -Werror
SRC      = src/main.c src/jedec.c src/model.c src/decode.c src/structures.c src/tables.c
HDR      = src/cy370.h

all: cy370dump

cy370dump: $(SRC) $(HDR)
	$(CC) $(WARN) $(CFLAGS) -s -o $@ $(SRC)

cy370dump.exe: $(SRC) $(HDR)
	$(WINCC) $(WARN) $(CFLAGS) -s -static -Wl,--stack,33554432 -Wl,--no-insert-timestamp -o $@ $(SRC)

clean:
	rm -f cy370dump cy370dump.exe

.PHONY: all clean
