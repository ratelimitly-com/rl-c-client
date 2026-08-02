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
development, or staging DNS. A nonzero `cfg.tenant.key_id` or
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
- quota fields used by the client for local input clamping/validation

The raw secret is sensitive. Use it only for validation or diagnostics that do
not expose secret bytes.

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
- the final effective `buffer_size`; and
- `min_sample_threshold`.

The guard's `threshold_ms` is excluded because different guards may evaluate
the same tracker against different thresholds. A report's `observed_latency`
is a sample and is also excluded.

Both helpers accept names as an explicit pointer and length, including names
with embedded NUL bytes. They return `RCLIENT_OK` on success and
`RCLIENT_ERR_CONFIG` for invalid arguments or a derivation failure. Call them
again whenever an identity-defining setting changes; reusing an ID with
different settings is a malformed protocol request.

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

The callback receives:

- `RCLIENT_OK` and a non-null `r_rate_limit_result_t` on a parsed response
- an error status such as `RCLIENT_ERR_TIMEOUT`, `RCLIENT_ERR_DNS`, or
  `RCLIENT_ERR_PROTOCOL` when no usable result is available

`result->success` combines resource and latency-guard decisions: it is true
only when every resource has zero token deficit and every latency guard passes.
Inspect `result->resources` and `result->guards` when an application needs to
distinguish rate denial from latency load shedding.

Pointers inside `r_rate_limit_result_t` are valid only during the callback.
The `r_client_req_t *` passed to the callback is owned by the client and is not
valid after the callback returns. Calling `r_client_cancel_request` on that same
request from inside the completion callback is harmless and treated as a no-op.

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

Measure the protected operation rather than the RateLimitly request. Wall-clock
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

All reports in a call are framed into one datagram. A batch too large to fit
returns `RCLIENT_ERR_PROTOCOL` and sends nothing, so split large batches across
calls; 30 reports per call fits under either auth mode.

Reports whose `buffer_size` exceeds the credential quota are filtered. If all
reports are filtered, the function returns `RCLIENT_OK` without sending. Other
failures include `RCLIENT_ERR_DNS` when no server is available and
`RCLIENT_ERR_IO` when the UDP send hook fails.

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
report from their own completion callback instead.

The portable runtime requires `RATELIMITLY_AUTH_KEY` through
`r_runtime_options_from_env()`. It derives the production tenant DNS name from
the key. Optional `RATELIMITLY_TENANT` overrides that default. The optional
`RATELIMITLY_EXAMPLE_SERVER_HOST`/`RATELIMITLY_EXAMPLE_SERVER_PORT` pair selects
an explicit development endpoint; set both or neither. Runtime-owned socket
handles remain valid until `r_runtime_client_destroy()`.

The example runtime can override the resource-request scheduler without
changing the library default:

- `RATELIMITLY_REQUEST_UNIT_MS` sets the base scheduling unit `U`;
- `RATELIMITLY_REQUEST_REPLAY_COUNT` sets the number of replays after the
  initial send; and
- `RATELIMITLY_REQUEST_PROFILE=1` emits one credential-free completion profile
  per request.

Either scheduler value may be supplied independently; the omitted value keeps
the normal default. The unit must be a nonzero decimal integer. The replay
count is a decimal integer from zero through `R_CLIENT_HA_MAX_REPLAY_COUNT`.
Invalid or out-of-range values make `r_runtime_options_from_env()` fail with
`RCLIENT_ERR_CONFIG`. The trusted-main production tests set `U=25 ms`, three
replays, and profiling, giving a 125 ms maximum admission wait while
release/default clients remain at `U=20 ms`, one replay, and 60 ms.

Applications using the core API can set `r_client_config_t.request_profile_cb`
instead. Its `r_request_profile_t` reports `wait_ms`, the completion round,
round or final-receive phase, status, and whether a response was selected.
`wait_ms` is measured in the client's scheduling clock from the start of the
initial send attempt through selection or failure. It intentionally excludes
discovery, protected work, latency reporting, and cleanup.

## Datagrams and Timers

The host owns network receive and timers:

- call `r_client_on_datagram` for UDP packets received on the client socket
- call `r_client_request_deadline_ms` after submitting a request and again
  after every nonterminal datagram or timeout event
- call `r_client_on_timeout` when the host timer fires
- call `r_client_cancel_request` if the HTTP/request context is abandoned

`r_io_ops_t.now_ms`, `r_client_request_deadline_ms`, and the `now_ms` passed to
`r_client_on_timeout` use the same Unix-epoch millisecond clock domain. This is
separate from the monotonic duration clock used to measure protected-work
latency.

Request-profile `wait_ms` uses this same scheduling clock. It describes how
much of the request policy was consumed; it is not the protected-operation
latency sample, which remains a separate monotonic measurement.

A valid non-oldest response can move the next deadline earlier, from the replay
deadline to the response-preference deadline. Hosts must therefore re-arm from
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
`growth.exponential_factor` per round. Every schedule is capped by
`max_units`. The policy requires positive replay gaps and
`P(k) <= B(k)` for every transmission round. `replay_count` must not exceed
`R_CLIENT_HA_MAX_REPLAY_COUNT`; the credential TTL normally imposes a much
smaller practical bound.

For transmission rounds `0..N`, the complete horizon is:

```text
H = U * (sum(B(k), k = 0..N) + F)
```

The strategy uses `H` as its wire deduplication TTL. Request creation fails
with `RCLIENT_ERR_CONFIG` if any schedule is invalid, arithmetic overflows,
`H` cannot be represented by the wire field, or `H` exceeds the API key's
`dedup_ttl_ms_max`. All deadlines are absolute, and the client never initiates
a replay or completion-delivery send at or after the deduplication deadline.

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
selects the documented defaults of 300 seconds for periodic refresh and one
second for the minimum forced-refresh interval; jitter defaults to zero.

## Error Codes

All errors are negative:

- `RCLIENT_ERR_IO`
- `RCLIENT_ERR_TIMEOUT`
- `RCLIENT_ERR_PROTOCOL`
- `RCLIENT_ERR_AUTH`
- `RCLIENT_ERR_DNS`
- `RCLIENT_ERR_CONFIG`
- `RCLIENT_ERR_NOMEM`

`RCLIENT_ERR_CONFIG` covers invalid arguments and invalid API key credentials.

## API boundary

The public surface consists of `include/r_client.h`, `include/r_client_io.h`,
`include/r_client_workflow.h`, and `include/r_client_runtime.h`. Internal
protocol builders, crypto helpers, and packet parsers are implementation
details. Until the project declares a stable post-MVP API, releases may remove
or replace public declarations directly instead of carrying aliases or legacy
execution paths.
