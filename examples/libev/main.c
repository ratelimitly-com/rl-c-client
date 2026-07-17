#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <ev.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. One ev_io watcher observes each runtime-owned UDP socket.
 * 2. A one-shot ev_timer follows the admission request deadline.
 * 3. Read callbacks drain datagrams; timeout callbacks advance retry state.
 * 4. Only resource- and latency-admitted work prepares a response.
 * 5. The runtime measures completed work and reports one latency sample.
 *
 * Ownership: application owns the loop, watchers, request, and copied outcome.
 * runtime owns the client and sockets; watchers stop before socket destruction.
 */

typedef struct application application_t;

typedef struct socket_watcher {
    ev_io io;
    application_t *app;
    r_runtime_socket_t socket_value;
} socket_watcher_t;

struct application {
    struct ev_loop *loop;
    ev_timer timer;
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
    ev_break(app->loop, EVBREAK_ALL);
}

static int prepare_response(void *user) {
    application_t *app = user;
    int length = snprintf(
        app->response,
        sizeof(app->response),
        "inventory response prepared by libev"
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
    ev_timer_stop(app->loop, &app->timer);
    ev_break(app->loop, EVBREAK_ALL);
}

static int arm_timer(application_t *app);

static void on_timeout(EV_P_ ev_timer *timer, int revents) {
    (void)loop;
    (void)revents;
    application_t *app = timer->data;
    int status = r_runtime_admission_on_timeout(&app->runtime, &app->request);
    if (status != RCLIENT_OK) {
        stop_with_error(app, status);
    } else if (app->request.active && arm_timer(app) != RCLIENT_OK) {
        stop_with_error(app, RCLIENT_ERR_IO);
    }
}

static int arm_timer(application_t *app) {
    uint64_t delay_ms = 0u;
    int status = r_runtime_admission_delay_ms(&app->request, &delay_ms);
    if (status != RCLIENT_OK) {
        return status;
    }
    ev_timer_stop(app->loop, &app->timer);
    ev_timer_set(&app->timer, (ev_tstamp)delay_ms / 1000.0, 0.0);
    ev_timer_start(app->loop, &app->timer);
    return RCLIENT_OK;
}

static void on_udp_readable(EV_P_ ev_io *io, int revents) {
    (void)loop;
    socket_watcher_t *watcher = io->data;
    if ((revents & EV_ERROR) != 0 || (revents & EV_READ) == 0) {
        stop_with_error(watcher->app, RCLIENT_ERR_IO);
        return;
    }
    int status = r_runtime_client_on_readable(
        &watcher->app->runtime,
        watcher->socket_value
    );
    if (status != RCLIENT_OK) {
        stop_with_error(watcher->app, status);
    }
}

static int initialize_watchers(application_t *app) {
    ev_timer_init(&app->timer, on_timeout, 0.0, 0.0);
    app->timer.data = app;
    size_t count = r_runtime_socket_count(&app->runtime);
    for (size_t i = 0; i < count; i++) {
        socket_watcher_t *watcher = &app->sockets[i];
        watcher->app = app;
        watcher->socket_value = r_runtime_socket_at(&app->runtime, i);
        ev_io_init(&watcher->io, on_udp_readable, (int)watcher->socket_value, EV_READ);
        watcher->io.data = watcher;
        ev_io_start(app->loop, &watcher->io);
        app->socket_count++;
    }
    return app->socket_count > 0u ? RCLIENT_OK : RCLIENT_ERR_IO;
}

static int start_admission(application_t *app) {
    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "libev-example";
    config.service_name = "libev-protected-service";
    config.metrics_label = "libev-example";
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
    ev_timer_stop(app->loop, &app->timer);
    for (size_t i = 0; i < app->socket_count; i++) {
        ev_io_stop(app->loop, &app->sockets[i].io);
    }
    r_runtime_client_destroy(&app->runtime);
    ev_loop_destroy(app->loop);
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
    app.loop = ev_loop_new(EVFLAG_AUTO);
    if (!app.loop) {
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
        ev_run(app.loop, 0);
        status = app.status;
    }

    destroy_application(&app);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "libev example failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }
    print_outcome(&app);
    return app.outcome.allowed ? EXIT_SUCCESS : 2;
}
