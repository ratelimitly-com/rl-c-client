# Event-Loop Integration

`rl-c-client` is deliberately I/O agnostic. It never blocks, creates sockets, or
starts threads. The embedding application owns the event loop and passes network,
time, timer, and DNS events into the client.

## Responsibilities

The host application must provide:

- UDP send through `r_io_ops_t.udp_send`
- current wall-clock milliseconds through `r_io_ops_t.now_ms`
- optional log forwarding through `r_io_ops_t.log`
- optional source-port steering callback through `r_io_ops_t.on_steering_feedback`
- SRV lookup through `r_resolver_ops_t.resolve_srv`
- A/AAAA lookup through `r_resolver_ops_t.resolve_addrs`
- best-effort DNS cancellation through `r_resolver_ops_t.cancel`
- UDP receive delivery through `r_client_on_datagram`
- per-request timers using `r_client_request_deadline_ms` and `r_client_on_timeout`

All callbacks may be synchronous or asynchronous. Resolver callback buffers are
copied by the client during the callback and do not need to live afterward.
Asynchronous resolver implementations should set `out_req_id` to a nonzero
request ID before returning so the client can call `cancel` during teardown.
Late resolver callbacks after cancellation are allowed; the client will ignore
them.

## Request Flow

1. Create the client with `r_client_create`.
2. Submit a request with `r_client_check_rate_limit_async` or
   `r_client_check_rate_limit_async_borrowed`.
3. The client calls `udp_send` for one or more resolved server addresses.
4. The host reads UDP responses and calls `r_client_on_datagram`.
5. The host schedules the deadline returned by `r_client_request_deadline_ms`.
6. If the timer fires, the host calls `r_client_on_timeout`.
7. The client invokes the request callback exactly once unless the request is
   canceled.

The callback owns no result memory. Copy fields during the callback if they are
needed afterward.

## DNS

The client discovers servers with:

```text
_ratelimitly._udp.<configured-dns-name>
```

For each SRV record, the host resolver must resolve the SRV target hostname to
A/AAAA addresses. The SRV target name and port are part of the server identity
and routing input.

If TTL values are available, pass them in `r_srv_record_t.ttl_ms`. The client
uses TTLs to cap refresh intervals. If TTLs are unavailable, set `ttl_ms` to
zero and the configured refresh policy is used.

## Timers

The client does not schedule timers directly. After submitting a request, call:

```c
uint64_t deadline_ms;
if (r_client_request_deadline_ms(req, &deadline_ms) == RCLIENT_OK) {
    /* schedule host timer for deadline_ms */
}
```

When the timer fires:

```c
r_client_on_timeout(client, req, now_ms);
```

Retries may update the next deadline. Hosts should ask for the next deadline
after timeout handling if the request is still active.

## Borrowed Buffers

`r_client_check_rate_limit_async_borrowed` avoids copying resources, guards, and
metrics labels. The caller must keep every borrowed buffer valid until the
request callback fires or the request is canceled.

This is the preferred path for embedders that already have per-request memory
with a lifetime that extends to callback completion.

## Steering Feedback

Servers can ask clients to change UDP source port. If a completed request
contains a response that requests rebinding, the client calls:

```c
on_steering_feedback(ctx, false)
```

Do not close or rebind the socket while a request is in flight. Event-loop
integrations should mark a worker-level rebind flag and reopen the UDP socket
after the current request has completed.

## Proxy Module Notes

A proxy or HTTP-server module can wire the client as follows:

- `udp_send`: module UDP socket wrapper
- `now_ms`: event-loop clock in Unix epoch milliseconds
- `log`: module logging facility
- `on_steering_feedback`: mark socket rebind pending
- `resolve_srv`: event-loop resolver SRV query
- `resolve_addrs`: event-loop resolver A/AAAA query
- request timers: one host timer per in-flight request
- request memory: request pool plus borrowed API

Rate-limit request failures should be mapped by the module according to its
configured fail-open/fail-close policy. Latency-report send failures should be
logged but must not change the HTTP response outcome.
