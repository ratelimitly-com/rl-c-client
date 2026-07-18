#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <civetweb.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. A CivetWeb worker queues one stack-owned job and wakes the bridge thread.
 * 2. The bridge thread starts the combined rate-limit and latency-guard check.
 * 3. If admitted, that same thread runs and measures the protected operation.
 * 4. The result callback signals the waiting worker, which sends the response.
 *
 * Ownership: only the bridge thread touches rl-c-client, its UDP sockets,
 * active admission requests, and deadlines. The waiting worker owns its
 * bridge_job_t. CivetWeb must stop and join its workers before bridge teardown.
 */
typedef struct civetweb_bridge civetweb_bridge_t;

typedef struct bridge_job {
    struct bridge_job *next;
    civetweb_bridge_t *bridge;
    r_admission_request_t request;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    r_admission_outcome_t outcome;
    char response_body[96];
    uint32_t observed_latency_ms;
    int status;
    bool done;
} bridge_job_t;

struct civetweb_bridge {
    r_runtime_client_t runtime;
    pthread_t thread;
    pthread_mutex_t queue_mutex;
    bridge_job_t *queue_head;
    bridge_job_t *queue_tail;
    bridge_job_t *active;
    int wake_pipe[2];
    bool stop;
};

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static void wake_bridge(civetweb_bridge_t *bridge) {
    char byte = 0;
    ssize_t written;
    /* EAGAIN only means an earlier byte already guarantees a wakeup. */
    do {
        written = write(bridge->wake_pipe[1], &byte, 1);
    } while (written < 0 && errno == EINTR);
}

static void complete_job(
    bridge_job_t *job,
    int status,
    const r_admission_outcome_t *outcome
) {
    pthread_mutex_lock(&job->mutex);
    job->status = status;
    if (outcome) {
        job->outcome = *outcome;
    } else {
        memset(&job->outcome, 0, sizeof(job->outcome));
        job->outcome.decision = R_ADMISSION_ERROR;
    }
    job->done = true;
    pthread_cond_signal(&job->condition);
    pthread_mutex_unlock(&job->mutex);
}

static void remove_active(civetweb_bridge_t *bridge, bridge_job_t *job) {
    bridge_job_t **cursor = &bridge->active;
    while (*cursor) {
        if (*cursor == job) {
            *cursor = job->next;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

static int prepare_protected_response(void *user) {
    bridge_job_t *job = user;
    /* Replace this with the database/API work the endpoint should protect. */
    int length = snprintf(job->response_body, sizeof(job->response_body),
        "allowed (protected work: %s)\n", "complete");
    return length > 0 && (size_t)length < sizeof(job->response_body)
        ? RCLIENT_OK
        : RCLIENT_ERR_IO;
}

static void on_admission(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    bridge_job_t *job = user;
    civetweb_bridge_t *bridge = job->bridge;
    remove_active(bridge, job);

    if (status == RCLIENT_OK && outcome->allowed) {
        int report_status = r_runtime_admission_run_and_report(
            &bridge->runtime,
            &job->request,
            prepare_protected_response,
            job,
            &job->observed_latency_ms
        );
        if (report_status != RCLIENT_OK) {
            /* The protected work completed; telemetry failure must not undo it. */
            fprintf(stderr, "latency report failed: %s (%d)\n",
                r_runtime_status_name(report_status), report_status);
        }
    }
    complete_job(job, status, outcome);
}

static bridge_job_t *take_queue(civetweb_bridge_t *bridge) {
    /* Detach the whole FIFO quickly so workers spend minimal time on the lock. */
    pthread_mutex_lock(&bridge->queue_mutex);
    bridge_job_t *jobs = bridge->queue_head;
    bridge->queue_head = NULL;
    bridge->queue_tail = NULL;
    pthread_mutex_unlock(&bridge->queue_mutex);
    return jobs;
}

static void start_queued_jobs(civetweb_bridge_t *bridge) {
    bridge_job_t *job = take_queue(bridge);
    while (job) {
        bridge_job_t *next = job->next;
        job->next = bridge->active;
        bridge->active = job;

        r_admission_config_t config;
        r_client_admission_config_defaults(&config);
        config.bucket_name = "civetweb-example";
        config.service_name = "civetweb-protected-service";
        config.metrics_label = "civetweb-example";
        int status = r_client_admission_start(
            bridge->runtime.handle,
            &job->request,
            &config,
            on_admission,
            job
        );
        if (status != RCLIENT_OK) {
            remove_active(bridge, job);
            complete_job(job, status, NULL);
        }
        job = next;
    }
}

static void drain_wake_pipe(civetweb_bridge_t *bridge) {
    char buffer[64];
    while (read(bridge->wake_pipe[0], buffer, sizeof(buffer)) > 0) {
    }
}

static int next_timeout_ms(civetweb_bridge_t *bridge) {
    /* poll() must wake for the earliest active admission deadline. */
    int timeout_ms = 1000;
    for (bridge_job_t *job = bridge->active; job; job = job->next) {
        uint64_t delay_ms = 0;
        int status = r_runtime_admission_delay_ms(&job->request, &delay_ms);
        if (status != RCLIENT_OK) {
            return 0;
        }
        int candidate = delay_ms > INT_MAX ? INT_MAX : (int)delay_ms;
        if (candidate < timeout_ms) {
            timeout_ms = candidate;
        }
    }
    return timeout_ms;
}

static int expire_requests(civetweb_bridge_t *bridge) {
    bridge_job_t *job = bridge->active;
    while (job) {
        /* Timeout completion may unlink job, so retain next first. */
        bridge_job_t *next = job->next;
        uint64_t delay_ms = 0;
        int status = r_runtime_admission_delay_ms(&job->request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        if (delay_ms == 0) {
            status = r_runtime_admission_on_timeout(
                &bridge->runtime,
                &job->request
            );
            if (status != RCLIENT_OK) {
                return status;
            }
        }
        job = next;
    }
    return RCLIENT_OK;
}

static bool bridge_should_stop(civetweb_bridge_t *bridge) {
    pthread_mutex_lock(&bridge->queue_mutex);
    bool stop = bridge->stop;
    pthread_mutex_unlock(&bridge->queue_mutex);
    return stop;
}

static void mark_bridge_stopped(civetweb_bridge_t *bridge) {
    pthread_mutex_lock(&bridge->queue_mutex);
    bridge->stop = true;
    pthread_mutex_unlock(&bridge->queue_mutex);
}

static void fail_all_jobs(civetweb_bridge_t *bridge, int status) {
    /* Shutdown/error paths wake every waiting CivetWeb worker exactly once. */
    bridge_job_t *job = take_queue(bridge);
    while (job) {
        bridge_job_t *next = job->next;
        complete_job(job, status, NULL);
        job = next;
    }
    while (bridge->active) {
        job = bridge->active;
        bridge->active = job->next;
        r_runtime_admission_cancel(&bridge->runtime, &job->request);
        complete_job(job, status, NULL);
    }
}

static int poll_client(civetweb_bridge_t *bridge) {
    struct pollfd poll_fds[3] = {
        {.fd = bridge->wake_pipe[0], .events = POLLIN},
    };
    size_t socket_count = r_runtime_socket_count(&bridge->runtime);
    for (size_t i = 0; i < socket_count; i++) {
        poll_fds[i + 1].fd = r_runtime_socket_at(&bridge->runtime, i);
        poll_fds[i + 1].events = POLLIN;
    }

    int ready = poll(
        poll_fds,
        (nfds_t)(socket_count + 1),
        next_timeout_ms(bridge)
    );
    if (ready < 0) {
        return errno == EINTR ? RCLIENT_OK : RCLIENT_ERR_IO;
    }
    if ((poll_fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return RCLIENT_ERR_IO;
    }
    if ((poll_fds[0].revents & POLLIN) != 0) {
        drain_wake_pipe(bridge);
    }
    /* Also inspect the queue after timeout, making wake-pipe saturation safe. */
    start_queued_jobs(bridge);

    for (size_t i = 0; i < socket_count; i++) {
        if ((poll_fds[i + 1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return RCLIENT_ERR_IO;
        }
        if ((poll_fds[i + 1].revents & POLLIN) != 0) {
            int status = r_runtime_client_on_readable(
                &bridge->runtime,
                poll_fds[i + 1].fd
            );
            if (status != RCLIENT_OK) {
                return status;
            }
        }
    }
    return expire_requests(bridge);
}

static void *bridge_loop(void *user) {
    civetweb_bridge_t *bridge = user;
    int loop_status = RCLIENT_OK;
    while (!bridge_should_stop(bridge)) {
        loop_status = poll_client(bridge);
        if (loop_status != RCLIENT_OK) {
            break;
        }
    }
    /* Future handlers must fail fast instead of queueing to a dead thread. */
    mark_bridge_stopped(bridge);
    fail_all_jobs(bridge, loop_status == RCLIENT_OK ? RCLIENT_ERR_IO : loop_status);
    return NULL;
}

static int set_nonblocking(int file_descriptor) {
    int flags = fcntl(file_descriptor, F_GETFL, 0);
    return flags >= 0 && fcntl(file_descriptor, F_SETFL, flags | O_NONBLOCK) == 0
        ? 0
        : -1;
}

static int bridge_start(
    civetweb_bridge_t *bridge,
    const r_runtime_options_t *options
) {
    memset(bridge, 0, sizeof(*bridge));
    bridge->wake_pipe[0] = -1;
    bridge->wake_pipe[1] = -1;
    int status = r_runtime_client_init(&bridge->runtime, options);
    if (status != RCLIENT_OK) {
        return status;
    }
    if (pthread_mutex_init(&bridge->queue_mutex, NULL) != 0) {
        r_runtime_client_destroy(&bridge->runtime);
        return RCLIENT_ERR_IO;
    }
    if (pipe(bridge->wake_pipe) != 0
        || set_nonblocking(bridge->wake_pipe[0]) != 0
        || set_nonblocking(bridge->wake_pipe[1]) != 0
        || pthread_create(&bridge->thread, NULL, bridge_loop, bridge) != 0) {
        if (bridge->wake_pipe[0] >= 0) {
            close(bridge->wake_pipe[0]);
            close(bridge->wake_pipe[1]);
        }
        pthread_mutex_destroy(&bridge->queue_mutex);
        r_runtime_client_destroy(&bridge->runtime);
        return RCLIENT_ERR_IO;
    }
    return RCLIENT_OK;
}

static void bridge_stop(civetweb_bridge_t *bridge) {
    mark_bridge_stopped(bridge);
    wake_bridge(bridge);
    pthread_join(bridge->thread, NULL);
    close(bridge->wake_pipe[0]);
    close(bridge->wake_pipe[1]);
    pthread_mutex_destroy(&bridge->queue_mutex);
    r_runtime_client_destroy(&bridge->runtime);
}

static int enqueue_and_wait(
    civetweb_bridge_t *bridge,
    int *out_status,
    r_admission_outcome_t *out_outcome,
    char *out_body,
    size_t out_body_size
) {
    bridge_job_t job = {.bridge = bridge};
    if (pthread_mutex_init(&job.mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&job.condition, NULL) != 0) {
        pthread_mutex_destroy(&job.mutex);
        return -1;
    }

    pthread_mutex_lock(&job.mutex);
    pthread_mutex_lock(&bridge->queue_mutex);
    if (bridge->stop) {
        pthread_mutex_unlock(&bridge->queue_mutex);
        job.done = true;
        job.status = RCLIENT_ERR_IO;
        job.outcome.decision = R_ADMISSION_ERROR;
    } else {
        if (bridge->queue_tail) {
            bridge->queue_tail->next = &job;
        } else {
            bridge->queue_head = &job;
        }
        bridge->queue_tail = &job;
        pthread_mutex_unlock(&bridge->queue_mutex);
        wake_bridge(bridge);
    }
    while (!job.done) {
        pthread_cond_wait(&job.condition, &job.mutex);
    }
    pthread_mutex_unlock(&job.mutex);

    *out_status = job.status;
    *out_outcome = job.outcome;
    if (out_body_size > 0) {
        snprintf(out_body, out_body_size, "%s", job.response_body);
    }
    pthread_cond_destroy(&job.condition);
    pthread_mutex_destroy(&job.mutex);
    return 0;
}

static int send_denial(
    struct mg_connection *connection,
    const r_admission_outcome_t *outcome
) {
    if (outcome->rate_limited && outcome->latency_limited) {
        mg_send_http_error(connection, 429, "%s",
            "denied by resource limit and latency guard\n");
        return 429;
    }
    if (outcome->latency_limited) {
        mg_send_http_error(connection, 503, "%s",
            "denied by latency guard\n");
        return 503;
    }
    mg_send_http_error(connection, 429, "%s",
        "denied by resource rate limit\n");
    return 429;
}

static int limited_handler(struct mg_connection *connection, void *user) {
    civetweb_bridge_t *bridge = user;
    int status = RCLIENT_ERR_IO;
    r_admission_outcome_t outcome = {.decision = R_ADMISSION_ERROR};
    char body[96] = {0};
    if (enqueue_and_wait(
            bridge,
            &status,
            &outcome,
            body,
            sizeof(body)
        ) != 0
        || status != RCLIENT_OK) {
        mg_send_http_error(connection, 503, "%s",
            "rate-limit service unavailable\n");
        return 503;
    }
    if (!outcome.allowed) {
        return send_denial(connection, &outcome);
    }

    size_t body_length = strlen(body);
    mg_send_http_ok(connection, "text/plain", body_length);
    mg_write(connection, body, body_length);
    return 200;
}

int main(void) {
    r_runtime_options_t options;
    if (r_runtime_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_AUTH_KEY; RATELIMITLY_TENANT is optional\n");
        return EXIT_FAILURE;
    }

    civetweb_bridge_t bridge;
    int status = bridge_start(&bridge, &options);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "bridge initialization failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }

    mg_init_library(0);
    const char *server_options[] = {
        "listening_ports", "8000",
        "num_threads", "4",
        NULL,
    };
    struct mg_context *server = mg_start(NULL, NULL, server_options);
    if (!server) {
        mg_exit_library();
        bridge_stop(&bridge);
        return EXIT_FAILURE;
    }
    mg_set_request_handler(server, "/limited", limited_handler, &bridge);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (!stop_requested) {
        sleep(1);
    }

    /* mg_stop joins workers; they may still need the bridge while draining. */
    mg_stop(server);
    bridge_stop(&bridge);
    mg_exit_library();
    return EXIT_SUCCESS;
}
