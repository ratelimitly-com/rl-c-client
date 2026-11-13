# Wide rate windows (source support, unreleased)

Existing `r_resource_request_t` and request functions retain their ABI and RT/RR bytes.
For a server with capability bit 5, call `r_client_check_rate_limit_wide_async`
with `r_resource_request_wide_t`; this explicitly selects QW/RW. Derive IDs with
`r_client_derive_bucket_id_wide`. Short members retain v1 IDs in mixed packets.

`r_client_parse_auth_key_wide` returns `rate_window_cap` with an explicit
`unlimited` tag and exact finite `milliseconds`. The old parser fails closed
when its uint32 cap cannot represent the key; client construction uses the full
internal reader. Unlimited is not a request window or TTL. Wide windows are
1..31536000000 ms; count limits remain 16 and datagrams 1200 bytes.

No automatic format downgrade, timeout fallback, key migration, release or
server deployment is performed. Existing credential bytes/defaults are unchanged.
