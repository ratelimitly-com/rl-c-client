ROOT ?= ../..
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS ?= -I$(ROOT)/include
LDFLAGS ?=
LDLIBS ?= $(ROOT)/librclient.a -lcrypto -lresolv -pthread
TARGET := epoll-example

.PHONY: all clean
all: $(TARGET)

$(TARGET): main.c $(ROOT)/librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) main.c -o $@ $(LDLIBS)

clean:
	rm -f $(TARGET)
