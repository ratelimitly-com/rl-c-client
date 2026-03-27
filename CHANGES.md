# Changes On Top Of `main`

This document describes the current workspace changes relative to `main`.
It reflects the tree in `/home/wojtek/glar/rl/clients/c`, not just committed
history.

## Files Changed

- `Makefile`
- `README.md`
- `bin/perf_client.c`
- `src/r_client.c`

## Build System

`Makefile` now supports an optional OpenSSL prefix and passes `LDFLAGS` into
the perf client, shared library, and quota test link steps.

Behavioral detail:

- On Darwin, `OPENSSL_PREFIX` defaults to `/opt/homebrew/opt/openssl@3`.
- If `OPENSSL_PREFIX` is set, its `include` and `lib` directories are added to
  `CPPFLAGS` and `LDFLAGS`.

## DNS Lookup Strategy

The largest runtime change is in `bin/perf_client.c`.

### Previous behavior on `main`

Each worker created its own live resolver context with `res_ninit()` and used
that resolver directly through `perf_resolve_srv()` and `perf_resolve_addrs()`.
DNS lookups happened on demand from worker threads.

### Current behavior

The perf client now pre-resolves and caches DNS once in `main()` before worker
threads are started.

Startup flow:

1. `main()` calls `perf_dns_cache_init(&dns_cache, cfg.srv_domain)`.
2. The cache initializer creates one resolver context with `dns_resolver_init()`.
3. It constructs the SRV name `_ratelimitly._udp.<tenant_name>`.
4. It queries SRV records for that name with `dns_query_srv()`.
5. For each SRV target returned, it resolves A/AAAA records with
   `dns_query_addrs()`.
6. It also performs a direct A/AAAA lookup for `<tenant_name>` itself and saves
   that as a legacy fallback address entry in the perf client cache. This is
   not the intended normative discovery contract for Ratelimitly clients.
7. The resolver context is closed after the cache is populated.
8. Each worker receives a pointer to the shared immutable cache through
   `perf_config.dns_cache`.

Worker/runtime behavior:

- `perf_resolve_srv()` now serves only from the prebuilt cache.
- `perf_resolve_addrs()` now serves only from the prebuilt cache.
- Workers no longer call `res_ninit()` or `res_nclose()`.
- If cache allocation/setup fails, `main()` exits before starting workers.

Important nuance:

- `perf_dns_cache_init()` ignores SRV lookup failure and direct address lookup
  failure as long as the cache machinery itself initializes correctly. That
  means this perf-client startup path can still succeed with an empty cache;
  resolution failure is then surfaced later through the resolver callbacks.

Net effect:

- DNS work moved from per-worker live resolution to process-wide startup
  pre-resolution plus in-memory callback responses.
- Resolver behavior is now deterministic for the lifetime of a run unless the
  library itself triggers its own internal DNS refresh logic.

## Retry Parameters

### Perf client CLI surface

`bin/perf_client.c` now accepts and prints these retry-related parameters:

- `--attempt-timeout-ms=<n>`
- `--retry-attempts=<n>`
- `--retry-on=timeout|quorum|inconsistent|never`
- `--retry-resend=all|missing`
- `--retry-total-timeout-ms=<n>`
- `--retry-refresh-dns`

Defaults in the current tree:

- `attempt_timeout_ms = 500`
- `retry_attempts = 0`
- `retry_on = never`
- `retry_resend = all`
- `retry_total_timeout_ms = 0`
- `retry_refresh_dns_on_retry = false`

The perf client now prints the active attempt timeout and retry policy at
startup so runs are self-describing.

### Effective retry backoff behavior

Retry backoff is now hard-wired to immediate resend:

- `policy.retry.backoff.kind = R_BACKOFF_NONE`
- `policy.retry.backoff.delay_ms = 0`
- `policy.retry.backoff.base_delay_ms = 0`
- `policy.retry.backoff.max_delay_ms = 0`
- `policy.retry.backoff.jitter_ms = 0`

The perf client no longer exposes any CLI flags for backoff, fixed delay,
exponential delay, max delay, or jitter.

## Retry Execution Semantics

`src/r_client.c` changed the retry path itself.

### Previous behavior on `main`

Retries could be scheduled for the future:

- Request state tracked `pending_retry` and `retry_at_ms`.
- `r_backoff_delay_ms()` computed delay from the retry backoff policy.
- `r_client_request_deadline_ms()` returned either the retry wakeup time or the
  attempt deadline.
- `r_client_on_timeout()` could park a request in pending-retry state and wait
  until a later timer tick to start the next attempt.
- While pending retry, `r_client_on_datagram()` ignored datagrams for that
  request.

### Current behavior

Retries are immediate:

- `pending_retry` and `retry_at_ms` were removed from request state.
- `r_backoff_delay_ms()` was removed.
- `r_client_request_deadline_ms()` now always returns `attempt_deadline_ms`.
- `r_client_on_timeout()` now calls `r_request_retry_now(...)` immediately when
  a retry condition is met.
- `r_request_retry_now(...)` increments the attempt counter and starts the next
  attempt immediately unless `total_deadline_ms` has already been reached.
- `r_client_on_datagram()` no longer has a pending-retry short-circuit.

Current retry triggers:

- `retry_on = timeout`: retry only when no candidate result exists by the
  attempt deadline.
- `retry_on = quorum`: retry when quorum was required, not met, and configured
  as hard quorum.
- `retry_on = inconsistent`: retry immediately when a candidate exists but the
  response set is inconsistent.
- `retry_on = never`: do not retry.

Current retry limits:

- Retries still stop when `req->attempt + 1 >= req->total_attempts`.
- `retry.total_timeout_ms`, when non-zero, still caps the total request
  lifetime.
- `retry.refresh_dns_on_retry` and the existing DNS resync policy hooks still
  run before an immediate retry starts.

## Documentation

`README.md` was updated to match the current perf client interface:

- Added examples using `--attempt-timeout-ms` and retry flags.
- Added a retry-flags section.
- Removed backoff/jitter flag documentation because those flags no longer
  exist in the current tree.

## Verification Performed

The current tree was validated with:

- `make perf_client`
- `bin/perf_client --help`
- `make test`
