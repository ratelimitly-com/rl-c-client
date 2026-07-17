ROOT ?= ../..
CC ?= x86_64-w64-mingw32-gcc
OPENSSL_PREFIX ?=
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS ?= -I$(ROOT)/include
LDFLAGS ?=
ifneq ($(OPENSSL_PREFIX),)
  CPPFLAGS += -I$(OPENSSL_PREFIX)/include
  LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif
LDLIBS ?= $(ROOT)/librclient.a -lcrypto -lws2_32 -ldnsapi
TARGET := win32-example.exe

.PHONY: all clean
all: $(TARGET)
$(TARGET): main.c $(ROOT)/librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) main.c -o $@ $(LDLIBS)
clean:
	rm -f $(TARGET)
