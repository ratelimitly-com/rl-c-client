#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <mongoose.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. MG_EV_HTTP_MSG validates GET /limited and starts an asynchronous check.
 * 2. The connection stays open while pending state links it to that check.
 * 3. Each short mg_mgr_poll() tick is followed by UDP and deadline processing.
 * 4. Completion distinguishes rate/latency denial before replying.
 * 5. Allowed response work is measured and reported after completion.
 *
 * Ownership: the thread calling mg_mgr_poll() owns HTTP connections, the
 * runtime, UDP sockets, and pending requests. No locks or worker
 * handoff are needed. Mongoose has no arbitrary-fd watcher in this compact
 * pattern, so a 10 ms poll interval bounds UDP response latency.
 */
typedef struct mongoose_app mongoose_app_t;

typedef struct pending_request {
    struct pending_request *next;
    mongoose_app_t *app;
    struct mg_connection *connection;
    r_admission_request_t request;
} pending_request_t;

struct mongoose_app {
    struct mg_mgr manager;
    r_runtime_client_t runtime;
    pending_request_t *pending;
};

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static void remove_pending(mongoose_app_t *app, pending_request_t *pending) {
    pending_request_t **cursor = &app->pending;
    while (*cursor) {
        if (*cursor == pending) {
            *cursor = pending->next;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

static pending_request_t *find_pending(
    mongoose_app_t *app,
    struct mg_connection *connection
) {
    for (pending_request_t *item = app->pending; item; item = item->next) {
        if (item->connection == connection) {
            return item;
        }
    }
    return NULL;
}

static int send_allowed_reply(void *user) {
    pending_request_t *pending = user;
    mg_http_reply(pending->connection, 200, "Content-Type: text/plain\r\n",
        "allowed\n");
    return RCLIENT_OK;
}

static void on_admission(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    pending_request_t *pending = user;
    mongoose_app_t *app = pending->app;
    /* Unlink before replying: mg_http_reply may schedule connection teardown. */
    remove_pending(app, pending);
    if (status != RCLIENT_OK) {
        mg_http_reply(pending->connection, 503, "Content-Type: text/plain\r\n",
            "rate-limit service unavailable\n");
    } else if (outcome->latency_limited && outcome->rate_limited) {
        mg_http_reply(pending->connection, 429, "Content-Type: text/plain\r\n",
            "denied by resource limit and latency guard\n");
    } else if (outcome->latency_limited) {
        mg_http_reply(pending->connection, 503, "Content-Type: text/plain\r\n",
            "denied by latency guard\n");
    } else if (!outcome->allowed) {
        mg_http_reply(pending->connection, 429, "Content-Type: text/plain\r\n",
            "denied by resource rate limit\n");
    } else {
        status = r_runtime_admission_run_and_report(
            &app->runtime,
            &pending->request,
            send_allowed_reply,
            pending,
            NULL
        );
        if (status != RCLIENT_OK) {
            fprintf(stderr, "latency report failed: %s (%d)\n",
                r_runtime_status_name(status), status);
        }
    }
    free(pending);
}

static void cancel_connection_request(
    mongoose_app_t *app,
    struct mg_connection *connection
) {
    pending_request_t *pending = find_pending(app, connection);
    if (!pending) {
        return;
    }
    remove_pending(app, pending);
    /* MG_EV_CLOSE means the callback may no longer touch this connection. */
    r_runtime_admission_cancel(&app->runtime, &pending->request);
    free(pending);
}

static void begin_rate_limit(
    mongoose_app_t *app,
    struct mg_connection *connection
) {
    if (find_pending(app, connection)) {
        mg_http_reply(connection, 409, "Content-Type: text/plain\r\n",
            "request already pending\n");
        return;
    }
    pending_request_t *pending = calloc(1, sizeof(*pending));
    if (!pending) {
        mg_http_reply(connection, 503, "Content-Type: text/plain\r\n",
            "out of memory\n");
        return;
    }
    pending->app = app;
    pending->connection = connection;
    pending->next = app->pending;
    app->pending = pending;
    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "mongoose-example";
    config.service_name = "mongoose-protected-service";
    config.metrics_label = "mongoose-example";
    int status = r_client_admission_start(
        app->runtime.handle,
        &pending->request,
        &config,
        on_admission,
        pending
    );
    if (status != RCLIENT_OK) {
        remove_pending(app, pending);
        free(pending);
        mg_http_reply(connection, 503, "Content-Type: text/plain\r\n",
            "rate-limit check failed\n");
    }
}

static void on_http_event(
    struct mg_connection *connection,
    int event,
    void *event_data
) {
    mongoose_app_t *app = connection->fn_data;
    if (event == MG_EV_HTTP_MSG) {
        struct mg_http_message *message = event_data;
        if (!mg_match(message->uri, mg_str("/limited"), NULL)) {
            mg_http_reply(connection, 404, "Content-Type: text/plain\r\n",
                "not found\n");
        } else if (mg_strcmp(message->method, mg_str("GET")) != 0) {
            mg_http_reply(connection, 405,
                "Allow: GET\r\nContent-Type: text/plain\r\n",
                "method not allowed\n");
        } else {
            begin_rate_limit(app, connection);
        }
    } else if (event == MG_EV_CLOSE) {
        cancel_connection_request(app, connection);
    }
}

static int drive_rate_limits(mongoose_app_t *app) {
    /* Reads are nonblocking, so polling both possible UDP sockets is cheap. */
    size_t socket_count = r_runtime_socket_count(&app->runtime);
    for (size_t i = 0; i < socket_count; i++) {
        int status = r_runtime_client_on_readable(
            &app->runtime,
            r_runtime_socket_at(&app->runtime, i)
        );
        if (status != RCLIENT_OK) {
            return status;
        }
    }

    /* Save next first: timeout processing may complete and free pending. */
    pending_request_t *pending = app->pending;
    while (pending) {
        pending_request_t *next = pending->next;
        uint64_t delay_ms = 0;
        int status = r_runtime_admission_delay_ms(&pending->request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        if (delay_ms == 0) {
            status = r_runtime_admission_on_timeout(&app->runtime, &pending->request);
            if (status != RCLIENT_OK) {
                return status;
            }
        }
        pending = next;
    }
    return RCLIENT_OK;
}

static void cancel_all(mongoose_app_t *app) {
    while (app->pending) {
        pending_request_t *pending = app->pending;
        app->pending = pending->next;
        r_runtime_admission_cancel(&app->runtime, &pending->request);
        free(pending);
    }
}

int main(void) {
    r_runtime_options_t options;
    if (r_runtime_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n");
        return EXIT_FAILURE;
    }

    mongoose_app_t app = {0};
    int loop_status = r_runtime_client_init(&app.runtime, &options);
    if (loop_status != RCLIENT_OK) {
        fprintf(stderr, "client initialization failed: %s (%d)\n",
            r_runtime_status_name(loop_status), loop_status);
        return EXIT_FAILURE;
    }
    mg_mgr_init(&app.manager);
    if (!mg_http_listen(&app.manager, "http://0.0.0.0:8000", on_http_event, &app)) {
        fprintf(stderr, "failed to listen on port 8000\n");
        mg_mgr_free(&app.manager);
        r_runtime_client_destroy(&app.runtime);
        return EXIT_FAILURE;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (!stop_requested) {
        /* Ten milliseconds bounds latency because UDP fds are polled after it. */
        mg_mgr_poll(&app.manager, 10);
        loop_status = drive_rate_limits(&app);
        if (loop_status != RCLIENT_OK) {
            fprintf(stderr, "rate-limit event loop failed: %s (%d)\n",
                r_runtime_status_name(loop_status), loop_status);
            break;
        }
    }

    cancel_all(&app);
    mg_mgr_free(&app.manager);
    r_runtime_client_destroy(&app.runtime);
    return loop_status == RCLIENT_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
