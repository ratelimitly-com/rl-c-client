# Changes

## Unreleased

## 2.0.0 - 2026-08-29

- **Breaking:** removed the per-tracker `buffer_size` field from latency guards,
  latency reports, canonical latency-tracker IDs, and their wire encodings.
- Changed guard blocks from 40 to 36 bytes and latency-report blocks from 36 to
  32 bytes, matching Ratelimitly wire protocol version 2.
- Versioned the revised canonical identity as
  `ratelimitly.latency-tracker.v2\0`; resource bucket identity remains v1.
- Coalesce source-port steering until all in-flight requests drain, and add a
  shared full-range monotonic port selector with no port-zero fallback.
- Make the portable runtime and performance client apply deterministic
  steering; runtime sockets use exclusive wildcard binds on Windows.

## 1.0.0 - 2026-08-18

- **Breaking:** adopted versioned API-key format 1 and intentionally rejected
  legacy unversioned credentials and unknown format versions.
- Replaced the five unpacked API-key quota values with one compact quota word
  carrying six limits, including `rate_window_size_ms_max`.
- Added `format_version` and `rate_window_size_ms_max` to
  `r_auth_key_info_t`, and rejected complete resource requests locally when a
  resource window exceeds the credential quota, before DNS, serialization, or
  UDP transmission.
- Established API-key format 1, the public C API, and the documented request
  policy as the first stable `rl-c-client` release contract.

## 0.6.0 - 2026-08-10

- Allowed resource-only, guard-only, combined, and empty core request shapes.
  Guard-only requests use the normal r-server path; empty requests complete
  synchronously as successful local no-ops without DNS or UDP activity.

## 0.5.1 - 2026-08-06

- **Bugfix / API Simplification:** simplified `r_request_policy_t` by removing
  the redundant `preference` and `final_preference_units` schedule parameters.
- Standardized request policy preference deadlines so Round 0 ($k=0$) always
  prefers the oldest discovered server until the Round 0 deadline (`replay_gap`),
  completing immediately if the oldest server answers or at round end otherwise.
- Guaranteed that consecutive replay rounds ($k \ge 1$) and the final receive
  phase complete immediately upon receiving a valid response from any server
  with zero additional latency delay.

## 0.5.0 - 2026-08-02

- **Breaking:** replaced the previous wait/quorum/selection/retry policy
  surface with one flat `r_request_policy_t`. The first MVP carries no aliases,
  policy discriminator, or legacy execution path.
- Added configurable fixed, linear, and exponential replay schedules,
  independent oldest-response preference timing, and optional
  outcome-independent completion delivery. The complete schedule derives its
  deduplication TTL and is checked against the credential quota.
- Moved DNS refresh pacing to `r_client_config_t.dns_refresh`; it is not part
  of request response-selection policy.
- Renamed the performance client policy options to `--unit-ms` and
  `--replay-count` and removed the former retry-oriented options.
- Added canonical, cross-client bucket and latency-tracker ID derivation from
  application names and every setting that defines the corresponding stored
  server state.
- Rejected latency-report batches that cannot fit the 1200-byte packet before
  writing them, closing a stack-buffer overflow in the public report API.
- Hardened the portable runtime against stale Windows UDP reset notifications
  and against malformed or unauthenticated datagrams, which are now discarded
  as packet-local noise while receive draining continues.
- Made production discovery strictly SRV-based; failed, empty, or
  non-conforming membership now returns `RCLIENT_ERR_DNS` instead of silently
  falling back to the tenant address on UDP port 8080.
- Failed resource-request and latency-report creation without sending when
  secure request-ID generation fails.
- Guaranteed that `r_runtime_admission_run_and_report()` invokes protected work
  at most once per admission, including when latency-report delivery fails.
- Defined custom `udp_send` hooks as non-reentrant: synchronous loopback input
  must be queued until the send call returns.
- Stopped performance-client diagnostics from echoing invalid credentials and
  corrected production-test redaction for supported `rl-cookie` keys.
- Expanded the self-contained public documentation with integration-layer,
  DNS, request-lifecycle, error, quota, security, result, and release contracts.
- Added conservative production-test policy profiling with a 25 ms unit and
  three replays while leaving the library defaults unchanged.

## 0.4.0 - 2026-07-26

- Made oldest-trusted-server response selection the default resource-request
  strategy.
- Added 20 ms attempt timing and retries through the 300 ms deduplication
  window when no valid response arrives.
- Documented the logical-request retry and response-selection behavior.

## 0.3.0 - 2026-07-26

- Added native Ubuntu, Debian, Fedora, macOS, and Windows release artifacts for
  AMD64 and AArch64, including a universal macOS SDK.
- Added deterministic embeddable source archives, exact-set checksums and
  manifests, SBOM metadata, and build-provenance attestations.
- Added automatic GitHub Release publication after a version-bumping pull
  request merges to `main`.

## 0.2.0 - 2026-07-15

- Specified a versioned test responder contract for downstream integration
  suites without expanding the production C API. Its required `--listen`
  endpoint is test-harness process control, not a Ratelimitly server option or
  address restriction.
- Added the responder executable with synthetic AES/cookie authentication,
  deterministic allow/deny/guard/quota/malformed scenarios, JSONL observations,
  latency-report capture, steering feedback, and clean signal shutdown.

## 0.1.0 - 2026-07-13

- Added `r_client_parse_auth_key` as the public API key credential parser.
- Kept low-level packet and crypto helpers private.
- Added a public-header-only API test.
- Reworked documentation so public integration guidance is self-contained in
  this repository.
- Added open-source readiness files and CI.
- Hardened credential handling with constant-time cookie comparison, cleansing
  of library-retained secret material, and removal of unused SHA-256 cookie
  helper code.
