# Integration examples

These examples show how to drive `rl-c-client` from common C event loops, HTTP
servers, and parsers. They use only the public client headers under `include/`.
Each source file starts with a numbered flow and an ownership summary; read
those comments before transplanting the integration into an application.

| Example | Integration model | Main technique |
| --- | --- | --- |
| Latency tracker | Guard and reporting workflow | Gate work, measure it, then report one sample |
| libuv | Native event loop | `uv_poll_t` plus a one-shot `uv_timer_t` |
| libevent | Native event loop | Persistent `EV_READ` plus `evtimer` |
| GLib/GIO | Portable main loop | Non-owning `GIOChannel` watches plus timeout source |
| libev | Compact event loop | `ev_io` watchers plus a one-shot `ev_timer` |
| sd-event | systemd event loop | `sd_event_add_io` plus monotonic time source |
| kqueue | Native macOS/BSD readiness | Direct `kevent` wait plus request deadline |
| libdispatch | Serial dispatch queue | Read sources plus a one-shot timer source |
| Win32 | Native WinSock wait loop | `WSAEVENT` readiness plus deadline timeout |
| libhv | Native event loop | `hio_t` readiness plus `htimer_t` |
| liburing | Linux completion ring | `IORING_OP_POLL_ADD` through liburing |
| epoll | Linux readiness API | Direct `epoll_wait` with a request deadline |
| io_uring | Raw Linux completion ring | Syscalls and shared ring mappings |
| Mongoose | Single-thread HTTP loop | Pending connection state on `mg_mgr_poll` |
| CivetWeb | Worker-thread HTTP server | Queue plus a dedicated client thread |
| GNU libmicrohttpd | External HTTP loop | Suspended connections and merged `select` |
| H2O | Native HTTP event loop | Descriptor watchers and pool cleanup |
| Lwan | Coroutine HTTP server | Yielding handlers and a dedicated client thread |
| libreactor | Native HTTP event loop | Descriptor and timer objects on one thread |
| facil.io | Native HTTP event loop | Pause/resume handles with retained timers |
| Onion | Worker-thread HTTP server | `OCS_YIELD` long-poll lifecycle |
| Kore | Task-based HTTP server | Sleeping requests and task-channel results |
| Ulfius | Threaded HTTP server | A private client in each endpoint callback |
| llhttp | Parser only | Fragmented input and resumable backpressure |

## Choose an integration pattern

The examples intentionally cover several ownership models. Choose based on the
concurrency model already used by the host application.

| Host architecture | Start with | Trade-off |
| --- | --- | --- |
| Any host using latency load shedding | Latency tracker, then the matching loop or framework | Shows policy and measurement lifecycle separately from framework plumbing. |
| One event-loop thread | libuv, libevent, libhv, epoll, io_uring, Mongoose, H2O, libreactor, or facil.io | No client lock is needed when all calls stay on the loop thread. |
| Worker-thread HTTP server | CivetWeb, Lwan, or Onion | A dedicated bridge thread serializes client access; queue and shutdown ownership need care. |
| Isolated request tasks | Kore | Each task owns its client and reports through the framework's task channel. |
| Lowest-complexity threaded handler | Ulfius | A private client is simple, but the handler waits and client setup repeats per request. |
| HTTP parser without a loop | llhttp | The host must provide TCP I/O, UDP readiness, deadlines, and connection lifetime. |

Prefer a single long-lived client per event loop or bridge thread in a busy
service. The per-request Ulfius pattern is included because its ownership is
especially easy to understand, not because it minimizes setup cost.

## Shared adapter and configuration

`common/rl_example.c` removes protocol boilerplate from the integrations. It:

- owns nonblocking IPv4 and IPv6 UDP sockets;
- performs SRV and A/AAAA discovery;
- passes readable datagrams to `r_client_on_datagram()`; and
- exposes the current request deadline to the host loop.

DNS resolution is synchronous to keep the examples focused. Production event
loops should normally substitute their asynchronous resolver during startup or
configuration refresh.

Set the tenant and authentication key before running an example:

```sh
export RATELIMITLY_TENANT=tenant.example.com
export RATELIMITLY_AUTH_KEY=rl-aes1...
```

For local development, bypass DNS and point the adapter at the repository's
synthetic responder:

```sh
export RATELIMITLY_EXAMPLE_SERVER_HOST=127.0.0.1
export RATELIMITLY_EXAMPLE_SERVER_PORT=39082
```

Leave the fixed endpoint variables unset in a normal deployment. The adapter
then discovers `_ratelimitly._udp.<tenant>` SRV records.

Build the library from the repository root, then enter this directory:

```sh
make
cd examples
```

The commands below assume `../librclient.a` exists. Exact framework compiler
flags can vary by distribution; `pkg-config` commands use the names exported by
the upstream projects or common Linux packages.

Run all repository tests, including the example inventory and shared-adapter
tests, from the repository root:

```sh
make test
```

## Latency tracking workflow

`latency_tracker.c` demonstrates both halves of latency-based load shedding:

1. Hash a stable service name into `r_latency_guard_t.service_id`.
2. Include that guard in the rate-limit request.
3. Copy the guard decision during the completion callback; result arrays are
   owned by `rl-c-client` and expire when the callback returns.
4. Perform protected work only when the combined resource and guard result
   passes.
5. Measure only that work with `CLOCK_MONOTONIC`.
6. Send the observation with `r_client_report_latency()` using the same service
   id and tracker configuration.

Never report latency for work rejected by the guard. No operation occurred, so
a zero or synthetic sample would corrupt the service tracker. Likewise, do not
measure the RateLimitly request round trip unless that round trip is itself the
service whose latency should control admission.

The guard's policy fields have distinct purposes:

| Field | Meaning |
| --- | --- |
| `threshold_ms` | Reject protected work when tracked service latency reaches this value. |
| `ttl_ms` | Maximum age of samples retained for this tracker. |
| `max_samples` | Maximum sample count considered by the tracker. |
| `buffer_size` | Tracker storage request; must fit the credential quota. |
| `min_sample_threshold` | Samples required before the tracker estimate controls admission. |

The report repeats `service_id`, `ttl_ms`, `max_samples`, `buffer_size`, and
`min_sample_threshold` so it updates the same tracker. `threshold_ms` belongs
only to the guard decision.

Build and run the standalone workflow:

```sh
cc -I../include -Icommon latency_tracker.c common/rl_example.c \
  ../librclient.a -lcrypto -lresolv -pthread -o latency-tracker-example

# Optional: simulated protected-work duration; default is 25 ms.
export RATELIMITLY_EXAMPLE_WORK_MS=25
./latency-tracker-example
```

Passing output resembles:

```text
guard passed: current=20 ms threshold=100 ms
latency reported: service=example-inventory-backend observed=25 ms
```

The sleep represents a backend call only in this command-line demonstration.
Production event loops should start the real asynchronous operation after the
guard passes, record its monotonic start time, and report from its completion
callback. `r_client_report_latency()` is fire-and-forget: it uses the client's
UDP send hook immediately and creates no request deadline or response watcher.

## Event-loop examples

The six examples in this section submit one check, print `allowed` or `denied`,
and exit. They are deliberately small references for readiness and deadline
integration rather than HTTP servers.

### libuv

**Model.** `libuv.c` registers both UDP sockets with `uv_poll_t`. A one-shot
`uv_timer_t` is rearmed from the current request deadline after every event.
The loop thread owns the client and all watcher callbacks.

```sh
cc -I../include -Icommon libuv.c common/rl_example.c ../librclient.a \
  $(pkg-config --cflags --libs libuv) -lcrypto -lresolv -pthread \
  -o libuv-example
./libuv-example
```

**Production note.** Keep watcher teardown on the loop thread. Do not destroy
the shared client while a poll or timer callback can still run.

### libevent

**Model.** `libevent.c` uses persistent `EV_READ` events for UDP ingress and an
`evtimer` for the active request. The event-base thread owns the client.

```sh
cc -I../include -Icommon libevent.c common/rl_example.c ../librclient.a \
  $(pkg-config --cflags --libs libevent) -lcrypto -lresolv -pthread \
  -o libevent-example
./libevent-example
```

**Production note.** Free every successfully created event, including partial
initialization paths, before destroying the client.

### GLib/GIO

**Model.** `glib/main.c` attaches non-owning `GIOChannel` watches for the
runtime's UDP sockets and uses a one-shot GLib timeout for the request deadline.
The same source supports Unix sockets and WinSock handles.

```sh
cd glib
make
./glib-example
```

**Production note.** Remove every source and unref every non-owning channel
before destroying the runtime that owns the underlying sockets.

### libev

**Model.** `libev/main.c` uses `ev_io` for UDP readiness and re-arms a one-shot
`ev_timer` from the current admission deadline.

```sh
cd libev
make
./libev-example
```

**Production note.** Stop all fd watchers before destroying runtime sockets.
This example targets Unix fd backends rather than narrowing WinSock handles.

### sd-event (Linux)

**Model.** `sd_event/main.c` combines `sd_event_add_io` socket sources with a
one-shot monotonic time source derived from the relative admission delay.

```sh
cd sd_event
make
./sd-event-example
```

**Production note.** Keep wall-clock client deadlines and monotonic timer
timestamps in separate domains; convert through a relative delay.

### kqueue (macOS/BSD)

**Model.** `kqueue/main.c` registers runtime UDP sockets with `EVFILT_READ` and
passes the current relative admission delay directly to `kevent`.

```sh
cd kqueue
make
./kqueue-example
```

**Production note.** Treat `EV_ERROR` and terminal `EV_EOF` as watcher errors,
and close the kqueue before destroying runtime-owned sockets.

### libdispatch

**Model.** `libdispatch/main.c` serializes client calls on a private queue,
using read sources for UDP sockets and a timer source for admission deadlines.

```sh
cd libdispatch
make
./libdispatch-example
```

**Production note.** Cancel sources and serialize runtime teardown on the same
queue. A semaphore gives this teaching program a finite shutdown path.

### Win32

**Model.** `win32/main.c` associates each native `SOCKET` with a `WSAEVENT` and
passes the admission delay to `WSAWaitForMultipleEvents`.

```sh
cd win32
make CC=x86_64-w64-mingw32-gcc OPENSSL_PREFIX=/path/to/mingw/openssl
```

**Production note.** Preserve pointer-width `SOCKET` values. Clear event
associations before closing events or destroying runtime-owned sockets.

### libhv

**Model.** `libhv.c` attaches each client descriptor with
`hio_add(..., HV_READ)` and maps the request deadline to a one-shot `htimer_t`.
All client calls remain on the libhv loop thread.

```sh
cc -I../include -Icommon libhv.c common/rl_example.c ../librclient.a \
  $(pkg-config --cflags --libs libhv) -lcrypto -lresolv -pthread \
  -o libhv-example
./libhv-example
```

**Production note.** If application work can run on other threads, marshal it
onto this loop instead of calling the shared client concurrently.

### liburing (Linux)

**Model.** `liburing.c` submits one `IORING_OP_POLL_ADD` per UDP socket and
uses `io_uring_wait_cqe_timeout()` to cap each wait at the request deadline.
Each completion is consumed before the corresponding poll is rearmed.

```sh
cc -I../include -Icommon liburing.c common/rl_example.c ../librclient.a \
  $(pkg-config --cflags --libs liburing) -lcrypto -lresolv -pthread \
  -o liburing-example
./liburing-example
```

**Production note.** User-data values identify watcher completions. Reserve a
non-overlapping namespace if the same ring also serves application I/O.

### epoll (Linux)

**Model.** `epoll.c` registers the nonblocking UDP descriptors directly. It
recomputes the timeout passed to `epoll_wait()` from the client deadline on
every iteration.

```sh
cc -I../include -Icommon epoll.c common/rl_example.c ../librclient.a \
  -lcrypto -lresolv -pthread -o epoll-example
./epoll-example
```

**Production note.** Treat `EPOLLERR` and `EPOLLHUP` as terminal watcher
failures; repeatedly retrying a broken descriptor creates a busy loop.

### io_uring without liburing (Linux)

**Model.** `io_uring.c` uses Linux UAPI headers and syscalls directly. It maps
the submission and completion rings, submits `IORING_OP_POLL_ADD`, and supplies
the deadline through `IORING_ENTER_EXT_ARG`.

```sh
cc -I../include -Icommon io_uring.c common/rl_example.c ../librclient.a \
  -lcrypto -lresolv -pthread -o io-uring-example
./io-uring-example
```

**Production note.** This is an educational raw-ring implementation. liburing
is usually the safer compatibility layer for production code, especially when
supporting several kernel versions or sharing a ring with other operations.

## HTTP framework examples

Unless a section says otherwise, these examples listen on port 8000. Send
`GET /limited` to run a check. Non-GET requests are rejected before starting a
check, and an abandoned request is cancelled where the framework exposes a
reliable disconnect lifecycle.

### Mongoose

**Model.** `mongoose.c` leaves an HTTP connection pending while its check runs.
`mg_mgr_poll()` drains client UDP sockets, advances request deadlines, and
cancels the check if the connection closes. One Mongoose thread owns all state.

```sh
MONGOOSE=/path/to/mongoose
cc -I../include -Icommon -I"$MONGOOSE" mongoose.c common/rl_example.c \
  "$MONGOOSE/mongoose.c" ../librclient.a -lcrypto -lresolv -pthread \
  -o mongoose-example
./mongoose-example
curl -i http://127.0.0.1:8000/limited
```

**Production note.** The sample uses a 10 ms Mongoose poll interval. A host
with stricter latency or power requirements should derive its wait from both
Mongoose work and the nearest client deadline.

### CivetWeb

**Model.** [`civetweb/main.c`](civetweb/main.c) keeps the client on one dedicated
poll thread. CivetWeb workers enqueue jobs and wait on per-request condition
variables, so no two threads enter the client concurrently. An admitted request
runs measured protected work on the bridge and reports it to the latency tracker.
Server workers stop before bridge state is destroyed.

```sh
cd civetweb
make CIVETWEB_ROOT=/path/to/civetweb
./civetweb-example
curl -i http://127.0.0.1:8000/limited
```

**Production note.** Waiting consumes one CivetWeb worker per in-flight check.
Size the worker pool for the expected concurrency or adapt the bridge to an
asynchronous response API if the service holds many simultaneous requests.

### GNU libmicrohttpd

**Model.** [`libmicrohttpd/main.c`](libmicrohttpd/main.c) runs MHD in
external-`select` mode. It suspends an HTTP connection during the asynchronous
combined admission check, measures and reports admitted protected work, resumes
the connection from the callback, and merges MHD's timeout with the client
request deadline.

```sh
cd libmicrohttpd
make
./libmicrohttpd-example
curl -i http://127.0.0.1:8000/limited
```

**Production note.** Always call the MHD timeout API again after processing
activity; both MHD and the client can move their nearest deadline.

### H2O

**Model.** [`h2o/main.c`](h2o/main.c) wraps duplicate UDP descriptors with
`H2O_SOCKET_FLAG_DONT_READ`, so H2O reports readiness while the public runtime
consumes datagrams from the original sockets. Request-pool destructors cancel
abandoned checks, one-shot H2O timers enforce deadlines, and admitted protected
work is measured and reported to the latency tracker.

```sh
cd h2o
make
./h2o-example
curl -i http://127.0.0.1:8000/limited
```

**Production note.** The duplicate descriptors transfer readiness observation,
not datagram ownership. Only the adapter should read the original client
sockets, and its state must outlive every H2O watcher.

### Lwan

**Model.** [`lwan/main.c`](lwan/main.c) keeps client state off Lwan's small
coroutine stacks. A dedicated thread owns the runtime and UDP sockets; handlers
enqueue combined admission checks and yield with `lwan_request_sleep()` until
completion. Reference-counted jobs remain valid after disconnect, and admitted
protected work is measured and reported on the bridge.

Build Lwan, then compile against its generated configuration and static library:

```sh
cd lwan
make LWAN_ROOT=/path/to/lwan LWAN_BUILD=/path/to/lwan/build
./lwan-example
curl -i http://127.0.0.1:8080/limited
```

The whole-archive flags retain Lwan's linker-discovered module table. A shared
installation can instead use its generated `lwan.pc`. Lwan listens on port
8080 by default; change its normal configuration if that port is unsuitable.

**Production note.** Lwan's public status table omits 429, so this example
returns 403 for a denied check. Using an unknown numeric status triggers an
assertion in supported Lwan versions.

### libreactor

**Model.** [`libreactor/main.c`](libreactor/main.c) runs HTTP, UDP readiness,
and deadlines on one reactor thread. Duplicate descriptors observe readiness
while the public runtime owns datagram reads. Synchronous completions are
deferred so callback state is not freed while a client operation remains on the
stack. Allowed protected work is measured and reported before response.

```sh
cd libreactor
make
./libreactor-example
curl -i http://127.0.0.1:8000/limited
```

**Production note.** Preserve deferred completion if surrounding code can
complete a request synchronously; immediate destruction would make reentrant
callbacks unsafe.

### facil.io

**Model.** `facil_io.c` attaches duplicate UDP descriptors as facil.io
protocols and uses HTTP pause/resume for asynchronous checks. Timer callbacks
retain request state through `on_finish`, preventing a use-after-free when a
UDP response races its deadline. One event-loop thread serializes client calls.

```sh
FACIL=/path/to/facil.io
FACIL_BUILD=/path/to/facil.io-build
cc -I../include -Icommon -I"$FACIL/lib" -I"$FACIL/lib/facil" \
  -I"$FACIL/lib/facil/tls" -I"$FACIL/lib/facil/fiobj" \
  -I"$FACIL/lib/facil/cli" -I"$FACIL/lib/facil/http" \
  -I"$FACIL/lib/facil/http/parsers" -I"$FACIL/lib/facil/redis" \
  facil_io.c common/rl_example.c ../librclient.a \
  "$FACIL_BUILD/libfacil.io.a" -lcrypto -lssl -lresolv -ldl -lm -pthread \
  -o facil-io-example
./facil-io-example
curl -i http://127.0.0.1:8000/limited
```

**Production note.** Keep the explicit retain/release pairs around scheduled
timers. A failed schedule must release the timer's reference immediately.

### Onion

**Model.** `onion.c` returns `OCS_YIELD`, so Onion workers do not synchronously
poll client sockets. A dedicated thread owns the client and completes each
yielded response through Onion's long-poll lifecycle. Shutdown cancels active
checks and releases every yielded request exactly once.

```sh
cc -I../include -Icommon $(pkg-config --cflags onion) onion.c \
  common/rl_example.c ../librclient.a $(pkg-config --libs onion) \
  -lcrypto -lresolv -pthread -o onion-example
./onion-example
curl -i http://127.0.0.1:8000/limited
```

**Production note.** Keep response writes in the completion path documented by
the Onion version being used. A bridge failure must wake all yielded requests
with an error rather than leaving them suspended.

### Kore

**Model.** `kore.c` runs each exchange as a `kore_task`, putting the HTTP
request to sleep until the task channel reports a result. Task-local client
ownership avoids cross-thread access. The Linux module also declares the extra
socket syscalls required by Kore's seccomp filter.

Build Kore with task support and its no-TLS backend, then build the module:

```sh
make -C /path/to/kore TASKS=1 TLS_BACKEND=none
cc -fPIC -shared -DKORE_USE_TASKS -I/path/to/kore/include \
  -I../include -Icommon kore.c common/rl_example.c ../librclient.a \
  -lcrypto -lresolv -pthread -o kore-example.so
kore -fnc kore.conf
curl -i http://127.0.0.1:8000/limited
```

**Production note.** `kore.conf` grants the module's plain-HTTP route and task
thread count. Reconcile the syscall declarations with a hardened deployment's
own Kore build and sandbox policy.

### Ulfius

**Model.** `ulfius.c` gives each endpoint callback a private client and polls
its UDP sockets on that callback's libmicrohttpd worker. This avoids shared
mutable client state and does not block the listener thread.

```sh
cc -I../include -Icommon $(pkg-config --cflags libulfius) ulfius.c \
  common/rl_example.c ../librclient.a $(pkg-config --libs libulfius) \
  -lcrypto -lresolv -pthread -o ulfius-example
./ulfius-example
curl -i http://127.0.0.1:8000/limited
```

**Production note.** One worker remains occupied and one client is initialized
per request. For high concurrency, prefer a shared dedicated-thread bridge such
as the CivetWeb or Onion pattern.

## llhttp parser adapter

`llhttp` parses HTTP but does not provide an event loop. `llhttp.c` collects URL
fragments, removes query data from the bucket key, and starts a check when a
request completes. The companion `llhttp_adapter.h` is the host-facing API.

Compile both into a host that already builds llhttp:

```sh
cc -I. -I/path/to/llhttp/include -I../include -Icommon -c llhttp.c
```

The host lifecycle is:

1. Call `rl_llhttp_adapter_init()` once per accepted connection and check its
   return status.
2. Pass every TCP fragment to `rl_llhttp_adapter_feed()`.
3. If it returns `HPE_PAUSED`, keep the unconsumed bytes beginning at
   `llhttp_get_error_pos()`. One check is already active.
4. Drive the shared client's UDP descriptors and the adapter request's deadline
   using one of the loop patterns above.
5. The result callback runs after the adapter calls `llhttp_resume()`. Feed the
   retained bytes only after that callback begins.
6. Call `rl_llhttp_adapter_finish()` on clean TCP EOF, then
   `rl_llhttp_adapter_dispose()` before releasing connection state.

The pause is resumable backpressure for a pipelined next request; validation
failures use `HPE_USER` and are terminal parser errors. The host owns TCP I/O,
UDP readiness, deadlines, and the adapter allocation. The shared client may be
used by multiple adapters only when the host serializes all access on one event
loop thread.
