#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <ulfius.h>

#include "common/rl_example.h"

/*
 * Ulfius executes endpoint callbacks on GNU libmicrohttpd connection threads.
 * This example gives each callback a private rl-c-client instance and drives
 * its UDP sockets with poll(2).  No client state is shared between callbacks,
 * and blocking one connection thread does not block Ulfius's listener.
 *
 * Per-request clients make ownership and cleanup especially easy to audit.
 * Applications with heavier traffic should use a long-lived dedicated client
 * thread and submit checks from callbacks, as demonstrated by onion.c and
 * civetweb.c in this directory.
 */

typedef struct check_result {
    bool done;
    int status;
    bool allowed;
} check_result_t;

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static void on_rate_limit(void *user, int status, bool allowed) {
    check_result_t *result = user;
    result->done = true;
    result->status = status;
    result->allowed = allowed;
}

static int wait_for_result(
    rl_example_client_t *client,
    rl_example_request_t *request,
    check_result_t *result
) {
    while (!result->done) {
        struct pollfd sockets[2] = {0};
        size_t socket_count = rl_example_socket_count(client);
        if (socket_count == 0 || socket_count > 2) {
            return RCLIENT_ERR_IO;
        }
        for (size_t i = 0; i < socket_count; i++) {
            sockets[i].fd = rl_example_socket_at(client, i);
            sockets[i].events = POLLIN;
        }

        /* The rl-c-client deadline remains the sole timeout policy. */
        uint64_t delay_ms = 0;
        int status = rl_example_request_delay_ms(request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        int timeout_ms = delay_ms > INT_MAX ? INT_MAX : (int)delay_ms;
        int ready = poll(sockets, (nfds_t)socket_count, timeout_ms);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            return RCLIENT_ERR_IO;
        }
        if (ready == 0) {
            status = rl_example_request_on_timeout(client, request);
            if (status != RCLIENT_OK) {
                return status;
            }
            continue;
        }
        for (size_t i = 0; i < socket_count; i++) {
            if ((sockets[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return RCLIENT_ERR_IO;
            }
            if ((sockets[i].revents & POLLIN) != 0) {
                status = rl_example_client_on_readable(client, sockets[i].fd);
                if (status != RCLIENT_OK) {
                    return status;
                }
            }
        }
    }
    return result->status;
}

static int run_check(
    const rl_example_options_t *options,
    bool *allowed
) {
    /* Client state is kept off libmicrohttpd's callback stack. */
    rl_example_client_t *client = calloc(1, sizeof(*client));
    if (!client) {
        return RCLIENT_ERR_NOMEM;
    }
    int status = rl_example_client_init(client, options);
    if (status != RCLIENT_OK) {
        free(client);
        return status;
    }

    check_result_t result = {0};
    rl_example_request_t request = {0};
    status = rl_example_check(
        client,
        &request,
        "ulfius-example",
        on_rate_limit,
        &result
    );
    if (status == RCLIENT_OK) {
        status = wait_for_result(client, &request, &result);
    }
    if (request.active) {
        rl_example_request_cancel(client, &request);
    }
    if (status == RCLIENT_OK) {
        *allowed = result.allowed;
    }
    rl_example_client_destroy(client);
    free(client);
    return status;
}

static int limited(
    const struct _u_request *request,
    struct _u_response *response,
    void *user_data
) {
    (void)request;
    bool allowed = false;
    int status = run_check(user_data, &allowed);
    if (status != RCLIENT_OK) {
        ulfius_set_string_body_response(response, 503,
            "rate-limit service unavailable\n");
    } else if (!allowed) {
        ulfius_set_string_body_response(response, 429, "denied\n");
    } else {
        ulfius_set_string_body_response(response, 200, "allowed\n");
    }
    u_map_put(response->map_header, "Content-Type", "text/plain");
    return U_CALLBACK_COMPLETE;
}

int main(void) {
    rl_example_options_t options;
    if (rl_example_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n");
        return EXIT_FAILURE;
    }

    struct _u_instance instance;
    if (ulfius_init_instance(&instance, 8000, NULL, NULL) != U_OK) {
        fprintf(stderr, "failed to initialize Ulfius\n");
        return EXIT_FAILURE;
    }
    if (ulfius_add_endpoint_by_val(&instance,
            "GET", NULL, "/limited", 0, limited, &options) != U_OK
        || ulfius_start_framework(&instance) != U_OK) {
        fprintf(stderr, "failed to start Ulfius on port 8000\n");
        ulfius_clean_instance(&instance);
        return EXIT_FAILURE;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (!stop_requested) {
        sleep(1);
    }
    ulfius_stop_framework(&instance);
    ulfius_clean_instance(&instance);
    return EXIT_SUCCESS;
}
