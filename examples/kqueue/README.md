# Pure kqueue integration

This example uses kqueue directly, without an event-loop library. It registers
runtime-owned UDP sockets with `EVFILT_READ` and supplies each current admission
delay as the `kevent` timeout. The request contains one resource rate limit and
one latency guard; only admitted, completed work produces a latency report.

## Control flow

```mermaid
flowchart TD
    Start["Start resource + latency admission"] --> Register["Register UDP sockets with EVFILT_READ"]
    Register --> Delay["Compute current relative deadline"]
    Delay --> Wait["kevent wait"]
    Wait --> Result{"Read event or timeout?"}
    Result -->|Read event| Read["Drain runtime datagrams"]
    Result -->|Timeout| Timeout["Advance admission timeout"]
    Read --> Decision{"Admission complete?"}
    Timeout --> Decision
    Decision -->|No| Delay
    Decision -->|Denied| Reject["No protected work or sample"]
    Decision -->|Allowed| Work["Run work, measure, report latency"]
```

## Build and run

On macOS or a BSD with kqueue:

```sh
make -C ../..
make
./kqueue-example
```

```sh
cmake -S . -B build
cmake --build build
./build/kqueue-example
```

Set `RATELIMITLY_TENANT` and `RATELIMITLY_AUTH_KEY`; fixed responder variables
are optional for local tests.

## Platform support

kqueue is available on macOS and the BSD family. It is not a native Linux or
Windows API; use epoll/io_uring on Linux and the Win32 example on Windows.

## Production notes

- Treat `EV_ERROR` and terminal `EV_EOF` results as watcher failures.
- Drain readable datagram sockets to `EAGAIN`.
- Recompute the relative timeout after every client transition.
- Close the kqueue before destroying runtime-owned sockets.
- Keep request storage alive until callback or explicit cancellation.
