PREFIX ?= /
CC ?= gcc
CFLAGS ?= -Wall

all: compsize

debug: CFLAGS += -Wall -DDEBUG -g
debug: compsize

compsize.o: compsize.c
	$(CC) $(CFLAGS) -c -o $@ $^

radix-tree.o: radix-tree.c
	$(CC) $(CFLAGS) -c -o $@ $^

compsize: compsize.o radix-tree.o
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f compsize
	rm -f *.o

install:
	install -Dm755 compsize $(PREFIX)/usr/bin/compsize
