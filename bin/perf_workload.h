#ifndef PERF_WORKLOAD_H
#define PERF_WORKLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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
