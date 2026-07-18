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

/* Kore workers install a seccomp filter. The base filter already permits
 * poll, sendto, recvfrom, fcntl, and file lookup. glibc's res_query() uses a
 * connected UDP socket. Its getaddrinfo() path also probes nscd over AF_UNIX,
 * batches A/AAAA queries with sendmmsg(), and optionally inspects local
 * addresses over AF_NETLINK. Report both probes as unavailable instead of
 * granting local-socket or netlink access. Kore's EACCES policy for ioctl also
 * stays in force: glibc grows its DNS receive buffer when FIONREAD is denied. */
KORE_SECCOMP_FILTER("ratelimitly",
    KORE_SYSCALL_DENY_ARG(socket, 0, AF_UNIX, ENOENT),
    KORE_SYSCALL_DENY_ARG(socket, 0, AF_NETLINK, EAFNOSUPPORT),
    KORE_SYSCALL_ALLOW_ARG(socket, 0, AF_INET),
    KORE_SYSCALL_ALLOW_ARG(socket, 0, AF_INET6),
    KORE_SYSCALL_ALLOW(connect),
    KORE_SYSCALL_ALLOW(bind),
    KORE_SYSCALL_ALLOW(getsockname),
    KORE_SYSCALL_ALLOW(sendmmsg)
);
#endif

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. GET /limited creates a kore_task and returns KORE_RESULT_RETRY.
 * 2. The task creates a private runtime and polls combined admission to completion.
 * 3. Admitted work is measured/reported, then sent through the task channel.
 * 4. Kore wakes the sleeping HTTP request; the handler reads and maps the result.
 *
 * Ownership: the task owns its runtime, sockets, and admission. Kore owns
 * hdlr_extra and frees it with the request. This model favors clarity; a
 * high-volume service can instead use one long-lived task and channel-fed queue.
 */

typedef struct handler_state {
    struct kore_task task;
} handler_state_t;

typedef struct task_result {
    int status;
    r_admission_outcome_t outcome;
    uint32_t observed_latency_ms;
    bool protected_work_complete;
} task_result_t;

typedef struct check_result {
    bool done;
    int status;
    r_admission_outcome_t outcome;
} check_result_t;

int limited(struct http_request *request);
int run_rate_limit_task(struct kore_task *task);

static void on_admission(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    check_result_t *result = user;
    result->done = true;
    result->status = status;
    result->outcome = *outcome;
}

static int wait_for_result(
    r_runtime_client_t *runtime,
    r_admission_request_t *request,
    check_result_t *result
) {
    while (!result->done) {
        struct pollfd sockets[2] = {0};
        size_t socket_count = r_runtime_socket_count(runtime);
        if (socket_count == 0 || socket_count > 2) {
            return RCLIENT_ERR_IO;
        }
        for (size_t i = 0; i < socket_count; i++) {
            sockets[i].fd = r_runtime_socket_at(runtime, i);
            sockets[i].events = POLLIN;
        }

        uint64_t delay_ms = 0;
        int status = r_runtime_admission_delay_ms(request, &delay_ms);
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
            status = r_runtime_admission_on_timeout(runtime, request);
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
                status = r_runtime_client_on_readable(runtime, sockets[i].fd);
                if (status != RCLIENT_OK) {
                    return status;
                }
            }
        }
    }
    return result->status;
}

static int perform_protected_work(void *user) {
    task_result_t *output = user;
    /* Replace this with the database/API operation the route protects. */
    output->protected_work_complete = true;
    return RCLIENT_OK;
}

int run_rate_limit_task(struct kore_task *task) {
    task_result_t output = {.status = RCLIENT_ERR_CONFIG};
    r_runtime_options_t options;
    if (r_runtime_options_from_env(&options) != RCLIENT_OK) {
        kore_task_channel_write(task, &output, sizeof(output));
        return KORE_RESULT_OK;
    }

    r_runtime_client_t runtime;
    output.status = r_runtime_client_init(&runtime, &options);
    if (output.status == RCLIENT_OK) {
        check_result_t result = {0};
        r_admission_request_t request = {0};
        r_admission_config_t config;
        r_client_admission_config_defaults(&config);
        config.bucket_name = "kore-example";
        config.service_name = "kore-protected-service";
        config.metrics_label = "kore-example";
        output.status = r_client_admission_start(
            runtime.handle,
            &request,
            &config,
            on_admission,
            &result
        );
        if (output.status == RCLIENT_OK) {
            output.status = wait_for_result(&runtime, &request, &result);
        }
        if (request.active) {
            r_runtime_admission_cancel(&runtime, &request);
        }
        if (output.status == RCLIENT_OK) {
            output.outcome = result.outcome;
            if (result.outcome.allowed) {
                int report_status = r_runtime_admission_run_and_report(
                    &runtime,
                    &request,
                    perform_protected_work,
                    &output,
                    &output.observed_latency_ms
                );
                if (report_status != RCLIENT_OK) {
                    kore_log(LOG_ERR, "latency report failed: %s (%d)",
                        r_runtime_status_name(report_status), report_status);
                }
            }
        }
        r_runtime_client_destroy(&runtime);
    }
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
    } else if (result.outcome.latency_limited
        && !result.outcome.rate_limited) {
        http_response(request, 503, unavailable, sizeof(unavailable) - 1);
    } else if (!result.outcome.allowed) {
        http_response(request, 429, denied, sizeof(denied) - 1);
    } else {
        http_response(request, 200, allowed, sizeof(allowed) - 1);
    }
    return KORE_RESULT_OK;
}
