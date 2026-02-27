CC = cc
CFLAGS = -Wall -Wextra -O2 -std=c99

all: z80_test zxs

z80_test: z80_test.c z80.c z80.h
	$(CC) $(CFLAGS) -o z80_test z80_test.c z80.c

zxs: zxs.c z80.c z80.h
	$(CC) $(CFLAGS) -o zxs zxs.c z80.c

clean:
	rm -f z80_test zxs

.PHONY: all clean
