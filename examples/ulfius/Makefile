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
ULFIUS_CFLAGS := $(shell pkg-config --cflags libulfius)
ULFIUS_CPPFLAGS := $(patsubst -I%,-isystem %,$(filter -I%,$(ULFIUS_CFLAGS))) \
	$(filter-out -I%,$(ULFIUS_CFLAGS))
CPPFLAGS ?= -I$(ROOT)/include $(ULFIUS_CPPFLAGS)
LDFLAGS ?=
ifneq ($(OPENSSL_PREFIX),)
  CPPFLAGS += -I$(OPENSSL_PREFIX)/include
  LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif
TARGET := ulfius-example

.PHONY: all clean
all: $(TARGET)
$(TARGET): main.c $(ROOT)/librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -o $@ \
		$(ROOT)/librclient.a $(shell pkg-config --libs libulfius liborcania) \
		-lcrypto -lresolv -pthread
clean:
	rm -f $(TARGET)
