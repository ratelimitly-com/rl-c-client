# Changes

This repository has not published a versioned release yet. Until the first
release tag, use git history as the detailed changelog.

## Unreleased

- Added `r_client_parse_auth_key` as the public tenant credential parser.
- Kept low-level packet and crypto helpers private.
- Added a public-header-only API test.
- Reworked documentation for public integration without requiring private
  server source access.
- Added open-source readiness files and CI.
- Hardened credential handling by using constant-time cookie comparison,
  cleansing secret material, and removing unused SHA-256 cookie helper code.
