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
CPPFLAGS ?= -I$(ROOT)/include $(shell pkg-config --cflags libuv)
LDFLAGS ?=
LIBUV_LIBS := $(shell pkg-config --libs libuv)

ifeq ($(OS),Windows_NT)
  PLATFORM_LIBS := -lws2_32 -ldnsapi
else
  PLATFORM_LIBS := -lresolv -pthread
endif

ifneq ($(OPENSSL_PREFIX),)
  CPPFLAGS += -I$(OPENSSL_PREFIX)/include
  LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif

TARGET := libuv-example

.PHONY: all clean

all: $(TARGET)

$(TARGET): main.c $(ROOT)/librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) main.c -o $@ \
		$(ROOT)/librclient.a $(LIBUV_LIBS) -lcrypto $(PLATFORM_LIBS)

clean:
	rm -f $(TARGET)
