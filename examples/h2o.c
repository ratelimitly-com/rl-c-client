#define _POSIX_C_SOURCE 200809L

#if defined(__linux__)
#define H2O_USE_EPOLL 1
#elif defined(__APPLE__)
#define H2O_USE_KQUEUE 1
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <h2o.h>
#include <h2o/http1.h>
#include <h2o/http2.h>

#include "common/rl_example.h"

/*
 * Flow
 * ----
 * 1. H2O watches duplicates of the rl-c-client UDP descriptors.
 * 2. GET /limited allocates check state from the H2O request pool.
 * 3. A one-shot H2O timer advances the current request deadline.
 * 4. UDP readiness lets the adapter consume datagrams from original sockets.
 * 5. Completion sends HTTP 200, 429, or 503.
 *
 * Ownership: H2O closes duplicate watcher descriptors; the adapter owns the
 * originals. A request-pool disposer cancels abandoned checks. Compatibility
 * branches cover both H2O 2.2 and current 2.3 timer APIs.
 */
typedef struct h2o_app h2o_app_t;

typedef struct h2o_rl_handler {
    h2o_handler_t super;
    h2o_app_t *app;
} h2o_rl_handler_t;

typedef struct pending_request {
    h2o_app_t *app;
    h2o_req_t *http_request;
    rl_example_request_t request;
#if H2O_VERSION_MAJOR == 2 && H2O_VERSION_MINOR < 3
    h2o_timeout_t timeout;
    h2o_timeout_entry_t timer;
    bool timeout_initialized;
#else
    h2o_timer_t timer;
#endif
} pending_request_t;

typedef struct socket_watcher {
    h2o_app_t *app;
    h2o_socket_t *socket;
    int client_fd;
} socket_watcher_t;

struct h2o_app {
    h2o_globalconf_t config;
    h2o_context_t context;
    h2o_accept_ctx_t accept;
    h2o_socket_t *listener;
    socket_watcher_t watchers[2];
    size_t watcher_count;
    rl_example_client_t client;
};

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static int arm_timer(pending_request_t *pending);

static void unlink_timer(pending_request_t *pending) {
#if H2O_VERSION_MAJOR == 2 && H2O_VERSION_MINOR < 3
    h2o_timeout_unlink(&pending->timer);
#else
    h2o_timer_unlink(&pending->timer);
#endif
}

static void send_text_response(
    h2o_req_t *request,
    int status,
    const char *reason,
    const char *body
) {
    request->res.status = status;
    request->res.reason = reason;
    h2o_add_header(&request->pool, &request->res.headers,
        H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/plain"));
    h2o_send_inline(request, body, strlen(body));
}

static void send_result(pending_request_t *pending, int status, bool allowed) {
    h2o_req_t *request = pending->http_request;
    if (!request) {
        return;
    }
    /* Stop timeout callbacks before H2O begins finalizing the request pool. */
    unlink_timer(pending);
    if (status != RCLIENT_OK) {
        send_text_response(request, 503, "Service Unavailable",
            "rate-limit service unavailable\n");
        return;
    }
    send_text_response(request,
        allowed ? 200 : 429,
        allowed ? "OK" : "Too Many Requests",
        allowed ? "allowed\n" : "denied\n");
}

static void on_rate_limit(void *user, int status, bool allowed) {
    send_result(user, status, allowed);
}

#if H2O_VERSION_MAJOR == 2 && H2O_VERSION_MINOR < 3
static void on_timeout(h2o_timeout_entry_t *timer) {
#else
static void on_timeout(h2o_timer_t *timer) {
#endif
    pending_request_t *pending = H2O_STRUCT_FROM_MEMBER(
        pending_request_t,
        timer,
        timer
    );
    int status = rl_example_request_on_timeout(&pending->app->client, &pending->request);
    if (status != RCLIENT_OK) {
        rl_example_request_cancel(&pending->app->client, &pending->request);
        send_result(pending, status, false);
    } else if (pending->request.active && arm_timer(pending) != 0) {
        rl_example_request_cancel(&pending->app->client, &pending->request);
        send_result(pending, RCLIENT_ERR_IO, false);
    }
}

static int arm_timer(pending_request_t *pending) {
    uint64_t delay_ms = 0;
    int status = rl_example_request_delay_ms(&pending->request, &delay_ms);
    if (status != RCLIENT_OK) {
        return -1;
    }
#if H2O_VERSION_MAJOR == 2 && H2O_VERSION_MINOR < 3
    /* H2O 2.2 uses a timeout wheel plus a separately linked entry. */
    if (!pending->timeout_initialized) {
        h2o_timeout_init(pending->http_request->conn->ctx->loop,
            &pending->timeout, delay_ms);
        pending->timer.cb = on_timeout;
        pending->timeout_initialized = true;
    }
    h2o_timeout_link(pending->http_request->conn->ctx->loop,
        &pending->timeout, &pending->timer);
#else
    /* H2O 2.3 replaced the timeout pair with a direct timer object. */
    h2o_timer_link(pending->http_request->conn->ctx->loop, delay_ms, &pending->timer);
#endif
    return 0;
}

static void dispose_pending(void *data) {
    pending_request_t *pending = data;
    unlink_timer(pending);
#if H2O_VERSION_MAJOR == 2 && H2O_VERSION_MINOR < 3
    if (pending->timeout_initialized) {
        h2o_timeout_dispose(pending->http_request->conn->ctx->loop, &pending->timeout);
    }
#endif
    /* The pool can disappear because of success, error, or peer disconnect. */
    if (pending->request.active) {
        rl_example_request_cancel(&pending->app->client, &pending->request);
    }
    pending->http_request = NULL;
}

static int on_http_request(h2o_handler_t *handler, h2o_req_t *request) {
    if (!h2o_memis(request->method.base, request->method.len, H2O_STRLIT("GET"))) {
        h2o_add_header(&request->pool, &request->res.headers,
            H2O_TOKEN_ALLOW, NULL, H2O_STRLIT("GET"));
        send_text_response(request, 405, "Method Not Allowed",
            "method not allowed\n");
        return 0;
    }
    h2o_rl_handler_t *rl_handler = (h2o_rl_handler_t *)handler;
    pending_request_t *pending = h2o_mem_alloc_shared(
        &request->pool,
        sizeof(*pending),
        dispose_pending
    );
    memset(pending, 0, sizeof(*pending));
    pending->app = rl_handler->app;
    pending->http_request = request;
#if H2O_VERSION_MAJOR == 2 && H2O_VERSION_MINOR < 3
    pending->timer.cb = on_timeout;
#else
    h2o_timer_init(&pending->timer, on_timeout);
#endif

    int status = rl_example_check(
        &pending->app->client,
        &pending->request,
        "h2o-example",
        on_rate_limit,
        pending
    );
    if (status != RCLIENT_OK || arm_timer(pending) != 0) {
        if (pending->request.active) {
            rl_example_request_cancel(&pending->app->client, &pending->request);
        }
        send_result(pending, status != RCLIENT_OK ? status : RCLIENT_ERR_IO, false);
    }
    return 0;
}

static void on_udp_readable(h2o_socket_t *socket, const char *error) {
    socket_watcher_t *watcher = socket->data;
    if (error) {
        fprintf(stderr, "H2O UDP watcher failed: %s\n", error);
        return;
    }
    int status = rl_example_client_on_readable(&watcher->app->client, watcher->client_fd);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "Ratelimitly UDP ingress failed: %s (%d)\n",
            rl_example_status_name(status), status);
    }
}

static void on_accept(h2o_socket_t *listener, const char *error) {
    h2o_app_t *app = listener->data;
    if (error) {
        fprintf(stderr, "H2O accept failed: %s\n", error);
        return;
    }
    h2o_socket_t *socket = h2o_evloop_socket_accept(listener);
    if (socket) {
        h2o_accept(&app->accept, socket);
    }
}

static int create_listener(h2o_app_t *app) {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return -1;
    }
    int reuse_address = 1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(8000),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR,
            &reuse_address, sizeof(reuse_address)) != 0
        || bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0
        || listen(socket_fd, SOMAXCONN) != 0) {
        close(socket_fd);
        return -1;
    }
    app->listener = h2o_evloop_socket_create(
        app->context.loop,
        socket_fd,
        H2O_SOCKET_FLAG_DONT_READ
    );
    if (!app->listener) {
        close(socket_fd);
        return -1;
    }
    app->listener->data = app;
    h2o_socket_read_start(app->listener, on_accept);
    return 0;
}

static void close_loop_sockets(h2o_app_t *app) {
    if (app->listener) {
        h2o_socket_close(app->listener);
        app->listener = NULL;
    }
    for (size_t i = 0; i < app->watcher_count; i++) {
        h2o_socket_close(app->watchers[i].socket);
        app->watchers[i].socket = NULL;
    }
    app->watcher_count = 0;
}

static void shutdown_context(h2o_app_t *app, h2o_evloop_t *loop) {
    close_loop_sockets(app);
    h2o_context_request_shutdown(&app->context);
    /* Drain deferred connection cleanup before disposing the context. */
    for (size_t i = 0; i < 100; i++) {
        if (h2o_evloop_run(loop, 10) != 0) {
            break;
        }
    }
    h2o_context_dispose(&app->context);
}

static int create_udp_watchers(h2o_app_t *app) {
    size_t socket_count = rl_example_socket_count(&app->client);
    for (size_t i = 0; i < socket_count; i++) {
        socket_watcher_t *watcher = &app->watchers[i];
        watcher->app = app;
        watcher->client_fd = rl_example_socket_at(&app->client, i);
        /* H2O closes its wrapper; dup keeps adapter ownership independent. */
        int duplicate_fd = dup(watcher->client_fd);
        if (duplicate_fd < 0) {
            return -1;
        }
        watcher->socket = h2o_evloop_socket_create(
            app->context.loop,
            duplicate_fd,
            H2O_SOCKET_FLAG_DONT_READ
        );
        if (!watcher->socket) {
            close(duplicate_fd);
            return -1;
        }
        watcher->socket->data = watcher;
        h2o_socket_read_start(watcher->socket, on_udp_readable);
        app->watcher_count++;
    }
    return 0;
}

int main(void) {
    rl_example_options_t options;
    if (rl_example_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n");
        return EXIT_FAILURE;
    }

    h2o_app_t app = {0};
    int client_status = rl_example_client_init(&app.client, &options);
    if (client_status != RCLIENT_OK) {
        fprintf(stderr, "client initialization failed: %s (%d)\n",
            rl_example_status_name(client_status), client_status);
        return EXIT_FAILURE;
    }
    h2o_config_init(&app.config);
    h2o_hostconf_t *host = h2o_config_register_host(
        &app.config,
        h2o_iovec_init(H2O_STRLIT("default")),
        65535
    );
    h2o_pathconf_t *path = h2o_config_register_path(host, "/limited", 0);
    h2o_rl_handler_t *handler = (h2o_rl_handler_t *)h2o_create_handler(
        path,
        sizeof(*handler)
    );
    handler->super.on_req = on_http_request;
    handler->app = &app;

    h2o_evloop_t *loop = h2o_evloop_create();
    if (!loop) {
        fprintf(stderr, "failed to create H2O event loop\n");
        h2o_config_dispose(&app.config);
        rl_example_client_destroy(&app.client);
        return EXIT_FAILURE;
    }
    h2o_context_init(&app.context, loop, &app.config);
    app.accept.ctx = &app.context;
    app.accept.hosts = app.config.hosts;
    if (create_listener(&app) != 0 || create_udp_watchers(&app) != 0) {
        fprintf(stderr, "failed to initialize H2O sockets\n");
        shutdown_context(&app, loop);
        h2o_config_dispose(&app.config);
        h2o_evloop_destroy(loop);
        rl_example_client_destroy(&app.client);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    int loop_status = 0;
    while (!stop_requested) {
        loop_status = h2o_evloop_run(loop, 1000);
        if (loop_status != 0) {
            fprintf(stderr, "H2O event loop failed: %d\n", loop_status);
            break;
        }
    }

    shutdown_context(&app, loop);
    h2o_config_dispose(&app.config);
    h2o_evloop_destroy(loop);
    rl_example_client_destroy(&app.client);
    return loop_status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
