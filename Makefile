PREFIX ?= /
CC ?= gcc
CFLAGS ?= -Wall -std=gnu90
SRC_DIR := $(dir $(lastword $(MAKEFILE_LIST)))


BIN := $(SRC_DIR)/compsize
C_FILES := $(wildcard $(SRC_DIR)/*.c)
OBJ_FILES := $(patsubst $(SRC_DIR)/%.c, $(SRC_DIR)/%.o, $(C_FILES))


all: $(BIN)

debug: CFLAGS += -Wall -DDEBUG -g
debug: $(BIN)


$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $^

$(BIN): $(OBJ_FILES)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

BIN_I := $(PREFIX)/usr/bin/compsize

$(BIN_I): $(BIN)
	install -Dm755 $< $@

MAN_I := $(PREFIX)/usr/share/man/man8/compsize.8.gz

$(MAN_I): $(SRC_DIR)/compsize.8
	gzip -9n < $< > $@

install: $(BIN_I) $(MAN_I)

uninstall:
	@rm -vf $(BIN_I) $(MAN_I)

clean:
	@rm -vf $(BIN) $(OBJ_FILES)
