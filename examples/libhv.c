#include <stdio.h>
#include <stdlib.h>

#include <hloop.h>

#include "common/rl_example.h"

/*
 * Flow
 * ----
 * 1. hio_get() attaches readiness watchers to rl-c-client UDP descriptors.
 * 2. The adapter drains readable sockets and handles received datagrams.
 * 3. A one-shot htimer_t advances the current request deadline.
 * 4. The result callback records the decision and stops hloop_run().
 *
 * Ownership: the adapter owns sockets; the hloop owns hio_t and htimer_t
 * objects. Watchers are detached before the adapter closes its descriptors.
 */
typedef struct libhv_app {
    hloop_t *loop;
    hio_t *socket_events[2];
    size_t socket_count;
    htimer_t *timer;
    rl_example_client_t client;
    rl_example_request_t request;
    int status;
    bool allowed;
    bool done;
} libhv_app_t;

static void stop_with_error(libhv_app_t *app, int status) {
    app->status = status;
    app->done = true;
    hloop_stop(app->loop);
}

static void on_rate_limit(void *user, int status, bool allowed) {
    libhv_app_t *app = user;
    app->status = status;
    app->allowed = allowed;
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
    int status = rl_example_request_on_timeout(&app->client, &app->request);
    if (status != RCLIENT_OK) {
        stop_with_error(app, status);
    } else if (app->request.active && arm_timer(app) != 0) {
        stop_with_error(app, RCLIENT_ERR_IO);
    }
}

static int arm_timer(libhv_app_t *app) {
    uint64_t delay_ms = 0;
    int status = rl_example_request_delay_ms(&app->request, &delay_ms);
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
    /* hio owns readiness; the adapter performs the actual recvfrom(). */
    int status = rl_example_client_on_readable(&app->client, hio_fd(io));
    if (status != RCLIENT_OK) {
        stop_with_error(app, status);
    }
}

int main(void) {
    rl_example_options_t options;
    if (rl_example_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n");
        return EXIT_FAILURE;
    }

    libhv_app_t app = {0};
    app.status = RCLIENT_ERR_IO;
    app.loop = hloop_new(0);
    if (!app.loop) {
        return EXIT_FAILURE;
    }
    int status = rl_example_client_init(&app.client, &options);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "client initialization failed: %s (%d)\n",
            rl_example_status_name(status), status);
        hloop_free(&app.loop);
        return EXIT_FAILURE;
    }

    size_t available_sockets = rl_example_socket_count(&app.client);
    for (size_t i = 0; i < available_sockets; i++) {
        int socket_fd = rl_example_socket_at(&app.client, i);
        hio_t *io = hio_get(app.loop, socket_fd);
        if (!io || hio_add(io, on_udp_readable, HV_READ) != 0) {
            stop_with_error(&app, RCLIENT_ERR_IO);
            break;
        }
        hio_set_context(io, &app);
        app.socket_events[app.socket_count++] = io;
    }

    if (!app.done) {
        status = rl_example_check(
            &app.client,
            &app.request,
            "libhv-example",
            on_rate_limit,
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
        rl_example_request_cancel(&app.client, &app.request);
    }
    if (app.timer) {
        htimer_del(app.timer);
    }
    for (size_t i = 0; i < app.socket_count; i++) {
        hio_del(app.socket_events[i], HV_READ);
    }
    rl_example_client_destroy(&app.client);
    hloop_free(&app.loop);

    if (app.status != RCLIENT_OK) {
        fprintf(stderr, "rate-limit check failed: %s (%d)\n",
            rl_example_status_name(app.status), app.status);
        return EXIT_FAILURE;
    }
    puts(app.allowed ? "allowed" : "denied");
    return app.allowed ? EXIT_SUCCESS : 2;
}
