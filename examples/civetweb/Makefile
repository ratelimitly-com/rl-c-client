ROOT ?= ../..
CIVETWEB_ROOT ?=
CC ?= cc
UNAME_S := $(shell uname -s)
OPENSSL_PREFIX ?=
ifeq ($(UNAME_S),Darwin)
  ifeq ($(OPENSSL_PREFIX),)
    OPENSSL_PREFIX := /opt/homebrew/opt/openssl@3
  endif
endif
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CIVETWEB_CFLAGS ?= -O2 -std=c11
CPPFLAGS ?= -I$(ROOT)/include -I$(CIVETWEB_ROOT)/include
LDFLAGS ?=
PLATFORM_LIBS := -lresolv -pthread
ifeq ($(UNAME_S),Linux)
  PLATFORM_LIBS += -ldl
endif
ifneq ($(OPENSSL_PREFIX),)
  CPPFLAGS += -I$(OPENSSL_PREFIX)/include
  LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif
TARGET := civetweb-example
OBJECTS := main.o civetweb.o

.PHONY: all clean
all: $(TARGET)
$(TARGET): $(OBJECTS) $(ROOT)/librclient.a
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@ $(ROOT)/librclient.a \
		-lcrypto $(PLATFORM_LIBS)
main.o: main.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
civetweb.o: $(CIVETWEB_ROOT)/src/civetweb.c
	$(CC) $(CPPFLAGS) -DNO_SSL -D_GNU_SOURCE $(CIVETWEB_CFLAGS) -c $< -o $@
clean:
	rm -f $(TARGET) $(OBJECTS)
