ROOT ?= ../..
CC ?= cc
OPENSSL_PREFIX ?= /opt/homebrew/opt/openssl@3
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS ?= -I$(ROOT)/include -I$(OPENSSL_PREFIX)/include
LDFLAGS ?= -L$(OPENSSL_PREFIX)/lib
LDLIBS ?= $(ROOT)/librclient.a -lcrypto -lresolv -pthread
TARGET := kqueue-example

.PHONY: all clean
all: $(TARGET)
$(TARGET): main.c $(ROOT)/librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) main.c -o $@ $(LDLIBS)
clean:
	rm -f $(TARGET)
