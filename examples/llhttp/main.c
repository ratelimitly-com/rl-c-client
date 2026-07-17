#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/select.h>
#endif

#include <llhttp.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"
#include "llhttp_adapter.h"

/*
 * Flow
 * ----
 * 1. A host TCP loop feeds arbitrary fragments to llhttp_execute().
 * 2. The parser collects a bounded URL and starts one combined admission check.
 * 3. HPE_PAUSED applies backpressure while the host drives UDP and the deadline.
 * 4. Completion resumes parsing and distinguishes resource/latency denial.
 * 5. Allowed work is measured monotonically and its latency is reported once.
 *
 * Ownership: the host owns TCP buffers, the runtime, and one adapter per
 * connection. The adapter owns parser/admission state and borrows the runtime.
 * This one-shot program uses select() only to demonstrate the host contract;
 * llhttp itself is a parser and does not provide an event loop or network I/O.
 */
typedef struct example_app {
    r_runtime_client_t runtime;
    rl_llhttp_adapter_t adapter;
    bool finished;
    int result_status;
} example_app_t;

static int print_protected_response(
    void *user,
    const char *method,
    const char *path
) {
    (void)user;
    printf("protected work: %s %s\n", method, path);
    return RCLIENT_OK;
}

static const char *decision_name(r_admission_decision_t decision) {
    switch (decision) {
        case R_ADMISSION_ALLOWED:
            return "allowed";
        case R_ADMISSION_RATE_LIMITED:
            return "resource rate limited";
        case R_ADMISSION_LATENCY_LIMITED:
            return "latency limited";
        case R_ADMISSION_RATE_AND_LATENCY_LIMITED:
            return "resource and latency limited";
        default:
            return "error";
    }
}

static void on_result(
    void *user,
    int status,
    const r_admission_outcome_t *outcome,
    uint32_t observed_latency_ms
) {
    example_app_t *app = user;
    app->result_status = status;
    app->finished = true;
    if (status != RCLIENT_OK) {
        fprintf(stderr, "admission workflow failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return;
    }
    printf("decision: %s\n", decision_name(outcome->decision));
    if (outcome->allowed) {
        printf("reported latency: %u ms\n", observed_latency_ms);
    }
}

static int wait_for_activity(
    example_app_t *app,
    uint64_t delay_ms
) {
    fd_set readable;
    FD_ZERO(&readable);
    int highest_fd = -1;
    size_t socket_count = r_runtime_socket_count(&app->runtime);
    for (size_t i = 0; i < socket_count; i++) {
        r_runtime_socket_t socket_value =
            r_runtime_socket_at(&app->runtime, i);
        FD_SET(socket_value, &readable);
#ifndef _WIN32
        if (socket_value > highest_fd) {
            highest_fd = socket_value;
        }
#endif
    }

    /* A short cap also lets a larger host loop service unrelated connections. */
    uint64_t wait_ms = delay_ms < 250u ? delay_ms : 250u;
    struct timeval timeout;
    timeout.tv_sec = (long)(wait_ms / 1000u);
    timeout.tv_usec = (long)((wait_ms % 1000u) * 1000u);
    int ready = select(highest_fd + 1, &readable, NULL, NULL, &timeout);
    if (ready < 0) {
#ifdef _WIN32
        return RCLIENT_ERR_IO;
#else
        return errno == EINTR ? RCLIENT_OK : RCLIENT_ERR_IO;
#endif
    }
    for (size_t i = 0; i < socket_count && ready > 0; i++) {
        r_runtime_socket_t socket_value =
            r_runtime_socket_at(&app->runtime, i);
        if (FD_ISSET(socket_value, &readable)) {
            int status = r_runtime_client_on_readable(
                &app->runtime,
                socket_value
            );
            if (status != RCLIENT_OK) {
                return status;
            }
        }
    }
    return RCLIENT_OK;
}

static int drive_admission(example_app_t *app) {
    while (!app->finished) {
        r_admission_request_t *request =
            rl_llhttp_adapter_pending(&app->adapter);
        if (!request) {
            return RCLIENT_ERR_PROTOCOL;
        }
        uint64_t delay_ms = 0u;
        int status = r_runtime_admission_delay_ms(request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        if (delay_ms == 0u) {
            status = r_runtime_admission_on_timeout(&app->runtime, request);
        } else {
            status = wait_for_activity(app, delay_ms);
        }
        if (status != RCLIENT_OK) {
            return status;
        }
    }
    return app->result_status;
}

static int parse_example_request(example_app_t *app) {
    static const char request[] =
        "GET /limited?trace=not-a-bucket-key HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n\r\n";
    const size_t split = 17u; /* Deliberately split the URL across TCP reads. */
    size_t consumed = 0u;
    llhttp_errno_t parse_status = rl_llhttp_adapter_feed(
        &app->adapter,
        request,
        split,
        &consumed
    );
    if (parse_status != HPE_OK || consumed != split) {
        fprintf(stderr, "first fragment failed after %zu bytes: %s (%s)\n",
            consumed,
            llhttp_errno_name(parse_status),
            llhttp_get_error_reason(&app->adapter.parser));
        return RCLIENT_ERR_PROTOCOL;
    }
    parse_status = rl_llhttp_adapter_feed(
        &app->adapter,
        request + split,
        sizeof(request) - 1u - split,
        &consumed
    );
    if (parse_status != HPE_PAUSED) {
        fprintf(stderr, "second fragment failed after %zu bytes: %s (%s)\n",
            consumed,
            llhttp_errno_name(parse_status),
            llhttp_get_error_reason(&app->adapter.parser));
        return RCLIENT_ERR_PROTOCOL;
    }
    return drive_admission(app);
}

int main(void) {
    r_runtime_options_t options;
    int status = r_runtime_options_from_env(&options);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n");
        return EXIT_FAILURE;
    }

    example_app_t app = {0};
    status = r_runtime_client_init(&app.runtime, &options);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "client initialization failed: %s (%d)\n",
            r_runtime_status_name(status), status);
    }
    if (status == RCLIENT_OK) {
        status = rl_llhttp_adapter_init(
            &app.adapter,
            &app.runtime,
            print_protected_response,
            on_result,
            &app
        );
        if (status != RCLIENT_OK) {
            fprintf(stderr, "parser adapter initialization failed: %s (%d)\n",
                r_runtime_status_name(status), status);
        }
    }
    if (status == RCLIENT_OK) {
        status = parse_example_request(&app);
        if (status != RCLIENT_OK && !app.finished) {
            fprintf(stderr, "host driver failed: %s (%d)\n",
                r_runtime_status_name(status), status);
        }
    }
    rl_llhttp_adapter_dispose(&app.adapter);
    r_runtime_client_destroy(&app.runtime);
    return status == RCLIENT_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
