#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <ulfius.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. Ulfius dispatches GET /limited on a libmicrohttpd connection thread.
 * 2. The callback creates a private runtime and starts combined admission.
 * 3. poll(2) waits for UDP readiness or the current request deadline.
 * 4. Allowed work is measured/reported, then both decisions map to HTTP.
 *
 * Ownership: one callback owns one runtime, its sockets, and admission request;
 * no client state is shared. This blocks only that connection thread. Higher
 * volume services should use the dedicated bridge pattern in Onion/CivetWeb.
 */

typedef struct check_result {
    bool done;
    int status;
    r_admission_outcome_t outcome;
} check_result_t;

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static void on_admission(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    check_result_t *result = user;
    result->done = true;
    result->status = status;
    result->outcome = *outcome;
}

static int wait_for_result(
    r_runtime_client_t *runtime,
    r_admission_request_t *request,
    check_result_t *result
) {
    while (!result->done) {
        struct pollfd sockets[2] = {0};
        size_t socket_count = r_runtime_socket_count(runtime);
        if (socket_count == 0 || socket_count > 2) {
            return RCLIENT_ERR_IO;
        }
        for (size_t i = 0; i < socket_count; i++) {
            sockets[i].fd = r_runtime_socket_at(runtime, i);
            sockets[i].events = POLLIN;
        }

        /* The rl-c-client deadline remains the sole timeout policy. */
        uint64_t delay_ms = 0;
        int status = r_runtime_admission_delay_ms(request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        int timeout_ms = delay_ms > INT_MAX ? INT_MAX : (int)delay_ms;
        int ready = poll(sockets, (nfds_t)socket_count, timeout_ms);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            return RCLIENT_ERR_IO;
        }
        if (ready == 0) {
            status = r_runtime_admission_on_timeout(runtime, request);
            if (status != RCLIENT_OK) {
                return status;
            }
            continue;
        }
        for (size_t i = 0; i < socket_count; i++) {
            if ((sockets[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return RCLIENT_ERR_IO;
            }
            if ((sockets[i].revents & POLLIN) != 0) {
                status = r_runtime_client_on_readable(runtime, sockets[i].fd);
                if (status != RCLIENT_OK) {
                    return status;
                }
            }
        }
    }
    return result->status;
}

static int perform_protected_work(void *user) {
    bool *completed = user;
    /* Replace this with the application operation the endpoint protects. */
    *completed = true;
    return RCLIENT_OK;
}

static int run_check(
    const r_runtime_options_t *options,
    r_admission_outcome_t *outcome
) {
    /* Runtime state is kept off libmicrohttpd's callback stack. */
    r_runtime_client_t *runtime = calloc(1, sizeof(*runtime));
    if (!runtime) {
        return RCLIENT_ERR_NOMEM;
    }
    int status = r_runtime_client_init(runtime, options);
    if (status != RCLIENT_OK) {
        free(runtime);
        return status;
    }

    check_result_t result = {0};
    r_admission_request_t request = {0};
    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "ulfius-example";
    config.service_name = "ulfius-protected-service";
    config.metrics_label = "ulfius-example";
    status = r_client_admission_start(
        runtime->handle,
        &request,
        &config,
        on_admission,
        &result
    );
    if (status == RCLIENT_OK) {
        status = wait_for_result(runtime, &request, &result);
    }
    if (request.active) {
        r_runtime_admission_cancel(runtime, &request);
    }
    if (status == RCLIENT_OK) {
        *outcome = result.outcome;
        if (result.outcome.allowed) {
            bool protected_work_complete = false;
            int report_status = r_runtime_admission_run_and_report(
                runtime,
                &request,
                perform_protected_work,
                &protected_work_complete,
                NULL
            );
            if (report_status != RCLIENT_OK) {
                fprintf(stderr, "latency report failed: %s (%d)\n",
                    r_runtime_status_name(report_status), report_status);
            }
        }
    }
    r_runtime_client_destroy(runtime);
    free(runtime);
    return status;
}

static int limited(
    const struct _u_request *request,
    struct _u_response *response,
    void *user_data
) {
    (void)request;
    r_admission_outcome_t outcome = {.decision = R_ADMISSION_ERROR};
    int status = run_check(user_data, &outcome);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "rate-limit check failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        ulfius_set_string_body_response(response, 503,
            "rate-limit service unavailable\n");
    } else if (outcome.latency_limited && !outcome.rate_limited) {
        ulfius_set_string_body_response(response, 503,
            "denied by latency guard\n");
    } else if (!outcome.allowed) {
        ulfius_set_string_body_response(response, 429,
            "denied by resource rate limit\n");
    } else {
        ulfius_set_string_body_response(response, 200, "allowed\n");
    }
    u_map_put(response->map_header, "Content-Type", "text/plain");
    return U_CALLBACK_COMPLETE;
}

int main(void) {
    r_runtime_options_t options;
    if (r_runtime_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n");
        return EXIT_FAILURE;
    }

    struct _u_instance instance;
    if (ulfius_init_instance(&instance, 8000, NULL, NULL) != U_OK) {
        fprintf(stderr, "failed to initialize Ulfius\n");
        return EXIT_FAILURE;
    }
    if (ulfius_add_endpoint_by_val(&instance,
            "GET", NULL, "/limited", 0, limited, &options) != U_OK
        || ulfius_start_framework(&instance) != U_OK) {
        fprintf(stderr, "failed to start Ulfius on port 8000\n");
        ulfius_clean_instance(&instance);
        return EXIT_FAILURE;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (!stop_requested) {
        sleep(1);
    }
    ulfius_stop_framework(&instance);
    ulfius_clean_instance(&instance);
    return EXIT_SUCCESS;
}
