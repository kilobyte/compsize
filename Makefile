all: compsize
PREFIX ?= /
CC ?= gcc
CFLAGS ?= "-Wall"

debug: CXXFLAGS += -DDEBUG -g
debug: compsize

compsize: compsize.c radix-tree.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f compsize

install:
	install -Dm755 compsize $(PREFIX)/usr/bin/compsize
