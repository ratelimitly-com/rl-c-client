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
LWAN_PKG_CONFIG_PATH = $(LWAN_BUILD)$(if $(PKG_CONFIG_PATH),:$(PKG_CONFIG_PATH))
LWAN_BUILD_LIBS = $(shell PKG_CONFIG_PATH='$(LWAN_PKG_CONFIG_PATH)' \
	pkg-config --libs lwan)
LWAN_DEP_LIBS = $(filter-out -llwan,$(LWAN_BUILD_LIBS))

.PHONY: all clean
all: $(TARGET)
$(TARGET): main.o $(ROOT)/librclient.a $(LWAN_BUILD)/lwan.pc
	$(CC) $(LDFLAGS) main.o -o $@ \
		$(ROOT)/librclient.a -Wl,--whole-archive $(LWAN_LIBRARY) \
		-Wl,--no-whole-archive $(LWAN_DEP_LIBS) \
		-lcrypto -lresolv -ldl -lm -pthread
main.o: main.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
clean:
	rm -f $(TARGET) main.o
