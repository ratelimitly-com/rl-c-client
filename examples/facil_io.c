#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fio.h>
#include <http.h>

#include "common/rl_example.h"

/*
 * Flow
 * ----
 * 1. facil.io watches duplicates of the rl-c-client UDP descriptors.
 * 2. GET /limited pauses its HTTP handle and starts an asynchronous check.
 * 3. A one-shot fio_run_every() callback advances the request deadline.
 * 4. UDP readiness lets the adapter drain the original socket.
 * 5. Completion resumes HTTP and sends status 200, 429, or 503.
 *
 * Ownership: facil.io owns attached duplicate descriptors; the adapter owns
 * originals. Each timer and paused HTTP handle retains pending_request_t until
 * its finish callback. One facil.io thread serializes all rl-c-client access.
 */

typedef struct facil_app facil_app_t;

typedef struct udp_watcher {
    fio_protocol_s protocol; /* First member: safe protocol-to-watcher cast. */
    facil_app_t *app;
    int client_fd;
    intptr_t uuid;
} udp_watcher_t;

typedef struct pending_request {
    facil_app_t *app;
    http_pause_handle_s *http;
    rl_example_request_t request;
    size_t references;
    bool completed;
    bool defer_completion;
    bool completion_ready;
    int completion_status;
    bool completion_allowed;
} pending_request_t;

struct facil_app {
    rl_example_client_t client;
    udp_watcher_t watchers[2];
    size_t watcher_count;
};

static facil_app_t app;

static void retain_pending(pending_request_t *pending) {
    pending->references++;
}

static void release_pending(void *data) {
    pending_request_t *pending = data;
    if (--pending->references == 0) {
        free(pending);
    }
}

static void send_http_result(http_s *http) {
    pending_request_t *pending = http->udata;
    static char allowed_body[] = "allowed\n";
    static char denied_body[] = "denied\n";
    static char unavailable_body[] = "rate-limit service unavailable\n";
    char *body;
    size_t length;

    (void)http_set_header2(http,
        (fio_str_info_s){.data = "content-type", .len = 12},
        (fio_str_info_s){.data = "text/plain", .len = 10});
    if (pending->completion_status != RCLIENT_OK) {
        http->status = 503;
        body = unavailable_body;
        length = sizeof(unavailable_body) - 1;
    } else if (!pending->completion_allowed) {
        http->status = 429;
        body = denied_body;
        length = sizeof(denied_body) - 1;
    } else {
        http->status = 200;
        body = allowed_body;
        length = sizeof(allowed_body) - 1;
    }
    (void)http_send_body(http, body, length);
    release_pending(pending); /* Release the paused HTTP handle's reference. */
}

/* http_resume calls this when the peer disappeared before the resume task. */
static void discard_http_result(void *data) {
    release_pending(data);
}

static void complete_pending(
    pending_request_t *pending,
    int status,
    bool allowed
) {
    if (pending->completed) {
        return;
    }
    pending->completed = true;
    pending->completion_status = status;
    pending->completion_allowed = allowed;
    if (pending->request.active) {
        rl_example_request_cancel(&pending->app->client, &pending->request);
    }
    http_resume(pending->http, send_http_result, discard_http_result);
}

static void on_rate_limit(void *user, int status, bool allowed) {
    pending_request_t *pending = user;
    /* Do not resume/free from inside an rl-c-client call that still has this
     * object on its stack. */
    if (pending->defer_completion) {
        pending->completion_ready = true;
        pending->completion_status = status;
        pending->completion_allowed = allowed;
        return;
    }
    complete_pending(pending, status, allowed);
}

static void on_timer_finished(void *data) {
    release_pending(data);
}

static int arm_timer(pending_request_t *pending);

static void on_timer(void *data) {
    pending_request_t *pending = data;
    if (pending->completed) {
        return;
    }

    pending->defer_completion = true;
    int status = rl_example_request_on_timeout(
        &pending->app->client,
        &pending->request
    );
    pending->defer_completion = false;
    if (pending->completion_ready) {
        complete_pending(pending,
            pending->completion_status, pending->completion_allowed);
    } else if (status != RCLIENT_OK || arm_timer(pending) != RCLIENT_OK) {
        complete_pending(pending,
            status != RCLIENT_OK ? status : RCLIENT_ERR_IO, false);
    }
}

static int arm_timer(pending_request_t *pending) {
    uint64_t delay_ms = 0;
    int status = rl_example_request_delay_ms(&pending->request, &delay_ms);
    if (status != RCLIENT_OK) {
        return status;
    }
    retain_pending(pending);
    /* facil.io rejects a zero-millisecond timer. */
    size_t interval = delay_ms == 0 ? 1 : (size_t)delay_ms;
    if (fio_run_every(interval, 1,
            on_timer, pending, on_timer_finished) != 0) {
        /* No on_finish callback exists when scheduling itself fails. */
        release_pending(pending);
        return RCLIENT_ERR_IO;
    }
    return RCLIENT_OK;
}

static void on_http_paused(http_pause_handle_s *http) {
    pending_request_t *pending = http_paused_udata_get(http);
    pending->http = http;
    pending->defer_completion = true;
    int status = rl_example_check(
        &pending->app->client,
        &pending->request,
        "facil.io-example",
        on_rate_limit,
        pending
    );
    pending->defer_completion = false;
    if (pending->completion_ready) {
        complete_pending(pending,
            pending->completion_status, pending->completion_allowed);
    } else if (status != RCLIENT_OK || arm_timer(pending) != RCLIENT_OK) {
        complete_pending(pending,
            status != RCLIENT_OK ? status : RCLIENT_ERR_IO, false);
    }
}

static void on_http_request(http_s *http) {
    fio_str_info_s method = fiobj_obj2cstr(http->method);
    fio_str_info_s path = fiobj_obj2cstr(http->path);
    if (method.len != 3 || memcmp(method.data, "GET", 3) != 0
        || path.len != 8 || memcmp(path.data, "/limited", 8) != 0) {
        http->status = 404;
        (void)http_send_body(http, "not found\n", 10);
        return;
    }

    pending_request_t *pending = calloc(1, sizeof(*pending));
    if (!pending) {
        http->status = 503;
        (void)http_send_body(http, "allocation failed\n", 18);
        return;
    }
    pending->app = &app;
    pending->references = 1; /* Owned by the paused HTTP request. */
    http->udata = pending;
    http_pause(http, on_http_paused);
}

static void on_udp_readable(intptr_t uuid, fio_protocol_s *protocol) {
    (void)uuid;
    udp_watcher_t *watcher = (udp_watcher_t *)protocol;
    int status = rl_example_client_on_readable(
        &watcher->app->client,
        watcher->client_fd
    );
    if (status != RCLIENT_OK) {
        fprintf(stderr, "Ratelimitly UDP ingress failed: %s (%d)\n",
            rl_example_status_name(status), status);
    }
}

static int attach_udp_watchers(facil_app_t *application) {
    size_t socket_count = rl_example_socket_count(&application->client);
    for (size_t i = 0; i < socket_count; i++) {
        udp_watcher_t *watcher = &application->watchers[i];
        watcher->app = application;
        watcher->client_fd = rl_example_socket_at(&application->client, i);
        watcher->uuid = -1;
        watcher->protocol.on_data = on_udp_readable;
        int duplicate_fd = dup(watcher->client_fd);
        if (duplicate_fd < 0 || fio_set_non_block(duplicate_fd) != 0) {
            if (duplicate_fd >= 0) {
                close(duplicate_fd);
            }
            return -1;
        }
        fio_attach_fd(duplicate_fd, &watcher->protocol);
        watcher->uuid = fio_fd2uuid(duplicate_fd);
        if (watcher->uuid == -1) {
            close(duplicate_fd);
            return -1;
        }
        application->watcher_count++;
    }
    return 0;
}

static void detach_udp_watchers(facil_app_t *application) {
    for (size_t i = 0; i < application->watcher_count; i++) {
        if (application->watchers[i].uuid != -1) {
            fio_force_close(application->watchers[i].uuid);
            application->watchers[i].uuid = -1;
        }
    }
    application->watcher_count = 0;
}

int main(void) {
    rl_example_options_t options;
    if (rl_example_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n");
        return EXIT_FAILURE;
    }
    int status = rl_example_client_init(&app.client, &options);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "client initialization failed: %s (%d)\n",
            rl_example_status_name(status), status);
        return EXIT_FAILURE;
    }
    if (attach_udp_watchers(&app) != 0) {
        fprintf(stderr, "failed to initialize rate-limit UDP watchers\n");
        detach_udp_watchers(&app);
        rl_example_client_destroy(&app.client);
        return EXIT_FAILURE;
    }
    if (http_listen("8000", NULL, .on_request = on_http_request) == -1) {
        fprintf(stderr, "failed to listen on port 8000\n");
        detach_udp_watchers(&app);
        rl_example_client_destroy(&app.client);
        return EXIT_FAILURE;
    }

    fio_start(.threads = 1, .workers = 1);
    detach_udp_watchers(&app);
    rl_example_client_destroy(&app.client);
    return EXIT_SUCCESS;
}
