#include <stdio.h>
#include <stdlib.h>

#include <event2/event.h>

#include "common/rl_example.h"

/*
 * libevent integration map
 * ------------------------
 * Persistent EV_READ events watch the adapter-owned UDP descriptors. A
 * one-shot evtimer represents the request deadline. The result callback calls
 * event_base_loopbreak(), making this a minimal one-check command-line sample.
 */
typedef struct libevent_app {
    struct event_base *base;
    struct event *socket_events[2];
    size_t socket_count;
    struct event *timer;
    rl_example_client_t client;
    rl_example_request_t request;
    int status;
    bool allowed;
    bool done;
} libevent_app_t;

static void stop_with_error(libevent_app_t *app, int status) {
    app->status = status;
    app->done = true;
    event_base_loopbreak(app->base);
}

static void on_rate_limit(void *user, int status, bool allowed) {
    libevent_app_t *app = user;
    app->status = status;
    app->allowed = allowed;
    app->done = true;
    evtimer_del(app->timer);
    event_base_loopbreak(app->base);
}

static int arm_timer(libevent_app_t *app);

static void on_timeout(evutil_socket_t socket_fd, short events, void *user) {
    (void)socket_fd;
    (void)events;
    libevent_app_t *app = user;
    /* A timeout transition may synchronously invoke on_rate_limit(). */
    int status = rl_example_request_on_timeout(&app->client, &app->request);
    if (status != RCLIENT_OK) {
        stop_with_error(app, status);
    } else if (app->request.active && arm_timer(app) != 0) {
        stop_with_error(app, RCLIENT_ERR_IO);
    }
}

static int arm_timer(libevent_app_t *app) {
    uint64_t delay_ms = 0;
    int status = rl_example_request_delay_ms(&app->request, &delay_ms);
    if (status != RCLIENT_OK) {
        return -1;
    }
    struct timeval delay = {
        .tv_sec = (time_t)(delay_ms / 1000u),
        .tv_usec = (suseconds_t)((delay_ms % 1000u) * 1000u),
    };
    /* Re-add the one-shot event after every published client deadline. */
    return evtimer_add(app->timer, &delay);
}

static void on_udp_readable(evutil_socket_t socket_fd, short events, void *user) {
    libevent_app_t *app = user;
    if ((events & EV_READ) == 0) {
        stop_with_error(app, RCLIENT_ERR_IO);
        return;
    }
    /* Persistent readiness is safe because the adapter drains to EAGAIN. */
    int status = rl_example_client_on_readable(&app->client, (int)socket_fd);
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

    libevent_app_t app = {0};
    app.status = RCLIENT_ERR_IO;
    app.base = event_base_new();
    if (!app.base) {
        return EXIT_FAILURE;
    }
    if (rl_example_client_init(&app.client, &options) != RCLIENT_OK) {
        event_base_free(app.base);
        return EXIT_FAILURE;
    }

    app.timer = evtimer_new(app.base, on_timeout, &app);
    if (!app.timer) {
        stop_with_error(&app, RCLIENT_ERR_NOMEM);
    }
    size_t available_sockets = rl_example_socket_count(&app.client);
    for (size_t i = 0; !app.done && i < available_sockets; i++) {
        int socket_fd = rl_example_socket_at(&app.client, i);
        app.socket_events[i] = event_new(
            app.base,
            socket_fd,
            EV_READ | EV_PERSIST,
            on_udp_readable,
            &app
        );
        if (!app.socket_events[i] || event_add(app.socket_events[i], NULL) != 0) {
            stop_with_error(&app, RCLIENT_ERR_IO);
            break;
        }
        app.socket_count++;
    }

    if (!app.done) {
        int status = rl_example_check(
            &app.client,
            &app.request,
            "libevent-example",
            on_rate_limit,
            &app
        );
        if (status != RCLIENT_OK || arm_timer(&app) != 0) {
            stop_with_error(&app, status != RCLIENT_OK ? status : RCLIENT_ERR_IO);
        }
    }
    if (!app.done && event_base_dispatch(app.base) < 0) {
        stop_with_error(&app, RCLIENT_ERR_IO);
    }

    if (app.request.active) {
        rl_example_request_cancel(&app.client, &app.request);
    }
    for (size_t i = 0; i < app.socket_count; i++) {
        event_free(app.socket_events[i]);
    }
    if (app.timer) {
        event_free(app.timer);
    }
    rl_example_client_destroy(&app.client);
    event_base_free(app.base);

    if (app.status != RCLIENT_OK) {
        fprintf(stderr, "rate-limit check failed: %d\n", app.status);
        return EXIT_FAILURE;
    }
    puts(app.allowed ? "allowed" : "denied");
    return app.allowed ? EXIT_SUCCESS : 2;
}
