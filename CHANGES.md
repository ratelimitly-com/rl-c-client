# Changes

## 0.4.0 - 2026-07-26

- Made oldest-trusted-server response selection the default resource-request
  strategy.
- Added 20 ms attempt timing and retries through the 300 ms deduplication
  window when no valid response arrives.
- Documented the logical-request retry and response-selection behavior.

## Unreleased

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
- Added native Ubuntu, Debian, Fedora, macOS, and Windows release artifacts for
  AMD64 and AArch64, including a universal macOS SDK.
- Added deterministic embeddable source archives, exact-set checksums and
  manifests, SBOM metadata, and build-provenance attestations.
- Added automatic GitHub Release publication after a version-bumping pull
  request merges to `main`.

## 0.2.0 - 2026-07-15

- Specified a versioned test responder contract for downstream integration
  suites without expanding the production C API. Its required `--listen`
  endpoint is test-harness process control, not a RateLimitly server option or
  address restriction.
- Added the responder executable with synthetic AES/cookie authentication,
  deterministic allow/deny/guard/quota/malformed scenarios, JSONL observations,
  latency-report capture, steering feedback, and clean signal shutdown.

## 0.1.0 - 2026-07-15

- Added `r_client_parse_auth_key` as the public API key credential parser.
- Kept low-level packet and crypto helpers private.
- Added a public-header-only API test.
- Reworked documentation so public integration guidance is self-contained in
  this repository.
- Added open-source readiness files and CI.
- Hardened credential handling by using constant-time cookie comparison,
  cleansing secret material, and removing unused SHA-256 cookie helper code.
