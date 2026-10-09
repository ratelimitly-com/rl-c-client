#ifndef PERF_PACER_H
#define PERF_PACER_H

#include <stdbool.h>
#include <stdint.h>

/* Pure fixed-rate scheduler shared by the perf client and its unit tests.
 * Every worker owns slots worker_id + n * worker_count. A slow worker skips
 * obsolete slots instead of sending a catch-up burst. */
typedef struct perf_pacer {
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t target_rps;
    uint64_t worker_count;
    uint64_t worker_id;
    uint64_t next_ordinal;
    uint64_t skipped_slots;
} perf_pacer_t;

static inline bool perf_pacer_init(
    perf_pacer_t *pacer,
    uint64_t start_ns,
    uint64_t target_rps,
    uint64_t worker_count,
    uint64_t worker_id,
    uint64_t duration_ns
) {
    if (!pacer || target_rps == 0u || target_rps > 1000000u || worker_count == 0u ||
        worker_id >= worker_count || duration_ns == 0u || duration_ns > UINT64_MAX - start_ns) {
        return false;
    }
    *pacer = (perf_pacer_t){
        .start_ns = start_ns,
        .end_ns = start_ns + duration_ns,
        .target_rps = target_rps,
        .worker_count = worker_count,
        .worker_id = worker_id
    };
    return true;
}

static inline bool perf_pacer_next(perf_pacer_t *pacer, uint64_t now_ns, uint64_t *due_ns) {
    if (!pacer || !due_ns || now_ns >= pacer->end_ns) {
        return false;
    }

    /* 128-bit arithmetic avoids overflow for multi-hour runs at high rates. */
    uint64_t elapsed_ns = now_ns > pacer->start_ns ? now_ns - pacer->start_ns : 0u;
    uint64_t current_slot = (uint64_t)(((__uint128_t)elapsed_ns * pacer->target_rps) / 1000000000u);
    uint64_t desired_ordinal = current_slot > pacer->worker_id
        ? (current_slot - pacer->worker_id) / pacer->worker_count
        : 0u;
    if (desired_ordinal > pacer->next_ordinal) {
        pacer->skipped_slots += desired_ordinal - pacer->next_ordinal;
        pacer->next_ordinal = desired_ordinal;
    }

    uint64_t slot = pacer->worker_id + pacer->next_ordinal * pacer->worker_count;
    uint64_t offset_ns = (uint64_t)(((__uint128_t)slot * 1000000000u) / pacer->target_rps);
    if (offset_ns >= pacer->end_ns - pacer->start_ns) {
        return false;
    }
    *due_ns = pacer->start_ns + offset_ns;
    pacer->next_ordinal++;
    return true;
}

#endif
