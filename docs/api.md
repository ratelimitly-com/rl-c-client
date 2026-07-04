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

Create one `r_client_t` per tenant/event-loop context:

```c
r_auth_key_info_t key;
if (r_client_parse_auth_key(auth_key, &key) != RCLIENT_OK) {
    return -1;
}

r_client_config_t cfg = {0};
cfg.tenant.dns_name = "tenant.example.com";
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

`r_client_parse_auth_key` validates a tenant credential and returns:

- `type`: one of `R_AUTH_COOKIE`, `R_AUTH_AES_GCM`
- `key_id`: tenant identifier embedded in the key
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

Pointers inside `r_rate_limit_result_t` are valid only during the callback.

## Latency Reports

`r_client_report_latency` sends fire-and-forget latency data. It does not wait
for a response. If all reports are filtered out by quotas, the function returns
`RCLIENT_OK` without sending.

nginx-style integrations should call this after request completion when a
latency guard was applied.

## Datagrams and Timers

The host owns network receive and timers:

- call `r_client_on_datagram` for UDP packets received on the client socket
- call `r_client_request_deadline_ms` after submitting a request
- call `r_client_on_timeout` when the host timer fires
- call `r_client_cancel_request` if the HTTP/request context is abandoned

AES response replay handling is tied to the request lifecycle. The authenticated
`unique_id` in the tenant header must match an in-flight request; once that
request completes, times out, or is canceled, later datagrams with the same
`unique_id` are ignored. The authenticated timestamp is retained as protocol
framing, but the client does not apply a separate clock-skew freshness check.
Keep request deadlines short and deliver timeout/cancel events promptly.

## Error Codes

All errors are negative:

- `RCLIENT_ERR_IO`
- `RCLIENT_ERR_TIMEOUT`
- `RCLIENT_ERR_PROTOCOL`
- `RCLIENT_ERR_AUTH`
- `RCLIENT_ERR_DNS`
- `RCLIENT_ERR_CONFIG`
- `RCLIENT_ERR_NOMEM`

`RCLIENT_ERR_CONFIG` covers invalid arguments and invalid tenant credentials.

## Compatibility

The stable public surface is limited to `include/r_client.h` and
`include/r_client_io.h`. Internal protocol builders, crypto helpers, and packet
parsers are implementation details.
