#include "../include/r_client_steering.h"

#include <limits.h>

uint16_t r_client_next_steering_port(uint16_t port) {
    if (port < R_STEERING_PORT_MIN || port == UINT16_MAX) {
        return R_STEERING_PORT_MIN;
    }
    return (uint16_t)(port + 1u);
}

int r_client_select_steering_port(
    uint16_t first_port,
    r_steering_try_bind_fn try_bind,
    void *user,
    uint16_t *out_selected_port,
    uint16_t *out_next_port
) {
    if (!try_bind || !out_selected_port || !out_next_port) {
        return RCLIENT_ERR_CONFIG;
    }

    uint16_t candidate = first_port < R_STEERING_PORT_MIN
        ? R_STEERING_PORT_MIN
        : first_port;
    for (size_t i = 0u; i < R_STEERING_PORT_COUNT; i++) {
        r_steering_bind_result_t result = try_bind(user, candidate);
        if (result == R_STEERING_BIND_OK) {
            *out_selected_port = candidate;
            *out_next_port = r_client_next_steering_port(candidate);
            return RCLIENT_OK;
        }
        if (result != R_STEERING_BIND_OCCUPIED) {
            return RCLIENT_ERR_IO;
        }
        candidate = r_client_next_steering_port(candidate);
    }
    return RCLIENT_ERR_IO;
}
