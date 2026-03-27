# I/O Abstraction Layer (C r-client MVP)

This document defines the event-loop friendly I/O abstraction for the C r-client. The goal is to integrate cleanly with nginx while staying generic for other async frameworks.

## Design Principles
- **No internal threads**. The host event loop owns sockets and timers.
- **Non-blocking**. The client never blocks; it only reacts to events.
- **Push-based receive**. The host delivers UDP datagrams to the client.
- **Host-scheduled timers**. The host controls timers; the client exposes deadlines.
- **Pluggable DNS**. SRV lookup and SRV-target A/AAAA resolution are delegated to a resolver interface.

## Core Event Flow
1. Host creates `r_client_t` with `r_io_ops` and `r_resolver_ops`.
2. Host submits async requests (rate limit / latency report).
3. Client emits UDP sends via `udp_send`.
4. Host delivers datagrams via `r_client_on_datagram`.
5. Host schedules **per-request timers** using `r_client_request_deadline_ms` and calls `r_client_on_timeout`.

## I/O Operations (network + time)
The client uses a small set of host-provided ops. These calls must be non-blocking.

- `udp_send(ctx, to, buf, len)`
  - Send a UDP datagram to `to`.
  - Return 0 on success, negative on error.
- `now_ms(ctx)`
  - Return milliseconds since UNIX epoch.
  - Used for request timestamps and deadline evaluation.
- `log(ctx, level, msg)` (optional)
  - Logging hook for debug builds; may be NULL.
- `on_steering_feedback(ctx, keep_port)` (optional)
  - Called after a request completes if any response requested a port change.
  - `keep_port == false` means the server recommends changing the source port.

## Timer Integration
The client does not own timers. **Each in-flight request exposes its own deadline**:

- `r_client_request_deadline_ms(req, &deadline_ms)`
  - Returns the next deadline for this request (timeout/retry/backoff).
  - Host sets a per-request timer (nginx-style).
- `r_client_on_timeout(client, req, now_ms)`
  - Host calls this when the per-request timer fires.
  - The client triggers timeout handling, retries, or completion.

This matches nginx's per-request timer model and works with other reactors.

## Receive Integration
- `r_client_on_datagram(client, buf, len, from)`
  - Host calls this when a UDP datagram is received.
  - The client parses the response and completes any matching request.

## DNS Resolver Abstraction
The client requires SRV and A/AAAA resolution matching Rust behavior:
- `_ratelimitly._udp.<tenant>` SRV
- A/AAAA resolution for each SRV target hostname returned by that SRV lookup

DNS is delegated to a resolver interface so nginx can plug in its own resolver.
Synchronous resolvers may call the callback inline.

Key points:
- Results are copied by the client during callback; resolver-owned buffers
  only need to live for the callback duration.
- Cancellation is best-effort; it is acceptable to ignore late results.
- Optional TTL fields can be provided by the resolver; when present, the client caps its refresh interval to the minimum TTL seen. If absent, the client uses its configured refresh interval.

## Memory Ownership
- The client never takes ownership of caller buffers unless documented.
- The resolver and I/O adapters own their internal storage.
- Results passed into callbacks are copied immediately.

## Suggested Event Loop Usage (pseudo)

```
// setup
r_client_create(&cfg, &io_ops, &dns_ops);

// send a request
r_client_check_rate_limit_async(client, ..., cb, user);

// event loop
for (;;) {
  // 1) UDP read ready -> recvfrom -> r_client_on_datagram
  // 2) per-request timer fired -> r_client_on_timeout(client, req, now_ms)
  // 3) per-request deadlines are updated by r_client_request_deadline_ms
}
```

## Notes for nginx
- Use nginx UDP sockets and `ngx_event_t` for receive readiness.
- `now_ms` should map to `ngx_current_msec`.
- DNS resolver should map to nginx resolver (async, event-driven).
