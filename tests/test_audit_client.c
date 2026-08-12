#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/r_crypto.h"

static void test_management_key_decoder(void) {
    static const char encoded[] =
        "rl-secret1e6wxp6r89gyxwt2nnp7xv4hww8zk4afh7ju9vhdg6q647dpwjzus7ca4ms";
    static const uint8_t expected[32] = {
        0xce, 0x9c, 0x60, 0xe8, 0x67, 0x2a, 0x08, 0x67,
        0x2d, 0x53, 0x98, 0x7c, 0x66, 0x56, 0xee, 0x71,
        0xc5, 0x6a, 0xf5, 0x37, 0xf4, 0xb8, 0x56, 0x5d,
        0xa8, 0xd0, 0x35, 0x5f, 0x34, 0x2e, 0x90, 0xb9,
    };
    uint8_t decoded[32];
    memset(decoded, 0, sizeof(decoded));

    assert(r_decode_management_key_bech32(encoded, decoded) == 0);
    assert(memcmp(decoded, expected, sizeof(expected)) == 0);
    assert(r_decode_management_key_bech32("rl-secret1invalid", decoded) != 0);
    assert(r_decode_management_key_bech32(
        "RL-secret1e6wxp6r89gyxwt2nnp7xv4hww8zk4afh7ju9vhdg6q647dpwjzus7ca4ms",
        decoded
    ) != 0);
}

int main(void) {
    test_management_key_decoder();
    return 0;
}
