#include <assert.h>
#include <string.h>

#include "r_client_workflow.h"

static void ignore_completion(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    (void)user;
    (void)status;
    (void)outcome;
}

static void test_defaults_and_request_storage(void) {
    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "checkout-requests";
    config.service_name = "inventory-service";

    assert(config.window_size_ms == 1000u);
    assert(config.rate_limit == 100u);
    assert(config.tokens_requested == 1u);
    assert(config.latency_threshold_ms == 100u);
    assert(config.latency_ttl_ms == 10000u);
    assert(config.latency_max_samples == 100u);
    assert(config.latency_buffer_size == 32u);
    assert(config.latency_min_sample_threshold == 5u);

    r_admission_request_t request;
    assert(r_client_admission_prepare(
        &request,
        &config,
        ignore_completion,
        NULL
    ) == RCLIENT_OK);

    uint8_t expected[16];
    r_client_hash_id(config.bucket_name, expected);
    assert(memcmp(request.resource.bucket_id, expected, sizeof(expected)) == 0);
    r_client_hash_id(config.service_name, expected);
    assert(memcmp(request.guard.service_id, expected, sizeof(expected)) == 0);
    assert(request.resource.rate_limit == config.rate_limit);
    assert(request.guard.threshold_ms == config.latency_threshold_ms);
}

static r_admission_outcome_t classify(
    bool success,
    uint16_t tokens_deficit,
    bool guard_passed
) {
    r_resource_result_t resource = {.tokens_deficit = tokens_deficit};
    r_guard_result_t guard = {
        .passed = guard_passed,
        .current_latency_ms = 75u,
        .threshold_ms = 100u,
    };
    r_rate_limit_result_t result = {
        .success = success,
        .resources = &resource,
        .resource_count = 1u,
        .guards = &guard,
        .guard_count = 1u,
    };
    return r_client_admission_classify(RCLIENT_OK, &result);
}

static void test_outcome_classification(void) {
    r_admission_outcome_t outcome = classify(true, 0u, true);
    assert(outcome.decision == R_ADMISSION_ALLOWED);
    assert(outcome.allowed);

    outcome = classify(false, 1u, true);
    assert(outcome.decision == R_ADMISSION_RATE_LIMITED);
    assert(outcome.rate_limited);
    assert(!outcome.latency_limited);

    outcome = classify(false, 0u, false);
    assert(outcome.decision == R_ADMISSION_LATENCY_LIMITED);
    assert(!outcome.rate_limited);
    assert(outcome.latency_limited);
    assert(outcome.current_latency_ms == 75u);
    assert(outcome.latency_threshold_ms == 100u);

    outcome = classify(false, 2u, false);
    assert(outcome.decision == R_ADMISSION_RATE_AND_LATENCY_LIMITED);
    assert(outcome.rate_limited);
    assert(outcome.latency_limited);

    outcome = r_client_admission_classify(RCLIENT_ERR_TIMEOUT, NULL);
    assert(outcome.decision == R_ADMISSION_ERROR);
    assert(!outcome.allowed);
}

int main(void) {
    test_defaults_and_request_storage();
    test_outcome_classification();
    return 0;
}
