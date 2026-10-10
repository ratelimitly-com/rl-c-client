#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "../bin/perf_workload.h"

int main(void) {
    char first[32];
    char second[32];

    assert(perf_bucket_name(first, sizeof(first), "load", 0u, true));
    assert(perf_bucket_name(second, sizeof(second), "load", 1u, true));
    assert(strcmp(first, "load") == 0);
    assert(strcmp(first, second) == 0);

    assert(perf_bucket_name(first, sizeof(first), "load", 0u, false));
    assert(perf_bucket_name(second, sizeof(second), "load", 1u, false));
    assert(strcmp(first, "load_0") == 0);
    assert(strcmp(second, "load_1") == 0);

    assert(!perf_bucket_name(first, 4u, "load", 0u, true));
    assert(!perf_bucket_name(first, 4u, "load", 0u, false));
    assert(!perf_bucket_name(first, sizeof(first), "", 0u, true));

    r_rate_limit_result_t result = {0};
    result.success = true;
    assert(perf_request_outcome(RCLIENT_OK, &result) == PERF_OUTCOME_GRANTED);
    result.success = false;
    assert(perf_request_outcome(RCLIENT_OK, &result) == PERF_OUTCOME_DENIED);
    assert(perf_request_outcome(RCLIENT_OK, NULL) == PERF_OUTCOME_ERROR);
    assert(perf_request_outcome(RCLIENT_ERR_CONFIG, &result) == PERF_OUTCOME_ERROR);
    return 0;
}
