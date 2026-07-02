# Design Notes

`rl-c-client` is the C integration library for Ratelimitly clients. It is built
for event-loop based systems where the host process already owns sockets,
timers, DNS, and memory lifetimes.

## Goals

- Keep the public API small and stable.
- Avoid blocking calls and internal threads.
- Support nginx and other event-loop embedders.
- Keep packet encoding, credential handling, authentication, response parsing,
  retry policy, and server selection inside the library.
- Keep transport, resolver, timer, and logging ownership in the host.

## Public Boundary

The supported public headers are:

- `include/r_client.h`
- `include/r_client_io.h`

Files under `src/` are private. They may be used by tests inside this repo, but
external consumers must not include them.

The credential parser is intentionally public because embedders such as nginx
need to validate tenant keys at configuration load time. Low-level crypto and
packet helpers remain private.

## Authentication

Tenant keys are Bech32 credentials:

- `rl-none...`
- `rl-cookie...`
- `rl-aes...`

The parser validates the credential shape, extracts the tenant key id, exposes
quota values, and returns raw secret bytes for cookie/AES keys. The client also
parses the configured credential during `r_client_create` and rejects mismatches
between configured auth type, tenant id, and credential contents.

## DNS and Routing

The client asks the host resolver for SRV records under
`_ratelimitly._udp.<tenant-dns-name>`, then asks for A/AAAA addresses for each
SRV target. The host supplies DNS results; the client copies them and decides
which targets to use for each attempt.

## Request Lifecycle

Rate-limit checks are asynchronous:

1. The caller submits resources, optional latency guards, and an optional
   metrics label.
2. The client snapshots current targets and emits UDP sends.
3. The host schedules the request deadline.
4. Incoming datagrams are delivered to `r_client_on_datagram`.
5. Timeouts are delivered to `r_client_on_timeout`.
6. The client invokes the callback with either a selected result or an error.

The borrowed API is the preferred high-throughput path when the caller can keep
request buffers alive until completion.

## Latency Reports

Latency reports are fire-and-forget. They reuse the same tenant auth and routing
machinery but do not wait for responses. Reports that exceed credential quotas
are filtered before send.

## Release Readiness

Before public release:

- choose and add the project license
- keep CI green on a clean checkout
- verify no docs require the private server repository
- verify embedders compile using only public headers
