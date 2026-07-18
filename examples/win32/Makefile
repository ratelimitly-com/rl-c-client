ROOT ?= ../..
CC ?= x86_64-w64-mingw32-gcc
OPENSSL_PREFIX ?=
OPENSSL_LIBDIR ?= $(if $(wildcard $(OPENSSL_PREFIX)/lib64/libcrypto.a),$(OPENSSL_PREFIX)/lib64,$(OPENSSL_PREFIX)/lib)
RL_CLIENT_LIBRARY ?=
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS ?= -I$(ROOT)/include
LDFLAGS ?=
ifneq ($(OPENSSL_PREFIX),)
  CPPFLAGS += -I$(OPENSSL_PREFIX)/include
  LDFLAGS += -L$(OPENSSL_LIBDIR)
endif
LDLIBS ?= $(RL_CLIENT_LIBRARY) -lcrypto -lws2_32 -lgdi32 -lcrypt32 -ldnsapi
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
