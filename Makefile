CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O0 -g
PREFIX  ?= /usr/local

LIB_SRC := src/core/cweb.c

all: build/libcweb.a

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

build/libcweb.a: $(LIB_SRC:%.c=build/%.o)
	$(AR) rcs $@ $^

build/test_version: tests/test_version.c build/libcweb.a
	$(CC) $(CFLAGS) -Iinclude -o $@ tests/test_version.c build/libcweb.a

test: build/test_version
	./build/test_version

clean:
	rm -rf build

.PHONY: all test clean