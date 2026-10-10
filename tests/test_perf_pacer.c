#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../bin/perf_pacer.h"

static void test_staggered_workers(void) {
    perf_pacer_t first = {0};
    perf_pacer_t second = {0};
    assert(perf_pacer_init(&first, 1000000000ull, 10000u, 2u, 0u, 1000000000ull));
    assert(perf_pacer_init(&second, 1000000000ull, 10000u, 2u, 1u, 1000000000ull));

    uint64_t due = 0;
    assert(perf_pacer_next(&first, 1000000000ull, &due));
    assert(due == 1000000000ull);
    assert(perf_pacer_next(&second, 1000000000ull, &due));
    assert(due == 1000100000ull);
    assert(perf_pacer_next(&first, 1000000000ull, &due));
    assert(due == 1000200000ull);
}

static void test_slow_worker_skips_slots_without_catchup_burst(void) {
    perf_pacer_t pacer = {0};
    assert(perf_pacer_init(&pacer, 1000000000ull, 10000u, 2u, 0u, 1000000000ull));

    uint64_t due = 0;
    assert(perf_pacer_next(&pacer, 1000000000ull, &due));
    assert(perf_pacer_next(&pacer, 1000750000ull, &due));
    assert(due == 1000800000ull);
    assert(pacer.skipped_slots == 3u);
    assert(perf_pacer_next(&pacer, 1000750000ull, &due));
    assert(due == 1001000000ull);
}

static void test_no_requests_past_end_and_invalid_configuration(void) {
    perf_pacer_t pacer = {0};
    uint64_t due = 0;
    assert(!perf_pacer_init(&pacer, 0u, 0u, 2u, 0u, 1000000000ull));
    assert(!perf_pacer_init(&pacer, 0u, 10000u, 2u, 2u, 1000000000ull));
    assert(perf_pacer_init(&pacer, 1000000000ull, 10000u, 2u, 0u, 1000000000ull));
    assert(!perf_pacer_next(&pacer, 2000000000ull, &due));
}

int main(void) {
    test_staggered_workers();
    test_slow_worker_skips_slots_without_catchup_burst();
    test_no_requests_past_end_and_invalid_configuration();
    puts("test_perf_pacer: ok");
    return 0;
}
