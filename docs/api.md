# Public API

This document describes the supported public C API. Headers under `src/` are
private and may change without compatibility guarantees.

## Headers

Use:

```c
#include "r_client.h"
#include "r_client_io.h"
```

`r_client.h` includes `r_client_io.h`, so most integrations only need
`r_client.h`.

## Configuration

Create one `r_client_t` per API-key/event-loop context. The public C structures
use `tenant` for the per-credential context because that context bundles the DNS
name, key id, and authentication settings used by one configured client:

```c
r_auth_key_info_t key;
if (r_client_parse_auth_key(auth_key, &key) != RCLIENT_OK) {
    return -1;
}

r_client_config_t cfg = {0};
cfg.tenant.dns_name = "api-key.example.com";
cfg.tenant.key_id = key.key_id;
cfg.tenant.auth.type = key.type;
cfg.tenant.auth.secret = auth_key;

r_request_policy_t policy;
r_client_default_request_policy(&policy);
policy.attempt_timeout_ms = 20;
policy.retry.retry_attempts = 0;
cfg.request_policy = &policy;

r_client_t *client = NULL;
int rc = r_client_create(&cfg, &io_ops, &resolver_ops, &client);
```

`cfg.tenant.auth.secret` is the encoded Bech32 credential string itself
(`rl-cookie...` or `rl-aes...`), not raw binary key material.
`cfg.tenant.auth.secret_len` is the byte length of that encoded text string;
leave it as `0` for ordinary null-terminated C strings. The supported Bech32
credential alphabet is printable ASCII and does not carry embedded NUL bytes.
The client validates and decodes the credential internally before copying the
raw 32-byte cookie/AES material into private client state.

The client copies `tenant.dns_name` and the auth secret string during
`r_client_create`, so those configuration strings only need to remain valid for
the duration of the call.

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

## Hashing IDs

`r_client_hash_id(input, out_id)` maps application strings to 16-byte IDs for
resource buckets and latency services.

The function returns no status. If `input` or `out_id` is `NULL`, it leaves
`out_id` unchanged.

## Rate Requests

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

## Latency Guards and Reports

Latency tracking forms one admission-and-feedback loop:

1. Create a guard for a stable service ID and include it in a rate request.
2. Copy its result during the request callback.
3. Perform protected work only when the combined result passes.
4. Measure that work with a monotonic clock.
5. Report the observation using the same service ID and tracker settings.

Never report latency for work rejected by its guard. The operation did not run,
so a zero or synthetic observation would bias the tracker and admit too much
future work.

### Guard configuration

Hash an application-level service name exactly as for resource bucket names:

```c
r_latency_guard_t guard = {
    .threshold_ms = 100,
    .ttl_ms = 10000,
    .max_samples = 100,
    .buffer_size = 32,
    .min_sample_threshold = 5,
};
r_client_hash_id("inventory-backend", guard.service_id);
```

| Field | Contract |
| --- | --- |
| `service_id` | Stable 16-byte ID identifying one latency tracker. |
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

The report must repeat the guard's `service_id`, `ttl_ms`, `max_samples`,
`buffer_size`, and `min_sample_threshold`. `threshold_ms` appears only in the
guard because it controls admission, not sample storage.

```c
r_service_latency_report_t report = {
    .observed_latency = elapsed_ms,
    .ttl_ms = guard.ttl_ms,
    .max_samples = guard.max_samples,
    .buffer_size = guard.buffer_size,
    .min_sample_threshold = guard.min_sample_threshold,
};
memcpy(report.service_id, guard.service_id, sizeof(report.service_id));

int rc = r_client_report_latency(client, &report, 1);
if (rc != RCLIENT_OK) {
    /* Log telemetry failure; do not rewrite an HTTP response already sent. */
}
```

`r_client_report_latency` sends fire-and-forget data. It does not wait for a
response, create a request handle, or require a deadline watcher. Report
storage is borrowed only for the duration of the call because the packet is
serialized synchronously.

Reports whose `buffer_size` exceeds the credential quota are filtered. If all
reports are filtered, the function returns `RCLIENT_OK` without sending. Other
failures include `RCLIENT_ERR_DNS` when no server is available and
`RCLIENT_ERR_IO` when the UDP send hook fails.

See [`examples/latency_tracker.c`](../examples/latency_tracker.c) for complete
guard-pass, protected-work, report, and guard-deny control flow.

## Datagrams and Timers

The host owns network receive and timers:

- call `r_client_on_datagram` for UDP packets received on the client socket
- call `r_client_request_deadline_ms` after submitting a request
- call `r_client_on_timeout` when the host timer fires
- call `r_client_cancel_request` if the HTTP/request context is abandoned

`r_io_ops_t.now_ms`, `r_client_request_deadline_ms`, and the `now_ms` passed to
`r_client_on_timeout` use the same Unix-epoch millisecond clock domain. This is
separate from the monotonic duration clock used to measure protected-work
latency.

AES response replay handling is tied to the request lifecycle. The authenticated
`unique_id` in the authenticated packet header must match an in-flight request.
Once that request completes, times out, or is canceled, later datagrams with
the same `unique_id` are ignored. The authenticated timestamp is retained as
protocol framing, but the client does not apply a separate clock-skew freshness
check. Keep request deadlines short and deliver timeout/cancel events promptly.

`cfg.request_policy` is borrowed only for the duration of `r_client_create`; the
client copies the policy by value and does not retain the caller's pointer.

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

## Compatibility

The stable public surface is limited to `include/r_client.h` and
`include/r_client_io.h`. Internal protocol builders, crypto helpers, and packet
parsers are implementation details.
