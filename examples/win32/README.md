# Native Win32 integration

This example uses native WinSock event objects. `WSAEventSelect` associates a
`WSAEVENT` with each UDP `SOCKET`, and `WSAWaitForMultipleEvents` combines those
readiness notifications with the current admission timeout.

Every request contains one resource rate limit and one latency guard. Only
admitted, completed work is measured and reported; denials and cancellation do
not emit latency samples.

## Build with MinGW-w64

First build a Windows `librclient.a` with a Windows OpenSSL build, then:

```sh
make CC=x86_64-w64-mingw32-gcc \
  RL_CLIENT_LIBRARY=/path/to/windows/librclient.a \
  OPENSSL_PREFIX=/path/to/mingw/openssl
```

Run `win32-example.exe` directly on Windows, through Wine on Linux, or through
CrossOver on macOS. Set `RATELIMITLY_TENANT`, `RATELIMITLY_AUTH_KEY`, and the
optional fixed responder variables in that environment.

## Build with CMake/MSVC

```powershell
cmake -S . -B build -A x64 -DRL_CLIENT_ROOT=C:\src\rl-c-client
cmake --build build --config Release
```

The imported library path can be overridden with `RL_CLIENT_LIBRARY`.

## Platform support

This example is intentionally Windows-only and supports both native MSVC and
MinGW-w64 builds. The separate library path is required for cross-compilation so
the linker cannot silently consume a Linux or macOS `librclient.a` from the
repository root. Use the epoll, kqueue/libdispatch, or portable third-party loop
examples for native Linux and macOS hosts.

## Ownership and shutdown

The runtime owns WinSock startup and every `SOCKET`. The application owns event
objects, request storage, and the copied outcome. Clear each `WSAEventSelect`
association and close its event before destroying runtime sockets. Never cast a
`SOCKET` to `int`; its type is pointer-width on 64-bit Windows.
