#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <event2/event.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. Persistent EV_READ events observe the runtime's UDP sockets.
 * 2. A one-shot evtimer follows the request's current deadline.
 * 3. The callback distinguishes resource and latency-guard denials.
 * 4. Only admitted work prepares the application response.
 * 5. The runtime measures that completed work and reports one sample.
 *
 * Ownership: application owns the event base, events, request, and outcome.
 * runtime owns the client and sockets; events are freed before those sockets.
 */

typedef struct application {
    struct event_base *base;
    struct event *socket_events[2];
    size_t socket_count;
    struct event *timer;
    r_runtime_client_t runtime;
    r_admission_request_t request;
    r_admission_outcome_t outcome;
    char response[96];
    uint32_t observed_latency_ms;
    int status;
    bool done;
} application_t;

static void stop_with_error(application_t *app, int status) {
    app->status = status;
    app->done = true;
    event_base_loopbreak(app->base);
}

static int prepare_response(void *user) {
    application_t *app = user;
    int length = snprintf(
        app->response,
        sizeof(app->response),
        "inventory response prepared by libevent"
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
    evtimer_del(app->timer);
    event_base_loopbreak(app->base);
}

static int arm_timer(application_t *app);

static void on_timeout(evutil_socket_t socket_value, short events, void *user) {
    (void)socket_value;
    (void)events;
    application_t *app = user;
    int status = r_runtime_admission_on_timeout(&app->runtime, &app->request);
    if (status != RCLIENT_OK) {
        stop_with_error(app, status);
    } else if (app->request.active && arm_timer(app) != 0) {
        stop_with_error(app, RCLIENT_ERR_IO);
    }
}

static int arm_timer(application_t *app) {
    uint64_t delay_ms = 0u;
    if (r_runtime_admission_delay_ms(&app->request, &delay_ms) != RCLIENT_OK) {
        return -1;
    }
    struct timeval delay = {
        .tv_sec = (time_t)(delay_ms / 1000u),
        .tv_usec = (suseconds_t)((delay_ms % 1000u) * 1000u),
    };
    return evtimer_add(app->timer, &delay);
}

static void on_udp_readable(
    evutil_socket_t socket_value,
    short events,
    void *user
) {
    application_t *app = user;
    if ((events & EV_READ) == 0) {
        stop_with_error(app, RCLIENT_ERR_IO);
        return;
    }
    int status = r_runtime_client_on_readable(
        &app->runtime,
        (r_runtime_socket_t)socket_value
    );
    if (status != RCLIENT_OK) {
        stop_with_error(app, status);
    }
}

static int initialize_events(application_t *app) {
    app->timer = evtimer_new(app->base, on_timeout, app);
    if (!app->timer) {
        return RCLIENT_ERR_NOMEM;
    }
    size_t count = r_runtime_socket_count(&app->runtime);
    for (size_t i = 0; i < count; i++) {
        evutil_socket_t socket_value =
            (evutil_socket_t)r_runtime_socket_at(&app->runtime, i);
        struct event *socket_event = event_new(
            app->base,
            socket_value,
            EV_READ | EV_PERSIST,
            on_udp_readable,
            app
        );
        if (!socket_event) {
            return RCLIENT_ERR_NOMEM;
        }
        app->socket_events[app->socket_count++] = socket_event;
        if (event_add(socket_event, NULL) != 0) {
            return RCLIENT_ERR_IO;
        }
    }
    return app->socket_count > 0u ? RCLIENT_OK : RCLIENT_ERR_IO;
}

static int start_admission(application_t *app) {
    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "libevent-example";
    config.service_name = "libevent-protected-service";
    config.metrics_label = "libevent-example";
    int status = r_client_admission_start(
        app->runtime.handle,
        &app->request,
        &config,
        on_admission,
        app
    );
    if (status != RCLIENT_OK || arm_timer(app) != 0) {
        return status != RCLIENT_OK ? status : RCLIENT_ERR_IO;
    }
    return RCLIENT_OK;
}

static void destroy_application(application_t *app) {
    if (app->request.active) {
        r_runtime_admission_cancel(&app->runtime, &app->request);
    }
    for (size_t i = 0; i < app->socket_count; i++) {
        event_free(app->socket_events[i]);
    }
    if (app->timer) {
        event_free(app->timer);
    }
    r_runtime_client_destroy(&app->runtime);
    event_base_free(app->base);
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
    app.base = event_base_new();
    if (!app.base) {
        return EXIT_FAILURE;
    }
    int status = r_runtime_client_init(&app.runtime, &options);
    if (status == RCLIENT_OK) {
        status = initialize_events(&app);
    }
    if (status == RCLIENT_OK) {
        status = start_admission(&app);
    }
    if (status == RCLIENT_OK && event_base_dispatch(app.base) < 0) {
        status = RCLIENT_ERR_IO;
    } else if (status == RCLIENT_OK) {
        status = app.status;
    }

    destroy_application(&app);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "libevent example failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }
    print_outcome(&app);
    return app.outcome.allowed ? EXIT_SUCCESS : 2;
}
