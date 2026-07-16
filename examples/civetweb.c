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

#include "common/rl_example.h"

/*
 * Flow
 * ----
 * 1. A CivetWeb worker queues one stack-owned job and writes to a wake pipe.
 * 2. A dedicated bridge thread starts the check and polls client UDP sockets.
 * 3. The result callback signals the job's condition variable.
 * 4. The worker maps the decision to HTTP 200, 429, or 503.
 *
 * Ownership: only the bridge thread touches rl-c-client, its sockets, active
 * requests, and deadlines. The waiting worker owns its bridge_job_t. CivetWeb
 * must stop and join all workers before the bridge is destroyed.
 */
typedef struct civetweb_bridge civetweb_bridge_t;

typedef struct bridge_job {
    struct bridge_job *next;
    civetweb_bridge_t *bridge;
    rl_example_request_t request;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int status;
    bool allowed;
    bool done;
} bridge_job_t;

struct civetweb_bridge {
    rl_example_client_t client;
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
    /* One byte is enough; EAGAIN only means a wakeup is already pending. */
    do {
        written = write(bridge->wake_pipe[1], &byte, 1);
    } while (written < 0 && errno == EINTR);
}

static void complete_job(bridge_job_t *job, int status, bool allowed) {
    pthread_mutex_lock(&job->mutex);
    job->status = status;
    job->allowed = allowed;
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

static void on_rate_limit(void *user, int status, bool allowed) {
    bridge_job_t *job = user;
    remove_active(job->bridge, job);
    complete_job(job, status, allowed);
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

static int start_queued_jobs(civetweb_bridge_t *bridge) {
    bridge_job_t *job = take_queue(bridge);
    while (job) {
        bridge_job_t *next = job->next;
        job->next = bridge->active;
        bridge->active = job;
        int status = rl_example_check(
            &bridge->client,
            &job->request,
            "civetweb-example",
            on_rate_limit,
            job
        );
        if (status != RCLIENT_OK) {
            remove_active(bridge, job);
            complete_job(job, status, false);
        }
        job = next;
    }
    return RCLIENT_OK;
}

static void drain_wake_pipe(civetweb_bridge_t *bridge) {
    char buffer[64];
    while (read(bridge->wake_pipe[0], buffer, sizeof(buffer)) > 0) {
    }
}

static int next_timeout(civetweb_bridge_t *bridge) {
    /* poll() must wake for the earliest active rl-c-client request. */
    int timeout = 1000;
    for (bridge_job_t *job = bridge->active; job; job = job->next) {
        uint64_t delay_ms = 0;
        int status = rl_example_request_delay_ms(&job->request, &delay_ms);
        if (status != RCLIENT_OK) {
            return 0;
        }
        int candidate = delay_ms > INT_MAX ? INT_MAX : (int)delay_ms;
        if (candidate < timeout) {
            timeout = candidate;
        }
    }
    return timeout;
}

static int expire_requests(civetweb_bridge_t *bridge) {
    bridge_job_t *job = bridge->active;
    while (job) {
        bridge_job_t *next = job->next;
        uint64_t delay_ms = 0;
        int status = rl_example_request_delay_ms(&job->request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        if (delay_ms == 0) {
            status = rl_example_request_on_timeout(&bridge->client, &job->request);
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

static void fail_all_jobs(civetweb_bridge_t *bridge, int status) {
    /* Shutdown/error paths wake every waiting CivetWeb worker exactly once. */
    bridge_job_t *job = take_queue(bridge);
    while (job) {
        bridge_job_t *next = job->next;
        complete_job(job, status, false);
        job = next;
    }
    while (bridge->active) {
        job = bridge->active;
        bridge->active = job->next;
        rl_example_request_cancel(&bridge->client, &job->request);
        complete_job(job, status, false);
    }
}

static void *bridge_loop(void *user) {
    civetweb_bridge_t *bridge = user;
    int loop_status = RCLIENT_OK;
    while (!bridge_should_stop(bridge)) {
        struct pollfd poll_fds[3] = {
            {.fd = bridge->wake_pipe[0], .events = POLLIN},
        };
        size_t socket_count = rl_example_socket_count(&bridge->client);
        for (size_t i = 0; i < socket_count; i++) {
            poll_fds[i + 1].fd = rl_example_socket_at(&bridge->client, i);
            poll_fds[i + 1].events = POLLIN;
        }
        int ready = poll(poll_fds, (nfds_t)(socket_count + 1), next_timeout(bridge));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            loop_status = RCLIENT_ERR_IO;
            break;
        }
        if ((poll_fds[0].revents & POLLIN) != 0) {
            drain_wake_pipe(bridge);
        }
        /* Also inspect the queue after timeout, making wake-pipe saturation safe. */
        start_queued_jobs(bridge);
        for (size_t i = 0; i < socket_count; i++) {
            if ((poll_fds[i + 1].revents & POLLIN) != 0) {
                loop_status = rl_example_client_on_readable(
                    &bridge->client,
                    poll_fds[i + 1].fd
                );
                if (loop_status != RCLIENT_OK) {
                    break;
                }
            }
        }
        if (loop_status != RCLIENT_OK) {
            break;
        }
        loop_status = expire_requests(bridge);
        if (loop_status != RCLIENT_OK) {
            break;
        }
    }
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
    const rl_example_options_t *options
) {
    memset(bridge, 0, sizeof(*bridge));
    bridge->wake_pipe[0] = -1;
    bridge->wake_pipe[1] = -1;
    int status = rl_example_client_init(&bridge->client, options);
    if (status != RCLIENT_OK) {
        return status;
    }
    if (pthread_mutex_init(&bridge->queue_mutex, NULL) != 0) {
        rl_example_client_destroy(&bridge->client);
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
        rl_example_client_destroy(&bridge->client);
        return RCLIENT_ERR_IO;
    }
    return RCLIENT_OK;
}

static void bridge_stop(civetweb_bridge_t *bridge) {
    pthread_mutex_lock(&bridge->queue_mutex);
    bridge->stop = true;
    pthread_mutex_unlock(&bridge->queue_mutex);
    wake_bridge(bridge);
    pthread_join(bridge->thread, NULL);
    close(bridge->wake_pipe[0]);
    close(bridge->wake_pipe[1]);
    pthread_mutex_destroy(&bridge->queue_mutex);
    rl_example_client_destroy(&bridge->client);
}

static int bridge_check(civetweb_bridge_t *bridge, int *status, bool *allowed) {
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
    *status = job.status;
    *allowed = job.allowed;
    pthread_cond_destroy(&job.condition);
    pthread_mutex_destroy(&job.mutex);
    return 0;
}

static int limited_handler(struct mg_connection *connection, void *user) {
    civetweb_bridge_t *bridge = user;
    int status = RCLIENT_ERR_IO;
    bool allowed = false;
    if (bridge_check(bridge, &status, &allowed) != 0 || status != RCLIENT_OK) {
        mg_send_http_error(connection, 503, "%s", "rate-limit service unavailable");
        return 503;
    }
    const char *body = allowed ? "allowed\n" : "denied\n";
    int http_status = allowed ? 200 : 429;
    if (http_status == 200) {
        mg_send_http_ok(connection, "text/plain", strlen(body));
        mg_write(connection, body, strlen(body));
    } else {
        mg_send_http_error(connection, http_status, "%s", body);
    }
    return http_status;
}

int main(void) {
    rl_example_options_t options;
    if (rl_example_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n");
        return EXIT_FAILURE;
    }

    civetweb_bridge_t bridge;
    int status = bridge_start(&bridge, &options);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "bridge initialization failed: %s (%d)\n",
            rl_example_status_name(status), status);
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
