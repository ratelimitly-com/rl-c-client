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
} r_runtime_options_t;

/*
 * A small, portable runtime for examples and command-line programs.
 *
 * It owns nonblocking IPv4/IPv6 UDP sockets and performs synchronous DNS.
 * Applications with an asynchronous resolver should instead provide their
 * own r_io_ops_t and r_resolver_ops_t to r_client_create().
 */
typedef struct r_runtime_client {
    r_client_t *handle;
    r_runtime_socket_t sockets[2];
    size_t socket_count;
    char server_host[256];
    uint16_t server_port;
    bool network_started;
} r_runtime_client_t;

const char *r_runtime_status_name(int status);

/* Read the required key plus optional DNS/fixed-endpoint overrides. */
int r_runtime_options_from_env(r_runtime_options_t *out_options);

int r_runtime_client_init(
    r_runtime_client_t *runtime,
    const r_runtime_options_t *options
);

void r_runtime_client_destroy(r_runtime_client_t *runtime);

/* Returned sockets remain owned by runtime. */
size_t r_runtime_socket_count(const r_runtime_client_t *runtime);
r_runtime_socket_t r_runtime_socket_at(
    const r_runtime_client_t *runtime,
    size_t index
);

/* Drain one ready socket and deliver all complete datagrams to the client. */
int r_runtime_client_on_readable(
    r_runtime_client_t *runtime,
    r_runtime_socket_t socket_value
);

/* Unix-epoch time for client deadlines; do not use it to measure durations. */
uint64_t r_runtime_wall_time_ms(void);

/* Monotonic time for measuring protected-operation latency. */
int r_runtime_monotonic_time_ms(uint64_t *out_milliseconds);

/* Convert a workflow's absolute client deadline into a relative loop delay. */
int r_runtime_admission_delay_ms(
    const r_admission_request_t *request,
    uint64_t *out_delay_ms
);

int r_runtime_admission_on_timeout(
    r_runtime_client_t *runtime,
    r_admission_request_t *request
);

void r_runtime_admission_cancel(
    r_runtime_client_t *runtime,
    r_admission_request_t *request
);

typedef int (*r_runtime_protected_work_cb)(void *user);

/*
 * Run admitted work, measure it monotonically, and report one sample.
 * Denied/cancelled requests and failed work never emit a latency report.
 */
int r_runtime_admission_run_and_report(
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
