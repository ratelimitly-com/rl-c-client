# Event-Loop Integration

The core client (`r_client.h` + `r_client_io.h`) is deliberately I/O agnostic.
It never blocks, creates sockets, or starts threads. The embedding application
owns the event loop and passes network, time, timer, and DNS events into the
client. (The optional public runtime layer is different: it owns UDP sockets
and performs synchronous, blocking DNS — see the Public runtime section of
[docs/api.md](docs/api.md).) The client contains no locks: confine each client
to one thread or event loop and serialize all calls on it.

Working integrations for common loops and HTTP frameworks live in
[`examples/`](examples/README.md). This document defines the host contract;
examples show framework-specific descriptor, timer, callback, and shutdown
ownership.

## Responsibilities

The host application must provide:

- UDP send through `r_io_ops_t.udp_send`
- current wall-clock milliseconds through `r_io_ops_t.now_ms`
- optionally, a source-port steering callback through
  `r_io_ops_t.on_steering_feedback`
- SRV lookup through `r_resolver_ops_t.resolve_srv`
- A/AAAA lookup through `r_resolver_ops_t.resolve_addrs`
- optionally, best-effort DNS cancellation through `r_resolver_ops_t.cancel`
  (NULL is tolerated; recommended for asynchronous resolvers)
- UDP receive delivery through `r_client_on_datagram`
- per-request timers using `r_client_request_deadline_ms` and `r_client_on_timeout`

The `r_io_ops_t.log` hook is reserved: the current library never invokes it.
Do not rely on it for diagnostics.

`r_client_create` does not validate individual function pointers, and the
consequences of missing ones differ: a NULL `resolve_srv` or `resolve_addrs`
crashes during the first discovery (inside `r_client_create` when the resolver
is synchronous); a NULL `now_ms` silently reads time zero and breaks all
scheduling; a NULL `udp_send` surfaces later as `RCLIENT_ERR_IO`. Treat
`udp_send`, `now_ms`, `resolve_srv`, and `resolve_addrs` as mandatory.

All callbacks may be synchronous or asynchronous. Resolver callback buffers are
copied by the client during the callback and do not need to live afterward.
The `name` argument passed to `resolve_srv` and `resolve_addrs` is borrowed
only for the duration of the call — an asynchronous resolver that stores it
for a later lookup must copy the string before returning.
Asynchronous resolver implementations should set `out_req_id` to a nonzero
request ID before returning so the client can call `cancel` during teardown.
Late resolver callbacks after cancellation are allowed; the client will ignore
them.

`udp_send` is different from a resolver callback: it must return before the
host calls any `r_client_*` API. If a test transport or custom I/O layer obtains
a response synchronously, queue that datagram and deliver it through
`r_client_on_datagram` only after `udp_send` has unwound.

## Request Flow

1. Create the client with `r_client_create`.
2. Submit a request with `r_client_check_rate_limit_async` or
   `r_client_check_rate_limit_async_borrowed`.
3. The client calls `udp_send` for every resolved server address; a failing
   `udp_send` return aborts the attempt with `RCLIENT_ERR_IO`.
4. The host reads UDP responses and calls `r_client_on_datagram`.
5. The host schedules the deadline returned by `r_client_request_deadline_ms`.
6. If the timer fires, the host calls `r_client_on_timeout`.
7. The client invokes the request callback exactly once — unless the request
   is canceled, or the client is destroyed with the request still in flight.
   Both suppression paths free the request without any callback, and both act
   as release points for borrowed buffers.

The callback owns no result memory. Copy fields during the callback if they are
needed afterward. From inside the callback, submitting, reporting, and
canceling are safe; calling `r_client_destroy` is not — destroy the client
only after the stack has unwound to the event loop.

## DNS

The client discovers servers with:

```text
_ratelimitly._udp.<configured-dns-name>
```

For each SRV record, the host resolver must resolve the SRV target hostname to
A/AAAA addresses. The SRV target name and port are part of the server identity
and routing input, under a strict naming contract:

**Each SRV target hostname's first DNS label must encode that server's 64-bit
ID in decimal as `s-<server_id>`** — for example `s-1015809.rl1.example.com`.
The client parses the label to learn each server's identity. SRV targets whose
first label does not match are **silently skipped**, and when at least one
target carries an ID, responses claiming a server ID outside the SRV-derived
set are **silently dropped**. The ID also encodes the server's start time
(`start_seconds_since_2025 = server_id >> 23`), which drives the HA policy's
oldest-server preference (see docs/api.md). A zone published without this
convention yields no usable servers. The client ignores SRV `priority` and
`weight`; record order does not matter.

Discovery is SRV-only. A failed or empty SRV lookup, or one containing no
conforming targets, does not fall back to the configured tenant name on a
hard-coded UDP port. Resource-request submission and latency reporting return
`RCLIENT_ERR_DNS` until usable SRV membership is available.

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

Replay rounds may update the next deadline. Completion is signaled **only** by
the request callback firing during the `r_client_on_timeout` (or
`r_client_on_datagram`) call — the return value is `RCLIENT_OK` both when the
event was nonterminal and when it completed and freed the request. Track a
flag from the callback: if it fired, the request handle is invalid and must
not be used again; if it did not, re-query
`r_client_request_deadline_ms` and re-arm the host timer. Firing the timer
early is a safe no-op.

## Clock domains

Client control time uses Unix-epoch milliseconds throughout:

- `r_io_ops_t.now_ms` returns current Unix-epoch milliseconds;
- `r_client_request_deadline_ms` returns an absolute value in that domain; and
- `r_client_on_timeout` receives current time in that same domain.

Convert an absolute deadline into a relative host-loop delay by subtracting a
fresh `now_ms` value and clamping expired deadlines to zero. Do not pass a raw
relative timeout to `r_client_on_timeout`.

Protected-work latency uses a separate duration clock. Measure it with
`CLOCK_MONOTONIC` or the event loop's equivalent, so wall-clock correction
cannot create negative or inflated samples. Convert only the elapsed duration
to `r_service_latency_report_t.observed_latency`; never feed that monotonic
clock value into `r_io_ops_t.now_ms`.

## Borrowed Buffers

`r_client_check_rate_limit_async_borrowed` avoids copying resources, guards, and
metrics labels. The caller must keep every borrowed buffer valid until the
request callback fires, the request is canceled, or the client is destroyed.

This is the preferred path for embedders that already have per-request memory
with a lifetime that extends to callback completion.

## Latency reports

`r_client_report_latency` serializes and sends reports during the call. It
creates no in-flight request, response callback, or request timer. Event-loop
integrations therefore need no new read watcher or deadline path for reports;
the existing UDP send hook is sufficient.

Latency reports are independent of resource requests and guards. A client may
report measured service latency without issuing any resource request. When an
application deliberately pairs a report with guarded work, it should report
only a real operation that ran, use the same latency-tracker ID and tracker
settings, and never invent a zero sample for work the guard rejected. Log send
failures, but do not change an HTTP response outcome after protected work
completed.

See the [example latency tracking workflow](examples/README.md#example-latency-tracking-workflow)
for runnable pass/report and deny/no-report behavior.

## Steering Feedback

Servers can ask clients to change UDP source port — typically to redistribute
load across server-side receive paths; ignoring the request costs load-balance
quality, not correctness. If a completed request contains any response that
requests rebinding, the client calls, once, after the completion callback has
returned:

```c
on_steering_feedback(ctx, false)
```

`keep_port == false` is the only value the hook is ever called with; a
keep-port response simply produces no call. The selected result's
`steering_feedback` field carries the raw wire flag (`true` = keep port).

Do not close or rebind the socket while a request is in flight. Event-loop
integrations should mark a worker-level rebind flag and reopen the UDP socket
after the current request has completed.

## Proxy Module Notes

A proxy or HTTP-server module can wire the client as follows:

- `udp_send`: module UDP socket wrapper
- `now_ms`: event-loop clock in Unix epoch milliseconds
- `log`: leave NULL (reserved; the library currently emits no log messages)
- `on_steering_feedback`: mark socket rebind pending
- `resolve_srv`: event-loop resolver SRV query
- `resolve_addrs`: event-loop resolver A/AAAA query
- request timers: one host timer per in-flight request
- request memory: request pool plus borrowed API

Resource-request failures should be mapped by the module according to its
configured fail-open/fail-close policy. Latency-report send failures should be
logged but must not change the HTTP response outcome.
