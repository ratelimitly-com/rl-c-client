#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <onion/onion.h>
#include <onion/request.h>
#include <onion/response.h>
#include <onion/url.h>

#include "common/rl_example.h"

/*
 * Onion's OCS_YIELD contract transfers request/response ownership to the
 * application.  This makes a clean asynchronous bridge possible without
 * blocking Onion's worker pool: handlers enqueue a yielded request, and one
 * client thread owns every rl-c-client operation plus its UDP poll loop.
 *
 * The official Onion long-poll example also writes and frees yielded requests
 * from a background thread.  We follow that lifecycle here.  Even if the HTTP
 * peer disconnects, response_flush() merely fails and the yielded objects are
 * still released exactly once.
 */

typedef struct onion_bridge onion_bridge_t;

typedef struct bridge_job {
    struct bridge_job *next;
    onion_bridge_t *bridge;
    onion_request *http_request;
    onion_response *http_response;
    rl_example_request_t request;
} bridge_job_t;

struct onion_bridge {
    rl_example_client_t client;
    pthread_t thread;
    pthread_mutex_t queue_mutex;
    bridge_job_t *queue_head;
    bridge_job_t *queue_tail;
    bridge_job_t *active;
    int wake_pipe[2];
    bool stop;
};

static void finish_job(bridge_job_t *job, int status, bool allowed) {
    int http_status = status != RCLIENT_OK ? 503 : (allowed ? 200 : 429);
    const char *body = status != RCLIENT_OK
        ? "rate-limit service unavailable\n"
        : (allowed ? "allowed\n" : "denied\n");

    onion_response_set_code(job->http_response, http_status);
    onion_response_set_header(job->http_response,
        "Content-Type", "text/plain");
    onion_response_set_length(job->http_response, strlen(body));
    onion_response_write0(job->http_response, body);
    (void)onion_response_flush(job->http_response);
    onion_response_free(job->http_response);
    onion_request_free(job->http_request);
    free(job);
}

static void remove_active(onion_bridge_t *bridge, bridge_job_t *job) {
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
    finish_job(job, status, allowed);
}

static bridge_job_t *take_queue(onion_bridge_t *bridge) {
    pthread_mutex_lock(&bridge->queue_mutex);
    bridge_job_t *jobs = bridge->queue_head;
    bridge->queue_head = NULL;
    bridge->queue_tail = NULL;
    pthread_mutex_unlock(&bridge->queue_mutex);
    return jobs;
}

static void start_queued_jobs(onion_bridge_t *bridge) {
    bridge_job_t *job = take_queue(bridge);
    while (job) {
        bridge_job_t *next = job->next;
        job->next = bridge->active;
        bridge->active = job;
        int status = rl_example_check(
            &bridge->client,
            &job->request,
            "onion-example",
            on_rate_limit,
            job
        );
        if (status != RCLIENT_OK) {
            remove_active(bridge, job);
            finish_job(job, status, false);
        }
        job = next;
    }
}

static void drain_wake_pipe(onion_bridge_t *bridge) {
    char buffer[64];
    while (read(bridge->wake_pipe[0], buffer, sizeof(buffer)) > 0) {
    }
}

static int next_timeout(onion_bridge_t *bridge) {
    int timeout_ms = 1000;
    for (bridge_job_t *job = bridge->active; job; job = job->next) {
        uint64_t delay_ms = 0;
        if (rl_example_request_delay_ms(&job->request, &delay_ms) != RCLIENT_OK) {
            return 0;
        }
        int candidate = delay_ms > INT_MAX ? INT_MAX : (int)delay_ms;
        if (candidate < timeout_ms) {
            timeout_ms = candidate;
        }
    }
    return timeout_ms;
}

static int expire_requests(onion_bridge_t *bridge) {
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

static bool bridge_should_stop(onion_bridge_t *bridge) {
    pthread_mutex_lock(&bridge->queue_mutex);
    bool stop = bridge->stop;
    pthread_mutex_unlock(&bridge->queue_mutex);
    return stop;
}

static void fail_all_jobs(onion_bridge_t *bridge, int status) {
    bridge_job_t *job = take_queue(bridge);
    while (job) {
        bridge_job_t *next = job->next;
        finish_job(job, status, false);
        job = next;
    }
    while (bridge->active) {
        job = bridge->active;
        bridge->active = job->next;
        rl_example_request_cancel(&bridge->client, &job->request);
        finish_job(job, status, false);
    }
}

static void *bridge_loop(void *user) {
    onion_bridge_t *bridge = user;
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

        int ready = poll(poll_fds, (nfds_t)(socket_count + 1),
            next_timeout(bridge));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            loop_status = RCLIENT_ERR_IO;
            break;
        }
        if ((poll_fds[0].revents & POLLIN) != 0) {
            drain_wake_pipe(bridge);
            start_queued_jobs(bridge);
        }
        for (size_t i = 0; i < socket_count; i++) {
            if ((poll_fds[i + 1].revents & POLLIN) != 0) {
                loop_status = rl_example_client_on_readable(
                    &bridge->client, poll_fds[i + 1].fd);
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
    onion_bridge_t *bridge,
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

static void bridge_stop(onion_bridge_t *bridge) {
    pthread_mutex_lock(&bridge->queue_mutex);
    bridge->stop = true;
    pthread_mutex_unlock(&bridge->queue_mutex);
    char byte = 0;
    (void)write(bridge->wake_pipe[1], &byte, 1);
    pthread_join(bridge->thread, NULL);
    close(bridge->wake_pipe[0]);
    close(bridge->wake_pipe[1]);
    pthread_mutex_destroy(&bridge->queue_mutex);
    rl_example_client_destroy(&bridge->client);
}

static onion_connection_status limited(
    void *user,
    onion_request *request,
    onion_response *response
) {
    onion_bridge_t *bridge = user;
    bridge_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        onion_response_set_code(response, 503);
        onion_response_write0(response, "allocation failed\n");
        return OCS_PROCESSED;
    }
    job->bridge = bridge;
    job->http_request = request;
    job->http_response = response;

    pthread_mutex_lock(&bridge->queue_mutex);
    if (bridge->stop) {
        pthread_mutex_unlock(&bridge->queue_mutex);
        free(job);
        onion_response_set_code(response, 503);
        onion_response_write0(response, "rate-limit service unavailable\n");
        return OCS_PROCESSED;
    }
    if (bridge->queue_tail) {
        bridge->queue_tail->next = job;
    } else {
        bridge->queue_head = job;
    }
    bridge->queue_tail = job;
    pthread_mutex_unlock(&bridge->queue_mutex);

    char byte = 0;
    (void)write(bridge->wake_pipe[1], &byte, 1);
    return OCS_YIELD;
}

int main(void) {
    rl_example_options_t options;
    if (rl_example_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n");
        return EXIT_FAILURE;
    }

    onion_bridge_t bridge;
    if (bridge_start(&bridge, &options) != RCLIENT_OK) {
        fprintf(stderr, "failed to start rate-limit client thread\n");
        return EXIT_FAILURE;
    }
    onion *server = onion_new(O_POOL);
    if (!server) {
        bridge_stop(&bridge);
        return EXIT_FAILURE;
    }
    onion_set_hostname(server, "0.0.0.0");
    onion_set_port(server, "8000");
    onion_url_add_with_data(onion_root_url(server),
        "^limited$", limited, &bridge, NULL);
    int status = onion_listen(server);
    onion_free(server);
    bridge_stop(&bridge);
    return status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
