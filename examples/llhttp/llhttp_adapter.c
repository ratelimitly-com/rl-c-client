#include <stdio.h>
#include <string.h>

#include <llhttp.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"
#include "llhttp_adapter.h"

static rl_llhttp_adapter_t *adapter_from_parser(llhttp_t *parser) {
    return parser->data;
}

static int on_message_begin(llhttp_t *parser) {
    rl_llhttp_adapter_t *adapter = adapter_from_parser(parser);
    adapter->url_length = 0u;
    adapter->url[0] = '\0';
    adapter->path[0] = '\0';
    adapter->method[0] = '\0';
    adapter->last_client_status = RCLIENT_OK;
    return HPE_OK;
}

static int on_url(llhttp_t *parser, const char *data, size_t length) {
    rl_llhttp_adapter_t *adapter = adapter_from_parser(parser);
    size_t available = sizeof(adapter->url) - adapter->url_length - 1u;
    if (length > available) {
        llhttp_set_error_reason(parser, "request URL exceeds adapter capacity");
        return HPE_USER;
    }
    memcpy(adapter->url + adapter->url_length, data, length);
    adapter->url_length += length;
    adapter->url[adapter->url_length] = '\0';
    return HPE_OK;
}

static int prepare_identity(rl_llhttp_adapter_t *adapter) {
    const char *method_name = llhttp_method_name(
        (llhttp_method_t)adapter->parser.method
    );
    if (!method_name || adapter->url_length == 0u) {
        return RCLIENT_ERR_PROTOCOL;
    }
    int method_length = snprintf(
        adapter->method,
        sizeof(adapter->method),
        "%s",
        method_name
    );
    size_t path_length = strcspn(adapter->url, "?");
    if (method_length < 0
        || (size_t)method_length >= sizeof(adapter->method)
        || path_length >= sizeof(adapter->path)) {
        return RCLIENT_ERR_CONFIG;
    }
    memcpy(adapter->path, adapter->url, path_length);
    adapter->path[path_length] = '\0';
    int bucket_length = snprintf(
        adapter->bucket,
        sizeof(adapter->bucket),
        "llhttp:%s:%s",
        adapter->method,
        adapter->path
    );
    return bucket_length >= 0
        && (size_t)bucket_length < sizeof(adapter->bucket)
        ? RCLIENT_OK
        : RCLIENT_ERR_CONFIG;
}

static int run_protected_work(void *user) {
    rl_llhttp_adapter_t *adapter = user;
    return adapter->protected_work(
        adapter->user,
        adapter->method,
        adapter->path
    );
}

static void on_admission(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    rl_llhttp_adapter_t *adapter = user;
    uint32_t observed_latency_ms = 0u;
    if (status == RCLIENT_OK && outcome->allowed) {
        status = r_runtime_admission_run_and_report(
            adapter->runtime,
            &adapter->admission,
            run_protected_work,
            adapter,
            &observed_latency_ms
        );
    }
    if (llhttp_get_errno(&adapter->parser) == HPE_PAUSED) {
        llhttp_resume(&adapter->parser);
    }
    adapter->result(
        adapter->user,
        status,
        outcome,
        observed_latency_ms
    );
}

static int on_message_complete(llhttp_t *parser) {
    rl_llhttp_adapter_t *adapter = adapter_from_parser(parser);
    int status = prepare_identity(adapter);
    if (status != RCLIENT_OK) {
        adapter->last_client_status = status;
        llhttp_set_error_reason(parser, "request has no bounded method or path");
        return HPE_USER;
    }

    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = adapter->bucket;
    config.service_name = "llhttp-protected-service";
    config.metrics_label = "llhttp-example";
    status = r_client_admission_start(
        adapter->runtime->handle,
        &adapter->admission,
        &config,
        on_admission,
        adapter
    );
    adapter->last_client_status = status;
    if (status != RCLIENT_OK) {
        llhttp_set_error_reason(parser, "admission check submission failed");
        return HPE_USER;
    }
    /* Pause at the request boundary until the asynchronous decision arrives. */
    return HPE_PAUSED;
}

int rl_llhttp_adapter_init(
    rl_llhttp_adapter_t *adapter,
    r_runtime_client_t *runtime,
    rl_llhttp_protected_work_cb protected_work,
    rl_llhttp_result_cb result,
    void *user
) {
    if (!adapter || !runtime || !runtime->handle || !protected_work || !result) {
        return RCLIENT_ERR_CONFIG;
    }
    memset(adapter, 0, sizeof(*adapter));
    adapter->runtime = runtime;
    adapter->protected_work = protected_work;
    adapter->result = result;
    adapter->user = user;
    llhttp_settings_init(&adapter->settings);
    adapter->settings.on_message_begin = on_message_begin;
    adapter->settings.on_url = on_url;
    adapter->settings.on_message_complete = on_message_complete;
    llhttp_init(&adapter->parser, HTTP_REQUEST, &adapter->settings);
    adapter->parser.data = adapter;
    return RCLIENT_OK;
}

llhttp_errno_t rl_llhttp_adapter_feed(
    rl_llhttp_adapter_t *adapter,
    const char *data,
    size_t length,
    size_t *consumed
) {
    if (!adapter || !data || !consumed) {
        return HPE_USER;
    }
    llhttp_errno_t status = llhttp_execute(&adapter->parser, data, length);
    *consumed = length;
    if (status != HPE_OK) {
        const char *position = llhttp_get_error_pos(&adapter->parser);
        if (position >= data && position <= data + length) {
            *consumed = (size_t)(position - data);
        }
    }
    return status;
}

llhttp_errno_t rl_llhttp_adapter_finish(rl_llhttp_adapter_t *adapter) {
    if (!adapter) {
        return HPE_USER;
    }
    if (adapter->admission.active) {
        return HPE_PAUSED;
    }
    return llhttp_finish(&adapter->parser);
}

r_admission_request_t *rl_llhttp_adapter_pending(
    rl_llhttp_adapter_t *adapter
) {
    return adapter && adapter->admission.active
        ? &adapter->admission
        : NULL;
}

int rl_llhttp_adapter_last_client_status(
    const rl_llhttp_adapter_t *adapter
) {
    return adapter ? adapter->last_client_status : RCLIENT_ERR_CONFIG;
}

void rl_llhttp_adapter_dispose(rl_llhttp_adapter_t *adapter) {
    if (adapter && adapter->runtime && adapter->admission.active) {
        r_runtime_admission_cancel(adapter->runtime, &adapter->admission);
    }
}
