#ifndef PERF_LATENCY_HIST_H
#define PERF_LATENCY_HIST_H

#include <stdint.h>

/* Successful logical-request latency histogram: 1 us bins to 10 ms,
 * 10 us bins to 100 ms, 100 us bins to 1 s, then one overflow bin. */
#define PERF_LATENCY_OVERFLOW_BUCKET 28000u
#define PERF_LATENCY_BUCKETS (PERF_LATENCY_OVERFLOW_BUCKET + 1u)

static inline uint32_t perf_latency_bucket(uint64_t us) {
    if (us < 10000u) {
        return (uint32_t)us;
    }
    if (us < 100000u) {
        return 10000u + (uint32_t)((us - 10000u) / 10u);
    }
    if (us < 1000000u) {
        return 19000u + (uint32_t)((us - 100000u) / 100u);
    }
    return PERF_LATENCY_OVERFLOW_BUCKET;
}

static inline uint64_t perf_latency_bucket_upper_us(uint32_t bucket) {
    if (bucket < 10000u) {
        return (uint64_t)bucket + 1u;
    }
    if (bucket < 19000u) {
        return 10000u + ((uint64_t)(bucket - 10000u) + 1u) * 10u;
    }
    if (bucket < PERF_LATENCY_OVERFLOW_BUCKET) {
        return 100000u + ((uint64_t)(bucket - 19000u) + 1u) * 100u;
    }
    return 1000000u;
}

static inline uint64_t perf_latency_percentile_upper_us(
    const uint64_t *hist,
    uint64_t total,
    uint32_t percentile
) {
    if (!hist || total == 0u || percentile == 0u || percentile > 100u) {
        return 0u;
    }
    uint64_t rank = (uint64_t)(((__uint128_t)total * percentile + 99u) / 100u);
    uint64_t seen = 0u;
    for (uint32_t bucket = 0u; bucket < PERF_LATENCY_BUCKETS; bucket++) {
        seen += hist[bucket];
        if (seen >= rank) {
            return perf_latency_bucket_upper_us(bucket);
        }
    }
    return 0u;
}

#endif
