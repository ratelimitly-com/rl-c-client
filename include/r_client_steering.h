#ifndef R_CLIENT_STEERING_H
#define R_CLIENT_STEERING_H

#include <stddef.h>
#include <stdint.h>

#include "r_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define R_STEERING_PORT_MIN UINT16_C(49152)
#define R_STEERING_PORT_COUNT 16384u

typedef enum r_steering_bind_result {
    R_STEERING_BIND_ERROR = -1,
    R_STEERING_BIND_OK = 0,
    R_STEERING_BIND_OCCUPIED = 1,
} r_steering_bind_result_t;

typedef r_steering_bind_result_t (*r_steering_try_bind_fn)(
    void *user,
    uint16_t port
);

/* Advance once through the IANA dynamic/private port range. */
RCLIENT_API uint16_t r_client_next_steering_port(uint16_t port);

/*
 * Try every dynamic port in monotonic order, beginning with first_port.
 * The callback retains any socket or transport resource created when it
 * returns R_STEERING_BIND_OK. Occupied candidates are skipped; any other
 * callback error stops the scan. No port-zero fallback is performed.
 */
RCLIENT_API int r_client_select_steering_port(
    uint16_t first_port,
    r_steering_try_bind_fn try_bind,
    void *user,
    uint16_t *out_selected_port,
    uint16_t *out_next_port
);

#ifdef __cplusplus
}
#endif

#endif
