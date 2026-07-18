#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. Submit one request containing a resource and a latency guard.
 * 2. Drive UDP readiness and the request deadline with poll().
 * 3. Run protected work only after both admission checks pass.
 * 4. Measure only that work with a monotonic clock.
 * 5. Report the completed operation's latency with the same guard identity.
 *
 * Ownership: app owns the runtime, request, and copied outcome until shutdown.
 * The public runtime owns its UDP sockets and client handle. The workflow owns
 * no heap memory and borrows its request storage until completion or cancel.
 */

enum {
    DEFAULT_WORK_MS = 25,
    MAX_WORK_MS = 60000,
};

typedef struct application {
    r_runtime_client_t runtime;
    r_admission_request_t request;
    r_admission_outcome_t outcome;
    int status;
    bool done;
} application_t;

static void on_admission(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    application_t *app = user;
    app->status = status;
    app->outcome = *outcome;
    app->done = true;
}

static void configure_admission(r_admission_config_t *config) {
    r_client_admission_config_defaults(config);
    config->bucket_name = "example-latency-tracker";
    config->service_name = "example-inventory-backend";
    config->metrics_label = "latency-tracker-example";
}

static int poll_client_sockets(application_t *app, int timeout_ms) {
    struct pollfd descriptors[2] = {0};
    size_t count = r_runtime_socket_count(&app->runtime);
    if (count == 0u || count > 2u) {
        return RCLIENT_ERR_CONFIG;
    }
    for (size_t i = 0; i < count; i++) {
        descriptors[i].fd = (int)r_runtime_socket_at(&app->runtime, i);
        descriptors[i].events = POLLIN;
    }

    int ready;
    do {
        ready = poll(descriptors, (nfds_t)count, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0) {
        return ready;
    }
    for (size_t i = 0; i < count; i++) {
        if ((descriptors[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return RCLIENT_ERR_IO;
        }
        if ((descriptors[i].revents & POLLIN) != 0) {
            int status = r_runtime_client_on_readable(
                &app->runtime,
                r_runtime_socket_at(&app->runtime, i)
            );
            if (status != RCLIENT_OK) {
                return status;
            }
        }
    }
    return ready;
}

static int wait_for_admission(application_t *app) {
    while (!app->done) {
        uint64_t deadline_ms = 0u;
        int status = r_client_admission_deadline_ms(
            &app->request,
            &deadline_ms
        );
        if (status != RCLIENT_OK) {
            return status;
        }
        uint64_t now_ms = r_runtime_wall_time_ms();
        uint64_t delay_ms = deadline_ms > now_ms ? deadline_ms - now_ms : 0u;
        int timeout_ms = delay_ms > INT_MAX ? INT_MAX : (int)delay_ms;
        int ready = poll_client_sockets(app, timeout_ms);
        if (ready < 0) {
            return ready == -1 ? RCLIENT_ERR_IO : ready;
        }
        if (ready == 0) {
            status = r_client_admission_on_timeout(
                app->runtime.handle,
                &app->request,
                r_runtime_wall_time_ms()
            );
            if (status != RCLIENT_OK) {
                return status;
            }
        }
    }
    return app->status;
}

static int read_work_duration(uint32_t *out_work_ms) {
    *out_work_ms = DEFAULT_WORK_MS;
    const char *text = getenv("RATELIMITLY_EXAMPLE_WORK_MS");
    if (!text || text[0] == '\0') {
        return RCLIENT_OK;
    }
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value > MAX_WORK_MS) {
        return RCLIENT_ERR_CONFIG;
    }
    *out_work_ms = (uint32_t)value;
    return RCLIENT_OK;
}

static int perform_protected_work(
    uint32_t work_ms,
    uint32_t *out_observed_ms
) {
    uint64_t started_ms = 0u;
    int status = r_runtime_monotonic_time_ms(&started_ms);
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

    uint64_t finished_ms = 0u;
    status = r_runtime_monotonic_time_ms(&finished_ms);
    if (status != RCLIENT_OK || finished_ms < started_ms) {
        return RCLIENT_ERR_IO;
    }
    uint64_t elapsed_ms = finished_ms - started_ms;
    *out_observed_ms = elapsed_ms > UINT32_MAX
        ? UINT32_MAX
        : (uint32_t)elapsed_ms;
    return RCLIENT_OK;
}

static void print_denial(const r_admission_outcome_t *outcome) {
    if (outcome->decision == R_ADMISSION_RATE_AND_LATENCY_LIMITED) {
        puts("denied: rate limit and latency guard both rejected the work");
    } else if (outcome->decision == R_ADMISSION_LATENCY_LIMITED) {
        printf("guard failed: current=%" PRIu32 " ms threshold=%" PRIu32 " ms\n",
            outcome->current_latency_ms,
            outcome->latency_threshold_ms);
    } else {
        puts("denied: resource rate limit rejected the work");
    }
}

static int run_example(application_t *app, uint32_t work_ms) {
    r_admission_config_t config;
    configure_admission(&config);
    int status = r_client_admission_start(
        app->runtime.handle,
        &app->request,
        &config,
        on_admission,
        app
    );
    if (status == RCLIENT_OK) {
        status = wait_for_admission(app);
    }
    if (status != RCLIENT_OK || !app->outcome.allowed) {
        if (status == RCLIENT_OK) {
            print_denial(&app->outcome);
        }
        return status;
    }

    puts("guard passed: resource and latency checks admitted the work");
    uint32_t observed_ms = 0u;
    status = perform_protected_work(work_ms, &observed_ms);
    if (status == RCLIENT_OK) {
        status = r_client_admission_report_latency(
            app->runtime.handle,
            &app->request,
            observed_ms
        );
    }
    if (status == RCLIENT_OK) {
        printf("latency reported: service=example-inventory-backend "
            "observed=%" PRIu32 " ms\n", observed_ms);
    }
    return status;
}

int main(void) {
    r_runtime_options_t options;
    int status = r_runtime_options_from_env(&options);
    uint32_t work_ms = 0u;
    if (status == RCLIENT_OK) {
        status = read_work_duration(&work_ms);
    }
    if (status != RCLIENT_OK) {
        fputs("set RATELIMITLY_AUTH_KEY; RATELIMITLY_TENANT is optional; "
            "RATELIMITLY_EXAMPLE_WORK_MS must be 0..60000\n", stderr);
        return EXIT_FAILURE;
    }

    application_t app = {0};
    status = r_runtime_client_init(&app.runtime, &options);
    if (status == RCLIENT_OK) {
        status = run_example(&app, work_ms);
    }
    if (app.request.active) {
        r_client_admission_cancel(app.runtime.handle, &app.request);
    }
    r_runtime_client_destroy(&app.runtime);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "example failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
