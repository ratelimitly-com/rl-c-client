ROOT ?= ../..
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS ?= -I$(ROOT)/include $(shell pkg-config --cflags liburing)
LDFLAGS ?=
LDLIBS ?= $(ROOT)/librclient.a $(shell pkg-config --libs liburing) \
	-lcrypto -lresolv -pthread
TARGET := liburing-example

.PHONY: all clean
all: $(TARGET)

$(TARGET): main.c $(ROOT)/librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) main.c -o $@ $(LDLIBS)

clean:
	rm -f $(TARGET)
