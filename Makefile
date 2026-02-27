CC = cc
CFLAGS = -Wall -Wextra -O2

all: zxs z80_test

zxs: zxs.c z80.c z80.h
	$(CC) $(CFLAGS) -o zxs zxs.c z80.c

z80_test: z80_test.c z80.c z80.h
	$(CC) $(CFLAGS) -o z80_test z80_test.c z80.c

clean:
	rm -f zxs z80_test

test: z80_test
	./z80_test

.PHONY: all clean test
