#ifndef R_CLIENT_WORKFLOW_H
#define R_CLIENT_WORKFLOW_H

#include "r_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The combined reason protected work was admitted or rejected. */
typedef enum r_admission_decision {
    R_ADMISSION_ALLOWED = 0,
    R_ADMISSION_RATE_LIMITED = 1,
    R_ADMISSION_LATENCY_LIMITED = 2,
    R_ADMISSION_RATE_AND_LATENCY_LIMITED = 3,
    R_ADMISSION_ERROR = 4,
} r_admission_decision_t;

/*
 * One resource limit and one latency guard. The defaults are intentionally
 * useful for examples, but production callers should tune them for their SLO
 * and credential quotas.
 */
typedef struct r_admission_config {
    const char *bucket_name;
    uint32_t window_size_ms;
    uint32_t rate_limit;
    uint16_t tokens_requested;

    const char *service_name;
    uint32_t latency_threshold_ms;
    uint32_t latency_ttl_ms;
    uint32_t latency_max_samples;
    uint32_t latency_buffer_size;
    uint32_t latency_min_sample_threshold;

    const char *metrics_label;
} r_admission_config_t;

/* A durable copy of the decision; no server-owned result pointers escape. */
typedef struct r_admission_outcome {
    r_admission_decision_t decision;
    bool allowed;
    bool rate_limited;
    bool latency_limited;
    /* First guard when admitted; first failed guard when rejected. */
    uint32_t current_latency_ms;
    uint32_t latency_threshold_ms;
} r_admission_outcome_t;

typedef void (*r_admission_cb)(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
);

/*
 * Caller-owned storage for one asynchronous admission check. Keep it alive
 * until the callback runs or r_client_admission_cancel() is called. Treat all
 * fields as read-only after preparation; they are public only to permit stack
 * allocation without a separate allocator.
 */
typedef struct r_admission_request {
    r_client_req_t *handle;
    r_resource_request_t resource;
    r_latency_guard_t guard;
    const char *metrics_label;
    r_admission_cb callback;
    void *user;
    r_admission_outcome_t outcome;
    bool active;
    bool admitted;
    bool latency_reported;
} r_admission_request_t;

void r_client_admission_config_defaults(r_admission_config_t *out_config);

/* Prepare storage without submitting I/O. Useful for validation and tests. */
int r_client_admission_prepare(
    r_admission_request_t *request,
    const r_admission_config_t *config,
    r_admission_cb callback,
    void *user
);

/* Convert a callback result into an explicit, durable application decision. */
r_admission_outcome_t r_client_admission_classify(
    int status,
    const r_rate_limit_result_t *result
);

/* Prepare and submit one combined rate-limit and latency-guard check. */
int r_client_admission_start(
    r_client_t *client,
    r_admission_request_t *request,
    const r_admission_config_t *config,
    r_admission_cb callback,
    void *user
);

int r_client_admission_deadline_ms(
    const r_admission_request_t *request,
    uint64_t *out_deadline_ms
);

int r_client_admission_on_timeout(
    r_client_t *client,
    r_admission_request_t *request,
    uint64_t now_ms
);

void r_client_admission_cancel(
    r_client_t *client,
    r_admission_request_t *request
);

/*
 * Report one measured protected-operation duration. This succeeds only after
 * admission, never for denied/cancelled work, and at most once per request.
 */
int r_client_admission_report_latency(
    r_client_t *client,
    r_admission_request_t *request,
    uint32_t observed_latency_ms
);

#ifdef __cplusplus
}
#endif

#endif
