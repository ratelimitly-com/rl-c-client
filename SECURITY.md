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

## Supported Versions

No public releases have been made yet. Security support begins with the first
published release.

## Reporting

Use the private reporting process above. Do not publish proof-of-concept code or
real credentials in public issues, pull requests, logs, or examples.
