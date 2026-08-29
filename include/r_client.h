#ifndef R_CLIENT_H
#define R_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "r_client_export.h"
#include "r_client_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Includes space for c-<uint64>.p0.ratelimitly.com and its trailing NUL. */
#define R_CLIENT_DEFAULT_TENANT_DNS_CAPACITY 42u
/* Bounds policy validation and per-request scheduler state. */
#define R_CLIENT_HA_MAX_REPLAY_COUNT 65535u

// Opaque handles.
typedef struct r_client r_client_t;
typedef struct r_client_req r_client_req_t;

// Error codes (negative for failures).
typedef enum r_client_error {
    RCLIENT_OK = 0,
    RCLIENT_ERR_IO = -1,
    RCLIENT_ERR_TIMEOUT = -2,
    RCLIENT_ERR_PROTOCOL = -3,
    RCLIENT_ERR_AUTH = -4,
    RCLIENT_ERR_DNS = -5,
    RCLIENT_ERR_CONFIG = -6,
    RCLIENT_ERR_NOMEM = -7,
} r_client_error_t;

// Auth configuration.
typedef enum r_auth_type {
    R_AUTH_COOKIE = 1,
    R_AUTH_AES_GCM = 2,
} r_auth_type_t;

typedef struct r_auth_config {
    /* Zero derives the authentication type from the encoded key. */
    r_auth_type_t type;
    const char *secret; // Encoded Bech32 credential string: rl-cookie... / rl-aes...
    size_t secret_len; // Encoded string length; 0 means null-terminated. Not raw secret bytes.
} r_auth_config_t;

typedef struct r_auth_key_info {
    uint8_t format_version;
    r_auth_type_t type;
    uint64_t key_id;
    uint8_t secret[32];
    size_t secret_len;
    uint32_t rate_buckets_max;
    uint32_t latency_services_max;
    uint32_t metrics_labels_max;
    uint32_t latency_buffer_size_max;
    uint32_t dedup_ttl_ms_max;
    uint32_t rate_window_size_ms_max;
} r_auth_key_info_t;

typedef struct r_tenant_config {
    /* NULL/empty derives c-<key-id>.p0.ratelimitly.com from the auth key. */
    const char *dns_name;
    /* Zero derives the identifier from the auth key; nonzero must match it. */
    uint64_t key_id;
    r_auth_config_t auth;
} r_tenant_config_t;

typedef enum r_ha_schedule_kind {
    R_HA_SCHEDULE_FIXED = 0,
    R_HA_SCHEDULE_LINEAR = 1,
    R_HA_SCHEDULE_EXPONENTIAL = 2,
} r_ha_schedule_kind_t;

typedef union r_ha_schedule_growth {
    uint32_t linear_step_units;
    uint32_t exponential_factor;
} r_ha_schedule_growth_t;

typedef struct r_ha_schedule {
    r_ha_schedule_kind_t kind;
    uint32_t initial_units;
    uint32_t max_units;
    r_ha_schedule_growth_t growth;
} r_ha_schedule_t;

// The sole non-empty resource-request policy implemented by the MVP client.
typedef struct r_request_policy {
    uint64_t unit_ms;                    // Base scheduling unit U.
    uint32_t replay_count;               // Replays after the initial send.
    r_ha_schedule_t replay_gap;          // Round durations B(k), in U.
    uint32_t final_receive_units;        // Receive-only duration F, in U.
    bool completion_delivery;            // Best-effort delivery to missing servers.
} r_request_policy_t;

typedef enum r_request_completion_phase {
    R_REQUEST_COMPLETION_ROUND = 0,
    R_REQUEST_COMPLETION_FINAL_RECEIVE = 1,
} r_request_completion_phase_t;

/*
 * One completed request's scheduler profile. wait_ms measures only the
 * admission interval from the initial send through selection or failure. It
 * excludes DNS discovery, protected work, latency reporting, and cleanup.
 */
typedef struct r_request_profile {
    uint64_t wait_ms;
    uint32_t round;
    r_request_completion_phase_t phase;
    int status;
    bool response_selected;
} r_request_profile_t;

typedef void (*r_request_profile_cb)(
    void *user,
    const r_request_profile_t *profile
);

typedef struct r_dns_refresh_policy {
    uint64_t refresh_interval_ms;            // Periodic refresh; 0 uses 300 seconds.
    uint64_t forced_refresh_min_interval_ms; // Zero uses 1 second.
    uint64_t forced_refresh_jitter_ms;
} r_dns_refresh_policy_t;

// Client configuration.
typedef struct r_client_config {
    r_tenant_config_t tenant;
    // Borrowed during r_client_create; NULL selects default behavior.
    const r_request_policy_t *request_policy;
    /* Optional completion observer; called before the request callback. */
    r_request_profile_cb request_profile_cb;
    void *request_profile_user;
    r_dns_refresh_policy_t dns_refresh;
} r_client_config_t;

// Request inputs.
typedef struct r_resource_request {
    uint8_t bucket_id[16];
    uint32_t window_size_ms;
    uint32_t rate_limit;
    uint16_t tokens_requested;
} r_resource_request_t;

typedef struct r_latency_guard {
    uint8_t latency_tracker_id[16];
    uint32_t threshold_ms;
    uint32_t ttl_ms;
    uint32_t max_samples;
    uint32_t min_sample_threshold;
} r_latency_guard_t;

typedef struct r_service_latency_report {
    uint8_t latency_tracker_id[16];
    uint32_t observed_latency;
    uint32_t ttl_ms;
    uint32_t max_samples;
    uint32_t min_sample_threshold;
} r_service_latency_report_t;

// Result structures (valid only during callback). Arrays mirror the server
// response; counts may differ from the request, so match entries by ID.
typedef struct r_guard_result {
    uint8_t latency_tracker_id[16];
    uint32_t threshold_ms;
    uint32_t current_latency_ms; // Tracker latency estimate on the responder.
    bool passed;                 // current_latency_ms < threshold_ms.
} r_guard_result_t;

typedef struct r_resource_result {
    uint8_t bucket_id[16];
    uint16_t tokens_deficit; // 0 = granted; nonzero = requested tokens not supplied.
    uint32_t actual_rate;    // Bucket's current consumed tokens in its window.
} r_resource_result_t;

typedef struct r_rate_limit_result {
    bool success;            // All guards passed and all deficits are zero.
    uint64_t server_id;      // Responding server's 64-bit ID.
    bool steering_feedback;  // Wire keep-port flag: true = keep the current
                             // source port; false = server requested a rebind.
    const r_guard_result_t *guards;
    size_t guard_count;
    const r_resource_result_t *resources;
    size_t resource_count;
} r_rate_limit_result_t;

// Async callback for rate limit checks.
typedef void (*r_rate_limit_cb)(
    void *user,
    r_client_req_t *req,
    int status,
    const r_rate_limit_result_t *result
);

// Client lifecycle.
RCLIENT_API int r_client_create(
    const r_client_config_t *config,
    const r_io_ops_t *io_ops,
    const r_resolver_ops_t *resolver_ops,
    r_client_t **out_client
);

// Destroys the client. In-flight requests are freed WITHOUT invoking their
// callbacks (borrowed buffers are implicitly released). Must not be called
// from inside a completion callback; serialize with all other calls on the
// same client — the client contains no locks.
RCLIENT_API void r_client_destroy(r_client_t *client);

// Async rate limit request. Resource and guard counts are independent; a
// positive count requires a non-NULL array. A request with neither resources
// nor guards is a successful local no-op: no packet is sent, *out_req is set
// to NULL when supplied, and cb fires synchronously before this call returns
// with req == NULL and an empty successful result. Its metrics label is ignored.
RCLIENT_API int r_client_check_rate_limit_async(
    r_client_t *client,
    const r_resource_request_t *resources,
    size_t resource_count,
    const r_latency_guard_t *guards,
    size_t guard_count,
    const char *metrics_label,
    size_t metrics_label_len, // 0 means null-terminated
    r_rate_limit_cb cb,
    void *user,
    r_client_req_t **out_req
);

// Same request-shape and empty-completion contract as the copying API, using
// caller-owned buffers for non-empty requests (no internal copies).
// Caller must keep buffers alive until the callback fires, the request is
// canceled, or the client is destroyed (the latter two suppress the callback).
RCLIENT_API int r_client_check_rate_limit_async_borrowed(
    r_client_t *client,
    const r_resource_request_t *resources,
    size_t resource_count,
    const r_latency_guard_t *guards,
    size_t guard_count,
    const char *metrics_label,
    size_t metrics_label_len, // 0 means null-terminated
    r_rate_limit_cb cb,
    void *user,
    r_client_req_t **out_req
);

// Fire-and-forget latency reporting. Reports are framed into a single
// datagram, so a batch too large to fit is rejected with
// RCLIENT_ERR_PROTOCOL and nothing is sent; split large batches across
// calls (30 reports per call is always within capacity). Secure request-ID
// generation failure returns RCLIENT_ERR_AUTH and sends nothing.
RCLIENT_API int r_client_report_latency(
    r_client_t *client,
    const r_service_latency_report_t *reports,
    size_t report_count
);

// Datagram ingress from host. May invoke a request's completion callback and
// free the request before returning; the return value does NOT signal
// completion (RCLIENT_OK is returned on completing paths too).
// RCLIENT_ERR_PROTOCOL and RCLIENT_ERR_AUTH are packet-local noise; other
// non-OK returns report a client failure.
RCLIENT_API int r_client_on_datagram(
    r_client_t *client,
    const uint8_t *buf,
    size_t len,
    const r_addr_t *from
);

// Per-request timer support for non-empty requests. The req handle is invalid
// once the completion callback has fired (during on_datagram/on_timeout) —
// track a flag from the callback before touching req again. Empty requests
// complete during submission and return no handle.
RCLIENT_API int r_client_request_deadline_ms(
    const r_client_req_t *req,
    uint64_t *out_deadline_ms
);

// May invoke the completion callback and free req before returning; returns
// RCLIENT_OK on completing paths too (see r_client_on_datagram). Calling it
// before the deadline is a safe no-op.
RCLIENT_API int r_client_on_timeout(
    r_client_t *client,
    r_client_req_t *req,
    uint64_t now_ms
);

// Cancellation.
RCLIENT_API void r_client_cancel_request(
    r_client_t *client,
    r_client_req_t *req
);

// Canonical content-defined identifier helpers. The byte strings may contain
// embedded NULs; pass their exact byte lengths.
RCLIENT_API void r_client_default_request_policy(
    r_request_policy_t *out_policy
);
RCLIENT_API int r_client_derive_bucket_id(
    const void *bucket_name,
    size_t bucket_name_len,
    uint32_t window_size_ms,
    uint32_t rate_limit,
    uint8_t out_id[16]
);
RCLIENT_API int r_client_derive_latency_tracker_id(
    const void *latency_tracker_name,
    size_t latency_tracker_name_len,
    uint32_t ttl_ms,
    uint32_t max_samples,
    uint32_t min_sample_threshold,
    uint8_t out_id[16]
);
RCLIENT_API int r_client_parse_auth_key(
    const char *encoded,
    r_auth_key_info_t *out_info
);
/* Format c-<key-id>.p0.ratelimitly.com; clears a provided buffer on error. */
RCLIENT_API int r_client_format_default_tenant_dns(
    uint64_t key_id,
    char *out,
    size_t out_capacity
);

#ifdef __cplusplus
}
#endif

#endif
