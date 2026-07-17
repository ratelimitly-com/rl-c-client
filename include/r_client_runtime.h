#ifndef R_CLIENT_RUNTIME_H
#define R_CLIENT_RUNTIME_H

#include "r_client.h"

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

/* Read credentials and an optional fixed development endpoint. */
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

#ifdef __cplusplus
}
#endif

#endif
