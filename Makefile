CC ?= cc
AR ?= ar

CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS ?= -Iinclude -Isrc

LIB_OBJS = \
	src/r_client.o \
	src/r_protocol.o \
	src/r_crypto.o \
	src/r_policy.o

.PHONY: all clean test

all: librclient.a librclient.so

librclient.a: $(LIB_OBJS)
	$(AR) rcs $@ $(LIB_OBJS)

librclient.so: $(LIB_OBJS)
	$(CC) -shared -o $@ $(LIB_OBJS) -lcrypto

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

test: tests/test_protocol
	./tests/test_protocol

tests/test_protocol: tests/test_protocol.c src/r_protocol.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

clean:
	rm -f $(LIB_OBJS) librclient.a librclient.so tests/test_protocol
