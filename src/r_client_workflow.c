#include "../include/r_client_workflow.h"

#include <string.h>

static const char R_DEFAULT_METRICS_LABEL[] = "rl-c-client-example";

void r_client_admission_config_defaults(r_admission_config_t *out_config) {
    if (!out_config) {
        return;
    }
    *out_config = (r_admission_config_t){
        .window_size_ms = 1000u,
        .rate_limit = 100u,
        .tokens_requested = 1u,
        .latency_threshold_ms = 100u,
        .latency_ttl_ms = 10000u,
        .latency_max_samples = 100u,
        .latency_buffer_size = 32u,
        .latency_min_sample_threshold = 5u,
        .metrics_label = R_DEFAULT_METRICS_LABEL,
    };
}

static bool r_admission_config_valid(const r_admission_config_t *config) {
    return config
        && config->bucket_name
        && config->bucket_name[0] != '\0'
        && config->service_name
        && config->service_name[0] != '\0'
        && config->window_size_ms > 0u
        && config->rate_limit > 0u
        && config->tokens_requested > 0u
        && config->latency_threshold_ms > 0u
        && config->latency_ttl_ms > 0u
        && config->latency_max_samples > 0u
        && config->latency_buffer_size > 0u
        && config->latency_min_sample_threshold > 0u;
}

int r_client_admission_prepare(
    r_admission_request_t *request,
    const r_admission_config_t *config,
    r_admission_cb callback,
    void *user
) {
    if (!request || !callback || !r_admission_config_valid(config)) {
        return RCLIENT_ERR_CONFIG;
    }

    memset(request, 0, sizeof(*request));
    r_client_hash_id(config->bucket_name, request->resource.bucket_id);
    request->resource.window_size_ms = config->window_size_ms;
    request->resource.rate_limit = config->rate_limit;
    request->resource.tokens_requested = config->tokens_requested;

    r_client_hash_id(config->service_name, request->guard.service_id);
    request->guard.threshold_ms = config->latency_threshold_ms;
    request->guard.ttl_ms = config->latency_ttl_ms;
    request->guard.max_samples = config->latency_max_samples;
    request->guard.buffer_size = config->latency_buffer_size;
    request->guard.min_sample_threshold = config->latency_min_sample_threshold;

    request->metrics_label = config->metrics_label
        ? config->metrics_label
        : R_DEFAULT_METRICS_LABEL;
    request->callback = callback;
    request->user = user;
    request->outcome.decision = R_ADMISSION_ERROR;
    return RCLIENT_OK;
}

r_admission_outcome_t r_client_admission_classify(
    int status,
    const r_rate_limit_result_t *result
) {
    r_admission_outcome_t outcome = {.decision = R_ADMISSION_ERROR};
    if (status != RCLIENT_OK || !result) {
        return outcome;
    }
    if (result->success) {
        outcome.decision = R_ADMISSION_ALLOWED;
        outcome.allowed = true;
        return outcome;
    }

    for (size_t i = 0; i < result->resource_count; i++) {
        if (result->resources[i].tokens_deficit > 0u) {
            outcome.rate_limited = true;
            break;
        }
    }
    for (size_t i = 0; i < result->guard_count; i++) {
        if (!result->guards[i].passed) {
            outcome.latency_limited = true;
            outcome.current_latency_ms = result->guards[i].current_latency_ms;
            outcome.latency_threshold_ms = result->guards[i].threshold_ms;
            break;
        }
    }

    /* A denial without a failed guard is conservatively a resource denial. */
    if (!outcome.rate_limited && !outcome.latency_limited) {
        outcome.rate_limited = true;
    }
    if (outcome.rate_limited && outcome.latency_limited) {
        outcome.decision = R_ADMISSION_RATE_AND_LATENCY_LIMITED;
    } else if (outcome.latency_limited) {
        outcome.decision = R_ADMISSION_LATENCY_LIMITED;
    } else {
        outcome.decision = R_ADMISSION_RATE_LIMITED;
    }
    return outcome;
}

static void r_admission_complete(
    void *user,
    r_client_req_t *client_request,
    int status,
    const r_rate_limit_result_t *result
) {
    (void)client_request;
    r_admission_request_t *request = user;
    request->handle = NULL;
    request->active = false;
    request->outcome = r_client_admission_classify(status, result);
    request->admitted = request->outcome.allowed;
    request->callback(request->user, status, &request->outcome);
}

int r_client_admission_start(
    r_client_t *client,
    r_admission_request_t *request,
    const r_admission_config_t *config,
    r_admission_cb callback,
    void *user
) {
    if (!client) {
        return RCLIENT_ERR_CONFIG;
    }
    int status = r_client_admission_prepare(request, config, callback, user);
    if (status != RCLIENT_OK) {
        return status;
    }

    request->active = true;
    status = r_client_check_rate_limit_async_borrowed(
        client,
        &request->resource,
        1u,
        &request->guard,
        1u,
        request->metrics_label,
        0u,
        r_admission_complete,
        request,
        &request->handle
    );
    if (status != RCLIENT_OK) {
        request->handle = NULL;
        request->active = false;
    }
    return status;
}

int r_client_admission_deadline_ms(
    const r_admission_request_t *request,
    uint64_t *out_deadline_ms
) {
    if (!request || !request->active || !request->handle || !out_deadline_ms) {
        return RCLIENT_ERR_CONFIG;
    }
    return r_client_request_deadline_ms(request->handle, out_deadline_ms);
}

int r_client_admission_on_timeout(
    r_client_t *client,
    r_admission_request_t *request,
    uint64_t now_ms
) {
    if (!client || !request || !request->active || !request->handle) {
        return RCLIENT_ERR_CONFIG;
    }
    return r_client_on_timeout(client, request->handle, now_ms);
}

void r_client_admission_cancel(
    r_client_t *client,
    r_admission_request_t *request
) {
    if (!client || !request || !request->active || !request->handle) {
        return;
    }
    r_client_cancel_request(client, request->handle);
    request->handle = NULL;
    request->active = false;
    request->admitted = false;
}

int r_client_admission_report_latency(
    r_client_t *client,
    r_admission_request_t *request,
    uint32_t observed_latency_ms
) {
    if (!client || !request || request->active || !request->admitted
        || request->latency_reported) {
        return RCLIENT_ERR_CONFIG;
    }

    r_service_latency_report_t report = {
        .observed_latency = observed_latency_ms,
        .ttl_ms = request->guard.ttl_ms,
        .max_samples = request->guard.max_samples,
        .buffer_size = request->guard.buffer_size,
        .min_sample_threshold = request->guard.min_sample_threshold,
    };
    memcpy(report.service_id, request->guard.service_id, sizeof(report.service_id));
    int status = r_client_report_latency(client, &report, 1u);
    if (status == RCLIENT_OK) {
        request->latency_reported = true;
    }
    return status;
}
