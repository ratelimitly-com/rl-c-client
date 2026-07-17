ROOT ?= ../..
MONGOOSE_ROOT ?=
CC ?= cc
UNAME_S := $(shell uname -s)
OPENSSL_PREFIX ?=
ifeq ($(UNAME_S),Darwin)
  ifeq ($(OPENSSL_PREFIX),)
    OPENSSL_PREFIX := /opt/homebrew/opt/openssl@3
  endif
endif
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
MONGOOSE_CFLAGS ?= -O2 -std=c11
MONGOOSE_CPPFLAGS ?= -D_GNU_SOURCE
CPPFLAGS ?= -I$(ROOT)/include -I$(MONGOOSE_ROOT)
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
TARGET := mongoose-example
OBJECTS := main.o mongoose.o

.PHONY: all clean
all: $(TARGET)
$(TARGET): $(OBJECTS) $(ROOT)/librclient.a
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@ $(ROOT)/librclient.a \
		-lcrypto $(PLATFORM_LIBS)
main.o: main.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
mongoose.o: $(MONGOOSE_ROOT)/mongoose.c
	$(CC) $(CPPFLAGS) $(MONGOOSE_CPPFLAGS) $(MONGOOSE_CFLAGS) -c $< -o $@
clean:
	rm -f $(TARGET) $(OBJECTS)
