#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "common/rl_example.h"

/*
 * Direct epoll integration
 * ------------------------
 * The epoll set watches the common adapter's original UDP descriptors. Each
 * iteration derives epoll_wait()'s timeout from rl-c-client's current deadline,
 * drains every ready socket, then advances timeout state if the deadline passed.
 * No framework-specific wrapper owns or closes the descriptors.
 */
typedef struct epoll_app {
    rl_example_client_t client;
    rl_example_request_t request;
    int status;
    bool allowed;
    bool done;
} epoll_app_t;

static void on_rate_limit(void *user, int status, bool allowed) {
    epoll_app_t *app = user;
    app->status = status;
    app->allowed = allowed;
    app->done = true;
}

static int run_loop(epoll_app_t *app, int epoll_fd) {
    while (!app->done) {
        uint64_t delay_ms = 0;
        int status = rl_example_request_delay_ms(&app->request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        int timeout = delay_ms > INT_MAX ? INT_MAX : (int)delay_ms;
        struct epoll_event events[2];
        /* Readiness and the deadline share one blocking kernel call. */
        int count = epoll_wait(epoll_fd, events, 2, timeout);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return RCLIENT_ERR_IO;
        }
        if (count == 0) {
            status = rl_example_request_on_timeout(&app->client, &app->request);
            if (status != RCLIENT_OK) {
                return status;
            }
            continue;
        }
        for (int i = 0; i < count; i++) {
            if ((events[i].events & EPOLLIN) == 0) {
                return RCLIENT_ERR_IO;
            }
            /* data.fd is safe because every registration is a client fd. */
            status = rl_example_client_on_readable(&app->client, events[i].data.fd);
            if (status != RCLIENT_OK) {
                return status;
            }
        }
    }
    return RCLIENT_OK;
}

int main(void) {
    rl_example_options_t options;
    if (rl_example_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n");
        return EXIT_FAILURE;
    }

    epoll_app_t app = {0};
    app.status = RCLIENT_ERR_IO;
    int status = rl_example_client_init(&app.client, &options);
    if (status != RCLIENT_OK) {
        return EXIT_FAILURE;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        rl_example_client_destroy(&app.client);
        return EXIT_FAILURE;
    }
    size_t socket_count = rl_example_socket_count(&app.client);
    for (size_t i = 0; i < socket_count; i++) {
        int socket_fd = rl_example_socket_at(&app.client, i);
        struct epoll_event event = {
            .events = EPOLLIN,
            .data.fd = socket_fd,
        };
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &event) != 0) {
            status = RCLIENT_ERR_IO;
            break;
        }
    }

    if (status == RCLIENT_OK) {
        status = rl_example_check(
            &app.client,
            &app.request,
            "epoll-example",
            on_rate_limit,
            &app
        );
    }
    if (status == RCLIENT_OK) {
        status = run_loop(&app, epoll_fd);
    }
    if (status != RCLIENT_OK && !app.done) {
        app.status = status;
    }

    if (app.request.active) {
        rl_example_request_cancel(&app.client, &app.request);
    }
    close(epoll_fd);
    rl_example_client_destroy(&app.client);

    if (app.status != RCLIENT_OK) {
        fprintf(stderr, "rate-limit check failed: %d\n", app.status);
        return EXIT_FAILURE;
    }
    puts(app.allowed ? "allowed" : "denied");
    return app.allowed ? EXIT_SUCCESS : 2;
}
