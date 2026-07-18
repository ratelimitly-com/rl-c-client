#ifndef R_CLIENT_H
#define R_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "r_client_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Includes space for c-<uint64>.p0.ratelimitly.com and its trailing NUL. */
#define R_CLIENT_DEFAULT_TENANT_DNS_CAPACITY 42u

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
    r_auth_type_t type;
    uint64_t key_id;
    uint8_t secret[32];
    size_t secret_len;
    uint32_t rate_buckets_max;
    uint32_t latency_services_max;
    uint32_t metrics_labels_max;
    uint32_t latency_buffer_size_max;
    uint32_t dedup_ttl_ms_max;
} r_auth_key_info_t;

typedef struct r_tenant_config {
    /* NULL/empty derives c-<key-id>.p0.ratelimitly.com from the auth key. */
    const char *dns_name;
    /* Zero derives the identifier from the auth key; nonzero must match it. */
    uint64_t key_id;
    r_auth_config_t auth;
} r_tenant_config_t;

// Request policy surface for wait, quorum, retry, and DNS refresh behavior.
typedef enum r_wait_policy {
    R_WAIT_RETURN_ON_FIRST_VALID = 0,
    R_WAIT_RETURN_ON_FIRST_STABLE = 1,
    R_WAIT_FOR_DEADLINE = 2,
    R_WAIT_FOR_QUORUM = 3,
} r_wait_policy_t;

typedef enum r_response_quorum_kind {
    R_QUORUM_ONE = 0,
    R_QUORUM_MAJORITY = 1,
    R_QUORUM_ALL = 2,
    R_QUORUM_COUNT = 3,
} r_response_quorum_kind_t;

typedef struct r_response_quorum {
    r_response_quorum_kind_t kind;
    size_t count; // used when kind == R_QUORUM_COUNT
} r_response_quorum_t;

typedef enum r_quorum_requirement {
    R_QUORUM_SOFT = 0,
    R_QUORUM_HARD = 1,
} r_quorum_requirement_t;

typedef enum r_select_policy {
    R_SELECT_FIRST_VALID = 0,
    R_SELECT_BEST_BY_RELIABILITY = 1,
    R_SELECT_CONSERVATIVE_DENY = 2,
} r_select_policy_t;

typedef enum r_retry_on {
    R_RETRY_TIMEOUT_ONLY = 0,
    R_RETRY_QUORUM_NOT_MET = 1,
    R_RETRY_INCONSISTENT = 2,
    R_RETRY_NEVER = 3,
} r_retry_on_t;

typedef enum r_resend_policy {
    R_RESEND_ALL = 0,
    R_RESEND_MISSING_ONLY = 1,
} r_resend_policy_t;

typedef enum r_backoff_kind {
    R_BACKOFF_NONE = 0,
    R_BACKOFF_FIXED = 1,
    R_BACKOFF_EXPONENTIAL = 2,
} r_backoff_kind_t;

typedef struct r_backoff_policy {
    r_backoff_kind_t kind;
    uint64_t delay_ms;     // fixed
    uint64_t base_delay_ms; // exponential
    uint64_t max_delay_ms;  // exponential
    uint64_t jitter_ms;     // exponential
} r_backoff_policy_t;

typedef enum r_dns_resync_on {
    R_DNS_INTERVAL_ONLY = 0,
    R_DNS_ON_TIMEOUT = 1,
    R_DNS_ON_RETRY = 2,
    R_DNS_ON_QUORUM_MISS = 3,
    R_DNS_ON_ANY_ERROR = 4,
} r_dns_resync_on_t;

typedef struct r_dns_resync_policy {
    r_dns_resync_on_t on;
    uint64_t refresh_interval_ms; // periodic DNS refresh; 0 uses default
    uint64_t min_interval_ms;
    uint64_t jitter_ms;
} r_dns_resync_policy_t;

typedef struct r_retry_policy {
    uint32_t retry_attempts;
    r_retry_on_t retry_on;
    r_backoff_policy_t backoff;
    r_resend_policy_t resend;
    bool refresh_dns_on_retry;
    uint64_t total_timeout_ms; // 0 means none
} r_retry_policy_t;

typedef struct r_request_policy {
    uint64_t attempt_timeout_ms;
    uint32_t dedup_ttl_ms;
    r_wait_policy_t wait;
    r_response_quorum_t quorum;
    r_quorum_requirement_t quorum_requirement;
    r_select_policy_t select;
    r_retry_policy_t retry;
    r_dns_resync_policy_t dns_resync;
} r_request_policy_t;

// Client configuration.
typedef struct r_client_config {
    r_tenant_config_t tenant;
    uint64_t server_stability_threshold_ms;
    // Borrowed during r_client_create; NULL selects default behavior.
    const r_request_policy_t *request_policy;
} r_client_config_t;

// Request inputs.
typedef struct r_resource_request {
    uint8_t bucket_id[16];
    uint32_t window_size_ms;
    uint32_t rate_limit;
    uint16_t tokens_requested;
} r_resource_request_t;

typedef struct r_latency_guard {
    uint8_t service_id[16];
    uint32_t threshold_ms;
    uint32_t ttl_ms;
    uint32_t max_samples;
    uint32_t buffer_size;
    uint32_t min_sample_threshold;
} r_latency_guard_t;

typedef struct r_service_latency_report {
    uint8_t service_id[16];
    uint32_t observed_latency;
    uint32_t ttl_ms;
    uint32_t max_samples;
    uint32_t buffer_size;
    uint32_t min_sample_threshold;
} r_service_latency_report_t;

// Result structures (valid only during callback).
typedef struct r_guard_result {
    uint8_t service_id[16];
    uint32_t threshold_ms;
    uint32_t current_latency_ms;
    bool passed;
} r_guard_result_t;

typedef struct r_resource_result {
    uint8_t bucket_id[16];
    uint16_t tokens_deficit;
    uint32_t actual_rate;
} r_resource_result_t;

typedef struct r_rate_limit_result {
    bool success;
    uint64_t server_id;
    bool steering_feedback;
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
int r_client_create(
    const r_client_config_t *config,
    const r_io_ops_t *io_ops,
    const r_resolver_ops_t *resolver_ops,
    r_client_t **out_client
);

void r_client_destroy(r_client_t *client);

// Async rate limit request.
int r_client_check_rate_limit_async(
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

// Async rate limit request using caller-owned buffers (no internal copies).
// Caller must keep buffers alive until the callback is invoked.
int r_client_check_rate_limit_async_borrowed(
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
// calls (30 reports per call is always within capacity).
int r_client_report_latency(
    r_client_t *client,
    const r_service_latency_report_t *reports,
    size_t report_count
);

// Datagram ingress from host.
int r_client_on_datagram(
    r_client_t *client,
    const uint8_t *buf,
    size_t len,
    const r_addr_t *from
);

// Per-request timer support.
int r_client_request_deadline_ms(
    const r_client_req_t *req,
    uint64_t *out_deadline_ms
);

int r_client_on_timeout(
    r_client_t *client,
    r_client_req_t *req,
    uint64_t now_ms
);

// Cancellation.
void r_client_cancel_request(r_client_t *client, r_client_req_t *req);

// Helpers.
void r_client_default_request_policy(r_request_policy_t *out_policy);
void r_client_hash_id(const char *input, uint8_t out_id[16]);
int r_client_parse_auth_key(const char *encoded, r_auth_key_info_t *out_info);
/* Format c-<key-id>.p0.ratelimitly.com; clears a provided buffer on error. */
int r_client_format_default_tenant_dns(
    uint64_t key_id,
    char *out,
    size_t out_capacity
);

#ifdef __cplusplus
}
#endif

#endif
