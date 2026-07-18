ROOT ?= ../..
CC ?= cc
UNAME_S := $(shell uname -s)
OPENSSL_PREFIX ?=
LIBHV_PREFIX ?=
ifeq ($(UNAME_S),Darwin)
  ifeq ($(OPENSSL_PREFIX),)
    OPENSSL_PREFIX := /opt/homebrew/opt/openssl@3
  endif
endif
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS ?= -I$(ROOT)/include
LDFLAGS ?=
ifneq ($(LIBHV_PREFIX),)
  CPPFLAGS += -I$(LIBHV_PREFIX)/include
  LDFLAGS += -L$(LIBHV_PREFIX)/lib
endif
ifneq ($(OPENSSL_PREFIX),)
  CPPFLAGS += -I$(OPENSSL_PREFIX)/include
  LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif
LDLIBS ?= $(ROOT)/librclient.a -lhv -lcrypto -lresolv -pthread -lm -ldl
TARGET := libhv-example

.PHONY: all clean
all: $(TARGET)
$(TARGET): main.c $(ROOT)/librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) main.c -o $@ $(LDLIBS)
clean:
	rm -f $(TARGET)
