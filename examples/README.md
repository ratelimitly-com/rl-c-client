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
