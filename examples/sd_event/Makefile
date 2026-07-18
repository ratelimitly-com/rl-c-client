ROOT ?= ../..
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS ?= -I$(ROOT)/include $(shell pkg-config --cflags libsystemd)
LDFLAGS ?=
LDLIBS ?= $(ROOT)/librclient.a $(shell pkg-config --libs libsystemd) \
	-lcrypto -lresolv -pthread
TARGET := sd-event-example

.PHONY: all clean
all: $(TARGET)
$(TARGET): main.c $(ROOT)/librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) main.c -o $@ $(LDLIBS)
clean:
	rm -f $(TARGET)
