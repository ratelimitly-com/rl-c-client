ROOT ?= ../..
CC ?= cc
UNAME_S := $(shell uname -s)
OPENSSL_PREFIX ?=

ifeq ($(UNAME_S),Darwin)
  ifeq ($(OPENSSL_PREFIX),)
    OPENSSL_PREFIX := /opt/homebrew/opt/openssl@3
  endif
endif

CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS ?= -I$(ROOT)/include
LDFLAGS ?=
LDLIBS ?= $(ROOT)/librclient.a -lcrypto -lresolv -pthread

ifneq ($(OPENSSL_PREFIX),)
  CPPFLAGS += -I$(OPENSSL_PREFIX)/include
  LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif

TARGET := latency-tracker-example

.PHONY: all clean

all: $(TARGET)

$(TARGET): main.c $(ROOT)/librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) main.c -o $@ $(LDLIBS)

clean:
	rm -f $(TARGET)
