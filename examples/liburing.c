#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>

#include <liburing.h>

#include "common/rl_example.h"

typedef struct liburing_app {
    struct io_uring ring;
    rl_example_client_t client;
    rl_example_request_t request;
    int status;
    bool allowed;
    bool done;
} liburing_app_t;

static void on_rate_limit(void *user, int status, bool allowed) {
    liburing_app_t *app = user;
    app->status = status;
    app->allowed = allowed;
    app->done = true;
}

static int arm_socket(liburing_app_t *app, size_t index) {
    struct io_uring_sqe *submission = io_uring_get_sqe(&app->ring);
    if (!submission) {
        return -1;
    }
    io_uring_prep_poll_add(
        submission,
        rl_example_socket_at(&app->client, index),
        POLLIN
    );
    io_uring_sqe_set_data64(submission, index + 1u);
    return 0;
}

static int run_loop(liburing_app_t *app) {
    size_t socket_count = rl_example_socket_count(&app->client);
    for (size_t i = 0; i < socket_count; i++) {
        if (arm_socket(app, i) != 0) {
            return RCLIENT_ERR_IO;
        }
    }
    if (io_uring_submit(&app->ring) < 0) {
        return RCLIENT_ERR_IO;
    }

    while (!app->done) {
        uint64_t delay_ms = 0;
        int status = rl_example_request_delay_ms(&app->request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        struct __kernel_timespec timeout = {
            .tv_sec = (long long)(delay_ms / 1000u),
            .tv_nsec = (long long)((delay_ms % 1000u) * 1000000u),
        };
        struct io_uring_cqe *completion = NULL;
        status = io_uring_wait_cqe_timeout(&app->ring, &completion, &timeout);
        if (status == -ETIME) {
            status = rl_example_request_on_timeout(&app->client, &app->request);
            if (status != RCLIENT_OK) {
                return status;
            }
            continue;
        }
        if (status == -EINTR) {
            continue;
        }
        if (status < 0 || !completion || completion->res < 0) {
            return RCLIENT_ERR_IO;
        }

        uint64_t user_data = io_uring_cqe_get_data64(completion);
        io_uring_cqe_seen(&app->ring, completion);
        if (user_data == 0 || user_data > socket_count) {
            return RCLIENT_ERR_IO;
        }
        size_t index = (size_t)(user_data - 1u);
        status = rl_example_client_on_readable(
            &app->client,
            rl_example_socket_at(&app->client, index)
        );
        if (status != RCLIENT_OK) {
            return status;
        }
        if (!app->done) {
            if (arm_socket(app, index) != 0 || io_uring_submit(&app->ring) < 0) {
                return RCLIENT_ERR_IO;
            }
        }
    }
    return RCLIENT_OK;
}

int main(void) {
    rl_example_options_t options;
    if (rl_example_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n");
        return EXIT_FAILURE;
    }

    liburing_app_t app = {0};
    app.status = RCLIENT_ERR_IO;
    if (io_uring_queue_init(8, &app.ring, 0) < 0) {
        perror("io_uring_queue_init");
        return EXIT_FAILURE;
    }
    int status = rl_example_client_init(&app.client, &options);
    if (status == RCLIENT_OK) {
        status = rl_example_check(
            &app.client,
            &app.request,
            "liburing-example",
            on_rate_limit,
            &app
        );
    }
    if (status == RCLIENT_OK) {
        status = run_loop(&app);
    }
    if (status != RCLIENT_OK && !app.done) {
        app.status = status;
    }

    if (app.request.active) {
        rl_example_request_cancel(&app.client, &app.request);
    }
    rl_example_client_destroy(&app.client);
    io_uring_queue_exit(&app.ring);

    if (app.status != RCLIENT_OK) {
        fprintf(stderr, "rate-limit check failed: %d\n", app.status);
        return EXIT_FAILURE;
    }
    puts(app.allowed ? "allowed" : "denied");
    return app.allowed ? EXIT_SUCCESS : 2;
}
