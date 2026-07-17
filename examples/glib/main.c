#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <gio/gio.h>
#include <glib.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow
 * ----
 * 1. A non-owning GIOChannel wraps each runtime-owned UDP socket.
 * 2. GMainLoop dispatches typed socket watches and a one-shot timeout.
 * 3. Ready sockets are drained; timeout callbacks advance admission state.
 * 4. Protected work runs only after both rate and latency checks pass.
 * 5. The runtime measures completed work and reports one latency sample.
 *
 * Ownership: application owns the main loop, channels, watches, and request.
 * runtime owns the client and sockets; channels are explicitly non-owning.
 */

typedef struct application application_t;

typedef struct socket_watcher {
    application_t *app;
    r_runtime_socket_t socket_value;
} socket_watcher_t;

struct application {
    GMainLoop *loop;
    GIOChannel *channels[2];
    guint socket_watch_ids[2];
    socket_watcher_t watchers[2];
    size_t socket_count;
    guint timer_id;
    r_runtime_client_t runtime;
    r_admission_request_t request;
    r_admission_outcome_t outcome;
    char response[96];
    uint32_t observed_latency_ms;
    int status;
    bool done;
};

static void stop_with_error(application_t *app, int status) {
    app->status = status;
    app->done = true;
    g_main_loop_quit(app->loop);
}

static int prepare_response(void *user) {
    application_t *app = user;
    int length = snprintf(
        app->response,
        sizeof(app->response),
        "inventory response prepared by GLib"
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
    if (app->timer_id != 0u) {
        g_source_remove(app->timer_id);
        app->timer_id = 0u;
    }
    app->done = true;
    g_main_loop_quit(app->loop);
}

static gboolean on_timeout(gpointer user);

static int arm_timeout(application_t *app) {
    uint64_t delay_ms = 0u;
    int status = r_runtime_admission_delay_ms(&app->request, &delay_ms);
    if (status != RCLIENT_OK) {
        return status;
    }
    guint glib_delay = delay_ms > G_MAXUINT ? G_MAXUINT : (guint)delay_ms;
    app->timer_id = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        glib_delay,
        on_timeout,
        app,
        NULL
    );
    return app->timer_id != 0u ? RCLIENT_OK : RCLIENT_ERR_NOMEM;
}

static gboolean on_timeout(gpointer user) {
    application_t *app = user;
    app->timer_id = 0u;
    int status = r_runtime_admission_on_timeout(&app->runtime, &app->request);
    if (status != RCLIENT_OK) {
        stop_with_error(app, status);
    } else if (app->request.active && arm_timeout(app) != RCLIENT_OK) {
        stop_with_error(app, RCLIENT_ERR_IO);
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_udp_readable(
    GIOChannel *channel,
    GIOCondition condition,
    gpointer user
) {
    (void)channel;
    socket_watcher_t *watcher = user;
    if ((condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) != 0
        || (condition & G_IO_IN) == 0) {
        stop_with_error(watcher->app, RCLIENT_ERR_IO);
        return G_SOURCE_REMOVE;
    }
    int status = r_runtime_client_on_readable(
        &watcher->app->runtime,
        watcher->socket_value
    );
    if (status != RCLIENT_OK) {
        stop_with_error(watcher->app, status);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static GIOChannel *wrap_socket(r_runtime_socket_t socket_value) {
#ifdef _WIN32
    GIOChannel *channel = g_io_channel_win32_new_socket((gint)socket_value);
#else
    GIOChannel *channel = g_io_channel_unix_new((gint)socket_value);
#endif
    if (channel) {
        g_io_channel_set_close_on_unref(channel, FALSE);
    }
    return channel;
}

static int initialize_socket_watches(application_t *app) {
    size_t count = r_runtime_socket_count(&app->runtime);
    for (size_t i = 0; i < count; i++) {
        socket_watcher_t *watcher = &app->watchers[i];
        watcher->app = app;
        watcher->socket_value = r_runtime_socket_at(&app->runtime, i);
        GIOChannel *channel = wrap_socket(watcher->socket_value);
        if (!channel) {
            return RCLIENT_ERR_NOMEM;
        }
        app->channels[i] = channel;
        app->socket_watch_ids[i] = g_io_add_watch_full(
            channel,
            G_PRIORITY_DEFAULT,
            G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
            on_udp_readable,
            watcher,
            NULL
        );
        if (app->socket_watch_ids[i] == 0u) {
            return RCLIENT_ERR_NOMEM;
        }
        app->socket_count++;
    }
    return app->socket_count > 0u ? RCLIENT_OK : RCLIENT_ERR_IO;
}

static int start_admission(application_t *app) {
    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "glib-example";
    config.service_name = "glib-protected-service";
    config.metrics_label = "glib-example";
    int status = r_client_admission_start(
        app->runtime.handle,
        &app->request,
        &config,
        on_admission,
        app
    );
    return status == RCLIENT_OK ? arm_timeout(app) : status;
}

static void destroy_application(application_t *app) {
    if (app->request.active) {
        r_runtime_admission_cancel(&app->runtime, &app->request);
    }
    if (app->timer_id != 0u) {
        g_source_remove(app->timer_id);
    }
    for (size_t i = 0; i < app->socket_count; i++) {
        if (app->socket_watch_ids[i] != 0u) {
            g_source_remove(app->socket_watch_ids[i]);
        }
        g_io_channel_unref(app->channels[i]);
    }
    r_runtime_client_destroy(&app->runtime);
    g_main_loop_unref(app->loop);
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
        fputs("set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n", stderr);
        return EXIT_FAILURE;
    }

    application_t app = {.status = RCLIENT_ERR_IO};
    app.loop = g_main_loop_new(NULL, FALSE);
    if (!app.loop) {
        return EXIT_FAILURE;
    }
    int status = r_runtime_client_init(&app.runtime, &options);
    if (status == RCLIENT_OK) {
        status = initialize_socket_watches(&app);
    }
    if (status == RCLIENT_OK) {
        status = start_admission(&app);
    }
    if (status == RCLIENT_OK) {
        g_main_loop_run(app.loop);
        status = app.status;
    }

    destroy_application(&app);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "GLib example failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return EXIT_FAILURE;
    }
    print_outcome(&app);
    return app.outcome.allowed ? EXIT_SUCCESS : 2;
}
