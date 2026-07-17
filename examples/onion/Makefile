ROOT ?= ../..
ONION_ROOT ?= /usr/local
CC ?= cc
UNAME_S := $(shell uname -s)
OPENSSL_PREFIX ?=
ifeq ($(UNAME_S),Darwin)
  ifeq ($(OPENSSL_PREFIX),)
    OPENSSL_PREFIX := /opt/homebrew/opt/openssl@3
  endif
endif
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS ?= -I$(ROOT)/include -isystem $(ONION_ROOT)/include
LDFLAGS ?= -L$(ONION_ROOT)/lib -Wl,-rpath,$(ONION_ROOT)/lib
ifneq ($(OPENSSL_PREFIX),)
  CPPFLAGS += -I$(OPENSSL_PREFIX)/include
  LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif
TARGET := onion-example

.PHONY: all clean
all: $(TARGET)
$(TARGET): main.c $(ROOT)/librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -o $@ \
		$(ROOT)/librclient.a -lonion \
		-lcrypto -lresolv -pthread
clean:
	rm -f $(TARGET)
