#include <stdio.h>
#include <stdlib.h>

#include <uv.h>

#include "common/rl_example.h"

/*
 * libuv integration map
 * ---------------------
 *   uv_poll_t  -> reports readable rl-c-client UDP sockets
 *   uv_timer_t -> wakes at the current request deadline
 *   callback   -> records the decision and stops uv_run()
 *
 * The common adapter owns the descriptors. uv_poll_t only observes them, so
 * cleanup stops the watchers before the adapter closes the sockets.
 */
typedef struct libuv_app libuv_app_t;

typedef struct socket_watcher {
    uv_poll_t poll;
    libuv_app_t *app;
    int socket_fd;
} socket_watcher_t;

struct libuv_app {
    uv_loop_t loop;
    uv_timer_t timer;
    socket_watcher_t sockets[2];
    size_t socket_count;
    rl_example_client_t client;
    rl_example_request_t request;
    int status;
    bool allowed;
    bool done;
};

static void stop_with_error(libuv_app_t *app, int status) {
    app->status = status;
    app->done = true;
    uv_stop(&app->loop);
}

static void on_rate_limit(void *user, int status, bool allowed) {
    libuv_app_t *app = user;
    app->status = status;
    app->allowed = allowed;
    app->done = true;
    uv_timer_stop(&app->timer);
    uv_stop(&app->loop);
}

static int arm_timer(libuv_app_t *app);

static void on_timeout(uv_timer_t *timer) {
    libuv_app_t *app = timer->data;
    /* This can synchronously complete the request, hence the active check. */
    int status = rl_example_request_on_timeout(&app->client, &app->request);
    if (status != RCLIENT_OK) {
        stop_with_error(app, status);
    } else if (app->request.active && arm_timer(app) != 0) {
        stop_with_error(app, RCLIENT_ERR_IO);
    }
}

static int arm_timer(libuv_app_t *app) {
    uint64_t delay_ms = 0;
    int status = rl_example_request_delay_ms(&app->request, &delay_ms);
    if (status != RCLIENT_OK) {
        return -1;
    }
    /* repeat=0: each client transition may publish a different deadline. */
    return uv_timer_start(&app->timer, on_timeout, delay_ms, 0);
}

static void on_udp_readable(uv_poll_t *poll, int status, int events) {
    socket_watcher_t *watcher = poll->data;
    libuv_app_t *app = watcher->app;
    if (status < 0 || (events & UV_READABLE) == 0) {
        stop_with_error(app, RCLIENT_ERR_IO);
        return;
    }
    /* The adapter drains the original nonblocking descriptor to EAGAIN. */
    int client_status = rl_example_client_on_readable(&app->client, watcher->socket_fd);
    if (client_status != RCLIENT_OK) {
        stop_with_error(app, client_status);
    }
}

static void close_handles(libuv_app_t *app) {
    uv_timer_stop(&app->timer);
    uv_close((uv_handle_t *)&app->timer, NULL);
    for (size_t i = 0; i < app->socket_count; i++) {
        uv_poll_stop(&app->sockets[i].poll);
        uv_close((uv_handle_t *)&app->sockets[i].poll, NULL);
    }
    uv_run(&app->loop, UV_RUN_DEFAULT);
}

int main(void) {
    rl_example_options_t options;
    if (rl_example_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n");
        return EXIT_FAILURE;
    }

    libuv_app_t app = {0};
    app.status = RCLIENT_ERR_IO;
    if (uv_loop_init(&app.loop) != 0) {
        return EXIT_FAILURE;
    }
    if (rl_example_client_init(&app.client, &options) != RCLIENT_OK) {
        uv_loop_close(&app.loop);
        return EXIT_FAILURE;
    }

    if (uv_timer_init(&app.loop, &app.timer) != 0) {
        rl_example_client_destroy(&app.client);
        uv_loop_close(&app.loop);
        return EXIT_FAILURE;
    }
    app.timer.data = &app;

    size_t available_sockets = rl_example_socket_count(&app.client);
    for (size_t i = 0; i < available_sockets; i++) {
        socket_watcher_t *watcher = &app.sockets[i];
        watcher->app = &app;
        watcher->socket_fd = rl_example_socket_at(&app.client, i);
        if (uv_poll_init(&app.loop, &watcher->poll, watcher->socket_fd) != 0) {
            stop_with_error(&app, RCLIENT_ERR_IO);
            break;
        }
        app.socket_count++;
        watcher->poll.data = watcher;
        if (uv_poll_start(&watcher->poll, UV_READABLE, on_udp_readable) != 0) {
            stop_with_error(&app, RCLIENT_ERR_IO);
            break;
        }
    }

    if (!app.done) {
        int status = rl_example_check(
            &app.client,
            &app.request,
            "libuv-example",
            on_rate_limit,
            &app
        );
        if (status != RCLIENT_OK || arm_timer(&app) != 0) {
            stop_with_error(&app, status != RCLIENT_OK ? status : RCLIENT_ERR_IO);
        }
    }

    if (!app.done) {
        uv_run(&app.loop, UV_RUN_DEFAULT);
    }
    if (app.request.active) {
        rl_example_request_cancel(&app.client, &app.request);
    }
    close_handles(&app);
    rl_example_client_destroy(&app.client);
    uv_loop_close(&app.loop);

    if (app.status != RCLIENT_OK) {
        fprintf(stderr, "rate-limit check failed: %d\n", app.status);
        return EXIT_FAILURE;
    }
    puts(app.allowed ? "allowed" : "denied");
    return app.allowed ? EXIT_SUCCESS : 2;
}
