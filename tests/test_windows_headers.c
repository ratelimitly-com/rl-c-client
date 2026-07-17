#include "r_client.h"

int main(void) {
    r_addr_t address = {0};
    address.len = (r_socklen_t)sizeof(address.sa);
    return address.len == 0;
}
