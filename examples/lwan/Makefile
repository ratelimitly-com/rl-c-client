ROOT ?= ../..
LWAN_ROOT ?=
LWAN_BUILD ?=$(LWAN_ROOT)/build
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=gnu11
CPPFLAGS ?= -I$(ROOT)/include -I$(LWAN_ROOT)/src/lib \
	-include $(LWAN_BUILD)/lwan-build-config.h
LDFLAGS ?=
TARGET := lwan-example
LWAN_LIBRARY := $(LWAN_BUILD)/src/lib/liblwan.a

.PHONY: all clean
all: $(TARGET)
$(TARGET): main.o $(ROOT)/librclient.a
	$(CC) $(LDFLAGS) main.o -o $@ \
		$(ROOT)/librclient.a -Wl,--whole-archive $(LWAN_LIBRARY) \
		-Wl,--no-whole-archive -lcrypto -lresolv -lz -lzstd -ldl -lm -pthread
main.o: main.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
clean:
	rm -f $(TARGET) main.o
