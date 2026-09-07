CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O0 -g
PREFIX  ?= /usr/local

LIB_SRC := src/core/cweb.c src/utils/strbuf.c src/utils/xmem.c

TESTS := test_version test_strbuf test_da

all: build/libcweb.a

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

build/libcweb.a: $(LIB_SRC:%.c=build/%.o)
	$(AR) rcs $@ $^

build/tests/%: tests/%.c build/libcweb.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -o $@ $< build/libcweb.a

test: $(TESTS:%=build/tests/%)
	@for t in $(TESTS); do ./build/tests/$$t || exit 1; done

clean:
	rm -rf build

.PHONY: all test clean