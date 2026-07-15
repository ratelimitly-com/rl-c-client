# Ratelimitly C Client

`rl-c-client` is a small C11 client library for integrating applications,
proxies, and event-loop-driven servers with Ratelimitly.

The library is intentionally host-loop agnostic. It builds request packets,
parses responses, tracks request deadlines, applies response-selection policy,
and handles API key credentials. The embedding application owns sockets, DNS,
timers, memory lifetime, and logging.

Proxy modules and other high-throughput embedders typically provide UDP
sockets, resolver callbacks, request timers, and use the borrowed request API to
avoid per-request copies.

## Features

- Async rate-limit checks over UDP.
- Fire-and-forget latency reports for load-shedding feedback.
- API key credentials in Bech32 form: `rl-cookie...` or `rl-aes...`.
- Cookie and AES-256-GCM authentication using OpenSSL libcrypto.
- SRV discovery for `_ratelimitly._udp.<configured-dns-name>`, followed by A/AAAA
  resolution for returned SRV targets.
- Per-request deadlines, timeout/retry policy, quorum policy, and server
  response selection.
- Optional metrics labels.
- Optional steering feedback callback for source-port rebinding.
- Static and shared library builds.

This repository contains the public C API and integration contract. Applications
do not construct or parse Ratelimitly packets directly; the library owns packet
encoding, authentication, response parsing, retry policy, and server selection.
Integrators provide credentials, resource IDs, latency data, UDP I/O, DNS, and
timers through the APIs documented here.

## Build

Requirements:

- C11 compiler
- `make`
- OpenSSL development headers and libcrypto
- resolver and pthread libraries on POSIX systems

Build the static and shared libraries:

```sh
make
```

Outputs:

- `librclient.a`
- `librclient.so`

Build the perf client:

```sh
make perf_client
```

Run local tests:

```sh
make test
```

Build the optional loopback-only protocol fixture used by downstream
integration tests:

```sh
make test-responder
```

Its test-support contract and deterministic scenarios are documented in
[docs/test-responder.md](docs/test-responder.md). The executable is not part of
the production library API and contains only synthetic credentials.

Clean generated files:

```sh
make clean
```

On macOS with Homebrew OpenSSL, the Makefile defaults `OPENSSL_PREFIX` to
`/opt/homebrew/opt/openssl@3`. Override it when needed:

```sh
OPENSSL_PREFIX=/custom/openssl make
```

## Public API

The public headers are:

- `include/r_client.h`
- `include/r_client_io.h`

Do not include files from `src/`; they are private implementation details.

Core operations:

- `r_client_create` / `r_client_destroy`
- `r_client_check_rate_limit_async`
- `r_client_check_rate_limit_async_borrowed`
- `r_client_report_latency`
- `r_client_on_datagram`
- `r_client_request_deadline_ms`
- `r_client_on_timeout`
- `r_client_cancel_request`
- `r_client_default_request_policy`
- `r_client_hash_id`
- `r_client_parse_auth_key`

See [docs/api.md](docs/api.md) for the API contract and
[IO_ABSTRACTION.md](IO_ABSTRACTION.md) for event-loop integration.

## API Key Credentials

Ratelimitly API key credentials are Bech32 strings:

- `rl-cookie...`: 32-byte cookie secret
- `rl-aes...`: 32-byte AES-256-GCM key

Use `rl-aes...` credentials for deployments that require packet
confidentiality and integrity over an untrusted network. Cookie mode is a
private-network mode: the cookie is sent on the wire and does not authenticate
the packet contents, so it must be used only where on-path modification and
capture are outside the deployment threat model.

The public C structures use `tenant` to describe the per-credential context:
DNS name, key id, and authentication settings. Each credential also carries
quota values. Use `r_client_parse_auth_key` to validate a key before
constructing config:

```c
r_auth_key_info_t info;
if (r_client_parse_auth_key(auth_key, &info) != RCLIENT_OK) {
    /* reject config */
}

r_client_config_t cfg = {0};
cfg.tenant.dns_name = "api-key.example.com";
cfg.tenant.key_id = info.key_id;
cfg.tenant.auth.type = info.type;
cfg.tenant.auth.secret = auth_key;
```

`cfg.tenant.auth.secret` is the encoded Bech32 credential string, not raw
secret bytes. Leave `cfg.tenant.auth.secret_len` as `0` for a normal
null-terminated credential string, or set it to the encoded string length if
the credential is not null-terminated. The client decodes the raw 32-byte
cookie/AES material internally after validating the Bech32 credential.

Do not log `info.secret`; it contains raw credential material for cookie and
AES keys.

## Event-Loop Model

The client never blocks and does not create threads. The host application must:

1. Provide `r_io_ops_t` with UDP send, current time, optional logging, and
   optional steering feedback.
2. Provide `r_resolver_ops_t` for SRV and A/AAAA lookup.
3. Call `r_client_check_rate_limit_async` or the borrowed variant.
4. Schedule the deadline from `r_client_request_deadline_ms`.
5. Deliver UDP responses through `r_client_on_datagram`.
6. Call `r_client_on_timeout` when request timers fire.

Response replay protection is scoped to this request lifecycle: AES responses
must carry a matching authenticated `unique_id` for an in-flight request, and
datagrams for completed, timed-out, or canceled requests are ignored.

For integrations with request-scoped memory pools, use
`r_client_check_rate_limit_async_borrowed` when request buffers live until
callback completion.

## ID Hashing

Applications map their own strings to Ratelimitly IDs:

- bucket strings become `r_resource_request_t.bucket_id`
- service strings become `r_latency_guard_t.service_id`

Use `r_client_hash_id(input, out_id)` to produce the required 16-byte ID.

## Perf Client

The perf client is a standalone load generator and smoke-test tool.

Examples:

```sh
bin/perf_client --clients=50 --requests=10000
bin/perf_client --duration=60 --auth=rl-aes1...
bin/perf_client --srv=api-key.example.com --duration=30 --clients=50
RCLIENT_DNS_SERVER=127.0.0.1:5353 bin/perf_client
bin/perf_client --attempt-timeout-ms=750 --retry-attempts=2 --retry-on=timeout
```

Retry-related flags:

- `--attempt-timeout-ms=<n>`
- `--retry-attempts=<n>`
- `--retry-on=timeout|quorum|inconsistent|never`
- `--retry-resend=all|missing`
- `--retry-total-timeout-ms=<n>`
- `--retry-refresh-dns`

## Repository Status

This repository is licensed under the MIT License; see [LICENSE](LICENSE).
