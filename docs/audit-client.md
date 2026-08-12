# RateLimitly server audit client

`audit_client` is a live, observational diagnostic for the r-servers assigned
to one tenant. It sends real authenticated resource requests and latency
reports, so running it changes that tenant's rate-bucket and latency-tracker
state. The tool isolates normal runs with fresh content-defined IDs and prints
what the servers did; it does not impose an expected grant/rejection ratio.

## Build and run

Build the executable from the repository root:

```sh
make audit_client
```

Pass a tenant API key with `--auth`:

```sh
bin/audit_client --auth=rl-aes1...
```

For a key stored in a file, avoid placing the credential directly in shell
history:

```sh
bin/audit_client \
  --auth="$(tr -d '\r\n' < /path/to/api.key)" \
  --unit-ms=25 \
  --replay-count=3
```

The key is mandatory. It must be an `rl-aes...` or `rl-cookie...` tenant
credential containing quotas sufficient for at least one rate bucket, one
latency tracker, a tracker buffer of ten samples, and the HA policy's derived
deduplication TTL.

## Discovery and isolated state

The audit has no CLI option for bypassing tenant discovery or selecting a
server directly. It performs the production sequence:

1. Parse the tenant API key and extract its tenant ID.
2. Derive `c-<tenant-id>.p0.ratelimitly.com`.
3. Resolve `_ratelimitly._udp.<derived-domain>` through DNS SRV.
4. Resolve each conforming `s-<decimal>` SRV target through A and AAAA records.
5. Send every logical resource request according to the configured HA policy.

`RCLIENT_DNS_SERVER=IPv4[:port]` may select a DNS resolver, including a local
DNS test fixture. It changes the resolver, not the derived tenant name or the
server-membership rules.

By default, the tool generates a random run ID and derives state from the
logical names `bucket/<run-id>` and `latency/<run-id>`. This prevents a recent
audit from changing a later audit's starting state. `--run-id=<text>` makes
the names repeatable for investigation, but reusing it while server state is
still live intentionally reuses that state.

Only the first eight bytes of each 16-byte content-defined ID are rendered in
log lines. This abbreviated value is a display label; the complete ID is sent
on the wire.

## Workloads

The tool performs four workloads in sequence.

### 1. Rate bucket saturation

The bucket is configured for ten tokens per ten seconds. The client sends 20
sequential requests, each requesting one token, without an intentional delay
between requests. Each line reports the selected grant, rejection, or client
error and the complete logical-request duration.

### 2. Latency tracker saturation

The client sends ten sequential guard-only requests for one tracker:

| Setting | Value |
| --- | ---: |
| threshold | 1000 ms |
| TTL | 10 seconds |
| maximum samples | 10 |
| buffer size | 10 |
| minimum sample threshold | 5 |

A granted guard causes the server to record its configured speculative
latency. The resulting ratio depends on the server's tracker algorithm and on
request timing. The audit reports the ratio without classifying it as correct
or incorrect.

### 3. Report followed by guards

The client sends one 100 ms latency report for the same tracker. Latency
reports are independent fire-and-forget operations; `Sent` means that the
client successfully constructed and transmitted the report, not that an
acknowledgement was received. After one scheduling unit, the client sends 20
sequential guard-only requests and records their results.

### 4. Tracker expiry

The client sends another 100 ms report, waits the ten-second tracker TTL plus a
one-millisecond boundary margin, and then sends ten sequential guard-only
requests. Both the beginning and end of the wait are timestamped.

## Output and conclusion

Each operation line begins with local wall-clock time at microsecond precision.
Request durations use a monotonic clock and cover one complete logical request,
including HA waiting and replay rounds, from submission through selected
response or client error. DNS discovery happens before the workloads and is
not part of those durations.

Example:

```text
Test 1 - Rate bucket saturation
Send 20 sequential one-token requests to bucket 03fd54d4b4e94f3e, configured for 10 tokens per 10 seconds.
The log records the selected result and complete logical-request duration.
2026/08/11 08:18:40.021705 Request{Guard:{},Tokens:{03fd54d4b4e94f3e:1}} Granted - 23.654 ms
2026/08/11 08:18:40.046023 Request{Guard:{},Tokens:{03fd54d4b4e94f3e:1}} Rejected - 24.172 ms
```

The final `Conclusion` lists measured granted, rejected, and error counts plus
the elapsed request-processing time for every workload. It intentionally has
no `PASS`, `FAIL`, or expected-ratio judgment.

## Optional server metrics

Tenant API keys cannot query administrative server metrics. Pass the server's
separate 32-byte Bech32 `rl-secret...` management credential to enable them:

```sh
bin/audit_client \
  --auth="$(tr -d '\r\n' < /path/to/api.key)" \
  --management-key="$(tr -d '\r\n' < /path/to/management.key)"
```

With the management key, the client queries every discovered SRV endpoint
before the workloads and after each one. It prints absolute counters and,
where the responding server identity is unchanged, deltas from the preceding
snapshot. The retrieved families are:

- tenant summary counters: successful, rate-limited, guard-failed, and
  authentication-failed requests plus accepted latency-report blocks;
- latency-tracker counters for this run's tracker: reports, checks, passed
  checks, and failed checks.

Metrics are listener-local rather than cluster-aggregated. Each endpoint is
printed separately, and different endpoints may legitimately show different
values. When a tracker expires and is recreated, its counters restart; the
client identifies that decrease and marks the delta unavailable instead of
printing a misleading zero. The management credential must authenticate every
discovered endpoint for a completely successful metrics run, and is never
printed. If it is omitted, each
metrics point is logged as skipped and does not affect the audit.

Metrics query controls:

| Option | Default | Meaning |
| --- | ---: | --- |
| `--metrics-timeout-ms=<n>` | 200 | Response wait for one administrative query attempt. |
| `--metrics-attempts=<n>` | 3 | Attempts per endpoint and page. |

## HA policy options

The default policy is the library default: a 20 ms unit, one replay, a fixed
one-unit round duration, one final receive-only unit, and completion delivery.

| Option | Meaning |
| --- | --- |
| `--unit-ms=<n>` | Base scheduling unit `U`. |
| `--replay-count=<n>` | Replays after the initial send. |
| `--replay-schedule=fixed\|linear\|exponential` | Round-duration schedule. |
| `--replay-initial-units=<n>` | First round duration in units of `U`. |
| `--replay-max-units=<n>` | Maximum round duration in units of `U`. |
| `--replay-growth=<n>` | Linear increment or exponential multiplier. Ignored by a fixed schedule. |
| `--final-receive-units=<n>` | Receive-only duration after the final send round. |
| `--completion-delivery=true\|false` | Whether to send the request to missing servers before returning a selected result. |

The client calculates and prints the resulting wire deduplication TTL. Startup
fails if the schedule is malformed, overflows, or exceeds the API key's
`dedup_ttl_ms_max`. See [the API guide](api.md#resource-request-ha-policy) for
the scheduler, response-selection, and deduplication contracts.

## Exit status

| Status | Meaning |
| ---: | --- |
| `0` | All operations completed without client or requested-metrics errors. Any observed grant/rejection ratio is accepted. |
| `1` | A runtime operation failed, including DNS, UDP, timeout, protocol, report-send, or requested-metrics retrieval failure. |
| `2` | CLI arguments, credentials, quotas, or HA policy are invalid. |

Run `bin/audit_client --help` for the current option summary.
