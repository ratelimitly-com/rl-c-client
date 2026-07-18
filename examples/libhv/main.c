#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <hv/hloop.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. hio_get() attaches readiness watchers to runtime-owned UDP sockets.
 * 2. A one-shot htimer_t follows the current admission deadline.
 * 3. Read and timeout callbacks advance the public workflow.
 * 4. Admitted work is measured and reported before hloop_run() stops.
 *
 * Ownership: runtime owns sockets; hloop owns hio_t and htimer_t objects.
 * Watchers are detached before runtime closes its descriptors.
 */
typedef struct libhv_app {
    hloop_t *loop;
    hio_t *socket_events[2];
    size_t socket_count;
    htimer_t *timer;
    r_runtime_client_t runtime;
    r_admission_request_t request;
    r_admission_outcome_t outcome;
    char response[96];
    uint32_t observed_latency_ms;
    int status;
    bool done;
} libhv_app_t;

static void stop_with_error(libhv_app_t *app, int status) {
    app->status = status;
    app->done = true;
    hloop_stop(app->loop);
}

static int prepare_response(void *user) {
    libhv_app_t *app = user;
    int length = snprintf(app->response, sizeof(app->response),
        "inventory response prepared by libhv");
    return length >= 0 && (size_t)length < sizeof(app->response)
        ? RCLIENT_OK
        : RCLIENT_ERR_IO;
}

static void on_admission(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    libhv_app_t *app = user;
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
    if (app->timer) {
        htimer_del(app->timer);
        app->timer = NULL;
    }
    hloop_stop(app->loop);
}

static int arm_timer(libhv_app_t *app);

static void on_timeout(htimer_t *timer) {
    libhv_app_t *app = hevent_userdata(timer);
    app->timer = NULL;
    /* Timeout processing can complete inline; only re-arm an active request. */
    int status = r_runtime_admission_on_timeout(&app->runtime, &app->request);
    if (status != RCLIENT_OK) {
        stop_with_error(app, status);
    } else if (app->request.active && arm_timer(app) != 0) {
        stop_with_error(app, RCLIENT_ERR_IO);
    }
}

static int arm_timer(libhv_app_t *app) {
    uint64_t delay_ms = 0;
    int status = r_runtime_admission_delay_ms(&app->request, &delay_ms);
    if (status != RCLIENT_OK || delay_ms > UINT32_MAX) {
        return -1;
    }
    /* repeat=1 in libhv means run once, not a repeating interval. */
    app->timer = htimer_add(app->loop, on_timeout, (uint32_t)delay_ms, 1);
    if (!app->timer) {
        return -1;
    }
    hevent_set_userdata(app->timer, app);
    return 0;
}

static void on_udp_readable(hio_t *io) {
    libhv_app_t *app = hio_context(io);
    int status = r_runtime_client_on_readable(
        &app->runtime,
        (r_runtime_socket_t)hio_fd(io)
    );
    if (status != RCLIENT_OK) {
        stop_with_error(app, status);
    }
}

int main(void) {
    r_runtime_options_t options;
    if (r_runtime_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_AUTH_KEY; RATELIMITLY_TENANT is optional\n");
        return EXIT_FAILURE;
    }

    libhv_app_t app = {0};
    app.status = RCLIENT_ERR_IO;
    app.loop = hloop_new(0);
    if (!app.loop) {
        return EXIT_FAILURE;
    }
    int status = r_runtime_client_init(&app.runtime, &options);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "client initialization failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        hloop_free(&app.loop);
        return EXIT_FAILURE;
    }

    size_t available_sockets = r_runtime_socket_count(&app.runtime);
    for (size_t i = 0; i < available_sockets; i++) {
        int socket_fd = (int)r_runtime_socket_at(&app.runtime, i);
        hio_t *io = hio_get(app.loop, socket_fd);
        if (!io || hio_add(io, on_udp_readable, HV_READ) != 0) {
            stop_with_error(&app, RCLIENT_ERR_IO);
            break;
        }
        hio_set_context(io, &app);
        app.socket_events[app.socket_count++] = io;
    }

    if (!app.done) {
        r_admission_config_t config;
        r_client_admission_config_defaults(&config);
        config.bucket_name = "libhv-example";
        config.service_name = "libhv-protected-service";
        config.metrics_label = "libhv-example";
        status = r_client_admission_start(
            app.runtime.handle,
            &app.request,
            &config,
            on_admission,
            &app
        );
        if (status != RCLIENT_OK || arm_timer(&app) != 0) {
            stop_with_error(&app, status != RCLIENT_OK ? status : RCLIENT_ERR_IO);
        }
    }
    if (!app.done && hloop_run(app.loop) != 0) {
        stop_with_error(&app, RCLIENT_ERR_IO);
    }

    if (app.request.active) {
        r_runtime_admission_cancel(&app.runtime, &app.request);
    }
    if (app.timer) {
        htimer_del(app.timer);
    }
    for (size_t i = 0; i < app.socket_count; i++) {
        hio_del(app.socket_events[i], HV_READ);
    }
    r_runtime_client_destroy(&app.runtime);
    hloop_free(&app.loop);

    if (app.status != RCLIENT_OK) {
        fprintf(stderr, "rate-limit check failed: %s (%d)\n",
            r_runtime_status_name(app.status), app.status);
        return EXIT_FAILURE;
    }
    if (app.outcome.allowed) {
        printf("allowed: %s; latency=%" PRIu32 " ms\n",
            app.response, app.observed_latency_ms);
    } else if (app.outcome.rate_limited && app.outcome.latency_limited) {
        puts("denied: resource limit and latency guard");
    } else if (app.outcome.latency_limited) {
        puts("denied: latency guard");
    } else {
        puts("denied: resource rate limit");
    }
    return app.outcome.allowed ? EXIT_SUCCESS : 2;
}
