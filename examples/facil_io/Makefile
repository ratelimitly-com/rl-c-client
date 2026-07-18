ROOT ?= ../..
FACIL_ROOT ?=
CC ?= cc
UNAME_S := $(shell uname -s)
OPENSSL_PREFIX ?=
ifeq ($(UNAME_S),Darwin)
  ifeq ($(OPENSSL_PREFIX),)
    OPENSSL_PREFIX := /opt/homebrew/opt/openssl@3
  endif
endif
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=gnu11
CPPFLAGS ?= -I$(ROOT)/include -isystem $(FACIL_ROOT)/libdump/include
LDFLAGS ?= -L$(FACIL_ROOT)/tmp -Wl,-rpath,$(FACIL_ROOT)/tmp
PLATFORM_LIBS := -lresolv -lm -pthread
ifeq ($(UNAME_S),Linux)
  PLATFORM_LIBS += -ldl
endif
ifneq ($(OPENSSL_PREFIX),)
  CPPFLAGS += -I$(OPENSSL_PREFIX)/include
  LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif
TARGET := facil-io-example

.PHONY: all clean
all: $(TARGET)
$(TARGET): main.c $(ROOT)/librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -o $@ \
		$(ROOT)/librclient.a -lfacil \
		-lcrypto -lssl $(PLATFORM_LIBS)
clean:
	rm -f $(TARGET)
