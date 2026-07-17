# libdispatch integration

This example serializes all client calls on a private dispatch queue. Read
sources observe runtime-owned UDP sockets, a one-shot timer source follows the
admission deadline, and a semaphore gives `main` a finite shutdown path.

Each request contains a resource rate limit and a latency guard. Only admitted,
successfully completed work is measured and reported.

## Control flow

```mermaid
flowchart TD
    Main["main creates private serial queue"] --> Start["Queue resource + latency admission"]
    Start --> Sources["Resume dispatch read sources"]
    Sources --> Timer["Arm one-shot dispatch timer"]
    Timer --> Event{"Serial-queue callback"}
    Event -->|Read source| Read["Drain runtime datagrams"]
    Event -->|Timer source| Timeout["Advance admission timeout"]
    Read --> Decision{"Admission complete?"}
    Timeout --> Decision
    Decision -->|No| Timer
    Decision -->|Denied| Reject["Signal completion without sample"]
    Decision -->|Allowed| Work["Run work, measure, report latency"]
    Reject --> Shutdown["Cancel sources, destroy runtime, signal semaphore"]
    Work --> Shutdown
```

## Build and run

On macOS, build the client library and then this folder:

```sh
make -C ../..
make
./libdispatch-example
```

```sh
cmake -S . -B build
cmake --build build
./build/libdispatch-example
```

Set `RATELIMITLY_TENANT` and `RATELIMITLY_AUTH_KEY`; fixed responder variables
are optional for local tests.

## Platform support

libdispatch is native on macOS and is also available as open source on some
Linux distributions. This example targets POSIX socket handles and is tested on
macOS. Use native Win32, libuv, libevent, or GLib on Windows.

## Ownership and shutdown

The application owns the queue, sources, semaphore, request, and copied
outcome. All transitions—including cancellation and runtime destruction—run on
the serial queue. Sources are cancelled before runtime sockets close. A private
queue plus semaphore is used instead of `dispatch_main`, which never returns
and would obscure teardown in a teaching example.
