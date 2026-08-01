# Public API

This document describes the public C API. The library is still at its first
MVP: the public API and private implementation may change without backward-
compatibility adapters.

## Operation Model

The core client exposes two independent operations:

1. A **resource request** contains at least one `r_resource_request_t` resource
   consumption and may contain any number of `r_latency_guard_t` latency
   guards. Ratelimitly evaluates the complete request atomically. A grant
   consumes every requested quantity; a rejection consumes none.
2. A **latency report** contains at least one
   `r_service_latency_report_t` service observation and contributes it to the
   corresponding server-side latency tracker.

Neither operation requires the other. A client may issue only resource
requests, only latency reports, or both. The optional workflow API demonstrates
one useful application policy that combines one resource consumption, one
latency guard, admitted work, and a subsequent latency report; that policy is
not part of the wire-protocol contract.

This operation model defines application semantics. The delivery mechanics of
latency reports and the discovery, fan-out, replay, and response-selection
policy for resource requests are documented with their corresponding APIs
below.

## Choosing an integration layer

Applications embed Ratelimitly in very different environments. A proxy module
normally already owns its sockets, DNS resolver, timers, and logging, while a
small command-line program may prefer a ready-made socket runtime.

The C client exposes its operations through three integration layers. In every
layer, the library owns packet encoding, authentication, request policy,
response parsing, and server selection. The layer determines how much
surrounding workflow and I/O the library also owns. Choose the ownership
boundary that fits the host application.

| Layer | What it owns | What the host still owns |
| --- | --- | --- |
| Core client (`r_client.h` + `r_client_io.h`) | Packets, authentication, request policy, deadlines, and response selection | UDP I/O, DNS callbacks, timers, and logging |
| Optional admission workflow (`r_client_workflow.h`) | One convenience policy combining one resource consumption, one latency guard, and at most one subsequent report | The core client's I/O plus protected-work timing and lifetime |
| Public runtime (`r_client_runtime.h`) | Core client, nonblocking IPv4/IPv6 UDP sockets, and synchronous DNS discovery | Readiness watchers, deadline callbacks, application work, and logging policy |

The normal asynchronous request API copies request inputs. The borrowed API
avoids those copies, so its input buffers must remain valid until callback or
cancellation. In both forms, copy any result data needed after the completion
callback returns; the request handle and result arrays expire with that
callback.

Proxy modules and other high-throughput embedders commonly choose the core and
borrowed APIs. The examples choose the workflow and public runtime so each
framework README can focus on its readiness, timer, and shutdown rules.

## Headers

Core embedders use:

```c
#include "r_client.h"
#include "r_client_io.h"
```

`r_client.h` includes `r_client_io.h`, so most integrations only need
`r_client.h`.

Applications that want the repository's combined admission lifecycle and
portable socket runtime can also use:

```c
#include "r_client_workflow.h"
#include "r_client_runtime.h"
```

`r_client_workflow.h` combines one resource consumption and one latency guard
into a durable application decision. `r_client_runtime.h` adds nonblocking UDP
sockets, synchronous SRV/A/AAAA resolution, portable clocks, and helpers for
examples or small command-line programs. A server with its own asynchronous
resolver and socket ownership should use the core I/O interfaces instead.

## Configuration

Create one `r_client_t` per API-key/event-loop context. The encoded key supplies
the tenant key ID, authentication type, and quota values. With no DNS override,
the client discovers `_ratelimitly._udp.c-<key-id>.p0.ratelimitly.com`:

```c
r_client_config_t cfg = {0};
cfg.tenant.auth.secret = auth_key;

r_request_policy_t policy;
r_client_default_request_policy(&policy);
policy.unit_ms = 20;
cfg.request_policy = &policy;

r_client_t *client = NULL;
int rc = r_client_create(&cfg, &io_ops, &resolver_ops, &client);
```

Set `cfg.tenant.dns_name` to override production discovery for custom,
development, or staging DNS. An override zone must follow the SRV
target-naming contract — each SRV target hostname's first label encodes the
server ID as `s-<decimal>` (see IO_ABSTRACTION.md, DNS) — or discovery
silently yields no usable servers. A nonzero `cfg.tenant.key_id` or
`cfg.tenant.auth.type` acts as an assertion and must match the encoded key.
Hosts that need the default name before `r_client_create`, such as a shared DNS
cache, can call `r_client_format_default_tenant_dns()` with the parsed key ID.

`cfg.tenant.auth.secret` is the encoded Bech32 credential string itself
(`rl-cookie...` or `rl-aes...`), not raw binary key material.
`cfg.tenant.auth.secret_len` is the byte length of that encoded text string;
leave it as `0` for ordinary null-terminated C strings. The supported Bech32
credential alphabet is printable ASCII and does not carry embedded NUL bytes.
The client validates and decodes the credential internally before copying the
raw 32-byte cookie/AES material into private client state.

The client copies an explicit `tenant.dns_name` and the auth secret string
during `r_client_create`, so those configuration strings only need to remain
valid for the duration of the call.

## Credentials

`r_client_parse_auth_key` validates an API key credential and returns:

- `type`: one of `R_AUTH_COOKIE`, `R_AUTH_AES_GCM`
- `key_id`: identifier embedded in the key
- `secret`: raw cookie/AES material for authenticated keys
- `secret_len`: `32`
- five quota fields describing tenant limits encoded in the key

Quota fields and their enforcement points differ per field:

| Quota field | Meaning | Enforcement |
| --- | --- | --- |
| `latency_buffer_size_max` | Largest `buffer_size` a guard or report may request. | Client-enforced: a resource request containing an over-quota guard fails at submit with `RCLIENT_ERR_PROTOCOL`; over-quota latency reports are silently filtered before send. |
| `dedup_ttl_ms_max` | Largest deduplication TTL the key may request. | Client-enforced: request creation fails with `RCLIENT_ERR_CONFIG` when the policy's derived TTL exceeds it (see Resource-Request HA Policy). |
| `rate_buckets_max` | Maximum distinct buckets the tenant may use. | Server-enforced; the client does not check it. Overruns surface as normal request rejections. |
| `latency_services_max` | Maximum distinct latency trackers. | Server-enforced; not checked client-side. |
| `metrics_labels_max` | Maximum distinct metrics labels. | Server-enforced. On overflow the server does not fail the request; it rewrites the label to the fixed label `overflow`. |

The raw secret is sensitive. Use it only for validation or diagnostics that do
not expose secret bytes. The library never cleanses the caller-owned
`r_auth_key_info_t`; zero it (for example with `OPENSSL_cleanse`) when the
inspection is done.

Do not pass `r_auth_key_info_t.secret` back into `cfg.tenant.auth.secret`.
Configuration expects the encoded Bech32 string. The decoded `secret` field is
provided only for callers that need to inspect or validate the credential.

## Content-defined IDs

The 16-byte ID on the wire identifies stored server state, so it must include
both the application name and every setting that defines that state.
`r_client_derive_bucket_id()` derives a bucket ID from:

- the exact bucket-name bytes and byte length;
- `window_size_ms`; and
- `rate_limit`.

`r_client_derive_latency_tracker_id()` derives a latency-tracker ID from:

- the exact tracker-name bytes and byte length;
- `ttl_ms`;
- `max_samples`;
- `buffer_size` exactly as it will be sent on the wire (the client never
  rewrites the value; over-quota values are rejected or filtered); and
- `min_sample_threshold`.

The guard's `threshold_ms` is excluded because different guards may evaluate
the same tracker against different thresholds. A report's `observed_latency`
is a sample and is also excluded.

Both helpers accept names as an explicit pointer and length, including names
with embedded NUL bytes. They return `RCLIENT_OK` on success and
`RCLIENT_ERR_CONFIG` for invalid arguments or a derivation failure. Call them
again whenever an identity-defining setting changes; reusing an ID with
different settings corrupts the identity contract for the stored server state
that the ID names, and the server is not guaranteed to detect or reject the
mismatch.

The derivation is deterministic and stable across processes, client instances,
and client implementations: it is BLAKE2s-256 truncated to the first 16 bytes,
computed over a fixed domain string (`ratelimitly.resource.v1` or
`ratelimitly.latency-tracker.v1`, including its terminating NUL), the
little-endian 32-bit name length, the exact name bytes, and each defining
setting as a little-endian 32-bit value, in the argument order shown above.
Any other producer following this recipe — another client implementation,
server-side tooling, a metrics pipeline — derives the same 16-byte ID.
Known-answer vectors live in `tests/test_public_api.c`.

The workflow API derives both IDs automatically. Low-level callers derive the
ID after filling the defining fields:

```c
r_resource_request_t resource = {
    .window_size_ms = 1000,
    .rate_limit = 100,
    .tokens_requested = 1,
};
int rc = r_client_derive_bucket_id(
    "checkout",                  /* exact bucket-name bytes */
    strlen("checkout"),          /* bucket-name byte length */
    resource.window_size_ms,     /* bucket window */
    resource.rate_limit,         /* bucket rate */
    resource.bucket_id           /* resulting 16-byte ID */
);
```

## Resource Requests

Use `r_client_check_rate_limit_async` when the client should copy request
buffers. Use `r_client_check_rate_limit_async_borrowed` when the caller keeps
the buffers alive until the callback fires.

Required input:

- at least one `r_resource_request_t`
- optional `r_latency_guard_t` array
- optional metrics label string
- callback

Each `r_resource_request_t` declares its own rate model: the request asks for
`tokens_requested` tokens from the bucket, and the bucket's capacity is the
request-declared `rate_limit` tokens per `window_size_ms` window. The limit and
window are client-declared per request, not server configuration; the server
tracks consumption per bucket ID. The window's exact accounting model
(continuous refill versus discrete window boundaries) is server-defined and not
part of this client contract — treat `rate_limit` per `window_size_ms` as the
sustained admission rate.

The metrics label tags the request for per-label server-side metrics. Labels
are counted against the credential's `metrics_labels_max` cardinality quota; on
overflow the server rewrites the label to the fixed label `overflow` instead of
rejecting the request. Pass an empty or NULL label to send none.
`metrics_label_len` of `0` means the label is null-terminated.

Capacity limits: the whole request must fit one 1200-byte datagram. With no
guards and no label that allows at most 39 resources; guards reduce the budget
(at most 27 guards with no resources). Oversized requests fail at submit with
`RCLIENT_ERR_PROTOCOL`.

Submit errors and callback errors are separate channels. When the submit
function returns a nonzero status, the request was never created: the callback
will not fire and `*out_req` is not set. `RCLIENT_ERR_CONFIG` (bad arguments or
an invalid policy), `RCLIENT_ERR_DNS` (no servers known yet), `RCLIENT_ERR_NOMEM`,
`RCLIENT_ERR_PROTOCOL` (the request does not fit one datagram, or a guard
exceeds the credential's buffer-size quota), and first-transmission
`RCLIENT_ERR_IO` all surface this way. A first-transmission `udp_send` failure
aborts the submit after some packets may already have left, so a failed submit
is not proof that no server received the request; resubmitting creates a new
request identity.

The callback receives:

- `RCLIENT_OK` and a non-null `r_rate_limit_result_t` on a parsed response
- `RCLIENT_ERR_TIMEOUT` when no acceptable response arrived in time — including
  every case where servers dropped the request silently (see Error Codes)
- `RCLIENT_ERR_IO` (or, rarely, `RCLIENT_ERR_AUTH`) when a later replay round
  failed to transmit

`RCLIENT_ERR_DNS` is never delivered through the callback; it is only a
synchronous submit result.

`result->success` combines resource and latency-guard decisions: it is true
only when every resource has zero token deficit and every latency guard passes.
Inspect `result->resources` and `result->guards` when an application needs to
distinguish rate denial from latency load shedding.

Result fields:

| Field | Meaning |
| --- | --- |
| `resources[i].bucket_id` | The bucket this entry describes. Match entries by ID, not by array index: the arrays mirror the server response, whose count and order may differ from the submitted request. |
| `resources[i].tokens_deficit` | `0` means the requested tokens were granted. Nonzero is the shortfall: how many of the requested tokens the bucket could not supply. Any nonzero deficit rejects the whole request and consumes nothing. |
| `resources[i].actual_rate` | The bucket's current consumed-token count in its window, as reported by the responding server. |
| `guards[i].latency_tracker_id` | The tracker this entry describes; match by ID. |
| `guards[i].current_latency_ms` | The tracker's current latency estimate on the responding server. |
| `guards[i].passed` | True when `current_latency_ms` is below the guard's threshold. |
| `server_id` | The 64-bit ID of the server whose response was selected. Servers place their own ID in the response header. Its upper bits encode the server's start time (see Resource-Request HA Policy). |
| `steering_feedback` | Wire keep-port flag: `true` means keep the current UDP source port; `false` means the server requested a source-port rebind. See IO_ABSTRACTION.md Steering Feedback. Despite the name, `true` requires no action. |

Pointers inside `r_rate_limit_result_t` are valid only during the callback.
The `r_client_req_t *` passed to the callback is owned by the client and is not
valid after the callback returns. Calling `r_client_cancel_request` on that same
request from inside the completion callback is harmless and treated as a no-op.

## Threading and reentrancy

The client contains no locks. Confine each `r_client_t` — and each
`r_runtime_client_t` — to one thread or event loop; every call on the same
client must be serialized. Create one client per worker for multi-threaded
servers.

From inside a completion callback it is safe to submit new requests, send
latency reports, and cancel requests (including the completing request itself,
which is a no-op). Do **not** call `r_client_destroy` from inside a callback:
the client dereferences internal state after the callback returns, so
destroying it there is undefined behavior. Defer destruction until the stack
has unwound to the event loop.

## Latency Guards and Independent Reports

Latency guards and reports are independent operations connected through a
server-side tracker identified by `latency_tracker_id`. A guard reads the
tracker's recent latency during a resource request. A report adds a measured
observation to that tracker.

One common application-level feedback loop is:

1. Create a guard for a stable latency tracker and include it in a resource request.
2. Copy its result during the request callback.
3. Perform protected work only when the combined result passes.
4. Measure that work with a monotonic clock.
5. Report the observation using the same tracker ID and settings.

This sequence is optional. A dedicated observer may send reports without ever
issuing a resource request, and a resource-requesting client need not send
reports.

When following the combined sequence, never fabricate a latency observation
for work that a guard prevented from running. No such operation occurred, so a
zero or synthetic observation would bias the tracker. This does not prohibit
reporting measurements for other services or independently completed work.

### Guard configuration

Derive the ID after filling every setting that defines the tracker:

```c
r_latency_guard_t guard = {
    .threshold_ms = 100,
    .ttl_ms = 10000,
    .max_samples = 100,
    .buffer_size = 32,
    .min_sample_threshold = 5,
};
int rc = r_client_derive_latency_tracker_id(
    "inventory-backend",                 /* exact tracker-name bytes */
    strlen("inventory-backend"),         /* tracker-name byte length */
    guard.ttl_ms,                         /* sample lifetime */
    guard.max_samples,                    /* samples considered */
    guard.buffer_size,                    /* final effective storage */
    guard.min_sample_threshold,           /* warm-up sample count */
    guard.latency_tracker_id              /* resulting 16-byte ID */
);
```

| Field | Contract |
| --- | --- |
| `latency_tracker_id` | Canonical 16-byte ID identifying this tracker definition. |
| `threshold_ms` | Guard fails when tracked latency is greater than or equal to this value. |
| `ttl_ms` | Maximum sample lifetime for this tracker. |
| `max_samples` | Maximum number of samples considered by the tracker. |
| `buffer_size` | Requested tracker storage; must not exceed the credential quota. |
| `min_sample_threshold` | Samples required before tracked latency controls admission. |

Pass `&guard` and guard count `1` to either rate-request function. Borrowed
requests must keep guard and resource storage alive through callback or
cancellation.

`r_guard_result_t` returns `threshold_ms`, `current_latency_ms`, and `passed`.
Guard and resource result arrays remain valid only during callback, so copy any
values needed by later work.

### Reporting measured work

Measure the protected operation rather than the Ratelimitly request. Wall-clock
adjustments must not change duration, so use `CLOCK_MONOTONIC` or the host
event loop's monotonic duration clock.

When a report is intended to update the tracker read by a particular guard, it
must repeat that guard's `latency_tracker_id`, `ttl_ms`, `max_samples`,
`buffer_size`, and `min_sample_threshold`. `threshold_ms` appears only in the
guard because it controls admission, not sample storage. A report that is not
paired with a local guard still derives its canonical tracker ID from its own
tracker name and those same tracker-definition fields.

```c
r_service_latency_report_t report = {
    .observed_latency = elapsed_ms,
    .ttl_ms = guard.ttl_ms,
    .max_samples = guard.max_samples,
    .buffer_size = guard.buffer_size,
    .min_sample_threshold = guard.min_sample_threshold,
};
memcpy(report.latency_tracker_id, guard.latency_tracker_id, sizeof(report.latency_tracker_id));

int rc = r_client_report_latency(client, &report, 1);
if (rc != RCLIENT_OK) {
    /* Log telemetry failure; do not rewrite an HTTP response already sent. */
}
```

The delivery contract for `r_client_report_latency` is fire-and-forget. Unlike
a resource request, a report does not create a request handle, wait for a
response, or require a deadline watcher. Report storage is borrowed only for
the duration of the call because the packet is serialized synchronously.

Reports whose `buffer_size` exceeds the credential quota are filtered. If all
reports are filtered, the function returns `RCLIENT_OK` without sending. Other
failures include `RCLIENT_ERR_DNS` when no server is available and
`RCLIENT_ERR_IO` when the UDP send hook fails.

Each call broadcasts one datagram — carrying every surviving report, at most
30 (cookie mode) or 31 (AES mode) per call — to every known server, under a
fresh request identity per call. The send loop stops at the first failing
`udp_send`, so `RCLIENT_ERR_IO` can mean partial delivery across servers.

See the self-contained
[`examples/latency_tracker/`](../examples/latency_tracker/) folder for complete
guard-pass, protected-work, report, and guard-deny control flow.

## Combined Admission Workflow

`r_client_workflow.h` packages one optional application policy:
one resource request, one latency guard, an explicit denial reason, and at most
one latency report after admitted work.

1. Start from `r_client_admission_config_defaults()` and replace the bucket,
   latency-tracker, and metrics names with stable application identifiers.
2. Keep one caller-owned `r_admission_request_t` alive from
   `r_client_admission_start()` through callback or cancellation.
3. Drive its deadline with `r_client_admission_deadline_ms()` and
   `r_client_admission_on_timeout()`, or the relative runtime helpers.
4. Inspect the copied `r_admission_outcome_t`; it distinguishes resource,
   latency, combined, and transport/protocol failures.
5. Only after `outcome.allowed`, run protected work and call
   `r_client_admission_report_latency()` once.

`r_runtime_admission_run_and_report()` performs the last step for synchronous
protected work. It measures with the runtime's monotonic clock and never reports
denied, cancelled, failed, or previously reported work. HTTP integrations whose
protected operation is asynchronous should record a monotonic start time and
report from their own completion callback instead. Call it at most once per
admission: a second call re-executes the protected work before the report step
fails. `r_client_admission_cancel` suppresses the completion callback, matching
core-client cancel semantics.

`r_client_admission_config_defaults()` fills: window 1000 ms, rate limit 100,
1 token, guard threshold 100 ms, tracker `ttl_ms` 10000, `max_samples` 100,
`buffer_size` 32, `min_sample_threshold` 5, metrics label
`rl-c-client-example`. Replace the names and the label with stable application
identifiers before production use.

## Public runtime

`r_client_runtime.h` wraps the core client with owned, nonblocking IPv4/IPv6
UDP sockets and synchronous DNS discovery, for programs that do not bring an
event loop of their own. Contracts that differ from the core client:

- Configuration comes from the environment via `r_runtime_options_from_env()`:
  `RATELIMITLY_AUTH_KEY` is required; optional `RATELIMITLY_TENANT` overrides
  the key-derived production DNS name (the SRV target-naming contract in
  IO_ABSTRACTION.md applies to the override zone); the optional
  `RATELIMITLY_EXAMPLE_SERVER_HOST`/`RATELIMITLY_EXAMPLE_SERVER_PORT` pair
  selects one explicit development endpoint — set both or neither. The fixed
  endpoint is given the synthetic identity `s-1.ratelimitly-example.invalid`
  (server ID 1), so it only works against a responder that claims server ID 1,
  such as the bundled test responder's default.
- DNS resolution is **synchronous and blocking** on the calling thread, during
  initialization, periodic refresh, and any submit that finds zero servers.
  Discovery keeps at most 32 SRV records per refresh. Servers with their own
  asynchronous resolver should use the core I/O interfaces instead.
- `r_runtime_client_on_readable()` drains one ready socket and stops early,
  returning the status, when a datagram produces a non-`RCLIENT_OK` ingress
  result. Those statuses are the same informational per-datagram errors
  described under Datagrams and Timers: log them and keep the loop running —
  never treat them as fatal, or hostile junk datagrams can stop the
  integration. Remaining datagrams are delivered on the next readiness event.
- The runtime installs **no steering-feedback hook**: server source-port
  rebind requests are ignored at this layer. Integrations that need steering
  must use the core client.
- Runtime-owned socket handles remain valid until
  `r_runtime_client_destroy()`. On Windows the runtime performs
  `WSAStartup`/`WSACleanup` as part of init/destroy; hosts managing their own
  Winsock lifetime should account for the reference counts.
- Threading follows the core rule: one runtime per thread or loop, calls
  serialized.

## Datagrams and Timers

The host owns network receive and timers:

- call `r_client_on_datagram` for UDP packets received on the client socket
- call `r_client_request_deadline_ms` after submitting a request and again
  after every nonterminal datagram or timeout event
- call `r_client_on_timeout` when the host timer fires
- call `r_client_cancel_request` if the HTTP/request context is abandoned

**Completion is signaled solely by the completion callback firing during a
`r_client_on_datagram` or `r_client_on_timeout` call.** Both functions return
`RCLIENT_OK` on paths that complete — and free — the request, so their return
values carry no liveness information. Track a per-request flag set by the
callback, and re-check it after every datagram and timeout delivery: if the
callback fired, the `r_client_req_t *` is already invalid and must not be
passed to `r_client_request_deadline_ms`, `r_client_on_timeout`, or
`r_client_cancel_request` again. If the callback did not fire, the event was
nonterminal — re-query the deadline and re-arm the timer. Calling
`r_client_on_timeout` before the deadline is a safe no-op.

Per-datagram return values from `r_client_on_datagram` are informational and
expected under normal operation on an open UDP port: `RCLIENT_ERR_PROTOCOL`
for malformed datagrams or AES responses that fail tag verification,
`RCLIENT_ERR_AUTH` for a wrong auth TLV type or cookie mismatch, and
`RCLIENT_OK` for valid-looking datagrams that match no in-flight request
(late, duplicate, or filtered responses are ignored silently). Any off-path
sender can produce the error returns, so treat them as counters to log —
never as fatal conditions. The affected request always remains in flight.
The datagram source address is not part of response acceptance; responses are
matched by authenticated request ID and server ID.

`r_io_ops_t.now_ms`, `r_client_request_deadline_ms`, and the `now_ms` passed to
`r_client_on_timeout` use the same Unix-epoch millisecond clock domain. This is
separate from the monotonic duration clock used to measure protected-work
latency.

A valid non-oldest response can move the next deadline earlier, from the replay
deadline to that round's preference deadline. Hosts must therefore re-arm from
the value returned after processing that datagram rather than retaining the
previous timer.

AES response replay handling is tied to the request lifecycle. The authenticated
`unique_id` in the authenticated packet header must match an in-flight request.
Once that request completes, times out, or is canceled, later datagrams with
the same `unique_id` are ignored. The authenticated timestamp is retained as
protocol framing, but the client does not apply a separate clock-skew freshness
check. Keep request deadlines short and deliver timeout/cancel events promptly.

## Resource-Request HA Policy

An API key may resolve through DNS to one or more r-servers. This membership is
a resource-request delivery concern; it does not change the meaning of the
logical request or its atomic grant/rejection result.

`r_request_policy_t` configures the client's sole resource-request strategy.
It owns fan-out, oldest-first response selection, replay scheduling, completion
delivery, and deduplication-TTL derivation.

Its parameters are:

| Field | Meaning |
| --- | --- |
| `unit_ms` | Base time unit `U`, frozen for one request. |
| `replay_count` | Number of replays `N` after the initial transmission. |
| `replay_gap` | Round-duration schedule `B(k)`, in units of `U`. |
| `preference` | Oldest-server preference schedule `P(k)`, in units of `U`. |
| `final_receive_units` | Final receive-only duration `F`. |
| `final_preference_units` | Oldest preference within the final interval. Zero makes its first valid response immediate. |
| `completion_delivery` | Before returning a selected allow or deny, fire-and-forget the same request to servers still missing a valid response. |

Always initialize the policy with `r_client_default_request_policy()` before
overriding individual fields.

Both schedules use `r_ha_schedule_t`. A fixed schedule always uses
`initial_units`; a linear schedule adds `growth.linear_step_units` per round;
and an exponential schedule multiplies by
`growth.exponential_factor` per round. Growth beyond `max_units` is capped at
`max_units` — but `initial_units` itself must not exceed `max_units`; that is a
validity error, not a cap. The full validity rules, all enforced with
`RCLIENT_ERR_CONFIG` at request submission (not at `r_client_create`):

- `unit_ms > 0`, and `replay_gap.initial_units > 0` (a preference schedule may
  start at zero);
- `initial_units <= max_units` for both schedules;
- a linear schedule needs `growth.linear_step_units >= 1`;
- an exponential schedule needs `growth.exponential_factor >= 2`;
- `P(k) <= B(k)` for every transmission round;
- `final_preference_units <= final_receive_units`;
- `replay_count <= R_CLIENT_HA_MAX_REPLAY_COUNT` (65535); the credential TTL
  normally imposes a much smaller practical bound.

Because the defaults set `max_units = 1`, raising `replay_gap.initial_units`
alone fails validation — raise `max_units` together with it.

For transmission rounds `0..N`, the complete horizon is:

```text
H = U * (sum(B(k), k = 0..N) + F)
```

The strategy uses `H` as its wire deduplication TTL: the request asks servers
to treat any duplicate of this request (same request identity) received within
`H` as already answered, replaying the cached response instead of processing it
again. This server-side at-most-once window is what makes replay rounds and
completion delivery safe — every transmission of one logical request carries
the same identity. The guarantee is conditional: while a server's deduplication
subsystem is degraded, duplicate suppression may lapse and a replayed request
can be processed twice. Integrations for which double consumption is costly
should treat replays (`replay_count > 0`) as a throughput/consistency
trade-off.

Request creation fails
with `RCLIENT_ERR_CONFIG` if any schedule is invalid, arithmetic overflows,
`H` cannot be represented by the wire field, or `H` exceeds the API key's
`dedup_ttl_ms_max`. All deadlines are absolute, and the client never initiates
a replay or completion-delivery send at or after the deduplication deadline.

"Oldest server" — the preference relation used throughout this policy — is
decided by decoded server start time: a server's 64-bit ID encodes its start
time in its upper bits (`start_seconds_since_2025 = server_id >> 23`, epoch
2025-01-01 00:00:00 UTC), and lower start time wins, with ties broken by the
numerically lower full ID. Server IDs come from DNS: each SRV target
hostname's first label must encode the ID as `s-<decimal>` (see
IO_ABSTRACTION.md, DNS). When discovery produced no server IDs — the
address-fallback path — oldest-preference and response filtering are disabled
and the first valid response wins.

At round zero the client sends to the immutable request membership snapshot.
A valid response from the oldest server completes immediately. Other valid
responses update the oldest candidate and wait only until that round's
preference deadline. If the preference deadline already passed, the response
completes immediately. A response-free replay deadline starts the next round
and sends only to servers still missing a valid response. After the last
transmission round, the final receive-only interval sends nothing.

Completion delivery is outcome-independent and best effort. It runs before
every successful selection path—including an immediate oldest response,
preference-deadline fallback, and final-phase response—and never changes or
delays the selected result.

The default configuration is:

```text
U = 20 ms
N = 1
B(0) = B(1) = 1
P(0) = P(1) = 1
F = 1
P_final = 0
completion_delivery = true
TTL = 3 * U = 60 ms
```

`cfg.request_policy` is borrowed only for the duration of `r_client_create`; the
client copies the policy by value and does not retain the caller's pointer.

## DNS Refresh

DNS refresh pacing is client configuration rather than request-selection
policy. Set `cfg.dns_refresh.refresh_interval_ms`,
`cfg.dns_refresh.forced_refresh_min_interval_ms`, or
`cfg.dns_refresh.forced_refresh_jitter_ms` before `r_client_create`. Zero
selects the defaults of 300 seconds for periodic refresh and one
second for the minimum forced-refresh interval; jitter defaults to zero and,
when set, adds a random extra delay on top of the minimum forced-refresh
interval. SRV record TTLs, when provided, cap the periodic interval lower.

Refresh is driven by client activity, not by a background timer: submitting a
request or report checks the interval, and a submit or report that finds zero
known servers both returns `RCLIENT_ERR_DNS` synchronously and forces a
refresh attempt. `r_client_create` starts the first discovery but never fails
because of it — with an asynchronous resolver, early submits fail with
`RCLIENT_ERR_DNS` until the first discovery completes, then self-heal; treat
that warm-up as expected. Failed refreshes do not arm the pacing throttle, so
retrying submits during an outage re-attempts resolution each time. With the
public runtime's synchronous resolver, each such failing submit performs
blocking DNS on the calling thread.

## Error Codes

All errors are negative:

| Code | Where it surfaces | Causes |
| --- | --- | --- |
| `RCLIENT_ERR_TIMEOUT` | Completion callback | No acceptable response before the deadline. This is also the **only** signature of every server-side rejection that produces no reply: wrong or unregistered credentials, a wrong key ID, a stale request timestamp from a skewed clock, and tampered responses the client discarded. Servers deliberately drop unauthenticated traffic without responding. |
| `RCLIENT_ERR_DNS` | Submit return only | No servers currently known (startup warm-up, DNS outage, or an over-long tenant DNS name). Never delivered through the callback. |
| `RCLIENT_ERR_IO` | Submit return (first transmission) or callback (replay rounds) | The host `udp_send` hook returned failure. |
| `RCLIENT_ERR_PROTOCOL` | Submit return, or `r_client_on_datagram` return | At submit: the request does not fit one 1200-byte datagram, or a guard's `buffer_size` exceeds the credential quota. From `on_datagram`: a malformed datagram or an AES response that failed tag verification (informational; the request stays in flight). |
| `RCLIENT_ERR_AUTH` | `r_client_on_datagram` return, or submit/callback on local crypto failure | A response with the wrong auth TLV type or a cookie mismatch (informational), or a local encryption failure. A *server-side* authentication failure never surfaces as `RCLIENT_ERR_AUTH` — it surfaces as `RCLIENT_ERR_TIMEOUT` (see above). |
| `RCLIENT_ERR_CONFIG` | Submit / create / helper returns | Invalid arguments, invalid API key credentials, mismatched key-ID/type assertions, and invalid HA-policy schedules (including a derived TTL above `dedup_ttl_ms_max`). |
| `RCLIENT_ERR_NOMEM` | Submit / create returns | Allocation failure. |

Debugging rule of thumb: 100% timeouts with working networking and DNS means
suspect the credential, the key ID, or the host clock — not packet loss.

## API boundary

The public surface consists of `include/r_client.h`, `include/r_client_io.h`,
`include/r_client_workflow.h`, `include/r_client_runtime.h`, and the support
header `include/r_client_export.h` (the `RCLIENT_API` export macro, included
by the others; Windows consumers of the shared `rclient.dll` outside CMake
must define `RCLIENT_SHARED` to get dllimport linkage). Internal
protocol builders, crypto helpers, and packet parsers are implementation
details. Until the project declares a stable post-MVP API, releases may remove
or replace public declarations directly instead of carrying aliases or legacy
execution paths.

Related documents: [SECURITY.md](../SECURITY.md) for credential handling and
the response replay model, [IO_ABSTRACTION.md](../IO_ABSTRACTION.md) for the
host contract, [EMBEDDING.md](../EMBEDDING.md) for source integration, and
[CHANGES.md](../CHANGES.md) for release history.
