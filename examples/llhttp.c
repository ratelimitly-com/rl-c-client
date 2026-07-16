#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <llhttp.h>

#include "common/rl_example.h"

/*
 * llhttp is a parser, not an event loop.  This adapter deliberately does not
 * poll sockets or schedule deadlines.  It turns one completed HTTP request
 * into rl_example_check(); the surrounding host must drive the returned
 * rl_example_request_t using any loop example in this directory.
 *
 * Parser input may arrive in arbitrary fragments.  on_url appends each span to
 * a bounded buffer, and on_message_complete derives a stable bucket from the
 * method and path (the query string is intentionally excluded).  While a check
 * is active, parsing a pipelined second request stops with HPE_USER.  That is
 * explicit backpressure: resume feeding only after the result callback.
 */

#define RL_LLHTTP_URL_CAPACITY 512
#define RL_LLHTTP_BUCKET_CAPACITY 640

typedef void (*rl_llhttp_result_cb)(
    void *user,
    int status,
    bool allowed
);

typedef struct rl_llhttp_adapter {
    llhttp_t parser;
    llhttp_settings_t settings;
    rl_example_client_t *client;
    rl_example_request_t request;
    rl_llhttp_result_cb callback;
    void *user;
    char url[RL_LLHTTP_URL_CAPACITY];
    size_t url_length;
    int last_client_status;
} rl_llhttp_adapter_t;

void rl_llhttp_adapter_init(
    rl_llhttp_adapter_t *adapter,
    rl_example_client_t *client,
    rl_llhttp_result_cb callback,
    void *user
);
llhttp_errno_t rl_llhttp_adapter_feed(
    rl_llhttp_adapter_t *adapter,
    const char *data,
    size_t length
);
void rl_llhttp_adapter_dispose(rl_llhttp_adapter_t *adapter);

static rl_llhttp_adapter_t *adapter_from_parser(llhttp_t *parser) {
    return parser->data;
}

static int on_message_begin(llhttp_t *parser) {
    rl_llhttp_adapter_t *adapter = adapter_from_parser(parser);
    if (adapter->request.active) {
        /* A nonzero callback return makes llhttp_execute() return HPE_USER. */
        return -1;
    }
    adapter->url_length = 0;
    adapter->url[0] = '\0';
    adapter->last_client_status = RCLIENT_OK;
    return 0;
}

static int on_url(llhttp_t *parser, const char *data, size_t length) {
    rl_llhttp_adapter_t *adapter = adapter_from_parser(parser);
    if (length > sizeof(adapter->url) - adapter->url_length - 1) {
        return -1; /* Reject instead of silently truncating a bucket key. */
    }
    memcpy(adapter->url + adapter->url_length, data, length);
    adapter->url_length += length;
    adapter->url[adapter->url_length] = '\0';
    return 0;
}

static void on_rate_limit(void *user, int status, bool allowed) {
    rl_llhttp_adapter_t *adapter = user;
    adapter->last_client_status = status;
    adapter->callback(adapter->user, status, allowed);
}

static int on_message_complete(llhttp_t *parser) {
    rl_llhttp_adapter_t *adapter = adapter_from_parser(parser);
    const char *method = llhttp_method_name((llhttp_method_t)parser->method);
    if (!method || adapter->url_length == 0) {
        return -1;
    }

    /* Query values often contain user input or secrets and should not create
     * unbounded bucket cardinality. */
    size_t path_length = strcspn(adapter->url, "?");
    char bucket[RL_LLHTTP_BUCKET_CAPACITY];
    int length = snprintf(bucket, sizeof(bucket),
        "llhttp:%s:%.*s", method, (int)path_length, adapter->url);
    if (length < 0 || (size_t)length >= sizeof(bucket)) {
        return -1;
    }

    adapter->last_client_status = rl_example_check(
        adapter->client,
        &adapter->request,
        bucket,
        on_rate_limit,
        adapter
    );
    return adapter->last_client_status == RCLIENT_OK ? 0 : -1;
}

/* Initialize one adapter per HTTP connection.  The client may be shared only
 * when the host serializes all access on its event-loop thread. */
void rl_llhttp_adapter_init(
    rl_llhttp_adapter_t *adapter,
    rl_example_client_t *client,
    rl_llhttp_result_cb callback,
    void *user
) {
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

/* Cancel before destroying a connection-owned adapter. */
void rl_llhttp_adapter_dispose(rl_llhttp_adapter_t *adapter) {
    if (adapter->request.active) {
        rl_example_request_cancel(adapter->client, &adapter->request);
    }
}
