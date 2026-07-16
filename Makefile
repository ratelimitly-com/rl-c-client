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
TEST_RESPONDER_BIN := $(BIN_DIR)/r_test_responder
TEST_RESPONDER_OBJS := \
	tools/r_test_responder.o \
	tools/r_test_responder_protocol.o

LIB_OBJS = \
	src/r_client.o \
	src/r_protocol.o \
	src/r_crypto.o \
	src/r_policy.o

.PHONY: all clean test perf_client test-responder

all: librclient.a librclient.so

perf_client: $(PERF_BIN)

test-responder: $(TEST_RESPONDER_BIN)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(PERF_BIN): $(BIN_DIR) librclient.a $(BIN_DIR)/perf_client.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(BIN_DIR)/perf_client.c librclient.a -lcrypto -lresolv -pthread

$(TEST_RESPONDER_BIN): $(BIN_DIR) librclient.a $(TEST_RESPONDER_OBJS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(TEST_RESPONDER_OBJS) librclient.a -lcrypto -lresolv -pthread

librclient.a: $(LIB_OBJS)
	$(AR) rcs $@ $(LIB_OBJS)

librclient.so: $(LIB_OBJS)
	$(CC) $(LDFLAGS) -shared -o $@ $(LIB_OBJS) -lcrypto

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

tools/%.o: tools/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test: tests/test_protocol tests/test_client_quota tests/test_public_api tests/test_responder tests/test_example_common $(TEST_RESPONDER_BIN)
	./tests/test_protocol
	./tests/test_client_quota
	./tests/test_public_api
	./tests/test_responder
	bash ./tests/test_responder_cli.sh
	bash ./tests/test_example_common.sh
	bash ./tests/test_examples.sh

tests/test_protocol: tests/test_protocol.c src/r_protocol.o src/r_crypto.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lcrypto

tests/test_client_quota: tests/test_client_quota.c librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lcrypto -lresolv -pthread

tests/test_public_api: tests/test_public_api.c librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lcrypto -lresolv -pthread

tests/test_responder: tests/test_responder.c tools/r_test_responder_protocol.o librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lcrypto -lresolv -pthread

tests/test_example_common: tests/test_example_common.c examples/common/rl_example.c librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Iexamples/common $^ -o $@ -lcrypto -lresolv -pthread

clean:
	rm -f $(LIB_OBJS) $(TEST_RESPONDER_OBJS) librclient.a librclient.so tests/test_protocol tests/test_client_quota tests/test_public_api tests/test_responder tests/test_example_common $(PERF_BIN) $(TEST_RESPONDER_BIN)
