#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <mongoose.h>

#include "common/rl_example.h"

typedef struct mongoose_app mongoose_app_t;

typedef struct pending_request {
    struct pending_request *next;
    mongoose_app_t *app;
    struct mg_connection *connection;
    rl_example_request_t request;
} pending_request_t;

struct mongoose_app {
    struct mg_mgr manager;
    rl_example_client_t client;
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

static void on_rate_limit(void *user, int status, bool allowed) {
    pending_request_t *pending = user;
    mongoose_app_t *app = pending->app;
    remove_pending(app, pending);
    if (status != RCLIENT_OK) {
        mg_http_reply(pending->connection, 503, "Content-Type: text/plain\r\n",
            "rate-limit service unavailable\n");
    } else if (!allowed) {
        mg_http_reply(pending->connection, 429, "Content-Type: text/plain\r\n",
            "denied\n");
    } else {
        mg_http_reply(pending->connection, 200, "Content-Type: text/plain\r\n",
            "allowed\n");
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
    rl_example_request_cancel(&app->client, &pending->request);
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
    int status = rl_example_check(
        &app->client,
        &pending->request,
        "mongoose-example",
        on_rate_limit,
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
        if (mg_match(message->uri, mg_str("/limited"), NULL)) {
            begin_rate_limit(app, connection);
        } else {
            mg_http_reply(connection, 404, "Content-Type: text/plain\r\n",
                "not found\n");
        }
    } else if (event == MG_EV_CLOSE) {
        cancel_connection_request(app, connection);
    }
}

static int drive_rate_limits(mongoose_app_t *app) {
    size_t socket_count = rl_example_socket_count(&app->client);
    for (size_t i = 0; i < socket_count; i++) {
        int status = rl_example_client_on_readable(
            &app->client,
            rl_example_socket_at(&app->client, i)
        );
        if (status != RCLIENT_OK) {
            return status;
        }
    }

    pending_request_t *pending = app->pending;
    while (pending) {
        pending_request_t *next = pending->next;
        uint64_t delay_ms = 0;
        int status = rl_example_request_delay_ms(&pending->request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        if (delay_ms == 0) {
            status = rl_example_request_on_timeout(&app->client, &pending->request);
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
        rl_example_request_cancel(&app->client, &pending->request);
        free(pending);
    }
}

int main(void) {
    rl_example_options_t options;
    if (rl_example_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n");
        return EXIT_FAILURE;
    }

    mongoose_app_t app = {0};
    if (rl_example_client_init(&app.client, &options) != RCLIENT_OK) {
        return EXIT_FAILURE;
    }
    mg_mgr_init(&app.manager);
    if (!mg_http_listen(&app.manager, "http://0.0.0.0:8000", on_http_event, &app)) {
        mg_mgr_free(&app.manager);
        rl_example_client_destroy(&app.client);
        return EXIT_FAILURE;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (!stop_requested) {
        mg_mgr_poll(&app.manager, 10);
        int status = drive_rate_limits(&app);
        if (status != RCLIENT_OK) {
            fprintf(stderr, "rate-limit event loop failed: %d\n", status);
            break;
        }
    }

    cancel_all(&app);
    mg_mgr_free(&app.manager);
    rl_example_client_destroy(&app.client);
    return EXIT_SUCCESS;
}
