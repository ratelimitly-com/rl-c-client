ROOT ?= ../..
CC ?= x86_64-w64-mingw32-gcc
OPENSSL_PREFIX ?=
RL_CLIENT_LIBRARY ?=
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS ?= -I$(ROOT)/include
LDFLAGS ?=
ifneq ($(OPENSSL_PREFIX),)
  CPPFLAGS += -I$(OPENSSL_PREFIX)/include
  LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif
LDLIBS ?= $(RL_CLIENT_LIBRARY) -lcrypto -lws2_32 -ldnsapi
TARGET := win32-example.exe

.PHONY: all clean
all: $(TARGET)
$(TARGET): main.c
	@test -n "$(RL_CLIENT_LIBRARY)" || { \
		echo "Set RL_CLIENT_LIBRARY to a Windows rl-c-client static library" >&2; \
		exit 1; \
	}
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) main.c -o $@ $(LDLIBS)
clean:
	rm -f $(TARGET)
