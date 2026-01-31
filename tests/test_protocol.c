#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/r_protocol.h"

static void test_metrics_label_tlv(void) {
    uint8_t buf[64];
    size_t len = 0;
    int rc = r_append_metrics_label_tlv(buf, sizeof(buf), &len, "api", 0);
    assert(rc == RCLIENT_OK);
    const uint8_t expected[] = {
        0x4D, 0x4C, // TLV type (LE)
        0x0C, 0x00, // TLV size = 12
        0x03, 0x00, // label length = 3
        'a', 'p', 'i',
        0x00, 0x00, 0x00,
    };
    assert(len == sizeof(expected));
    assert(memcmp(buf, expected, sizeof(expected)) == 0);
}

int main(void) {
    test_metrics_label_tlv();
    return 0;
}
