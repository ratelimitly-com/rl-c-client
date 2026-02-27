# C r-client Requirements (MVP)

## 1) Scope and References
- **Scope source of truth**: `docs/spec/r-client.md` and `docs/spec/wire_protocol.md`.
- **Reference implementation**: `clients/rust/` behavior is authoritative when specs are ambiguous.
- **Out of scope**: TLS (no TLS in protocol). Any features not described in the two specs or Rust client.

## 2) Goals
- Provide a **C r-client** that is compatible with the Ratelimitly wire protocol over UDP.
- Be **easy to integrate with nginx** (event-driven, non-blocking, minimal allocations).
- Be **generic**: usable in other async frameworks (libuv, custom reactors, etc.).
- Favor **speed and simplicity**; minimal error handling and minimal API surface for MVP.

## 3) Functional Requirements
### 3.1 Core Operations
- **checkRateLimit**: build and send rate request, parse rate response, return success/failure and per-guard/resource results.
- **reportLatency**: build and send latency report, fire-and-forget.

### 3.2 Protocol Compliance
- UDP transport, **single datagram per message**.
- **Little-endian** encoding for all integers.
- **Alignment/padding** must match spec: guard/service blocks are 40 bytes, resource blocks are 28 bytes with 2-byte padding.
- **Max payload** target ~1200 bytes to avoid fragmentation.
- Support **metrics label TLV** in rate requests.
- Support **tenant header** fields: key_id, unique_id (16 bytes), timestamp (ms since epoch), steering_feedback, tenant_mgmt_flag.
  - Client must always send `steering_feedback = 0` in requests (allow steering).
  - Client must always react to server steering feedback in responses.
- **unique_id** should be UUIDv4 (binary 16 bytes), or equivalent high-entropy identifier.

### 3.3 Authentication
- **None** (TLV 0x414E)
- **Cookie** (TLV 0x4143) where cookie is the 32-byte payload embedded in `rl-cookie...`
- **AES-256-GCM** (TLV 0x4541) with:
  - 32-byte AES key payload embedded in `rl-aes...`
  - 12-byte nonce, 16-byte auth tag
  - PDU encrypted; tenant header remains in clear

### 3.4 Discovery and HA
- DNS discovery via **SRV lookup** `_ratelimitly._udp.<tenant_dns_name>`.
- Fallback to **A/AAAA records** for `<tenant_dns_name>` on port 8080.
- Only SRV discovery is supported (no static server list).
- **Broadcast requests** to all discovered servers (same unique_id).
- **Response selection policy**: implement the full policy surface (see Rust `request_policy.rs`), not only the default behavior.
- Track **server_id** (from tenant header key_id in responses) for basic stability filtering (per Rust client behavior).

### 3.5 Timeouts and Retries
- Enforce per-request timeout for operations expecting responses.
- Minimal retry support: retry on timeout with configurable attempt count and delay.
- If auth fails, server blackholes request; timeout handling must cover this.

### 3.6 Hashing for IDs
- Use **BLAKE2s-128** (first 16 bytes of BLAKE2s-256) for:
  - `service_id`
  - `bucket_id`

## 4) API Requirements (C)
### 4.1 Public Types (suggested)
- `r_client_t` opaque handle for client state.
- `r_client_config_t` with:
  - tenant dns name
  - key_id (u64)
  - auth method (none/cookie/aes) + Bech32 auth key (`rl-none...`/`rl-cookie...`/`rl-aes...`)
  - timeout_ms, retry_attempts
  - dns_refresh_interval_s
  - server_stability_threshold_ms
  - (removed) ignore_steering_feedback flag
- Request/response structs mirroring Rust:
  - `r_resource_request_t`
  - `r_latency_guard_t`
  - `r_service_latency_report_t`
  - `r_rate_limit_result_t` with guard/resource results

### 4.2 Error Handling
- Minimal: integer error codes (enum). Optional debug string getter.
- Errors should cover: IO, timeout, protocol, auth, DNS, config.

### 4.3 Memory Management
- Explicit init/free for all owned structures.
- No global allocations; support passing allocators if nginx integration benefits.
- Avoid heap allocations on hot path where feasible.

## 5) Async and I/O Abstraction
- **Core client must be I/O-agnostic**: it should not assume a specific event loop.
- Provide an **I/O adapter interface** (send/receive + time + timers) so nginx can supply its own UDP + timer facilities.
- Provide an optional **POSIX UDP adapter** for non-nginx use (e.g., epoll or poll based), but keep it modular.

## 6) Threading
- Prefer **single-threaded**, event-driven usage.
- No internal threads required for MVP.
- Thread safety not required unless explicitly added later.

## 7) Build System
- **Makefile**-based build by default.
- Support building **shared** and **static** libraries (configurable flags).
- Produce `librclient.so` and/or `librclient.a` in `clients/c/build/` or similar.

## 8) Testing
- Provide a **C test suite** covering:
  - None/Cookie/AES auth
  - Rate request/response parsing
  - Latency report generation
  - DNS discovery and fallback
  - Retry/timeout behavior
- Tests should be runnable via Makefile target (e.g., `make test`).

## 9) Documentation
- Minimal README in `clients/c/README.md`:
  - Build instructions
  - Minimal usage example
  - Notes on nginx integration
- Code should be commented where it is not self-explanatory.

## 10) Clarifications (resolved)
- **Tenant management PDUs**: excluded from C client MVP.
- **Response selection policy**: implement the full policy surface (as in Rust `request_policy.rs`).
- **DNS resolver**: mimic Rust behavior (SRV → A fallback, server_id parsing from SRV target) but keep resolver pluggable for nginx or other hosts.
- **Metrics label**: no extra constraints beyond UTF-8 and length prefix per spec.
