CC ?= cc
AR ?= ar

UNAME_S := $(shell uname -s)
OPENSSL_PREFIX ?=

ifeq ($(UNAME_S),Darwin)
  ifeq ($(OPENSSL_PREFIX),)
    OPENSSL_PREFIX := /opt/homebrew/opt/openssl@3
  endif
endif

CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS ?= -Iinclude -Isrc
LDFLAGS ?=

ifneq ($(OPENSSL_PREFIX),)
  CPPFLAGS += -I$(OPENSSL_PREFIX)/include
  LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif

BIN_DIR := bin
PERF_BIN := $(BIN_DIR)/perf_client

LIB_OBJS = \
	src/r_client.o \
	src/r_protocol.o \
	src/r_crypto.o \
	src/r_policy.o

.PHONY: all clean test perf_client

all: librclient.a librclient.so

perf_client: $(PERF_BIN)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(PERF_BIN): $(BIN_DIR) librclient.a $(BIN_DIR)/perf_client.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(BIN_DIR)/perf_client.c librclient.a -lcrypto -lresolv -pthread

librclient.a: $(LIB_OBJS)
	$(AR) rcs $@ $(LIB_OBJS)

librclient.so: $(LIB_OBJS)
	$(CC) $(LDFLAGS) -shared -o $@ $(LIB_OBJS) -lcrypto

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

test: tests/test_protocol tests/test_client_quota tests/test_public_api
	./tests/test_protocol
	./tests/test_client_quota
	./tests/test_public_api

tests/test_protocol: tests/test_protocol.c src/r_protocol.o src/r_crypto.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lcrypto

tests/test_client_quota: tests/test_client_quota.c librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lcrypto -lresolv -pthread

tests/test_public_api: tests/test_public_api.c librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lcrypto -lresolv -pthread

clean:
	rm -f $(LIB_OBJS) librclient.a librclient.so tests/test_protocol tests/test_client_quota tests/test_public_api $(PERF_BIN)
