# Integration Examples

These examples connect `rl-c-client` to popular C event loops, HTTP servers,
and parsers. Every example includes only the public headers under `include/`.

`common/rl_example.c` keeps repeated setup out of each integration. It owns
nonblocking IPv4 and IPv6 UDP sockets, provides synchronous SRV plus A/AAAA
resolution, translates readable socket events into `r_client_on_datagram`, and
wraps request deadlines. Synchronous DNS keeps the examples small; production
event-loop integrations should replace it with their asynchronous resolver.

Required environment:

```sh
export RATELIMITLY_TENANT=tenant.example.com
export RATELIMITLY_AUTH_KEY=rl-aes1...
```

For local testing without DNS, point the shared adapter at the synthetic test
responder:

```sh
export RATELIMITLY_EXAMPLE_SERVER_HOST=127.0.0.1
export RATELIMITLY_EXAMPLE_SERVER_PORT=39082
```

The fixed endpoint variables are development-only. Normal deployments should
leave them unset so the adapter discovers `_ratelimitly._udp.<tenant>` SRV
records.

## libuv

`libuv.c` registers both UDP sockets with `uv_poll_t` and maps each request
deadline to a one-shot `uv_timer_t`.

```sh
cc -I../include -Icommon libuv.c common/rl_example.c ../librclient.a \
  $(pkg-config --cflags --libs libuv) -lcrypto -lresolv -pthread -o libuv-example
```

## libevent

`libevent.c` uses persistent `EV_READ` events for UDP ingress and an `evtimer`
for the current request deadline.

```sh
cc -I../include -Icommon libevent.c common/rl_example.c ../librclient.a \
  $(pkg-config --cflags --libs libevent) -lcrypto -lresolv -pthread \
  -o libevent-example
```

## libhv

`libhv.c` attaches each client UDP descriptor with `hio_add(..., HV_READ)` and
uses a one-shot `htimer_t` for request deadlines.

```sh
cc -I../include -Icommon libhv.c common/rl_example.c ../librclient.a \
  $(pkg-config --cflags --libs libhv) -lcrypto -lresolv -pthread \
  -o libhv-example
```

## liburing (Linux)

`liburing.c` submits one `IORING_OP_POLL_ADD` per UDP socket and uses
`io_uring_wait_cqe_timeout` to bound each wait by the Ratelimitly deadline.

```sh
cc -I../include -Icommon liburing.c common/rl_example.c ../librclient.a \
  $(pkg-config --cflags --libs liburing) -lcrypto -lresolv -pthread \
  -o liburing-example
```

## epoll (Linux)

`epoll.c` registers the nonblocking UDP descriptors directly and passes the
current Ratelimitly deadline to `epoll_wait`.

```sh
cc -I../include -Icommon epoll.c common/rl_example.c ../librclient.a \
  -lcrypto -lresolv -pthread -o epoll-example
```

## io_uring without liburing (Linux)

`io_uring.c` uses only Linux UAPI headers and syscalls. It maps submission and
completion rings directly, submits `IORING_OP_POLL_ADD`, and supplies the
request deadline through `IORING_ENTER_EXT_ARG`.

```sh
cc -I../include -Icommon io_uring.c common/rl_example.c ../librclient.a \
  -lcrypto -lresolv -pthread -o io-uring-example
```

## Mongoose

`mongoose.c` keeps HTTP connections pending while checks run. The Mongoose
poll loop drains Ratelimitly UDP sockets, advances per-request deadlines, and
cancels a check if its HTTP connection closes.

```sh
cc -I../include -Icommon -I/path/to/mongoose mongoose.c \
  common/rl_example.c /path/to/mongoose/mongoose.c ../librclient.a \
  -lcrypto -lresolv -pthread -o mongoose-example
```

Send `GET /limited` to port 8000.

## H2O

`h2o.c` wraps duplicate UDP descriptors with `H2O_SOCKET_FLAG_DONT_READ`, so
H2O reports readiness while `rl-c-client` consumes each datagram. Request-pool
destructors cancel abandoned checks; one-shot H2O timers enforce deadlines.

```sh
cc -I../include -Icommon h2o.c common/rl_example.c ../librclient.a \
  $(pkg-config --cflags --libs libh2o-evloop) \
  -lcrypto -lresolv -pthread -o h2o-example
```

Send `GET /limited` to port 8000.

## Lwan

`lwan.c` keeps client state off Lwan's small coroutine stacks. A dedicated
thread owns rl-c-client and its UDP sockets; handlers enqueue checks and yield
with `lwan_request_sleep()` until completion. Reference-counted jobs remain safe
when an HTTP peer disconnects while a check is active.
Lwan's built-in status table omits 429, so a denied check returns 403 instead;
returning an unknown numeric status would trip Lwan's status-table assertion.

Build Lwan, then compile against its public headers and library:

```sh
cc -I../include -Icommon -I/path/to/lwan/src/lib lwan.c \
  common/rl_example.c ../librclient.a /path/to/liblwan.a \
  -lcrypto -lresolv -pthread -o lwan-example
```

Set Lwan's listener in `lwan.conf`, then send `GET /limited`.

## libreactor

`libreactor.c` runs HTTP, UDP readiness, and per-request deadlines on one
libreactor thread. Duplicate UDP descriptors transfer only readiness ownership;
the common adapter still consumes datagrams from its original sockets. The
example also defers synchronous completions to avoid freeing callback state
while an rl-c-client operation remains on the stack.

```sh
cc -I../include -Icommon $(pkg-config --cflags libreactor) libreactor.c \
  common/rl_example.c ../librclient.a $(pkg-config --libs libreactor) \
  -lcrypto -lresolv -o libreactor-example
```

Send `GET /limited` to port 8000.

## GNU libmicrohttpd

`libmicrohttpd.c` runs MHD in external-select mode. It suspends each HTTP
connection while its asynchronous check is active, resumes it from the
Ratelimitly callback, and merges MHD plus request deadlines into one `select`.

```sh
cc -I../include -Icommon libmicrohttpd.c common/rl_example.c \
  ../librclient.a $(pkg-config --cflags --libs libmicrohttpd) \
  -lcrypto -lresolv -pthread -o libmicrohttpd-example
```

Send `GET /limited` to port 8000.

## CivetWeb

`civetweb.c` keeps `r_client_t` on one dedicated poll thread. CivetWeb worker
threads enqueue checks and wait on per-request condition variables, so no two
threads enter the client concurrently.

```sh
cc -I../include -Icommon $(pkg-config --cflags civetweb) civetweb.c \
  common/rl_example.c ../librclient.a $(pkg-config --libs civetweb) \
  -lcrypto -lresolv -pthread -o civetweb-example
```

Send `GET /limited` to port 8000.
