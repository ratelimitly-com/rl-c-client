ROOT ?= ../..
KORE_ROOT ?=
CC ?= cc
UNAME_S := $(shell uname -s)
OPENSSL_PREFIX ?=
ifeq ($(UNAME_S),Darwin)
  ifeq ($(OPENSSL_PREFIX),)
    OPENSSL_PREFIX := /opt/homebrew/opt/openssl@3
  endif
  MODULE_FLAGS := -bundle -undefined dynamic_lookup
else
  MODULE_FLAGS := -shared
endif
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11 -fPIC
CPPFLAGS ?= -I$(ROOT)/include -isystem $(KORE_ROOT)/include -DKORE_USE_TASKS
LDFLAGS ?=
ifneq ($(OPENSSL_PREFIX),)
  CPPFLAGS += -I$(OPENSSL_PREFIX)/include
  LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif
TARGET := kore-example.so

.PHONY: all clean
all: $(TARGET)
$(TARGET): main.c $(ROOT)/librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(MODULE_FLAGS) $< -o $@ \
		$(ROOT)/librclient.a -lcrypto -lresolv -pthread
clean:
	rm -f $(TARGET)
