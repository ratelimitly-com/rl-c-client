#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/rl_example.h"

/*
 * Latency tracking has two distinct halves:
 *
 * Flow
 * ----
 * 1. A rate-limit request includes a guard for the protected service.
 * 2. The normal readiness/deadline loop waits for the guard decision.
 * 3. Only a passing request performs the protected operation.
 * 4. CLOCK_MONOTONIC measures that operation, not the RateLimitly round trip.
 * 5. r_client_report_latency() sends the observation without awaiting a reply.
 *
 * Ownership: check owns borrowed resource and guard storage until its callback.
 * Result arrays are copied during callback because rl-c-client owns them. The
 * report is borrowed only for the synchronous report call and can stay local.
 * rl_example_client_t continues to own client handle and UDP descriptors.
 *
 * This command-line example sleeps to stand in for a protected backend call.
 * Event-loop applications should time their actual asynchronous operation and
 * report from its completion callback; they should never sleep on loop thread.
 */

enum {
    DEFAULT_WORK_MS = 25,
    MAX_EXAMPLE_WORK_MS = 60000,
    LATENCY_THRESHOLD_MS = 100,
    LATENCY_TTL_MS = 10000,
    LATENCY_MAX_SAMPLES = 100,
    LATENCY_BUFFER_SIZE = 32,
    LATENCY_MIN_SAMPLE_THRESHOLD = 5,
};

static const char SERVICE_NAME[] = "example-inventory-backend";
static const char BUCKET_NAME[] = "example-latency-tracker";

typedef struct latency_check {
    rl_example_request_t request;
    r_latency_guard_t guard;
    int status;
    bool done;
    bool allowed;
    bool guard_passed;
    uint32_t current_latency_ms;
    uint32_t threshold_ms;
} latency_check_t;

static void on_check_complete(
    void *user,
    r_client_req_t *request,
    int status,
    const r_rate_limit_result_t *result
) {
    (void)request;
    latency_check_t *check = user;
    check->request.handle = NULL;
    check->request.active = false;
    check->status = status;
    check->done = true;

    if (status != RCLIENT_OK || !result) {
        return;
    }
    /* Result arrays stop being valid when this callback returns. Copy every
     * value needed by later application work. */
    if (result->guard_count != 1 || result->resource_count != 1
        || memcmp(result->guards[0].service_id,
            check->guard.service_id, sizeof(check->guard.service_id)) != 0
        || memcmp(result->resources[0].bucket_id,
            check->request.resource.bucket_id,
            sizeof(check->request.resource.bucket_id)) != 0) {
        check->status = RCLIENT_ERR_PROTOCOL;
        return;
    }
    check->allowed = result->success;
    check->guard_passed = result->guards[0].passed;
    check->current_latency_ms = result->guards[0].current_latency_ms;
    check->threshold_ms = result->guards[0].threshold_ms;
}

static void configure_check(latency_check_t *check) {
    memset(check, 0, sizeof(*check));

    r_client_hash_id(BUCKET_NAME, check->request.resource.bucket_id);
    check->request.resource.window_size_ms = 1000;
    check->request.resource.rate_limit = 100;
    check->request.resource.tokens_requested = 1;

    r_client_hash_id(SERVICE_NAME, check->guard.service_id);
    /* Shed work at 100 ms. Retain at most 100 samples for 10 seconds in a
     * 32-entry tracker, and require five samples before using its estimate.
     * Deployments should tune these policy values for their service SLO and
     * credential quotas. */
    check->guard.threshold_ms = LATENCY_THRESHOLD_MS;
    check->guard.ttl_ms = LATENCY_TTL_MS;
    check->guard.max_samples = LATENCY_MAX_SAMPLES;
    check->guard.buffer_size = LATENCY_BUFFER_SIZE;
    check->guard.min_sample_threshold = LATENCY_MIN_SAMPLE_THRESHOLD;
}

static int submit_check(
    rl_example_client_t *client,
    latency_check_t *check
) {
    check->request.active = true;
    int status = r_client_check_rate_limit_async_borrowed(
        client->handle,
        &check->request.resource,
        1,
        &check->guard,
        1,
        "latency-tracker-example",
        0,
        on_check_complete,
        check,
        &check->request.handle
    );
    if (status != RCLIENT_OK) {
        check->request.handle = NULL;
        check->request.active = false;
    }
    return status;
}

static int drive_check(
    rl_example_client_t *client,
    latency_check_t *check
) {
    while (!check->done) {
        struct pollfd descriptors[2] = {0};
        size_t count = rl_example_socket_count(client);
        if (count == 0 || count > 2) {
            return RCLIENT_ERR_CONFIG;
        }
        for (size_t i = 0; i < count; i++) {
            descriptors[i].fd = rl_example_socket_at(client, i);
            descriptors[i].events = POLLIN;
        }

        uint64_t delay_ms = 0;
        int status = rl_example_request_delay_ms(&check->request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        int timeout = delay_ms > INT_MAX ? INT_MAX : (int)delay_ms;
        int ready;
        do {
            ready = poll(descriptors, (nfds_t)count, timeout);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) {
            return RCLIENT_ERR_IO;
        }
        if (ready == 0) {
            status = rl_example_request_on_timeout(client, &check->request);
            if (status != RCLIENT_OK) {
                return status;
            }
            continue;
        }

        for (size_t i = 0; i < count; i++) {
            if ((descriptors[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return RCLIENT_ERR_IO;
            }
            if ((descriptors[i].revents & POLLIN) != 0) {
                status = rl_example_client_on_readable(client, descriptors[i].fd);
                if (status != RCLIENT_OK) {
                    return status;
                }
            }
        }
    }
    return check->status;
}

static int read_work_duration(uint32_t *work_ms) {
    *work_ms = DEFAULT_WORK_MS;
    const char *text = getenv("RATELIMITLY_EXAMPLE_WORK_MS");
    if (!text || text[0] == '\0') {
        return RCLIENT_OK;
    }

    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value > MAX_EXAMPLE_WORK_MS) {
        return RCLIENT_ERR_CONFIG;
    }
    *work_ms = (uint32_t)value;
    return RCLIENT_OK;
}

static int monotonic_ms(uint64_t *milliseconds) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return RCLIENT_ERR_IO;
    }
    *milliseconds = (uint64_t)value.tv_sec * 1000u
        + (uint64_t)value.tv_nsec / 1000000u;
    return RCLIENT_OK;
}

static int simulate_protected_work(uint32_t work_ms, uint32_t *observed_ms) {
    uint64_t started_ms = 0;
    int status = monotonic_ms(&started_ms);
    if (status != RCLIENT_OK) {
        return status;
    }

    struct timespec remaining = {
        .tv_sec = (time_t)(work_ms / 1000u),
        .tv_nsec = (long)(work_ms % 1000u) * 1000000L,
    };
    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) {
            return RCLIENT_ERR_IO;
        }
    }

    uint64_t finished_ms = 0;
    status = monotonic_ms(&finished_ms);
    if (status != RCLIENT_OK || finished_ms < started_ms) {
        return RCLIENT_ERR_IO;
    }
    uint64_t elapsed_ms = finished_ms - started_ms;
    *observed_ms = elapsed_ms > UINT32_MAX
        ? UINT32_MAX
        : (uint32_t)elapsed_ms;
    return RCLIENT_OK;
}

static int report_latency(
    rl_example_client_t *client,
    const r_latency_guard_t *guard,
    uint32_t observed_ms
) {
    /* Reports must repeat the guard's service id and tracker configuration so
     * the observation updates the same server-side latency tracker. */
    r_service_latency_report_t report = {
        .observed_latency = observed_ms,
        .ttl_ms = guard->ttl_ms,
        .max_samples = guard->max_samples,
        .buffer_size = guard->buffer_size,
        .min_sample_threshold = guard->min_sample_threshold,
    };
    memcpy(report.service_id, guard->service_id, sizeof(report.service_id));
    return r_client_report_latency(client->handle, &report, 1);
}

int main(void) {
    rl_example_options_t options;
    int status = rl_example_options_from_env(&options);
    if (status != RCLIENT_OK) {
        fprintf(stderr,
            "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY; "
            "RATELIMITLY_EXAMPLE_WORK_MS is optional\n");
        return EXIT_FAILURE;
    }

    uint32_t work_ms = 0;
    status = read_work_duration(&work_ms);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "RATELIMITLY_EXAMPLE_WORK_MS must be 0..%d\n",
            MAX_EXAMPLE_WORK_MS);
        return EXIT_FAILURE;
    }

    rl_example_client_t client;
    status = rl_example_client_init(&client, &options);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "client initialization failed: %s (%d)\n",
            rl_example_status_name(status), status);
        return EXIT_FAILURE;
    }

    latency_check_t check;
    configure_check(&check);
    status = submit_check(&client, &check);
    if (status == RCLIENT_OK) {
        status = drive_check(&client, &check);
    }
    if (status != RCLIENT_OK) {
        rl_example_request_cancel(&client, &check.request);
        fprintf(stderr, "latency-guard check failed: %s (%d)\n",
            rl_example_status_name(status), status);
        rl_example_client_destroy(&client);
        return EXIT_FAILURE;
    }

    printf("guard %s: current=%" PRIu32 " ms threshold=%" PRIu32 " ms\n",
        check.guard_passed ? "passed" : "failed",
        check.current_latency_ms,
        check.threshold_ms);
    if (!check.allowed) {
        /* No backend call occurred, so there is no latency sample to report. */
        rl_example_client_destroy(&client);
        return EXIT_SUCCESS;
    }

    uint32_t observed_ms = 0;
    status = simulate_protected_work(work_ms, &observed_ms);
    if (status == RCLIENT_OK) {
        status = report_latency(&client, &check.guard, observed_ms);
    }
    if (status != RCLIENT_OK) {
        fprintf(stderr, "latency report failed: %s (%d)\n",
            rl_example_status_name(status), status);
        rl_example_client_destroy(&client);
        return EXIT_FAILURE;
    }

    printf("latency reported: service=%s observed=%" PRIu32 " ms\n",
        SERVICE_NAME, observed_ms);
    rl_example_client_destroy(&client);
    return EXIT_SUCCESS;
}
