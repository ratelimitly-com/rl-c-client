#ifndef R_CRYPTO_H
#define R_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#include "../include/r_client.h"

#ifdef __cplusplus
extern "C" {
#endif

int r_hash_id_blake2s_128(const char *input, uint8_t out_id[16]);
int r_sha256_cookie(const char *secret, size_t secret_len, uint8_t out[32]);
int r_derive_aes_key(const char *secret, size_t secret_len, uint8_t out_key[32]);
int r_decode_api_key_bech32(
    const char *encoded,
    r_auth_type_t *out_type,
    uint64_t *out_key_id,
    uint8_t *out_secret,
    size_t out_secret_cap,
    size_t *out_secret_len
);

int r_encrypt_pdu_aes_gcm(
    const uint8_t *pdu,
    size_t pdu_len,
    const uint8_t key[32],
    uint8_t *cipher,
    size_t cipher_cap,
    size_t *cipher_len,
    uint8_t nonce[12],
    uint8_t tag[16]
);

int r_decrypt_pdu_aes_gcm(
    const uint8_t *cipher,
    size_t cipher_len,
    const uint8_t key[32],
    const uint8_t nonce[12],
    const uint8_t tag[16],
    uint8_t *out,
    size_t out_cap,
    size_t *out_len
);

#ifdef __cplusplus
}
#endif

#endif
