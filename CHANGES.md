# Changes

## Unreleased

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
