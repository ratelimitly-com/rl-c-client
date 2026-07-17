#ifndef RL_LLHTTP_ADAPTER_H
#define RL_LLHTTP_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <llhttp.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RL_LLHTTP_URL_CAPACITY = 512,
    RL_LLHTTP_METHOD_CAPACITY = 32,
    RL_LLHTTP_BUCKET_CAPACITY = 640,
};

typedef int (*rl_llhttp_protected_work_cb)(
    void *user,
    const char *method,
    const char *path
);

typedef void (*rl_llhttp_result_cb)(
    void *user,
    int status,
    const r_admission_outcome_t *outcome,
    uint32_t observed_latency_ms
);

/*
 * Allocate one adapter per HTTP connection. The host owns this structure and
 * must keep it alive through rl_llhttp_adapter_dispose(). Fields are visible so
 * the adapter can be embedded without another allocation; treat them as private.
 */
typedef struct rl_llhttp_adapter {
    llhttp_t parser;
    llhttp_settings_t settings;
    r_runtime_client_t *runtime;
    r_admission_request_t admission;
    rl_llhttp_protected_work_cb protected_work;
    rl_llhttp_result_cb result;
    void *user;
    char url[RL_LLHTTP_URL_CAPACITY];
    char path[RL_LLHTTP_URL_CAPACITY];
    char method[RL_LLHTTP_METHOD_CAPACITY];
    char bucket[RL_LLHTTP_BUCKET_CAPACITY];
    size_t url_length;
    int last_client_status;
} rl_llhttp_adapter_t;

/* Configure one HTTP request parser. runtime and callbacks must outlive it. */
int rl_llhttp_adapter_init(
    rl_llhttp_adapter_t *adapter,
    r_runtime_client_t *runtime,
    rl_llhttp_protected_work_cb protected_work,
    rl_llhttp_result_cb result,
    void *user
);

/*
 * Feed exactly the bytes received from TCP. If this returns HPE_PAUSED, keep
 * bytes at and after *consumed until the result callback asks the host to run
 * again. The adapter resumes llhttp immediately before invoking that callback.
 */
llhttp_errno_t rl_llhttp_adapter_feed(
    rl_llhttp_adapter_t *adapter,
    const char *data,
    size_t length,
    size_t *consumed
);

/* Notify llhttp of clean TCP EOF after all paused input has been replayed. */
llhttp_errno_t rl_llhttp_adapter_finish(rl_llhttp_adapter_t *adapter);

/* The host loop drives this request's UDP readability and absolute deadline. */
r_admission_request_t *rl_llhttp_adapter_pending(
    rl_llhttp_adapter_t *adapter
);

/* Distinguish a parser failure from an rl-c-client submission failure. */
int rl_llhttp_adapter_last_client_status(
    const rl_llhttp_adapter_t *adapter
);

/* Cancel an active check before releasing connection-owned parser state. */
void rl_llhttp_adapter_dispose(rl_llhttp_adapter_t *adapter);

#ifdef __cplusplus
}
#endif

#endif
