#include <dispatch/dispatch.h>

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. A private serial dispatch queue owns every client transition.
 * 2. DISPATCH_SOURCE_TYPE_READ observes each runtime UDP socket.
 * 3. A one-shot timer source follows the current admission delay.
 * 4. Protected work runs only after resource and latency admission.
 * 5. Completed work is measured/reported, then a semaphore releases main().
 *
 * Ownership: application owns queue, sources, cancellation group, semaphore,
 * request, and outcome. runtime owns sockets, which stay open until every
 * dispatch source cancellation handler confirms that monitoring has stopped.
 */

typedef struct application application_t;

typedef struct socket_watcher {
    application_t *app;
    r_runtime_socket_t socket_value;
    dispatch_source_t source;
} socket_watcher_t;

struct application {
    dispatch_queue_t queue;
    dispatch_semaphore_t finished;
    dispatch_group_t source_cancellations;
    dispatch_source_t timer;
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

static void finish(application_t *app, int status) {
    if (app->done) {
        return;
    }
    app->status = status;
    app->done = true;
    dispatch_source_set_timer(
        app->timer,
        DISPATCH_TIME_FOREVER,
        DISPATCH_TIME_FOREVER,
        0u
    );
    dispatch_semaphore_signal(app->finished);
}

static int prepare_response(void *user) {
    application_t *app = user;
    int length = snprintf(
        app->response,
        sizeof(app->response),
        "inventory response prepared by libdispatch"
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
    app->outcome = *outcome;
    if (status == RCLIENT_OK && outcome->allowed) {
        status = r_runtime_admission_run_and_report(
            &app->runtime,
            &app->request,
            prepare_response,
            app,
            &app->observed_latency_ms
        );
    }
    finish(app, status);
}

static int arm_timer(application_t *app) {
    uint64_t delay_ms = 0u;
    int status = r_runtime_admission_delay_ms(&app->request, &delay_ms);
    if (status != RCLIENT_OK) {
        return status;
    }
    uint64_t max_delay_ms = (uint64_t)INT64_MAX / NSEC_PER_MSEC;
    int64_t delay_ns = delay_ms > max_delay_ms
        ? INT64_MAX
        : (int64_t)(delay_ms * NSEC_PER_MSEC);
    dispatch_source_set_timer(
        app->timer,
        dispatch_time(DISPATCH_TIME_NOW, delay_ns),
        DISPATCH_TIME_FOREVER,
        0u
    );
    return RCLIENT_OK;
}

static void on_timeout(void *context) {
    application_t *app = context;
    int status = r_runtime_admission_on_timeout(&app->runtime, &app->request);
    if (status != RCLIENT_OK) {
        finish(app, status);
    } else if (app->request.active && arm_timer(app) != RCLIENT_OK) {
        finish(app, RCLIENT_ERR_IO);
    }
}

static void on_udp_readable(void *context) {
    socket_watcher_t *watcher = context;
    int status = r_runtime_client_on_readable(
        &watcher->app->runtime,
        watcher->socket_value
    );
    if (status != RCLIENT_OK) {
        finish(watcher->app, status);
    }
}

/*
 * dispatch_source_cancel() only starts asynchronous cancellation. Each source
 * leaves this group from its cancellation handler, after libdispatch has
 * stopped referring to the source's timer or socket registration.
 */
static void on_timer_cancelled(void *context) {
    application_t *app = context;
    dispatch_group_leave(app->source_cancellations);
}

static void on_socket_cancelled(void *context) {
    socket_watcher_t *watcher = context;
    dispatch_group_leave(watcher->app->source_cancellations);
}

static int initialize_sources(application_t *app) {
    app->timer = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER,
        0u,
        0u,
        app->queue
    );
    if (!app->timer) {
        return RCLIENT_ERR_NOMEM;
    }
    dispatch_set_context(app->timer, app);
    dispatch_source_set_event_handler_f(app->timer, on_timeout);
    dispatch_group_enter(app->source_cancellations);
    dispatch_source_set_cancel_handler_f(app->timer, on_timer_cancelled);
    dispatch_resume(app->timer);

    size_t count = r_runtime_socket_count(&app->runtime);
    for (size_t i = 0; i < count; i++) {
        socket_watcher_t *watcher = &app->sockets[i];
        watcher->app = app;
        watcher->socket_value = r_runtime_socket_at(&app->runtime, i);
        watcher->source = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_READ,
            (uintptr_t)watcher->socket_value,
            0u,
            app->queue
        );
        if (!watcher->source) {
            return RCLIENT_ERR_NOMEM;
        }
        dispatch_set_context(watcher->source, watcher);
        dispatch_source_set_event_handler_f(watcher->source, on_udp_readable);
        dispatch_group_enter(app->source_cancellations);
        dispatch_source_set_cancel_handler_f(
            watcher->source,
            on_socket_cancelled
        );
        dispatch_resume(watcher->source);
        app->socket_count++;
    }
    return app->socket_count > 0u ? RCLIENT_OK : RCLIENT_ERR_IO;
}

static void start_admission(void *context) {
    application_t *app = context;
    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "libdispatch-example";
    config.service_name = "libdispatch-protected-service";
    config.metrics_label = "libdispatch-example";
    int status = r_client_admission_start(
        app->runtime.handle,
        &app->request,
        &config,
        on_admission,
        app
    );
    if (status == RCLIENT_OK) {
        status = arm_timer(app);
    }
    if (status != RCLIENT_OK) {
        finish(app, status);
    }
}

static void cancel_sources_on_queue(void *context) {
    application_t *app = context;
    if (app->request.active) {
        r_runtime_admission_cancel(&app->runtime, &app->request);
    }
    if (app->timer) {
        dispatch_source_cancel(app->timer);
        dispatch_release(app->timer);
        app->timer = NULL;
    }
    for (size_t i = 0; i < app->socket_count; i++) {
        dispatch_source_cancel(app->sockets[i].source);
        dispatch_release(app->sockets[i].source);
        app->sockets[i].source = NULL;
    }
}

static void destroy_runtime_on_queue(void *context) {
    application_t *app = context;
    r_runtime_client_destroy(&app->runtime);
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
    app.queue = dispatch_queue_create("com.ratelimitly.example", NULL);
    app.finished = dispatch_semaphore_create(0);
    app.source_cancellations = dispatch_group_create();
    if (!app.queue || !app.finished || !app.source_cancellations) {
        if (app.source_cancellations) {
            dispatch_release(app.source_cancellations);
        }
        if (app.finished) {
            dispatch_release(app.finished);
        }
        if (app.queue) {
            dispatch_release(app.queue);
        }
        return EXIT_FAILURE;
    }
    int status = r_runtime_client_init(&app.runtime, &options);
    if (status == RCLIENT_OK) {
        status = initialize_sources(&app);
    }
    if (status == RCLIENT_OK) {
        dispatch_async_f(app.queue, &app, start_admission);
        dispatch_semaphore_wait(app.finished, DISPATCH_TIME_FOREVER);
        status = app.status;
    }

    dispatch_sync_f(app.queue, &app, cancel_sources_on_queue);

    /*
     * Wait outside the serial queue: cancellation handlers are delivered on
     * that queue. Once all handlers have run, runtime-owned sockets are safe
     * to close.
     */
    dispatch_group_wait(app.source_cancellations, DISPATCH_TIME_FOREVER);
    dispatch_sync_f(app.queue, &app, destroy_runtime_on_queue);
    dispatch_release(app.source_cancellations);
    dispatch_release(app.finished);
    dispatch_release(app.queue);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "libdispatch example failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }
    print_outcome(&app);
    return app.outcome.allowed ? EXIT_SUCCESS : 2;
}
