#include "r_client.h"
#include "r_client_steering.h"

int main(void) {
    r_addr_t address = {0};
    address.len = (r_socklen_t)sizeof(address.sa);
    return address.len == 0
        || r_client_next_steering_port(UINT16_MAX) != R_STEERING_PORT_MIN;
}
