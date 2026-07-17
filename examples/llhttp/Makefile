ROOT ?= ../..
CC ?= cc
UNAME_S := $(shell uname -s)
OPENSSL_PREFIX ?=
ifeq ($(UNAME_S),Darwin)
  ifeq ($(OPENSSL_PREFIX),)
    OPENSSL_PREFIX := /opt/homebrew/opt/openssl@3
  endif
endif
LLHTTP_CFLAGS ?= $(shell pkg-config --cflags libllhttp 2>/dev/null)
LLHTTP_LIBS ?= $(shell pkg-config --libs libllhttp 2>/dev/null || echo -lllhttp)
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS ?= -I$(ROOT)/include $(LLHTTP_CFLAGS)
LDFLAGS ?=
ifeq ($(OS),Windows_NT)
  PLATFORM_LIBS := -lws2_32 -ldnsapi
else
  PLATFORM_LIBS := -lresolv -pthread
endif
ifneq ($(OPENSSL_PREFIX),)
  CPPFLAGS += -I$(OPENSSL_PREFIX)/include
  LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif
TARGET := llhttp-example
OBJECTS := main.o llhttp_adapter.o

.PHONY: all clean
all: $(TARGET)
$(TARGET): $(OBJECTS) $(ROOT)/librclient.a
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@ $(ROOT)/librclient.a \
		$(LLHTTP_LIBS) -lcrypto $(PLATFORM_LIBS)
main.o: main.c llhttp_adapter.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
llhttp_adapter.o: llhttp_adapter.c llhttp_adapter.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
clean:
	rm -f $(TARGET) $(OBJECTS)
