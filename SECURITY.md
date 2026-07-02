# Security

This repository is being prepared for public release. A permanent vulnerability
reporting contact must be added before publication.

## Credential Handling

Tenant credentials can contain raw cookie or AES key material. Do not log:

- `r_auth_config_t.secret`
- `r_auth_key_info_t.secret`
- packet bytes containing authenticated payloads

## Supported Versions

No public releases have been made yet. Security support begins with the first
published release.

## Reporting

Until a public contact is assigned, report security issues through the project
maintainers directly and do not open public issues for suspected vulnerabilities.
