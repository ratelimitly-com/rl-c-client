#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/r_crypto.h"
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

static void test_aes_gcm_aad_rejects_tamper(void) {
    const uint8_t pdu[] = {0x52, 0x54, 0x0c, 0x00, 0x2c, 0x01, 0x00, 0x00};
    uint8_t key[32];
    uint8_t aad_prefix[44];
    uint8_t aad[56];
    uint8_t tampered_aad[56];
    uint8_t cipher[sizeof(pdu)];
    uint8_t out[sizeof(pdu)];
    uint8_t nonce[12];
    uint8_t tag[16];
    size_t cipher_len = 0;
    size_t out_len = 0;

    memset(key, 0x11, sizeof(key));
    memset(aad_prefix, 0x22, sizeof(aad_prefix));

    int rc = r_encrypt_pdu_aes_gcm(
        pdu,
        sizeof(pdu),
        key,
        aad_prefix,
        sizeof(aad_prefix),
        cipher,
        sizeof(cipher),
        &cipher_len,
        nonce,
        tag
    );
    assert(rc == 0);
    assert(cipher_len == sizeof(pdu));

    memcpy(aad, aad_prefix, sizeof(aad_prefix));
    memcpy(aad + sizeof(aad_prefix), nonce, sizeof(nonce));
    rc = r_decrypt_pdu_aes_gcm(
        cipher,
        cipher_len,
        key,
        nonce,
        tag,
        aad,
        sizeof(aad),
        out,
        sizeof(out),
        &out_len
    );
    assert(rc == 0);
    assert(out_len == sizeof(pdu));
    assert(memcmp(out, pdu, sizeof(pdu)) == 0);

    memcpy(tampered_aad, aad, sizeof(aad));
    tampered_aad[4] ^= 0x01;
    rc = r_decrypt_pdu_aes_gcm(
        cipher,
        cipher_len,
        key,
        nonce,
        tag,
        tampered_aad,
        sizeof(tampered_aad),
        out,
        sizeof(out),
        &out_len
    );
    assert(rc != 0);
}

int main(void) {
    test_metrics_label_tlv();
    test_aes_gcm_aad_rejects_tamper();
    return 0;
}
