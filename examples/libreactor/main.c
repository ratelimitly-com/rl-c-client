#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <reactor.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. libreactor watches duplicates of the rl-c-client UDP descriptors.
 * 2. GET /limited allocates state and starts combined admission.
 * 3. A reactor timer advances that request's current deadline.
 * 4. UDP readiness lets the adapter drain the original socket.
 * 5. Admitted work is measured, reported, and mapped with both decisions.
 *
 * Ownership: the adapter owns original sockets; descriptor objects own their
 * duplicates. Each HTTP request owns one timer and check. libreactor keeps a
 * handling reference until server_respond(), including after peer disconnect.
 */

typedef struct reactor_app reactor_app_t;

typedef struct udp_watcher {
    descriptor descriptor;
    reactor_app_t *app;
    int client_fd;
} udp_watcher_t;

typedef struct pending_request {
    reactor_app_t *app;
    server_request *http_request;
    r_admission_request_t request;
    timer timer;
    bool defer_completion;
    bool completion_ready;
    int completion_status;
    r_admission_outcome_t completion_outcome;
    bool protected_work_complete;
    uint32_t observed_latency_ms;
} pending_request_t;

struct reactor_app {
    r_runtime_client_t runtime;
    server http_server;
    udp_watcher_t watchers[2];
    size_t watcher_count;
};

static void finish_request(
    pending_request_t *pending,
    int status,
    const r_admission_outcome_t *outcome
) {
    /* Stop all loop callbacks before releasing request-owned state. */
    timer_destruct(&pending->timer);
    if (pending->request.active) {
        r_runtime_admission_cancel(&pending->app->runtime, &pending->request);
    }

    if (status != RCLIENT_OK) {
        server_respond(pending->http_request,
            data_string("503 Service Unavailable"),
            data_string("text/plain"),
            data_string("rate-limit service unavailable\n"));
    } else if (outcome->latency_limited && !outcome->rate_limited) {
        server_respond(pending->http_request,
            data_string("503 Service Unavailable"),
            data_string("text/plain"),
            data_string("denied by latency guard\n"));
    } else if (!outcome->allowed) {
        server_respond(pending->http_request,
            data_string("429 Too Many Requests"),
            data_string("text/plain"),
            data_string("denied by resource limit\n"));
    } else {
        server_ok(pending->http_request,
            data_string("text/plain"), data_string("allowed\n"));
    }
    free(pending);
}

static int perform_protected_work(void *user) {
    pending_request_t *pending = user;
    /* Replace this with the application operation the route protects. */
    pending->protected_work_complete = true;
    return RCLIENT_OK;
}

static void on_admission(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    pending_request_t *pending = user;
    if (status == RCLIENT_OK && outcome->allowed) {
        int report_status = r_runtime_admission_run_and_report(
            &pending->app->runtime,
            &pending->request,
            perform_protected_work,
            pending,
            &pending->observed_latency_ms
        );
        if (report_status != RCLIENT_OK) {
            fprintf(stderr, "latency report failed: %s (%d)\n",
                r_runtime_status_name(report_status), report_status);
        }
    }
    /* Some rl-c-client operations may complete synchronously.  Defer freeing
     * the object until the operation that owns the current stack returns. */
    if (pending->defer_completion) {
        pending->completion_ready = true;
        pending->completion_status = status;
        pending->completion_outcome = *outcome;
        return;
    }
    finish_request(pending, status, outcome);
}

static int arm_timer(pending_request_t *pending) {
    uint64_t delay_ms = 0;
    int status = r_runtime_admission_delay_ms(&pending->request, &delay_ms);
    if (status != RCLIENT_OK) {
        return status;
    }
    /* timer_set() uses nanoseconds; zero is reserved for immediate startup in
     * some libreactor versions, so use one nanosecond for an expired deadline. */
    uint64_t delay_ns = delay_ms == 0 ? 1 : delay_ms * 1000000u;
    timer_set(&pending->timer, delay_ns, 0);
    return RCLIENT_OK;
}

static void on_timeout(reactor_event *event) {
    pending_request_t *pending = event->state;
    pending->defer_completion = true;
    int status = r_runtime_admission_on_timeout(
        &pending->app->runtime,
        &pending->request
    );
    pending->defer_completion = false;

    if (pending->completion_ready) {
        finish_request(pending,
            pending->completion_status, &pending->completion_outcome);
        return;
    }
    if (status != RCLIENT_OK || arm_timer(pending) != RCLIENT_OK) {
        finish_request(pending,
            status != RCLIENT_OK ? status : RCLIENT_ERR_IO,
            &pending->completion_outcome);
    }
}

static void on_udp_readable(reactor_event *event) {
    udp_watcher_t *watcher = event->state;
    if (event->type != DESCRIPTOR_READ) {
        fprintf(stderr, "libreactor UDP watcher closed unexpectedly\n");
        return;
    }
    int status = r_runtime_client_on_readable(
        &watcher->app->runtime,
        watcher->client_fd
    );
    if (status != RCLIENT_OK) {
        fprintf(stderr, "Ratelimitly UDP ingress failed: %s (%d)\n",
            r_runtime_status_name(status), status);
    }
}

static void on_http_request(reactor_event *event) {
    reactor_app_t *app = event->state;
    server_request *request = (server_request *)event->data;
    if (event->type != SERVER_REQUEST
        || !data_equal(request->method, data_string("GET"))
        || !data_equal(request->target, data_string("/limited"))) {
        server_not_found(request);
        return;
    }

    pending_request_t *pending = calloc(1, sizeof(*pending));
    if (!pending) {
        server_respond(request,
            data_string("503 Service Unavailable"),
            data_string("text/plain"),
            data_string("allocation failed\n"));
        return;
    }
    pending->app = app;
    pending->http_request = request;
    timer_construct(&pending->timer, on_timeout, pending);

    pending->defer_completion = true;
    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "libreactor-example";
    config.service_name = "libreactor-protected-service";
    config.metrics_label = "libreactor-example";
    int status = r_client_admission_start(
        app->runtime.handle,
        &pending->request,
        &config,
        on_admission,
        pending
    );
    pending->defer_completion = false;
    if (pending->completion_ready) {
        finish_request(pending,
            pending->completion_status, &pending->completion_outcome);
    } else if (status != RCLIENT_OK || arm_timer(pending) != RCLIENT_OK) {
        finish_request(pending,
            status != RCLIENT_OK ? status : RCLIENT_ERR_IO,
            &pending->completion_outcome);
    }
}

static int open_udp_watchers(reactor_app_t *app) {
    size_t socket_count = r_runtime_socket_count(&app->runtime);
    for (size_t i = 0; i < socket_count; i++) {
        udp_watcher_t *watcher = &app->watchers[i];
        watcher->app = app;
        watcher->client_fd = r_runtime_socket_at(&app->runtime, i);
        int duplicate_fd = dup(watcher->client_fd);
        if (duplicate_fd < 0) {
            return -1;
        }
        descriptor_construct(&watcher->descriptor, on_udp_readable, watcher);
        descriptor_open(&watcher->descriptor, duplicate_fd, DESCRIPTOR_READ);
        app->watcher_count++;
    }
    return 0;
}

int main(void) {
    r_runtime_options_t options;
    if (r_runtime_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_AUTH_KEY; RATELIMITLY_TENANT is optional\n");
        return EXIT_FAILURE;
    }

    reactor_app_t app = {0};
    int status = r_runtime_client_init(&app.runtime, &options);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "client initialization failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }

    int exit_status = EXIT_FAILURE;
    int listener = -1;
    reactor_construct();
    server_construct(&app.http_server, on_http_request, &app);
    listener = net_socket(net_resolve(
        "0.0.0.0", "8000", AF_INET, SOCK_STREAM, AI_PASSIVE));
    if (listener < 0 || open_udp_watchers(&app) != 0) {
        fprintf(stderr, "failed to initialize libreactor descriptors\n");
        goto cleanup;
    }
    server_open(&app.http_server, listener, NULL);
    listener = -1; /* server now owns the listening descriptor. */
    reactor_loop();
    exit_status = EXIT_SUCCESS;

cleanup:
    if (listener >= 0) {
        close(listener);
    }
    server_destruct(&app.http_server);
    for (size_t i = 0; i < app.watcher_count; i++) {
        descriptor_destruct(&app.watchers[i].descriptor);
    }
    reactor_destruct();
    r_runtime_client_destroy(&app.runtime);
    return exit_status;
}
