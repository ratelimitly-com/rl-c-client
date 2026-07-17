#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>

#include <liburing.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. Submit one IORING_OP_POLL_ADD for each runtime UDP socket.
 * 2. Wait for a completion or the current admission deadline.
 * 3. Consume a poll CQE, drain its socket, and re-arm the one-shot poll.
 * 4. Run protected work only after resource and latency admission.
 * 5. Measure the completed work and report one latency sample.
 *
 * Ownership: application owns the ring, request, and copied outcome. runtime
 * owns sockets. CQE user_data stores socket index + 1 and owns no pointer.
 */

typedef struct application {
    struct io_uring ring;
    r_runtime_client_t runtime;
    r_admission_request_t request;
    r_admission_outcome_t outcome;
    char response[96];
    uint32_t observed_latency_ms;
    int status;
    bool done;
} application_t;

static int prepare_response(void *user) {
    application_t *app = user;
    int length = snprintf(
        app->response,
        sizeof(app->response),
        "inventory response prepared through liburing"
    );
    return length >= 0 && (size_t)length < sizeof(app->response)
        ? RCLIENT_OK
        : RCLIENT_ERR_IO;
}

static void on_admission(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    application_t *app = user;
    app->status = status;
    app->outcome = *outcome;
    if (status == RCLIENT_OK && outcome->allowed) {
        app->status = r_runtime_admission_run_and_report(
            &app->runtime,
            &app->request,
            prepare_response,
            app,
            &app->observed_latency_ms
        );
    }
    app->done = true;
}

static int arm_socket(application_t *app, size_t index) {
    struct io_uring_sqe *submission = io_uring_get_sqe(&app->ring);
    if (!submission) {
        return RCLIENT_ERR_IO;
    }
    io_uring_prep_poll_add(
        submission,
        (int)r_runtime_socket_at(&app->runtime, index),
        POLLIN
    );
    io_uring_sqe_set_data64(submission, index + 1u);
    return RCLIENT_OK;
}

static int arm_all_sockets(application_t *app) {
    size_t count = r_runtime_socket_count(&app->runtime);
    for (size_t i = 0; i < count; i++) {
        if (arm_socket(app, i) != RCLIENT_OK) {
            return RCLIENT_ERR_IO;
        }
    }
    return count > 0u && io_uring_submit(&app->ring) >= 0
        ? RCLIENT_OK
        : RCLIENT_ERR_IO;
}

static int wait_for_completion(
    application_t *app,
    struct io_uring_cqe **out_completion
) {
    uint64_t delay_ms = 0u;
    int status = r_runtime_admission_delay_ms(&app->request, &delay_ms);
    if (status != RCLIENT_OK) {
        return status;
    }
    struct __kernel_timespec timeout = {
        .tv_sec = (long long)(delay_ms / 1000u),
        .tv_nsec = (long long)((delay_ms % 1000u) * 1000000u),
    };
    status = io_uring_wait_cqe_timeout(&app->ring, out_completion, &timeout);
    if (status == -ETIME) {
        return r_runtime_admission_on_timeout(&app->runtime, &app->request);
    }
    if (status == -EINTR) {
        return RCLIENT_OK;
    }
    return status < 0 ? RCLIENT_ERR_IO : RCLIENT_OK;
}

static int consume_completion(
    application_t *app,
    struct io_uring_cqe *completion
) {
    if (!completion || completion->res < 0) {
        return RCLIENT_ERR_IO;
    }
    size_t socket_count = r_runtime_socket_count(&app->runtime);
    uint64_t user_data = io_uring_cqe_get_data64(completion);
    io_uring_cqe_seen(&app->ring, completion);
    if (user_data == 0u || user_data > socket_count) {
        return RCLIENT_ERR_IO;
    }

    size_t index = (size_t)(user_data - 1u);
    int status = r_runtime_client_on_readable(
        &app->runtime,
        r_runtime_socket_at(&app->runtime, index)
    );
    if (status != RCLIENT_OK || app->done) {
        return status;
    }
    return arm_socket(app, index) == RCLIENT_OK
            && io_uring_submit(&app->ring) >= 0
        ? RCLIENT_OK
        : RCLIENT_ERR_IO;
}

static int run_loop(application_t *app) {
    int status = arm_all_sockets(app);
    while (status == RCLIENT_OK && !app->done) {
        struct io_uring_cqe *completion = NULL;
        status = wait_for_completion(app, &completion);
        if (status == RCLIENT_OK && completion) {
            status = consume_completion(app, completion);
        }
    }
    return status == RCLIENT_OK ? app->status : status;
}

static int start_admission(application_t *app) {
    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "liburing-example";
    config.service_name = "liburing-protected-service";
    config.metrics_label = "liburing-example";
    return r_client_admission_start(
        app->runtime.handle,
        &app->request,
        &config,
        on_admission,
        app
    );
}

static void print_outcome(const application_t *app) {
    if (app->outcome.allowed) {
        printf("allowed: %s; latency=%" PRIu32 " ms\n",
            app->response, app->observed_latency_ms);
    } else if (app->outcome.rate_limited && app->outcome.latency_limited) {
        puts("denied: resource limit and latency guard");
    } else if (app->outcome.latency_limited) {
        puts("denied: latency guard");
    } else {
        puts("denied: resource rate limit");
    }
}

int main(void) {
    r_runtime_options_t options;
    if (r_runtime_options_from_env(&options) != RCLIENT_OK) {
        fputs("set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n", stderr);
        return EXIT_FAILURE;
    }

    application_t app = {.status = RCLIENT_ERR_IO};
    if (io_uring_queue_init(8u, &app.ring, 0u) < 0) {
        perror("io_uring_queue_init");
        return EXIT_FAILURE;
    }
    int status = r_runtime_client_init(&app.runtime, &options);
    if (status == RCLIENT_OK) {
        status = start_admission(&app);
    }
    if (status == RCLIENT_OK) {
        status = run_loop(&app);
    }

    if (app.request.active) {
        r_runtime_admission_cancel(&app.runtime, &app.request);
    }
    io_uring_queue_exit(&app.ring);
    r_runtime_client_destroy(&app.runtime);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "liburing example failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }
    print_outcome(&app);
    return app.outcome.allowed ? EXIT_SUCCESS : 2;
}
