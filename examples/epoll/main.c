#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. Register each runtime-owned UDP socket for EPOLLIN.
 * 2. Pass the active admission deadline to epoll_wait().
 * 3. Drain readable sockets or advance timeout state.
 * 4. Perform protected work only after resource and latency admission.
 * 5. Measure the completed work and report one latency sample.
 *
 * Ownership: application owns the epoll instance, request, and copied outcome.
 * runtime owns the client and sockets; epoll only references those sockets.
 */

typedef struct application {
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
        "inventory response prepared by epoll"
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

static int register_sockets(application_t *app, int epoll_fd) {
    size_t count = r_runtime_socket_count(&app->runtime);
    for (size_t i = 0; i < count; i++) {
        int socket_fd = (int)r_runtime_socket_at(&app->runtime, i);
        struct epoll_event event = {
            .events = EPOLLIN,
            .data.fd = socket_fd,
        };
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &event) != 0) {
            return RCLIENT_ERR_IO;
        }
    }
    return count > 0u ? RCLIENT_OK : RCLIENT_ERR_IO;
}

static int handle_ready_events(
    application_t *app,
    const struct epoll_event *events,
    int count
) {
    for (int i = 0; i < count; i++) {
        if ((events[i].events & EPOLLIN) == 0
            || (events[i].events & (EPOLLERR | EPOLLHUP)) != 0) {
            return RCLIENT_ERR_IO;
        }
        int status = r_runtime_client_on_readable(
            &app->runtime,
            (r_runtime_socket_t)events[i].data.fd
        );
        if (status != RCLIENT_OK) {
            return status;
        }
    }
    return RCLIENT_OK;
}

static int run_loop(application_t *app, int epoll_fd) {
    while (!app->done) {
        uint64_t delay_ms = 0u;
        int status = r_runtime_admission_delay_ms(&app->request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        int timeout_ms = delay_ms > INT_MAX ? INT_MAX : (int)delay_ms;
        struct epoll_event events[2];
        int count = epoll_wait(epoll_fd, events, 2, timeout_ms);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return RCLIENT_ERR_IO;
        }
        if (count == 0) {
            status = r_runtime_admission_on_timeout(&app->runtime, &app->request);
        } else {
            status = handle_ready_events(app, events, count);
        }
        if (status != RCLIENT_OK) {
            return status;
        }
    }
    return app->status;
}

static int start_admission(application_t *app) {
    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "epoll-example";
    config.service_name = "epoll-protected-service";
    config.metrics_label = "epoll-example";
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
        fputs("set RATELIMITLY_AUTH_KEY; RATELIMITLY_TENANT is optional\n", stderr);
        return EXIT_FAILURE;
    }

    application_t app = {.status = RCLIENT_ERR_IO};
    int status = r_runtime_client_init(&app.runtime, &options);
    int epoll_fd = status == RCLIENT_OK ? epoll_create1(EPOLL_CLOEXEC) : -1;
    if (status == RCLIENT_OK && epoll_fd < 0) {
        status = RCLIENT_ERR_IO;
    }
    if (status == RCLIENT_OK) {
        status = register_sockets(&app, epoll_fd);
    }
    if (status == RCLIENT_OK) {
        status = start_admission(&app);
    }
    if (status == RCLIENT_OK) {
        status = run_loop(&app, epoll_fd);
    }

    if (app.request.active) {
        r_runtime_admission_cancel(&app.runtime, &app.request);
    }
    if (epoll_fd >= 0) {
        close(epoll_fd);
    }
    r_runtime_client_destroy(&app.runtime);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "epoll example failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }
    print_outcome(&app);
    return app.outcome.allowed ? EXIT_SUCCESS : 2;
}
