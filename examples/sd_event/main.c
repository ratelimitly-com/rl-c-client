#define _GNU_SOURCE

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <time.h>

#include <systemd/sd-event.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. sd_event_add_io() observes each runtime-owned UDP socket.
 * 2. One CLOCK_MONOTONIC time source follows the admission deadline.
 * 3. I/O callbacks drain datagrams; the timer advances retry/timeout state.
 * 4. Protected work runs only after resource and latency admission.
 * 5. The runtime measures successful work and reports one latency sample.
 *
 * Ownership: application owns sd-event sources, request, and copied outcome.
 * runtime owns the client and sockets; sources unref before socket destruction.
 */

typedef struct application application_t;

typedef struct socket_watcher {
    application_t *app;
    r_runtime_socket_t socket_value;
    sd_event_source *source;
} socket_watcher_t;

struct application {
    sd_event *event;
    sd_event_source *timer;
    socket_watcher_t sockets[2];
    size_t socket_count;
    r_runtime_client_t runtime;
    r_admission_request_t request;
    r_admission_outcome_t outcome;
    char response[96];
    uint32_t observed_latency_ms;
    int status;
    bool done;
};

static void stop_with_error(application_t *app, int status) {
    app->status = status;
    app->done = true;
    (void)sd_event_exit(app->event, 0);
}

static int prepare_response(void *user) {
    application_t *app = user;
    int length = snprintf(
        app->response,
        sizeof(app->response),
        "inventory response prepared by sd-event"
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
    if (app->timer) {
        (void)sd_event_source_set_enabled(app->timer, SD_EVENT_OFF);
    }
    app->done = true;
    (void)sd_event_exit(app->event, 0);
}

static int on_timeout(sd_event_source *source, uint64_t usec, void *user);

static int arm_timer(application_t *app) {
    uint64_t delay_ms = 0u;
    int status = r_runtime_admission_delay_ms(&app->request, &delay_ms);
    if (status != RCLIENT_OK) {
        return status;
    }
    uint64_t now_usec = 0u;
    if (sd_event_now(app->event, CLOCK_MONOTONIC, &now_usec) < 0) {
        return RCLIENT_ERR_IO;
    }
    uint64_t delay_usec = delay_ms > UINT64_MAX / 1000u
        ? UINT64_MAX
        : delay_ms * 1000u;
    uint64_t deadline_usec = UINT64_MAX - now_usec < delay_usec
        ? UINT64_MAX
        : now_usec + delay_usec;
    if (!app->timer) {
        status = sd_event_add_time(
            app->event,
            &app->timer,
            CLOCK_MONOTONIC,
            deadline_usec,
            0u,
            on_timeout,
            app
        );
    } else {
        status = sd_event_source_set_time(app->timer, deadline_usec);
    }
    if (status < 0
        || sd_event_source_set_enabled(app->timer, SD_EVENT_ONESHOT) < 0) {
        return RCLIENT_ERR_IO;
    }
    return RCLIENT_OK;
}

static int on_timeout(sd_event_source *source, uint64_t usec, void *user) {
    (void)source;
    (void)usec;
    application_t *app = user;
    int status = r_runtime_admission_on_timeout(&app->runtime, &app->request);
    if (status != RCLIENT_OK) {
        stop_with_error(app, status);
    } else if (app->request.active && arm_timer(app) != RCLIENT_OK) {
        stop_with_error(app, RCLIENT_ERR_IO);
    }
    return 0;
}

static int on_udp_readable(
    sd_event_source *source,
    int socket_fd,
    uint32_t revents,
    void *user
) {
    (void)source;
    socket_watcher_t *watcher = user;
    if ((revents & EPOLLIN) == 0 || (revents & (EPOLLERR | EPOLLHUP)) != 0
        || socket_fd != (int)watcher->socket_value) {
        stop_with_error(watcher->app, RCLIENT_ERR_IO);
        return 0;
    }
    int status = r_runtime_client_on_readable(
        &watcher->app->runtime,
        watcher->socket_value
    );
    if (status != RCLIENT_OK) {
        stop_with_error(watcher->app, status);
    }
    return 0;
}

static int initialize_sources(application_t *app) {
    size_t count = r_runtime_socket_count(&app->runtime);
    for (size_t i = 0; i < count; i++) {
        socket_watcher_t *watcher = &app->sockets[i];
        watcher->app = app;
        watcher->socket_value = r_runtime_socket_at(&app->runtime, i);
        if (sd_event_add_io(
                app->event,
                &watcher->source,
                (int)watcher->socket_value,
                EPOLLIN,
                on_udp_readable,
                watcher) < 0) {
            return RCLIENT_ERR_IO;
        }
        app->socket_count++;
    }
    return app->socket_count > 0u ? RCLIENT_OK : RCLIENT_ERR_IO;
}

static int start_admission(application_t *app) {
    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "sd-event-example";
    config.service_name = "sd-event-protected-service";
    config.metrics_label = "sd-event-example";
    int status = r_client_admission_start(
        app->runtime.handle,
        &app->request,
        &config,
        on_admission,
        app
    );
    return status == RCLIENT_OK ? arm_timer(app) : status;
}

static void destroy_application(application_t *app) {
    if (app->request.active) {
        r_runtime_admission_cancel(&app->runtime, &app->request);
    }
    app->timer = sd_event_source_unref(app->timer);
    for (size_t i = 0; i < app->socket_count; i++) {
        app->sockets[i].source = sd_event_source_unref(app->sockets[i].source);
    }
    r_runtime_client_destroy(&app->runtime);
    app->event = sd_event_unref(app->event);
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
    if (sd_event_default(&app.event) < 0) {
        return EXIT_FAILURE;
    }
    int status = r_runtime_client_init(&app.runtime, &options);
    if (status == RCLIENT_OK) {
        status = initialize_sources(&app);
    }
    if (status == RCLIENT_OK) {
        status = start_admission(&app);
    }
    if (status == RCLIENT_OK && sd_event_loop(app.event) < 0) {
        status = RCLIENT_ERR_IO;
    } else if (status == RCLIENT_OK) {
        status = app.status;
    }

    destroy_application(&app);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "sd-event example failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }
    print_outcome(&app);
    return app.outcome.allowed ? EXIT_SUCCESS : 2;
}
