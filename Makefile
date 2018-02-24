PREFIX ?= /
CC ?= gcc
CFLAGS ?= -Wall -std=gnu90

all: compsize

debug: CFLAGS += -Wall -DDEBUG -g
debug: compsize

compsize.o: compsize.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $^

radix-tree.o: radix-tree.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $^

compsize: compsize.o radix-tree.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

clean:
	rm -f compsize
	rm -f *.o

install:
	install -Dm755 compsize $(PREFIX)/usr/bin/compsize
	gzip -9n <compsize.8 >$(PREFIX)/usr/share/man/man8/compsize.8.gz
