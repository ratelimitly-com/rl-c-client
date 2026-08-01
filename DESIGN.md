# Design Notes

`rl-c-client` is the C integration library for Ratelimitly clients. It is built
for event-loop based systems where the host process already owns sockets,
timers, DNS, and memory lifetimes.

## Goals

- Keep the public API small and coherent while the MVP evolves.
- Avoid blocking calls and internal threads in the core client. (The optional
  public runtime layer deliberately trades this for convenience: it owns UDP
  sockets and performs synchronous DNS.)
- Support proxies and other event-loop embedders.
- Keep packet encoding, credential handling, authentication, response parsing,
  replay policy, and server selection inside the library.
- Keep transport, resolver, timer, and logging ownership in the host.

## Public Boundary

The supported public headers are:

- `include/r_client.h`
- `include/r_client_io.h`
- `include/r_client_workflow.h` (optional admission workflow)
- `include/r_client_runtime.h` (optional public runtime)
- `include/r_client_export.h` (export macro, included by the others)

Files under `src/` are private. They may be used by tests inside this repo, but
external consumers must not include them.

The credential parser is intentionally public because embedders often need to
validate credentials at configuration load time. Low-level crypto and packet
helpers remain private.

## Authentication

API key credentials are Bech32 strings:

- `rl-cookie...`
- `rl-aes...`

The parser validates the credential shape, extracts the key id, exposes quota
values, and returns raw secret bytes for cookie/AES keys. The client also parses
the configured credential during `r_client_create` and rejects mismatches
between configured auth type, key id, and credential contents.

## DNS and Routing

The client asks the host resolver for SRV records under
`_ratelimitly._udp.<configured-dns-name>` (or the key-derived default name),
then asks for A/AAAA addresses for each SRV target whose first label encodes a
server ID as `s-<decimal>`; targets without that label are skipped (see
IO_ABSTRACTION.md, DNS). The host supplies DNS results; the client copies them
and decides which targets to use for each attempt.

## Request Lifecycle

Resource requests are asynchronous:

1. The caller submits one or more resource consumptions, optional latency
   guards, and an optional metrics label.
2. The client snapshots current targets and emits UDP sends.
3. The host schedules the request deadline.
4. Incoming datagrams are delivered to `r_client_on_datagram`.
5. Timeouts are delivered to `r_client_on_timeout`.
6. The client invokes the callback with either a selected result or an error.

The borrowed API is the preferred high-throughput path when the caller can keep
request buffers alive until completion.

The default request policy is one parameterized oldest-first HA strategy. A
request is one logical operation with one `unique_id`, one deduplication
window, and an immutable server snapshot. The first transmission goes to every
server. A valid response from the oldest trusted server completes immediately;
other valid responses wait only for the independently configured preference
deadline.

When a round remains response-free, the policy may replay to missing servers
after a fixed, linear, or exponential gap. After the replay budget it may enter
a final receive-only interval. The complete schedule derives the wire
deduplication TTL and must fit the API-key limit.

Before returning an allow or deny, optional completion delivery
fire-and-forgets the same logical request to servers still missing a valid
response. This improves eventual convergence without changing the selected
result.

There is one request-policy implementation and one public policy structure.
The MVP deliberately does not carry legacy wait, quorum, selection, retry, or
policy-dispatch branches.

## Latency Reports

Latency reports are independent, fire-and-forget operations. They reuse the
same credential authentication, discovery, and routing machinery but do not
wait for responses and need not correspond to any resource request. Reports
whose `buffer_size` exceeds the key's latency-buffer-size quota are filtered
before send; other quota dimensions are server-enforced.

## Documentation Boundary

If behavior is required to integrate with this library, document it in this
repository. Public readers should be able to build, configure, and embed the
client using only files in this repository.

Documentation should describe packet-related behavior at the C API level. The
packet encoder, parser, and crypto helpers remain implementation details unless
a behavior is exposed through one of the public headers listed under Public
Boundary above.
