#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. Register each runtime-owned UDP socket with EVFILT_READ.
 * 2. Pass the active admission delay directly to kevent().
 * 3. Drain readable sockets or advance timeout/retry state.
 * 4. Run protected work only after resource and latency admission.
 * 5. Measure successful work and report one latency sample.
 *
 * Ownership: application owns the kqueue, request, and copied outcome. runtime
 * owns sockets; the kqueue is closed before those socket handles are destroyed.
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
        "inventory response prepared by kqueue"
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

static int register_sockets(application_t *app, int queue_fd) {
    size_t count = r_runtime_socket_count(&app->runtime);
    struct kevent changes[2];
    for (size_t i = 0; i < count; i++) {
        EV_SET(
            &changes[i],
            (uintptr_t)r_runtime_socket_at(&app->runtime, i),
            EVFILT_READ,
            EV_ADD | EV_ENABLE,
            0,
            0,
            NULL
        );
    }
    return count > 0u
            && kevent(queue_fd, changes, (int)count, NULL, 0, NULL) == 0
        ? RCLIENT_OK
        : RCLIENT_ERR_IO;
}

static int handle_events(
    application_t *app,
    const struct kevent *events,
    int count
) {
    for (int i = 0; i < count; i++) {
        if (events[i].filter != EVFILT_READ
            || (events[i].flags & (EV_ERROR | EV_EOF)) != 0) {
            return RCLIENT_ERR_IO;
        }
        int status = r_runtime_client_on_readable(
            &app->runtime,
            (r_runtime_socket_t)events[i].ident
        );
        if (status != RCLIENT_OK) {
            return status;
        }
    }
    return RCLIENT_OK;
}

static int run_loop(application_t *app, int queue_fd) {
    while (!app->done) {
        uint64_t delay_ms = 0u;
        int status = r_runtime_admission_delay_ms(&app->request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        struct timespec timeout = {
            .tv_sec = (time_t)(delay_ms / 1000u),
            .tv_nsec = (long)(delay_ms % 1000u) * 1000000L,
        };
        struct kevent events[2];
        int count = kevent(queue_fd, NULL, 0, events, 2, &timeout);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return RCLIENT_ERR_IO;
        }
        status = count == 0
            ? r_runtime_admission_on_timeout(&app->runtime, &app->request)
            : handle_events(app, events, count);
        if (status != RCLIENT_OK) {
            return status;
        }
    }
    return app->status;
}

static int start_admission(application_t *app) {
    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "kqueue-example";
    config.service_name = "kqueue-protected-service";
    config.metrics_label = "kqueue-example";
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
    int queue_fd = status == RCLIENT_OK ? kqueue() : -1;
    if (status == RCLIENT_OK && queue_fd < 0) {
        status = RCLIENT_ERR_IO;
    }
    if (status == RCLIENT_OK) {
        status = register_sockets(&app, queue_fd);
    }
    if (status == RCLIENT_OK) {
        status = start_admission(&app);
    }
    if (status == RCLIENT_OK) {
        status = run_loop(&app, queue_fd);
    }

    if (app.request.active) {
        r_runtime_admission_cancel(&app.runtime, &app.request);
    }
    if (queue_fd >= 0) {
        close(queue_fd);
    }
    r_runtime_client_destroy(&app.runtime);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "kqueue example failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }
    print_outcome(&app);
    return app.outcome.allowed ? EXIT_SUCCESS : 2;
}
