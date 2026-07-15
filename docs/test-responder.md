# Test responder contract

`r_test_responder` is a deterministic, loopback-only UDP fixture for testing
applications that embed `rl-c-client`. It is test support, not a RateLimitly
server and not part of the production C library API.

The responder lives in this repository so packet encoding, authentication, and
protocol changes remain next to their source of truth. Downstream tests invoke
the versioned executable; they must not include private headers from `src/` or
reimplement Ratelimitly packets.

## Build and start

Build the fixture explicitly:

```sh
make test-responder
```

Start one deterministic scenario on an explicit loopback address:

```sh
./bin/r_test_responder \
  --listen=127.0.0.1:39080 \
  --scenario=allow
```

The process must reject wildcard and non-loopback listen addresses. It must not
open a socket unless `--listen` is present.

On startup it writes exactly one readiness record to standard output:

```json
{"event":"ready","address":"127.0.0.1","port":39080,"server_id":1,"auth":"aes"}
```

All subsequent standard-output records are newline-delimited JSON. Diagnostics
go to standard error. `SIGINT` and `SIGTERM` cause a clean socket close and exit
status zero.

## Synthetic credentials

The responder contains only clearly marked synthetic AES and cookie fixtures.
The default is the repository AES fixture. `--auth=aes` and `--auth=cookie`
select between those built-in credentials; arbitrary credentials are not
accepted on the command line.

`--print-nginx-config` prints the matching synthetic tenant/auth directives,
SRV target, and responder address without opening a socket. This output is for
generated test configurations only and must label the credential as synthetic.

The server id defaults to `1` and may be replaced with `--server-id=<n>` so a
local DNS fixture can advertise the matching `s-<n>.localhost` target. The
credential tenant key id and response server id are distinct protocol fields.

## Scenarios

Exactly one base scenario is selected per process:

| Scenario | Required behavior |
| --- | --- |
| `allow` | Return every requested guard and resource in request order with passing values. |
| `deny` | Return every requested item, with a nonzero deficit on each resource. |
| `guard-pass` | Pass every guard and allow every resource. |
| `guard-deny` | Set each guard current latency to its threshold and allow resources. |
| `quota` | Allow the first `--allow-count=<n>` rate requests per bucket, then return a deficit. |
| `drop` | Authenticate and observe packets but send no response. |
| `malformed-auth` | Respond with authentication material that the client must reject. |
| `malformed-truncated` | Send a deliberately truncated authenticated response. |
| `malformed-request-id` | Send an otherwise valid response with a different request id. |
| `count-empty` | Send a valid authenticated success response with zero guards and resources. |
| `count-short` | Return one fewer result than requested, preferring a missing resource. |
| `count-extra` | Return one additional result beyond the request counts. |

`--delay-ms=<n>` delays a non-drop response without blocking signal handling.
`--steering=rebind` sets steering feedback so the client asks its host to change
the source port; the default is `--steering=keep`.

`--max-packets=<n>` exits successfully after observing the requested number of
authenticated packets. A zero value means run until signaled.

## Observations

The responder emits one record for each authenticated input packet. A rate
request record contains at least:

```json
{"event":"rate_request","sequence":1,"guards":1,"resources":2,"label":"api"}
```

A latency report record contains at least:

```json
{"event":"latency_report","sequence":2,"reports":1}
```

The process must never print credential material, raw authenticated packets,
bucket ids, or service ids. Counts, the optional metrics label, scenario name,
and response disposition are sufficient for downstream assertions.

Malformed or unauthenticated inputs produce an `input_rejected` record and no
response. Invalid command-line configuration exits nonzero before writing a
readiness record.

## Determinism and state

- Scenario behavior depends only on command-line options and authenticated
  packet order.
- `quota` counters start at zero for every process and are not persisted.
- The fixture uses one event loop and sends at most one response per rate
  request.
- Latency reports are observed but never answered.
- Repeated shutdown signals are safe.

These properties let downstream suites restart the responder between cases and
obtain the same result without a private service, wall-clock rate windows, or
network access.

## Compatibility

The test responder is versioned with `rl-c-client`. Its command line and JSONL
records are a test-support contract for the tagged release, but they are not
covered by production C API or ABI compatibility promises. Breaking fixture
changes require release-note entries so downstream test suites can update
their dependency lock deliberately.
