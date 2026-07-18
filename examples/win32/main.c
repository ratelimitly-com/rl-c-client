#ifndef _WIN32
#error "This example requires Win32"
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. WSAEventSelect associates one WSAEVENT with each runtime UDP socket.
 * 2. WSAWaitForMultipleEvents waits for socket input or admission timeout.
 * 3. WSAEnumNetworkEvents clears readiness before the runtime drains packets.
 * 4. Protected work runs only after resource and latency admission.
 * 5. The runtime measures successful work and reports one latency sample.
 *
 * Ownership: application owns WSAEVENTs, request, and copied outcome. runtime
 * owns WinSock startup, the client, and SOCKETs. Associations clear first.
 */

typedef struct application {
    WSAEVENT socket_events[2];
    size_t socket_count;
    r_runtime_client_t runtime;
    r_admission_request_t request;
    r_admission_outcome_t outcome;
    char response[96];
    uint32_t observed_latency_ms;
    int status;
    bool done;
} application_t;

static int prepare_response(void *user) {
    application_t *app = user;
    int length = snprintf(
        app->response,
        sizeof(app->response),
        "inventory response prepared by Win32"
    );
    return length >= 0 && (size_t)length < sizeof(app->response)
        ? RCLIENT_OK
        : RCLIENT_ERR_IO;
}

static void on_admission(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    application_t *app = user;
    app->status = status;
    app->outcome = *outcome;
    if (status == RCLIENT_OK && outcome->allowed) {
        app->status = r_runtime_admission_run_and_report(
            &app->runtime,
            &app->request,
            prepare_response,
            app,
            &app->observed_latency_ms
        );
    }
    app->done = true;
}

static int initialize_events(application_t *app) {
    size_t count = r_runtime_socket_count(&app->runtime);
    if (count == 0u || count > WSA_MAXIMUM_WAIT_EVENTS) {
        return RCLIENT_ERR_IO;
    }
    for (size_t i = 0; i < count; i++) {
        SOCKET socket_value = r_runtime_socket_at(&app->runtime, i);
        WSAEVENT socket_event = WSACreateEvent();
        if (socket_event == WSA_INVALID_EVENT
            || WSAEventSelect(socket_value, socket_event, FD_READ) != 0) {
            if (socket_event != WSA_INVALID_EVENT) {
                WSACloseEvent(socket_event);
            }
            return RCLIENT_ERR_IO;
        }
        app->socket_events[app->socket_count++] = socket_event;
    }
    return RCLIENT_OK;
}

static int handle_socket_event(application_t *app, size_t index) {
    SOCKET socket_value = r_runtime_socket_at(&app->runtime, index);
    WSANETWORKEVENTS network_events;
    if (WSAEnumNetworkEvents(
            socket_value,
            app->socket_events[index],
            &network_events) != 0) {
        return RCLIENT_ERR_IO;
    }
    if ((network_events.lNetworkEvents & FD_READ) == 0
        || network_events.iErrorCode[FD_READ_BIT] != 0) {
        return RCLIENT_ERR_IO;
    }
    return r_runtime_client_on_readable(&app->runtime, socket_value);
}

static int run_loop(application_t *app) {
    while (!app->done) {
        uint64_t delay_ms = 0u;
        int status = r_runtime_admission_delay_ms(&app->request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        DWORD timeout_ms = delay_ms >= WSA_INFINITE
            ? WSA_INFINITE - 1u
            : (DWORD)delay_ms;
        DWORD result = WSAWaitForMultipleEvents(
            (DWORD)app->socket_count,
            app->socket_events,
            FALSE,
            timeout_ms,
            FALSE
        );
        if (result == WSA_WAIT_TIMEOUT) {
            status = r_runtime_admission_on_timeout(&app->runtime, &app->request);
        } else if (result == WSA_WAIT_FAILED) {
            status = RCLIENT_ERR_IO;
        } else {
            size_t index = (size_t)(result - WSA_WAIT_EVENT_0);
            status = index < app->socket_count
                ? handle_socket_event(app, index)
                : RCLIENT_ERR_IO;
        }
        if (status != RCLIENT_OK) {
            return status;
        }
    }
    return app->status;
}

static int start_admission(application_t *app) {
    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "win32-example";
    config.service_name = "win32-protected-service";
    config.metrics_label = "win32-example";
    return r_client_admission_start(
        app->runtime.handle,
        &app->request,
        &config,
        on_admission,
        app
    );
}

static void destroy_application(application_t *app) {
    if (app->request.active) {
        r_runtime_admission_cancel(&app->runtime, &app->request);
    }
    for (size_t i = 0; i < app->socket_count; i++) {
        SOCKET socket_value = r_runtime_socket_at(&app->runtime, i);
        (void)WSAEventSelect(socket_value, NULL, 0);
        WSACloseEvent(app->socket_events[i]);
    }
    r_runtime_client_destroy(&app->runtime);
}

static void print_outcome(const application_t *app) {
    if (app->outcome.allowed) {
        printf("allowed: %s; latency=%" PRIu32 " ms\n",
            app->response, app->observed_latency_ms);
    } else if (app->outcome.rate_limited && app->outcome.latency_limited) {
        puts("denied: resource limit and latency guard");
    } else if (app->outcome.latency_limited) {
        puts("denied: latency guard");
    } else {
        puts("denied: resource rate limit");
    }
}

int main(void) {
    r_runtime_options_t options;
    if (r_runtime_options_from_env(&options) != RCLIENT_OK) {
        fputs("set RATELIMITLY_AUTH_KEY; RATELIMITLY_TENANT is optional\n", stderr);
        return EXIT_FAILURE;
    }

    application_t app = {.status = RCLIENT_ERR_IO};
    int status = r_runtime_client_init(&app.runtime, &options);
    if (status == RCLIENT_OK) {
        status = initialize_events(&app);
    }
    if (status == RCLIENT_OK) {
        status = start_admission(&app);
    }
    if (status == RCLIENT_OK) {
        status = run_loop(&app);
    }

    destroy_application(&app);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "Win32 example failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }
    print_outcome(&app);
    return app.outcome.allowed ? EXIT_SUCCESS : 2;
}
