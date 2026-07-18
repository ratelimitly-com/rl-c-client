#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lwan.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. A Lwan coroutine queues a reference-counted job, then yields.
 * 2. A dedicated thread starts admission and polls client UDP sockets.
 * 3. Allowed work is measured and reported before completion is published.
 * 4. The coroutine observes completion, resumes, and sends the HTTP response.
 *
 * Ownership: only the dedicated thread touches rl-c-client. Two references
 * protect each job—one owned by coroutine cleanup, one by the client thread.
 * A disconnected peer therefore cannot leave the thread a dangling pointer.
 */

typedef struct lwan_bridge lwan_bridge_t;

typedef struct bridge_job {
    struct bridge_job *next;
    lwan_bridge_t *bridge;
    r_admission_request_t request;
    r_admission_outcome_t outcome;
    atomic_uint references;
    atomic_bool done;
    uint32_t observed_latency_ms;
    bool protected_work_complete;
    int status;
} bridge_job_t;

struct lwan_bridge {
    r_runtime_client_t runtime;
    pthread_t thread;
    pthread_mutex_t queue_mutex;
    bridge_job_t *queue_head;
    bridge_job_t *queue_tail;
    bridge_job_t *active;
    int wake_pipe[2];
    bool stop;
};

static void wake_bridge(lwan_bridge_t *bridge) {
    char byte = 0;
    ssize_t written;
    /* EAGAIN means the nonblocking pipe already contains a wakeup. */
    do {
        written = write(bridge->wake_pipe[1], &byte, 1);
    } while (written < 0 && errno == EINTR);
}

static void release_job(void *data) {
    bridge_job_t *job = data;
    if (atomic_fetch_sub_explicit(
            &job->references, 1, memory_order_acq_rel) == 1) {
        free(job);
    }
}

/* Release publication makes result fields visible before the coroutine sees
 * done == true. This function also releases the bridge thread's reference. */
static void complete_job(
    bridge_job_t *job,
    int status,
    const r_admission_outcome_t *outcome
) {
    job->status = status;
    if (outcome) {
        job->outcome = *outcome;
    } else {
        memset(&job->outcome, 0, sizeof(job->outcome));
        job->outcome.decision = R_ADMISSION_ERROR;
    }
    atomic_store_explicit(&job->done, true, memory_order_release);
    release_job(job); /* Release the client thread's reference. */
}

static void remove_active(lwan_bridge_t *bridge, bridge_job_t *job) {
    bridge_job_t **cursor = &bridge->active;
    while (*cursor) {
        if (*cursor == job) {
            *cursor = job->next;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

static int perform_protected_work(void *user) {
    bridge_job_t *job = user;
    /* Replace this with the database/API operation the route protects. */
    job->protected_work_complete = true;
    return RCLIENT_OK;
}

static void on_admission(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    bridge_job_t *job = user;
    remove_active(job->bridge, job);
    if (status == RCLIENT_OK && outcome->allowed) {
        int report_status = r_runtime_admission_run_and_report(
            &job->bridge->runtime,
            &job->request,
            perform_protected_work,
            job,
            &job->observed_latency_ms
        );
        if (report_status != RCLIENT_OK) {
            fprintf(stderr, "latency report failed: %s (%d)\n",
                r_runtime_status_name(report_status), report_status);
        }
    }
    complete_job(job, status, outcome);
}

static bridge_job_t *take_queue(lwan_bridge_t *bridge) {
    pthread_mutex_lock(&bridge->queue_mutex);
    bridge_job_t *jobs = bridge->queue_head;
    bridge->queue_head = NULL;
    bridge->queue_tail = NULL;
    pthread_mutex_unlock(&bridge->queue_mutex);
    return jobs;
}

static void start_queued_jobs(lwan_bridge_t *bridge) {
    bridge_job_t *job = take_queue(bridge);
    while (job) {
        bridge_job_t *next = job->next;
        job->next = bridge->active;
        bridge->active = job;
        r_admission_config_t config;
        r_client_admission_config_defaults(&config);
        config.bucket_name = "lwan-example";
        config.service_name = "lwan-protected-service";
        config.metrics_label = "lwan-example";
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

static void drain_wake_pipe(lwan_bridge_t *bridge) {
    char buffer[64];
    while (read(bridge->wake_pipe[0], buffer, sizeof(buffer)) > 0) {
    }
}

static int next_timeout(lwan_bridge_t *bridge) {
    int timeout_ms = 1000;
    for (bridge_job_t *job = bridge->active; job; job = job->next) {
        uint64_t delay_ms = 0;
        if (r_runtime_admission_delay_ms(&job->request, &delay_ms)
            != RCLIENT_OK) {
            return 0;
        }
        int candidate = delay_ms > INT_MAX ? INT_MAX : (int)delay_ms;
        if (candidate < timeout_ms) {
            timeout_ms = candidate;
        }
    }
    return timeout_ms;
}

static int expire_requests(lwan_bridge_t *bridge) {
    bridge_job_t *job = bridge->active;
    while (job) {
        /* A timeout can complete and unlink this job, so save next first. */
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

static bool bridge_should_stop(lwan_bridge_t *bridge) {
    pthread_mutex_lock(&bridge->queue_mutex);
    bool stop = bridge->stop;
    pthread_mutex_unlock(&bridge->queue_mutex);
    return stop;
}

static void mark_bridge_stopped(lwan_bridge_t *bridge) {
    pthread_mutex_lock(&bridge->queue_mutex);
    bridge->stop = true;
    pthread_mutex_unlock(&bridge->queue_mutex);
}

static void fail_all_jobs(lwan_bridge_t *bridge, int status) {
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

static void *bridge_loop(void *user) {
    lwan_bridge_t *bridge = user;
    int loop_status = RCLIENT_OK;
    while (!bridge_should_stop(bridge)) {
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
            next_timeout(bridge)
        );
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            loop_status = RCLIENT_ERR_IO;
            break;
        }
        if ((poll_fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            loop_status = RCLIENT_ERR_IO;
            break;
        }
        if ((poll_fds[0].revents & POLLIN) != 0) {
            drain_wake_pipe(bridge);
            start_queued_jobs(bridge);
        }
        for (size_t i = 0; i < socket_count; i++) {
            if ((poll_fds[i + 1].revents
                    & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                loop_status = RCLIENT_ERR_IO;
                break;
            }
            if ((poll_fds[i + 1].revents & POLLIN) != 0) {
                loop_status = r_runtime_client_on_readable(
                    &bridge->runtime,
                    poll_fds[i + 1].fd
                );
                if (loop_status != RCLIENT_OK) {
                    break;
                }
            }
        }
        if (loop_status == RCLIENT_OK) {
            loop_status = expire_requests(bridge);
        }
        if (loop_status != RCLIENT_OK) {
            break;
        }
    }
    /* Future handlers must fail fast instead of queueing to a dead thread. */
    mark_bridge_stopped(bridge);
    fail_all_jobs(bridge,
        loop_status == RCLIENT_OK ? RCLIENT_ERR_IO : loop_status);
    return NULL;
}

static int set_nonblocking(int file_descriptor) {
    int flags = fcntl(file_descriptor, F_GETFL, 0);
    return flags >= 0
        && fcntl(file_descriptor, F_SETFL, flags | O_NONBLOCK) == 0
        ? 0
        : -1;
}

static int bridge_start(
    lwan_bridge_t *bridge,
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

static void bridge_stop(lwan_bridge_t *bridge) {
    mark_bridge_stopped(bridge);
    wake_bridge(bridge);
    pthread_join(bridge->thread, NULL);
    close(bridge->wake_pipe[0]);
    close(bridge->wake_pipe[1]);
    pthread_mutex_destroy(&bridge->queue_mutex);
    r_runtime_client_destroy(&bridge->runtime);
}

static bridge_job_t *submit_job(lwan_bridge_t *bridge) {
    bridge_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        return NULL;
    }
    job->bridge = bridge;
    atomic_init(&job->references, 2);
    atomic_init(&job->done, false);

    pthread_mutex_lock(&bridge->queue_mutex);
    if (bridge->stop) {
        pthread_mutex_unlock(&bridge->queue_mutex);
        complete_job(job, RCLIENT_ERR_IO, NULL);
        return job;
    }
    if (bridge->queue_tail) {
        bridge->queue_tail->next = job;
    } else {
        bridge->queue_head = job;
    }
    bridge->queue_tail = job;
    pthread_mutex_unlock(&bridge->queue_mutex);

    wake_bridge(bridge);
    return job;
}

static enum lwan_http_status limited(
    struct lwan_request *request,
    struct lwan_response *response,
    void *data
) {
    static const char allowed_body[] = "allowed\n";
    static const char denied_body[] = "denied\n";
    static const char unavailable_body[] = "rate-limit service unavailable\n";

    bridge_job_t *job = submit_job(data);
    if (!job) {
        lwan_strbuf_set_static(response->buffer,
            unavailable_body, sizeof(unavailable_body) - 1);
        return HTTP_UNAVAILABLE;
    }

    /* Lwan invokes deferred cleanup both after a normal return and when a
     * disconnected request destroys this coroutine. */
    coro_defer(request->conn->coro, release_job, job);
    while (!atomic_load_explicit(&job->done, memory_order_acquire)) {
        lwan_request_sleep(request, 1);
    }

    response->mime_type = "text/plain";
    if (job->status != RCLIENT_OK) {
        lwan_strbuf_set_static(response->buffer,
            unavailable_body, sizeof(unavailable_body) - 1);
        return HTTP_UNAVAILABLE;
    }
    if (job->outcome.latency_limited && !job->outcome.rate_limited) {
        lwan_strbuf_set_static(response->buffer,
            unavailable_body, sizeof(unavailable_body) - 1);
        return HTTP_UNAVAILABLE;
    }
    if (!job->outcome.allowed) {
        lwan_strbuf_set_static(response->buffer,
            denied_body, sizeof(denied_body) - 1);
        /* Lwan's public status table omits 429; an unknown status asserts. */
        return HTTP_FORBIDDEN;
    }
    lwan_strbuf_set_static(response->buffer,
        allowed_body, sizeof(allowed_body) - 1);
    return HTTP_OK;
}

int main(void) {
    r_runtime_options_t options;
    if (r_runtime_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_AUTH_KEY; RATELIMITLY_TENANT is optional\n");
        return EXIT_FAILURE;
    }

    lwan_bridge_t bridge;
    int status = bridge_start(&bridge, &options);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "bridge initialization failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }
    const struct lwan_url_map routes[] = {
        {.prefix = "/limited", .handler = limited, .data = &bridge},
        {.prefix = NULL},
    };
    struct lwan server;
    lwan_init(&server);
    lwan_set_url_map(&server, routes);
    lwan_main_loop(&server);
    lwan_shutdown(&server);
    bridge_stop(&bridge);
    return EXIT_SUCCESS;
}
