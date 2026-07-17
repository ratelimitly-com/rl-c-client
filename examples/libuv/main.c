#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <uv.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. uv_poll_t observes each UDP socket owned by the portable runtime.
 * 2. A one-shot uv_timer_t advances the request's current deadline.
 * 3. The admission callback distinguishes rate and latency denials.
 * 4. Only an allowed request builds the protected application response.
 * 5. The measured response work is reported before the loop stops.
 *
 * Ownership: application owns the loop, watchers, request, and copied outcome.
 * runtime owns the client handle and sockets; libuv only observes the sockets.
 * Watchers close before runtime destroys those sockets.
 */

typedef struct application application_t;

typedef struct socket_watcher {
    uv_poll_t poll;
    application_t *app;
    r_runtime_socket_t socket_value;
} socket_watcher_t;

struct application {
    uv_loop_t loop;
    uv_timer_t timer;
    socket_watcher_t sockets[2];
    size_t socket_count;
    r_runtime_client_t runtime;
    r_admission_request_t request;
    r_admission_outcome_t outcome;
    char response[96];
    uint32_t observed_latency_ms;
    int status;
    bool timer_initialized;
    bool done;
};

static void stop_with_error(application_t *app, int status) {
    app->status = status;
    app->done = true;
    uv_timer_stop(&app->timer);
    uv_stop(&app->loop);
}

static int perform_protected_work(application_t *app) {
    uint64_t started_ms = 0u;
    int status = r_runtime_monotonic_time_ms(&started_ms);
    if (status != RCLIENT_OK) {
        return status;
    }
    int length = snprintf(
        app->response,
        sizeof(app->response),
        "inventory response prepared by libuv"
    );
    if (length < 0 || (size_t)length >= sizeof(app->response)) {
        return RCLIENT_ERR_IO;
    }
    uint64_t finished_ms = 0u;
    status = r_runtime_monotonic_time_ms(&finished_ms);
    if (status != RCLIENT_OK || finished_ms < started_ms) {
        return RCLIENT_ERR_IO;
    }
    uint64_t elapsed_ms = finished_ms - started_ms;
    app->observed_latency_ms = elapsed_ms > UINT32_MAX
        ? UINT32_MAX
        : (uint32_t)elapsed_ms;
    return r_client_admission_report_latency(
        app->runtime.handle,
        &app->request,
        app->observed_latency_ms
    );
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
        app->status = perform_protected_work(app);
    }
    app->done = true;
    uv_timer_stop(&app->timer);
    uv_stop(&app->loop);
}

static int arm_deadline_timer(application_t *app);

static void on_timeout(uv_timer_t *timer) {
    application_t *app = timer->data;
    int status = r_client_admission_on_timeout(
        app->runtime.handle,
        &app->request,
        r_runtime_wall_time_ms()
    );
    if (status != RCLIENT_OK) {
        stop_with_error(app, status);
    } else if (app->request.active && arm_deadline_timer(app) != 0) {
        stop_with_error(app, RCLIENT_ERR_IO);
    }
}

static int arm_deadline_timer(application_t *app) {
    uint64_t deadline_ms = 0u;
    int status = r_client_admission_deadline_ms(
        &app->request,
        &deadline_ms
    );
    if (status != RCLIENT_OK) {
        return -1;
    }
    uint64_t now_ms = r_runtime_wall_time_ms();
    uint64_t delay_ms = deadline_ms > now_ms ? deadline_ms - now_ms : 0u;
    return uv_timer_start(&app->timer, on_timeout, delay_ms, 0u);
}

static void on_udp_readable(uv_poll_t *poll, int status, int events) {
    socket_watcher_t *watcher = poll->data;
    application_t *app = watcher->app;
    if (status < 0 || (events & UV_READABLE) == 0) {
        stop_with_error(app, RCLIENT_ERR_IO);
        return;
    }
    int client_status = r_runtime_client_on_readable(
        &app->runtime,
        watcher->socket_value
    );
    if (client_status != RCLIENT_OK) {
        stop_with_error(app, client_status);
    }
}

static int initialize_watchers(application_t *app) {
    if (uv_timer_init(&app->loop, &app->timer) != 0) {
        return RCLIENT_ERR_IO;
    }
    app->timer_initialized = true;
    app->timer.data = app;

    size_t count = r_runtime_socket_count(&app->runtime);
    for (size_t i = 0; i < count; i++) {
        socket_watcher_t *watcher = &app->sockets[i];
        watcher->app = app;
        watcher->socket_value = r_runtime_socket_at(&app->runtime, i);
        int uv_status = uv_poll_init_socket(
            &app->loop,
            &watcher->poll,
            (uv_os_sock_t)watcher->socket_value
        );
        if (uv_status != 0) {
            return RCLIENT_ERR_IO;
        }
        app->socket_count++;
        watcher->poll.data = watcher;
        if (uv_poll_start(&watcher->poll, UV_READABLE, on_udp_readable) != 0) {
            return RCLIENT_ERR_IO;
        }
    }
    return app->socket_count > 0u ? RCLIENT_OK : RCLIENT_ERR_IO;
}

static int start_admission(application_t *app) {
    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "libuv-example";
    config.service_name = "libuv-protected-service";
    config.metrics_label = "libuv-example";
    int status = r_client_admission_start(
        app->runtime.handle,
        &app->request,
        &config,
        on_admission,
        app
    );
    if (status != RCLIENT_OK || arm_deadline_timer(app) != 0) {
        return status != RCLIENT_OK ? status : RCLIENT_ERR_IO;
    }
    return RCLIENT_OK;
}

static void close_loop_handles(application_t *app) {
    if (app->timer_initialized) {
        uv_timer_stop(&app->timer);
        uv_close((uv_handle_t *)&app->timer, NULL);
    }
    for (size_t i = 0; i < app->socket_count; i++) {
        uv_poll_stop(&app->sockets[i].poll);
        uv_close((uv_handle_t *)&app->sockets[i].poll, NULL);
    }
    uv_run(&app->loop, UV_RUN_DEFAULT);
}

static void print_outcome(const application_t *app) {
    if (app->outcome.allowed) {
        printf("allowed: %s; latency=%" PRIu32 " ms\n",
            app->response, app->observed_latency_ms);
    } else if (app->outcome.latency_limited && app->outcome.rate_limited) {
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
    if (uv_loop_init(&app.loop) != 0) {
        return EXIT_FAILURE;
    }
    int status = r_runtime_client_init(&app.runtime, &options);
    if (status == RCLIENT_OK) {
        status = initialize_watchers(&app);
    }
    if (status == RCLIENT_OK) {
        status = start_admission(&app);
    }
    if (status == RCLIENT_OK) {
        uv_run(&app.loop, UV_RUN_DEFAULT);
        status = app.status;
    }

    if (app.request.active) {
        r_client_admission_cancel(app.runtime.handle, &app.request);
    }
    if (app.timer_initialized) {
        close_loop_handles(&app);
    }
    r_runtime_client_destroy(&app.runtime);
    uv_loop_close(&app.loop);

    if (status != RCLIENT_OK) {
        fprintf(stderr, "libuv example failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }
    print_outcome(&app);
    return app.outcome.allowed ? EXIT_SUCCESS : 2;
}
