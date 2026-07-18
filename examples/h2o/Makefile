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
H2O_PKG_CFLAGS := $(shell pkg-config --cflags libh2o-evloop)
H2O_CPPFLAGS := $(patsubst -I%,-isystem %,$(filter -I%,$(H2O_PKG_CFLAGS))) \
	$(filter-out -I%,$(H2O_PKG_CFLAGS))
CPPFLAGS ?= -I$(ROOT)/include $(H2O_CPPFLAGS)
LDFLAGS ?=
ifneq ($(OPENSSL_PREFIX),)
  CPPFLAGS += -I$(OPENSSL_PREFIX)/include
  LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif
TARGET := h2o-example

.PHONY: all clean
all: $(TARGET)
$(TARGET): main.c $(ROOT)/librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -o $@ \
		$(ROOT)/librclient.a $(shell pkg-config --libs libh2o-evloop) \
		-lssl -lcrypto -lm -lresolv -pthread
clean:
	rm -f $(TARGET)
