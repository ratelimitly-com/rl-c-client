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

int rl_example_options_from_env(rl_example_options_t *options);

int rl_example_client_init(
    rl_example_client_t *client,
    const rl_example_options_t *options
);

void rl_example_client_destroy(rl_example_client_t *client);

size_t rl_example_socket_count(const rl_example_client_t *client);
int rl_example_socket_at(const rl_example_client_t *client, size_t index);

int rl_example_client_on_readable(rl_example_client_t *client, int socket_fd);

int rl_example_check(
    rl_example_client_t *client,
    rl_example_request_t *request,
    const char *bucket,
    rl_example_result_cb callback,
    void *user
);

int rl_example_request_delay_ms(
    const rl_example_request_t *request,
    uint64_t *delay_ms
);

int rl_example_request_on_timeout(
    rl_example_client_t *client,
    rl_example_request_t *request
);

void rl_example_request_cancel(
    rl_example_client_t *client,
    rl_example_request_t *request
);

#ifdef __cplusplus
}
#endif

#endif
