#ifndef PERF_WORKLOAD_H
#define PERF_WORKLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../include/r_client.h"

typedef enum perf_outcome {
    PERF_OUTCOME_ERROR = 0,
    PERF_OUTCOME_GRANTED,
    PERF_OUTCOME_DENIED
} perf_outcome_t;

static inline perf_outcome_t perf_request_outcome(int status, const r_rate_limit_result_t *result) {
    if (status != RCLIENT_OK || !result) {
        return PERF_OUTCOME_ERROR;
    }
    return result->success ? PERF_OUTCOME_GRANTED : PERF_OUTCOME_DENIED;
}

/* One name means one server-side bucket, even with multiple load workers. */
static inline bool perf_bucket_name(
    char *out,
    size_t out_len,
    const char *prefix,
    size_t client_id,
    bool shared
) {
    if (!out || out_len == 0u || !prefix || prefix[0] == '\0') {
        return false;
    }
    size_t prefix_len = strlen(prefix);
    if (prefix_len >= out_len) {
        return false;
    }
    if (shared) {
        memcpy(out, prefix, prefix_len + 1u);
        return true;
    }
    int written = snprintf(out + prefix_len, out_len - prefix_len, "_%zu", client_id);
    return written >= 0 && (size_t)written < out_len - prefix_len;
}

#endif
