#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <llhttp.h>

#include "common/rl_example.h"
#include "llhttp_adapter.h"

/*
 * Flow
 * ----
 * 1. The host feeds arbitrary TCP fragments into llhttp_execute().
 * 2. URL callbacks append fragments to a bounded connection-owned buffer.
 * 3. A complete request starts a check; the next pipelined message pauses.
 * 4. The host retains paused bytes while driving client UDP and deadlines.
 * 5. Completion resumes llhttp, then tells the host it can feed retained bytes.
 *
 * Ownership: the host owns TCP, UDP readiness, deadlines, and one adapter per
 * connection. The adapter owns parser/check state but only borrows the shared
 * client. Query data is excluded from bucket keys to bound cardinality.
 */

static rl_llhttp_adapter_t *adapter_from_parser(llhttp_t *parser) {
    return parser->data;
}

static int on_message_begin(llhttp_t *parser) {
    rl_llhttp_adapter_t *adapter = adapter_from_parser(parser);
    if (adapter->request.active) {
        /* Pausing is resumable; HPE_USER would permanently poison the parser. */
        return HPE_PAUSED;
    }
    adapter->url_length = 0;
    adapter->url[0] = '\0';
    adapter->last_client_status = RCLIENT_OK;
    return 0;
}

static int on_url(llhttp_t *parser, const char *data, size_t length) {
    rl_llhttp_adapter_t *adapter = adapter_from_parser(parser);
    if (length > sizeof(adapter->url) - adapter->url_length - 1) {
        llhttp_set_error_reason(parser, "request URL exceeds adapter capacity");
        return HPE_USER; /* Reject instead of silently truncating a bucket key. */
    }
    memcpy(adapter->url + adapter->url_length, data, length);
    adapter->url_length += length;
    adapter->url[adapter->url_length] = '\0';
    return 0;
}

static void on_rate_limit(void *user, int status, bool allowed) {
    rl_llhttp_adapter_t *adapter = user;
    adapter->last_client_status = status;
    if (llhttp_get_errno(&adapter->parser) == HPE_PAUSED) {
        llhttp_resume(&adapter->parser);
    }
    adapter->callback(adapter->user, status, allowed);
}

static int on_message_complete(llhttp_t *parser) {
    rl_llhttp_adapter_t *adapter = adapter_from_parser(parser);
    const char *method = llhttp_method_name((llhttp_method_t)parser->method);
    if (!method || adapter->url_length == 0) {
        llhttp_set_error_reason(parser, "request has no method or URL");
        return HPE_USER;
    }

    /* Query values often contain user input or secrets and should not create
     * unbounded bucket cardinality. */
    size_t path_length = strcspn(adapter->url, "?");
    char bucket[RL_LLHTTP_BUCKET_CAPACITY];
    int length = snprintf(bucket, sizeof(bucket),
        "llhttp:%s:%.*s", method, (int)path_length, adapter->url);
    if (length < 0 || (size_t)length >= sizeof(bucket)) {
        llhttp_set_error_reason(parser, "derived bucket exceeds adapter capacity");
        return HPE_USER;
    }

    adapter->last_client_status = rl_example_check(
        adapter->client,
        &adapter->request,
        bucket,
        on_rate_limit,
        adapter
    );
    if (adapter->last_client_status != RCLIENT_OK) {
        llhttp_set_error_reason(parser, "rate-limit check submission failed");
        return HPE_USER;
    }
    return 0;
}

/* Initialize one adapter per HTTP connection.  The client may be shared only
 * when the host serializes all access on its event-loop thread. */
int rl_llhttp_adapter_init(
    rl_llhttp_adapter_t *adapter,
    rl_example_client_t *client,
    rl_llhttp_result_cb callback,
    void *user
) {
    if (!adapter || !client || !callback) {
        return RCLIENT_ERR_CONFIG;
    }
    memset(adapter, 0, sizeof(*adapter));
    adapter->client = client;
    adapter->callback = callback;
    adapter->user = user;
    llhttp_settings_init(&adapter->settings);
    adapter->settings.on_message_begin = on_message_begin;
    adapter->settings.on_url = on_url;
    adapter->settings.on_message_complete = on_message_complete;
    llhttp_init(&adapter->parser, HTTP_REQUEST, &adapter->settings);
    adapter->parser.data = adapter;
    return RCLIENT_OK;
}

/* Feed exactly the bytes received by the host.  HPE_OK means parsing and check
 * submission succeeded.  On failure, llhttp_get_error_reason() describes the
 * parser failure and last_client_status distinguishes rl-c-client errors. */
llhttp_errno_t rl_llhttp_adapter_feed(
    rl_llhttp_adapter_t *adapter,
    const char *data,
    size_t length
) {
    return llhttp_execute(&adapter->parser, data, length);
}

llhttp_errno_t rl_llhttp_adapter_finish(rl_llhttp_adapter_t *adapter) {
    return llhttp_finish(&adapter->parser);
}

int rl_llhttp_adapter_last_client_status(const rl_llhttp_adapter_t *adapter) {
    return adapter ? adapter->last_client_status : RCLIENT_ERR_CONFIG;
}

/* Cancel before destroying a connection-owned adapter. */
void rl_llhttp_adapter_dispose(rl_llhttp_adapter_t *adapter) {
    if (adapter->request.active) {
        rl_example_request_cancel(adapter->client, &adapter->request);
    }
}
