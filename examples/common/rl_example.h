#ifndef RL_EXAMPLE_H
#define RL_EXAMPLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "r_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rl_example_options {
    const char *tenant_dns_name;
    const char *auth_key;
    const char *server_host;
    uint16_t server_port;
} rl_example_options_t;

/*
 * Small event-loop-neutral adapter used by every example in this directory.
 *
 * rl-c-client deliberately does not own sockets, DNS, or a clock. Real
 * applications normally connect those hooks to facilities they already use.
 * This adapter supplies a compact POSIX implementation so each example can
 * focus on the integration points that are unique to its event loop.
 */
typedef struct rl_example_client {
    r_client_t *handle;
    int sockets[2];
    size_t socket_count;
    char server_host[256];
    uint16_t server_port;
} rl_example_client_t;

typedef void (*rl_example_result_cb)(void *user, int status, bool allowed);

typedef struct rl_example_request {
    r_client_req_t *handle;
    r_resource_request_t resource;
    rl_example_result_cb callback;
    void *user;
    bool active;
} rl_example_request_t;

/* Read credentials and the optional development endpoint override. */
int rl_example_options_from_env(rl_example_options_t *options);

/* Create the rl-c-client handle and its nonblocking IPv4/IPv6 UDP sockets. */
int rl_example_client_init(
    rl_example_client_t *client,
    const rl_example_options_t *options
);

void rl_example_client_destroy(rl_example_client_t *client);

/* Descriptors returned here remain owned by rl_example_client_t. */
size_t rl_example_socket_count(const rl_example_client_t *client);
int rl_example_socket_at(const rl_example_client_t *client, size_t index);

/* Drain one ready socket and pass every datagram into rl-c-client. */
int rl_example_client_on_readable(rl_example_client_t *client, int socket_fd);

/* Start one asynchronous check. The request must live until its callback. */
int rl_example_check(
    rl_example_client_t *client,
    rl_example_request_t *request,
    const char *bucket,
    rl_example_result_cb callback,
    void *user
);

/* Convert the absolute rl-c-client deadline into an event-loop delay. */
int rl_example_request_delay_ms(
    const rl_example_request_t *request,
    uint64_t *delay_ms
);

/* Advance retry/timeout state after the loop reports the timer as expired. */
int rl_example_request_on_timeout(
    rl_example_client_t *client,
    rl_example_request_t *request
);

/* Cancel before releasing callback state or shutting down the client. */
void rl_example_request_cancel(
    rl_example_client_t *client,
    rl_example_request_t *request
);

#ifdef __cplusplus
}
#endif

#endif
