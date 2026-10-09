#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../bin/perf_latency_hist.h"

static void test_bucket_resolution_and_tail(void) {
    assert(perf_latency_bucket(9999u) == 9999u);
    assert(perf_latency_bucket(10000u) == 10000u);
    assert(perf_latency_bucket(10009u) == 10000u);
    assert(perf_latency_bucket(100000u) == 19000u);
    assert(perf_latency_bucket(1000000u) == PERF_LATENCY_OVERFLOW_BUCKET);
    assert(perf_latency_bucket_upper_us(9999u) == 10000u);
    assert(perf_latency_bucket_upper_us(10000u) == 10010u);
}

static void test_p99_uses_successful_samples(void) {
    uint64_t hist[PERF_LATENCY_BUCKETS] = {0};
    for (uint64_t us = 1; us <= 100; us++) {
        hist[perf_latency_bucket(us)]++;
    }
    assert(perf_latency_percentile_upper_us(hist, 100u, 99u) == 100u);
    assert(perf_latency_percentile_upper_us(hist, 100u, 50u) == 51u);
    assert(perf_latency_percentile_upper_us(hist, 0u, 99u) == 0u);
}

int main(void) {
    test_bucket_resolution_and_tail();
    test_p99_uses_successful_samples();
    puts("test_perf_latency_hist: ok");
    return 0;
}
