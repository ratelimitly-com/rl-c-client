#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include <microhttpd.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. MHD contributes HTTP fd_sets; the adapter adds client UDP descriptors.
 * 2. One select() waits using the earliest MHD or rl-c-client deadline.
 * 3. GET /limited starts a check and suspends its HTTP connection.
 * 4. Admission runs and measures protected work, then resumes the connection.
 * 5. MHD invokes the handler again, which maps the combined decision to HTTP.
 *
 * Ownership: MHD owns connections and per-connection request_context pointers.
 * The app owns rl-c-client and the pending list. MHD completion notification
 * cancels abandoned work before freeing request state.
 */
typedef struct microhttpd_app microhttpd_app_t;

typedef struct pending_request {
    struct pending_request *next;
    microhttpd_app_t *app;
    struct MHD_Connection *connection;
    r_admission_request_t request;
    r_admission_outcome_t outcome;
    char response_body[96];
    uint32_t observed_latency_ms;
    int status;
    bool started;
    bool done;
} pending_request_t;

struct microhttpd_app {
    struct MHD_Daemon *daemon;
    r_runtime_client_t runtime;
    pending_request_t *pending;
};

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static void remove_pending(microhttpd_app_t *app, pending_request_t *pending) {
    pending_request_t **cursor = &app->pending;
    while (*cursor) {
        if (*cursor == pending) {
            *cursor = pending->next;
            pending->next = NULL;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

static int prepare_protected_response(void *user) {
    pending_request_t *pending = user;
    /* Replace this with the database/API operation the endpoint protects. */
    int length = snprintf(pending->response_body,
        sizeof(pending->response_body), "allowed (protected work complete)\n");
    return length > 0 && (size_t)length < sizeof(pending->response_body)
        ? RCLIENT_OK
        : RCLIENT_ERR_IO;
}

static void on_admission(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    pending_request_t *pending = user;
    pending->status = status;
    pending->outcome = *outcome;
    if (status == RCLIENT_OK && outcome->allowed) {
        int report_status = r_runtime_admission_run_and_report(
            &pending->app->runtime,
            &pending->request,
            prepare_protected_response,
            pending,
            &pending->observed_latency_ms
        );
        if (report_status != RCLIENT_OK) {
            fprintf(stderr, "latency report failed: %s (%d)\n",
                r_runtime_status_name(report_status), report_status);
        }
    }
    pending->done = true;
    /* Remove before resume because the handler may run again immediately. */
    remove_pending(pending->app, pending);
    MHD_resume_connection(pending->connection);
}

static enum MHD_Result queue_text(
    struct MHD_Connection *connection,
    unsigned int status,
    const char *body
) {
    struct MHD_Response *response = MHD_create_response_from_buffer(
        strlen(body),
        (void *)body,
        MHD_RESPMEM_PERSISTENT
    );
    if (!response) {
        return MHD_NO;
    }
    MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain");
    enum MHD_Result result = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return result;
}

static enum MHD_Result queue_method_not_allowed(
    struct MHD_Connection *connection
) {
    static const char body[] = "method not allowed\n";
    struct MHD_Response *response = MHD_create_response_from_buffer(
        sizeof(body) - 1,
        (void *)body,
        MHD_RESPMEM_PERSISTENT
    );
    if (!response) {
        return MHD_NO;
    }
    MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain");
    MHD_add_response_header(response, MHD_HTTP_HEADER_ALLOW, MHD_HTTP_METHOD_GET);
    enum MHD_Result result = MHD_queue_response(
        connection,
        MHD_HTTP_METHOD_NOT_ALLOWED,
        response
    );
    MHD_destroy_response(response);
    return result;
}

static enum MHD_Result queue_admission_result(
    struct MHD_Connection *connection,
    const pending_request_t *pending
) {
    if (pending->status != RCLIENT_OK) {
        return queue_text(connection, MHD_HTTP_SERVICE_UNAVAILABLE,
            "rate-limit service unavailable\n");
    }
    if (pending->outcome.rate_limited && pending->outcome.latency_limited) {
        return queue_text(connection, 429,
            "denied by resource limit and latency guard\n");
    }
    if (pending->outcome.latency_limited) {
        return queue_text(connection, MHD_HTTP_SERVICE_UNAVAILABLE,
            "denied by latency guard\n");
    }
    if (!pending->outcome.allowed) {
        return queue_text(connection, 429,
            "denied by resource rate limit\n");
    }
    return queue_text(connection, MHD_HTTP_OK, pending->response_body);
}

static enum MHD_Result begin_admission(pending_request_t *pending) {
    microhttpd_app_t *app = pending->app;
    pending->started = true;
    pending->next = app->pending;
    app->pending = pending;

    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "libmicrohttpd-example";
    config.service_name = "libmicrohttpd-protected-service";
    config.metrics_label = "libmicrohttpd-example";
    int status = r_client_admission_start(
        app->runtime.handle,
        &pending->request,
        &config,
        on_admission,
        pending
    );
    if (status != RCLIENT_OK) {
        remove_pending(app, pending);
        pending->status = status;
        pending->done = true;
        return queue_text(pending->connection, MHD_HTTP_SERVICE_UNAVAILABLE,
            "rate-limit check failed\n");
    }

    /* Suspension releases MHD from repeatedly calling this handler. */
    MHD_suspend_connection(pending->connection);
    return MHD_YES;
}

static enum MHD_Result handle_request(
    void *user,
    struct MHD_Connection *connection,
    const char *url,
    const char *method,
    const char *version,
    const char *upload_data,
    size_t *upload_data_size,
    void **request_context
) {
    (void)version;
    (void)upload_data;
    microhttpd_app_t *app = user;
    pending_request_t *pending = *request_context;
    if (!pending) {
        /* MHD's first call establishes per-connection state only. */
        pending = calloc(1, sizeof(*pending));
        if (!pending) {
            return MHD_NO;
        }
        pending->app = app;
        pending->connection = connection;
        *request_context = pending;
        return MHD_YES;
    }
    if (*upload_data_size != 0) {
        *upload_data_size = 0;
        return MHD_YES;
    }
    if (strcmp(url, "/limited") != 0) {
        return queue_text(connection, MHD_HTTP_NOT_FOUND, "not found\n");
    }
    if (strcmp(method, MHD_HTTP_METHOD_GET) != 0) {
        return queue_method_not_allowed(connection);
    }
    if (pending->done) {
        return queue_admission_result(connection, pending);
    }
    if (!pending->started) {
        return begin_admission(pending);
    }
    return MHD_YES;
}

static void request_completed(
    void *user,
    struct MHD_Connection *connection,
    void **request_context,
    enum MHD_RequestTerminationCode termination
) {
    (void)connection;
    (void)termination;
    microhttpd_app_t *app = user;
    pending_request_t *pending = *request_context;
    if (!pending) {
        return;
    }
    if (pending->request.active) {
        /* Completion notification also covers disconnects and daemon shutdown. */
        remove_pending(app, pending);
        r_runtime_admission_cancel(&app->runtime, &pending->request);
    }
    free(pending);
    *request_context = NULL;
}

static int expire_requests(microhttpd_app_t *app) {
    pending_request_t *pending = app->pending;
    while (pending) {
        pending_request_t *next = pending->next;
        uint64_t delay_ms = 0;
        int status = r_runtime_admission_delay_ms(&pending->request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        if (delay_ms == 0) {
            status = r_runtime_admission_on_timeout(
                &app->runtime,
                &pending->request
            );
            if (status != RCLIENT_OK) {
                return status;
            }
        }
        pending = next;
    }
    return RCLIENT_OK;
}

static uint64_t next_timeout_ms(microhttpd_app_t *app) {
    /* Start with a housekeeping bound, then take the minimum of both systems. */
    uint64_t timeout_ms = 1000;
    uint64_t mhd_timeout = 0;
    if (MHD_get_timeout64(app->daemon, &mhd_timeout) == MHD_YES
        && mhd_timeout < timeout_ms) {
        timeout_ms = mhd_timeout;
    }
    for (pending_request_t *pending = app->pending; pending; pending = pending->next) {
        uint64_t delay_ms = 0;
        if (r_runtime_admission_delay_ms(&pending->request, &delay_ms)
            != RCLIENT_OK) {
            return 0;
        }
        if (delay_ms < timeout_ms) {
            timeout_ms = delay_ms;
        }
    }
    return timeout_ms;
}

static int run_loop(microhttpd_app_t *app) {
    while (!stop_requested) {
        fd_set read_set;
        fd_set write_set;
        fd_set except_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_ZERO(&except_set);
        MHD_socket max_socket = 0;
        if (MHD_get_fdset(app->daemon, &read_set, &write_set,
                &except_set, &max_socket) != MHD_YES) {
            return RCLIENT_ERR_IO;
        }

        size_t socket_count = r_runtime_socket_count(&app->runtime);
        for (size_t i = 0; i < socket_count; i++) {
            int socket_fd = r_runtime_socket_at(&app->runtime, i);
            if (socket_fd >= FD_SETSIZE) {
                return RCLIENT_ERR_IO;
            }
            FD_SET(socket_fd, &read_set);
            if ((MHD_socket)socket_fd > max_socket) {
                max_socket = (MHD_socket)socket_fd;
            }
        }

        uint64_t timeout_ms = next_timeout_ms(app);
        struct timeval timeout = {
            .tv_sec = (time_t)(timeout_ms / 1000u),
            .tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u),
        };
        /* This is the only blocking call in the external event loop. */
        int ready = select((int)max_socket + 1, &read_set, &write_set,
            &except_set, &timeout);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            return RCLIENT_ERR_IO;
        }

        for (size_t i = 0; i < socket_count; i++) {
            int socket_fd = r_runtime_socket_at(&app->runtime, i);
            if (FD_ISSET(socket_fd, &read_set)) {
                int status = r_runtime_client_on_readable(
                    &app->runtime,
                    socket_fd
                );
                if (status != RCLIENT_OK) {
                    return status;
                }
            }
        }
        int status = expire_requests(app);
        if (status != RCLIENT_OK) {
            return status;
        }
        /* Let MHD consume the readiness already captured in its fd_sets. */
        if (MHD_run(app->daemon) != MHD_YES) {
            return RCLIENT_ERR_IO;
        }
    }
    return RCLIENT_OK;
}

int main(void) {
    r_runtime_options_t options;
    if (r_runtime_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n");
        return EXIT_FAILURE;
    }

    microhttpd_app_t app = {0};
    int status = r_runtime_client_init(&app.runtime, &options);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "client initialization failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }
    app.daemon = MHD_start_daemon(
        MHD_USE_ERROR_LOG | MHD_ALLOW_SUSPEND_RESUME,
        8000,
        NULL,
        NULL,
        handle_request,
        &app,
        MHD_OPTION_NOTIFY_COMPLETED,
        request_completed,
        &app,
        MHD_OPTION_END
    );
    if (!app.daemon) {
        fprintf(stderr, "failed to start libmicrohttpd on port 8000\n");
        r_runtime_client_destroy(&app.runtime);
        return EXIT_FAILURE;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    status = run_loop(&app);
    MHD_stop_daemon(app.daemon);
    r_runtime_client_destroy(&app.runtime);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "event loop failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
