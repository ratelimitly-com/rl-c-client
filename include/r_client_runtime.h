#ifndef R_CLIENT_RUNTIME_H
#define R_CLIENT_RUNTIME_H

#include "r_client_workflow.h"

#ifdef _WIN32
typedef SOCKET r_runtime_socket_t;
#define R_RUNTIME_INVALID_SOCKET INVALID_SOCKET
#else
typedef int r_runtime_socket_t;
#define R_RUNTIME_INVALID_SOCKET (-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct r_runtime_options {
    /* Optional override; NULL selects the key-derived production DNS name. */
    const char *tenant_dns_name;
    const char *auth_key;
    const char *server_host;
    uint16_t server_port;
    /* Optional explicit policy, populated by r_runtime_options_from_env(). */
    r_request_policy_t request_policy;
    bool has_request_policy;
    /* Emit one credential-free scheduler profile for every completed request. */
    bool profile_requests;
} r_runtime_options_t;

/*
 * A small, portable runtime for examples and command-line programs.
 *
 * It owns nonblocking IPv4/IPv6 UDP sockets and performs synchronous
 * (blocking) DNS during init, refresh, and any submit that finds no servers.
 * Applications with an asynchronous resolver should instead provide their
 * own r_io_ops_t and r_resolver_ops_t to r_client_create().
 *
 * Like the core client, the runtime contains no locks: confine each runtime
 * to one thread or event loop and serialize all calls. Source-port steering
 * uses monotonic dynamic-port selection after all in-flight requests drain.
 */
typedef struct r_runtime_client {
    r_client_t *handle;
    r_runtime_socket_t sockets[2];
    size_t socket_count;
    char server_host[256];
    uint16_t server_port;
    uint64_t request_unit_ms;
    uint32_t request_replay_count;
    bool network_started;
} r_runtime_client_t;

RCLIENT_API const char *r_runtime_status_name(int status);

/*
 * Read the required key plus optional DNS/fixed-endpoint overrides. The
 * optional RATELIMITLY_REQUEST_UNIT_MS, RATELIMITLY_REQUEST_REPLAY_COUNT, and
 * RATELIMITLY_REQUEST_PROFILE=1 settings configure the example runtime only.
 */
RCLIENT_API int r_runtime_options_from_env(
    r_runtime_options_t *out_options
);

RCLIENT_API int r_runtime_client_init(
    r_runtime_client_t *runtime,
    const r_runtime_options_t *options
);

RCLIENT_API void r_runtime_client_destroy(r_runtime_client_t *runtime);

/* Returned sockets remain owned by runtime. */
RCLIENT_API size_t r_runtime_socket_count(
    const r_runtime_client_t *runtime
);
RCLIENT_API r_runtime_socket_t r_runtime_socket_at(
    const r_runtime_client_t *runtime,
    size_t index
);

/*
 * Drain one ready socket and deliver all valid datagrams to the client.
 * Malformed or unauthenticated datagrams are discarded as packet-local noise.
 */
RCLIENT_API int r_runtime_client_on_readable(
    r_runtime_client_t *runtime,
    r_runtime_socket_t socket_value
);

/* Unix-epoch time for client deadlines; do not use it to measure durations. */
RCLIENT_API uint64_t r_runtime_wall_time_ms(void);

/* Monotonic time for measuring protected-operation latency. */
RCLIENT_API int r_runtime_monotonic_time_ms(uint64_t *out_milliseconds);

/* Convert a workflow's absolute client deadline into a relative loop delay. */
RCLIENT_API int r_runtime_admission_delay_ms(
    const r_admission_request_t *request,
    uint64_t *out_delay_ms
);

RCLIENT_API int r_runtime_admission_on_timeout(
    r_runtime_client_t *runtime,
    r_admission_request_t *request
);

RCLIENT_API void r_runtime_admission_cancel(
    r_runtime_client_t *runtime,
    r_admission_request_t *request
);

typedef int (*r_runtime_protected_work_cb)(void *user);

/*
 * Run admitted work, measure it monotonically, and report one sample.
 * Denied/cancelled requests and failed work never emit a latency report.
 * Protected work is invoked at most once; every later call returns
 * RCLIENT_ERR_CONFIG, including when the first call's report send failed.
 * After a report failure, callers that retained out_observed_latency_ms may
 * retry only r_client_admission_report_latency() with that measured value.
 */
RCLIENT_API int r_runtime_admission_run_and_report(
    r_runtime_client_t *runtime,
    r_admission_request_t *request,
    r_runtime_protected_work_cb protected_work,
    void *user,
    uint32_t *out_observed_latency_ms
);

#ifdef __cplusplus
}
#endif

#endif
