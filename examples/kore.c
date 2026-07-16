#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>

#include <kore/kore.h>
#include <kore/http.h>
#include <kore/tasks.h>

#if defined(__linux__)
#include <kore/seccomp.h>

/* Kore workers install a seccomp filter.  The base filter already permits
 * poll, sendto, recvfrom, fcntl, and file lookup; the adapter additionally
 * creates and binds its IPv4/IPv6 UDP sockets. */
KORE_SECCOMP_FILTER("ratelimitly",
    KORE_SYSCALL_ALLOW_ARG(socket, 0, AF_INET),
    KORE_SYSCALL_ALLOW_ARG(socket, 0, AF_INET6),
    KORE_SYSCALL_ALLOW(bind),
    KORE_SYSCALL_ALLOW(getsockname)
);
#endif

#include "common/rl_example.h"

/*
 * Flow
 * ----
 * 1. GET /limited creates a kore_task and returns KORE_RESULT_RETRY.
 * 2. The task creates a private client and polls its UDP sockets to completion.
 * 3. The task writes a fixed-size result to its channel.
 * 4. Kore wakes the sleeping HTTP request; the handler reads and maps the result.
 *
 * Ownership: the task owns its client, sockets, and check. Kore owns hdlr_extra
 * and frees it with the request. This per-request model favors clarity; a
 * high-volume service can instead use one long-lived task and channel-fed queue.
 */

typedef struct handler_state {
    struct kore_task task;
} handler_state_t;

typedef struct task_result {
    int status;
    bool allowed;
} task_result_t;

typedef struct check_result {
    bool done;
    int status;
    bool allowed;
} check_result_t;

int limited(struct http_request *request);
int run_rate_limit_task(struct kore_task *task);

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

int run_rate_limit_task(struct kore_task *task) {
    task_result_t output = {.status = RCLIENT_ERR_CONFIG};
    rl_example_options_t options;
    if (rl_example_options_from_env(&options) != RCLIENT_OK) {
        kore_task_channel_write(task, &output, sizeof(output));
        return KORE_RESULT_OK;
    }

    rl_example_client_t *client = calloc(1, sizeof(*client));
    if (!client) {
        output.status = RCLIENT_ERR_NOMEM;
        kore_task_channel_write(task, &output, sizeof(output));
        return KORE_RESULT_OK;
    }
    output.status = rl_example_client_init(client, &options);
    if (output.status == RCLIENT_OK) {
        check_result_t result = {0};
        rl_example_request_t request = {0};
        output.status = rl_example_check(
            client,
            &request,
            "kore-example",
            on_rate_limit,
            &result
        );
        if (output.status == RCLIENT_OK) {
            output.status = wait_for_result(client, &request, &result);
        }
        if (request.active) {
            rl_example_request_cancel(client, &request);
        }
        if (output.status == RCLIENT_OK) {
            output.allowed = result.allowed;
        }
        rl_example_client_destroy(client);
    }
    free(client);
    kore_task_channel_write(task, &output, sizeof(output));
    return KORE_RESULT_OK;
}

int limited(struct http_request *request) {
    static const char task_failed[] = "rate-limit task failed\n";
    static const char unavailable[] = "rate-limit service unavailable\n";
    static const char denied[] = "denied\n";
    static const char allowed[] = "allowed\n";
    handler_state_t *state;
    task_result_t result;
    u_int32_t length;

    if (request->method != HTTP_METHOD_GET) {
        http_response_header(request, "allow", "GET");
        http_response(request, 405, NULL, 0);
        return KORE_RESULT_OK;
    }

    if (request->hdlr_extra == NULL) {
        state = kore_calloc(1, sizeof(*state));
        request->hdlr_extra = state;
        kore_task_create(&state->task, run_rate_limit_task);
        kore_task_bind_request(&state->task, request);
        kore_task_run(&state->task);
        return KORE_RESULT_RETRY;
    }
    state = request->hdlr_extra;
    if (kore_task_state(&state->task) != KORE_TASK_STATE_FINISHED) {
        http_request_sleep(request);
        return KORE_RESULT_RETRY;
    }

    if (kore_task_result(&state->task) != KORE_RESULT_OK) {
        kore_task_destroy(&state->task);
        http_response(request, 503, task_failed, sizeof(task_failed) - 1);
        return KORE_RESULT_OK;
    }
    length = kore_task_channel_read(&state->task, &result, sizeof(result));
    kore_task_destroy(&state->task);
    http_response_header(request, "content-type", "text/plain");
    if (length != sizeof(result) || result.status != RCLIENT_OK) {
        http_response(request, 503, unavailable, sizeof(unavailable) - 1);
    } else if (!result.allowed) {
        http_response(request, 429, denied, sizeof(denied) - 1);
    } else {
        http_response(request, 200, allowed, sizeof(allowed) - 1);
    }
    return KORE_RESULT_OK;
}
