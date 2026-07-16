#ifndef RL_LLHTTP_ADAPTER_H
#define RL_LLHTTP_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>

#include <llhttp.h>

#include "common/rl_example.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RL_LLHTTP_URL_CAPACITY = 512,
    RL_LLHTTP_BUCKET_CAPACITY = 640,
};

typedef void (*rl_llhttp_result_cb)(
    void *user,
    int status,
    bool allowed
);

/*
 * Allocate one adapter per HTTP connection. The host owns this structure and
 * must keep it alive until rl_llhttp_adapter_dispose(). Fields are exposed only
 * so callers can embed the allocation; treat them as implementation details.
 */
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

/* Configure a request parser. The client must outlive the adapter. */
int rl_llhttp_adapter_init(
    rl_llhttp_adapter_t *adapter,
    rl_example_client_t *client,
    rl_llhttp_result_cb callback,
    void *user
);

/*
 * Feed received bytes. A complete request may return HPE_OK while its check is
 * active. If the same input contains another pipelined request, HPE_PAUSED
 * stops before that message; retain bytes at and after llhttp_get_error_pos().
 * The adapter resumes the parser immediately before invoking the result callback.
 */
llhttp_errno_t rl_llhttp_adapter_feed(
    rl_llhttp_adapter_t *adapter,
    const char *data,
    size_t length
);

/* Notify llhttp that the peer reached EOF. */
llhttp_errno_t rl_llhttp_adapter_finish(rl_llhttp_adapter_t *adapter);

/* Distinguish parser failures from an rl-c-client submission failure. */
int rl_llhttp_adapter_last_client_status(const rl_llhttp_adapter_t *adapter);

/* Cancel an active check before releasing connection-owned adapter state. */
void rl_llhttp_adapter_dispose(rl_llhttp_adapter_t *adapter);

#ifdef __cplusplus
}
#endif

#endif
