# Security

Do not open public issues for suspected vulnerabilities.

Until a dedicated security alias is published, report security concerns to:

```text
wojciech@ratelimitly.com
```

Include:

- affected commit or release
- compiler and operating system
- build mode, static or shared
- relevant configuration snippets with secrets removed
- reproduction steps
- expected and observed behavior

## Credential Handling

API key credentials can contain raw cookie or AES key material. Do not log:

- `r_auth_config_t.secret`
- `r_auth_key_info_t.secret`
- packet bytes containing authenticated payloads

## Authentication Modes

Use AES credentials for deployments that cross an untrusted network. Cookie
credentials are intended only for private-network deployments where passive
capture and on-path modification are outside the threat model. Cookie mode does
not provide packet integrity.

## Response Replay Model

AES responses authenticate the clear packet header and encrypted PDU. This binds
the response `unique_id`, server id, timestamp, and steering feedback to the GCM
tag, so an observed response cannot be retargeted to another request.

The client treats `unique_id` as the replay boundary. A response is accepted only
while a matching request is still in flight; after completion, timeout, or
cancel, later datagrams with that `unique_id` are ignored. Duplicate responses
from the same server id do not count as additional quorum members. The
authenticated timestamp is not used as a wall-clock freshness check.

Host integrations must keep request deadlines short and must call
`r_client_on_timeout` or `r_client_cancel_request` when the application request
is no longer active.

## Release Integrity

The release workflow builds each binary on a native runner, rejects incomplete
or unexpected artifact sets, and publishes `SHA256SUMS` plus
`RELEASE-MANIFEST.json`. The manifest binds every payload name to its version,
source commit, architecture, size, and SHA-256 digest.

Every published payload, `SHA256SUMS`, and the release manifest receives a
GitHub build-provenance attestation before the draft release becomes public.
Verify both the checksum and the attestation; a matching checksum alone does
not establish who produced an artifact.

Windows SDKs also contain a WDK-generated SPDX SBOM. Release packages are not
claimed to be Authenticode-signed, Apple-notarized, or distribution-repository
signed unless the individual release notes explicitly say so.

## Supported Versions

Security fixes are provided for the most recent published release. Users of
older releases should upgrade unless release notes explicitly extend support.

## Reporting

Use the private reporting process above. Do not publish proof-of-concept code or
real credentials in public issues, pull requests, logs, or examples.
