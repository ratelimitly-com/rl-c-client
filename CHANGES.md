# Changes

This repository has not published a versioned release yet. Until the first
release tag, use git history as the detailed changelog.

## Unreleased

- Specified a versioned, loopback-only test responder contract for downstream
  integration suites without expanding the production C API.
- Added the responder executable with synthetic AES/cookie authentication,
  deterministic allow/deny/guard/quota/malformed scenarios, JSONL observations,
  latency-report capture, steering feedback, and clean signal shutdown.
- Added `r_client_parse_auth_key` as the public API key credential parser.
- Kept low-level packet and crypto helpers private.
- Added a public-header-only API test.
- Reworked documentation so public integration guidance is self-contained in
  this repository.
- Added open-source readiness files and CI.
- Hardened credential handling by using constant-time cookie comparison,
  cleansing secret material, and removing unused SHA-256 cookie helper code.
