ROOT ?= ../..
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=gnu11
CPPFLAGS ?= -I$(ROOT)/include $(shell pkg-config --cflags libreactor)
LDFLAGS ?=
TARGET := libreactor-example

.PHONY: all clean
all: $(TARGET)
$(TARGET): main.c $(ROOT)/librclient.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -o $@ \
		$(ROOT)/librclient.a $(shell pkg-config --libs libreactor) \
		-lssl -lcrypto -lresolv -pthread
clean:
	rm -f $(TARGET)
