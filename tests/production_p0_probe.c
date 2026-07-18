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

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * This is a protocol smoke test, not a load test. It sends a small, bounded
 * number of requests to production and proves two pieces of server state:
 *
 *   1. A distinctive latency sample can be read back from the same tracker.
 *   2. A one-token rate bucket admits once and then rejects the next request.
 *
 * The shell wrapper supplies a per-run namespace so the test never relies on
 * state left by another CI job. The authentication key is read by the runtime
 * and is deliberately never copied into diagnostics.
 */

enum {
    MAX_NAMESPACE_LENGTH = 48,
    MAX_NAME_LENGTH = 96,
    MAX_WAIT_ITERATIONS = 8,
    MAX_INTERRUPTS = 4,
    LATENCY_POLL_ATTEMPTS = 20,
    LATENCY_POLL_DELAY_MS = 150,
    REPORTED_LATENCY_MS = 37,
    LATENCY_THRESHOLD_MS = 1000,
};

typedef struct admission_result {
    bool done;
    int status;
    r_admission_outcome_t outcome;
} admission_result_t;

static bool namespace_character_is_safe(char character) {
    return (character >= 'a' && character <= 'z')
        || (character >= 'A' && character <= 'Z')
        || (character >= '0' && character <= '9')
        || character == '-'
        || character == '_';
}

static bool namespace_is_safe(const char *name) {
    if (!name || name[0] == '\0') {
        return false;
    }
    size_t length = strlen(name);
    if (length > MAX_NAMESPACE_LENGTH) {
        return false;
    }
    for (size_t index = 0u; index < length; index++) {
        if (!namespace_character_is_safe(name[index])) {
            return false;
        }
    }
    return true;
}

static int format_scoped_name(
    char output[MAX_NAME_LENGTH],
    const char *test_namespace,
    const char *suffix
) {
    int length = snprintf(
        output,
        MAX_NAME_LENGTH,
        "p0-%s-%s",
        test_namespace,
        suffix
    );
    return length >= 0 && length < MAX_NAME_LENGTH
        ? RCLIENT_OK
        : RCLIENT_ERR_CONFIG;
}

static void on_admission(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    admission_result_t *result = user;
    result->status = status;
    if (outcome) {
        result->outcome = *outcome;
    }
    result->done = true;
}

static int poll_runtime(r_runtime_client_t *runtime, int timeout_ms) {
    struct pollfd descriptors[2] = {{0}};
    size_t count = r_runtime_socket_count(runtime);
    if (count == 0u || count > 2u) {
        return RCLIENT_ERR_CONFIG;
    }
    for (size_t index = 0u; index < count; index++) {
        descriptors[index].fd = (int)r_runtime_socket_at(runtime, index);
        descriptors[index].events = POLLIN;
    }

    int ready = -1;
    for (unsigned int attempt = 0u; attempt < MAX_INTERRUPTS; attempt++) {
        ready = poll(descriptors, (nfds_t)count, timeout_ms);
        if (ready >= 0 || errno != EINTR) {
            break;
        }
    }
    if (ready < 0) {
        return RCLIENT_ERR_IO;
    }
    for (size_t index = 0u; index < count && ready > 0; index++) {
        short error_events = POLLERR | POLLHUP | POLLNVAL;
        if ((descriptors[index].revents & error_events) != 0) {
            return RCLIENT_ERR_IO;
        }
        if ((descriptors[index].revents & POLLIN) != 0) {
            int status = r_runtime_client_on_readable(
                runtime,
                r_runtime_socket_at(runtime, index)
            );
            if (status != RCLIENT_OK) {
                return status;
            }
        }
    }
    return ready;
}

static int wait_for_admission(
    r_runtime_client_t *runtime,
    r_admission_request_t *request,
    admission_result_t *result
) {
    for (unsigned int iteration = 0u;
            iteration < MAX_WAIT_ITERATIONS && !result->done;
            iteration++) {
        uint64_t delay_ms = 0u;
        int status = r_runtime_admission_delay_ms(request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        int timeout_ms = delay_ms > (uint64_t)INT_MAX
            ? INT_MAX
            : (int)delay_ms;
        int ready = poll_runtime(runtime, timeout_ms);
        if (ready < 0) {
            return ready;
        }
        if (ready == 0 && !result->done) {
            status = r_runtime_admission_on_timeout(runtime, request);
            if (status != RCLIENT_OK) {
                return status;
            }
        }
    }
    if (!result->done) {
        r_runtime_admission_cancel(runtime, request);
        return RCLIENT_ERR_TIMEOUT;
    }
    return result->status;
}

static int run_admission(
    r_runtime_client_t *runtime,
    const r_admission_config_t *config,
    r_admission_request_t *request,
    r_admission_outcome_t *outcome
) {
    admission_result_t result = {0};
    memset(request, 0, sizeof(*request));
    int status = r_client_admission_start(
        runtime->handle,
        request,
        config,
        on_admission,
        &result
    );
    if (status == RCLIENT_OK) {
        status = wait_for_admission(runtime, request, &result);
    }
    if (request->active) {
        r_runtime_admission_cancel(runtime, request);
    }
    if (status == RCLIENT_OK) {
        *outcome = result.outcome;
    }
    return status;
}

static void delay_before_retry(void) {
    for (unsigned int attempt = 0u; attempt < MAX_INTERRUPTS; attempt++) {
        if (poll(NULL, 0u, LATENCY_POLL_DELAY_MS) >= 0 || errno != EINTR) {
            return;
        }
    }
}

static void configure_latency_probe(
    r_admission_config_t *config,
    const char *bucket_name,
    const char *service_name
) {
    r_client_admission_config_defaults(config);
    config->bucket_name = bucket_name;
    config->window_size_ms = 60000u;
    config->rate_limit = 1000u;
    config->service_name = service_name;
    config->latency_threshold_ms = LATENCY_THRESHOLD_MS;
    config->latency_ttl_ms = 10000u;
    /* One slot makes the real sample replace the speculative admission value. */
    config->latency_max_samples = 1u;
    config->latency_buffer_size = 1u;
    config->latency_min_sample_threshold = 1u;
    config->metrics_label = "production-p0-latency-probe";
}

static int prove_latency_tracker(
    r_runtime_client_t *runtime,
    const char *bucket_name,
    const char *service_name
) {
    r_admission_config_t config;
    configure_latency_probe(&config, bucket_name, service_name);

    r_admission_request_t initial_request;
    r_admission_outcome_t initial_outcome = {0};
    int status = run_admission(
        runtime,
        &config,
        &initial_request,
        &initial_outcome
    );
    if (status != RCLIENT_OK || !initial_outcome.allowed) {
        if (status == RCLIENT_OK) {
            fprintf(
                stderr,
                "production_p0_probe: fresh latency admission was denied "
                "(rate=%d, latency=%d, current=%" PRIu32 ")\n",
                initial_outcome.rate_limited,
                initial_outcome.latency_limited,
                initial_outcome.current_latency_ms
            );
        }
        return status == RCLIENT_OK ? RCLIENT_ERR_PROTOCOL : status;
    }
    status = r_client_admission_report_latency(
        runtime->handle,
        &initial_request,
        REPORTED_LATENCY_MS
    );
    if (status != RCLIENT_OK) {
        return status;
    }

    r_admission_outcome_t last_outcome = {0};
    for (unsigned int attempt = 0u;
            attempt < LATENCY_POLL_ATTEMPTS;
            attempt++) {
        r_admission_request_t request;
        r_admission_outcome_t outcome = {0};
        status = run_admission(runtime, &config, &request, &outcome);
        if (status != RCLIENT_OK) {
            return status;
        }
        if (outcome.allowed
                && !outcome.latency_limited
                && outcome.current_latency_ms == REPORTED_LATENCY_MS) {
            return RCLIENT_OK;
        }
        last_outcome = outcome;
        if (attempt + 1u < LATENCY_POLL_ATTEMPTS) {
            delay_before_retry();
        }
    }
    fprintf(
        stderr,
        "production_p0_probe: latency read-back expected=%d current=%" PRIu32
        " allowed=%d latency_limited=%d\n",
        REPORTED_LATENCY_MS,
        last_outcome.current_latency_ms,
        last_outcome.allowed,
        last_outcome.latency_limited
    );
    return RCLIENT_ERR_PROTOCOL;
}

static void configure_rate_probe(
    r_admission_config_t *config,
    const char *bucket_name,
    const char *service_name
) {
    r_client_admission_config_defaults(config);
    config->bucket_name = bucket_name;
    config->window_size_ms = 60000u;
    config->rate_limit = 1u;
    config->service_name = service_name;
    config->latency_threshold_ms = LATENCY_THRESHOLD_MS;
    config->latency_ttl_ms = 10000u;
    /* Keep two speculative samples below activation so only rate can deny. */
    config->latency_max_samples = 3u;
    config->latency_buffer_size = 3u;
    config->latency_min_sample_threshold = 3u;
    config->metrics_label = "production-p0-rate-probe";
}

static int prove_rate_limiter(
    r_runtime_client_t *runtime,
    const char *bucket_name,
    const char *service_name
) {
    r_admission_config_t config;
    configure_rate_probe(&config, bucket_name, service_name);

    r_admission_request_t first_request;
    r_admission_outcome_t first_outcome = {0};
    int status = run_admission(
        runtime,
        &config,
        &first_request,
        &first_outcome
    );
    if (status != RCLIENT_OK || !first_outcome.allowed) {
        if (status == RCLIENT_OK) {
            fprintf(
                stderr,
                "production_p0_probe: first rate admission was denied "
                "(rate=%d, latency=%d)\n",
                first_outcome.rate_limited,
                first_outcome.latency_limited
            );
        }
        return status == RCLIENT_OK ? RCLIENT_ERR_PROTOCOL : status;
    }

    r_admission_request_t second_request;
    r_admission_outcome_t second_outcome = {0};
    status = run_admission(
        runtime,
        &config,
        &second_request,
        &second_outcome
    );
    if (status != RCLIENT_OK) {
        return status;
    }
    if (second_outcome.allowed
            || !second_outcome.rate_limited
            || second_outcome.tokens_deficit == 0u
            || second_outcome.latency_limited) {
        fprintf(
            stderr,
            "production_p0_probe: second rate admission was not a pure "
            "rate denial (allowed=%d, rate=%d, deficit=%" PRIu16
            ", latency=%d)\n",
            second_outcome.allowed,
            second_outcome.rate_limited,
            second_outcome.tokens_deficit,
            second_outcome.latency_limited
        );
        return RCLIENT_ERR_PROTOCOL;
    }
    return RCLIENT_OK;
}

static int report_failure(const char *phase, int status) {
    fprintf(
        stderr,
        "production_p0_probe: %s failed: %s (%d)\n",
        phase,
        r_runtime_status_name(status),
        status
    );
    return EXIT_FAILURE;
}

int main(void) {
    const char *test_namespace = getenv("RATELIMITLY_P0_TEST_NAMESPACE");
    if (!namespace_is_safe(test_namespace)) {
        fputs("production_p0_probe: invalid test namespace\n", stderr);
        return EXIT_FAILURE;
    }

    char latency_bucket[MAX_NAME_LENGTH];
    char latency_service[MAX_NAME_LENGTH];
    char rate_bucket[MAX_NAME_LENGTH];
    char rate_service[MAX_NAME_LENGTH];
    if (format_scoped_name(
            latency_bucket,
            test_namespace,
            "latency-bucket"
        ) != RCLIENT_OK
        || format_scoped_name(
            latency_service,
            test_namespace,
            "latency-service"
        ) != RCLIENT_OK
        || format_scoped_name(rate_bucket, test_namespace, "rate-bucket")
            != RCLIENT_OK
        || format_scoped_name(rate_service, test_namespace, "rate-service")
            != RCLIENT_OK) {
        fputs("production_p0_probe: scoped name is too long\n", stderr);
        return EXIT_FAILURE;
    }

    r_runtime_options_t options;
    int status = r_runtime_options_from_env(&options);
    if (status != RCLIENT_OK) {
        return report_failure("configuration", status);
    }

    r_runtime_client_t runtime;
    status = r_runtime_client_init(&runtime, &options);
    if (status != RCLIENT_OK) {
        return report_failure("client initialization", status);
    }

    status = prove_latency_tracker(
        &runtime,
        latency_bucket,
        latency_service
    );
    if (status != RCLIENT_OK) {
        r_runtime_client_destroy(&runtime);
        return report_failure("latency tracker proof", status);
    }
    status = prove_rate_limiter(&runtime, rate_bucket, rate_service);
    r_runtime_client_destroy(&runtime);
    if (status != RCLIENT_OK) {
        return report_failure("rate limiter proof", status);
    }

    puts("production_p0_probe: PASS (latency read-back, rate denial)");
    return EXIT_SUCCESS;
}
