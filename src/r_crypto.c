#include "r_crypto.h"

#include <string.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#define R_PBKDF2_ITERS 100000
static const char k_ratelimitly_salt[] = "ratelimitly_salt";

int r_hash_id_blake2s_128(const char *input, uint8_t out_id[16]) {
    if (!input || !out_id) {
        return -1;
    }
    const EVP_MD *md = EVP_blake2s256();
    if (!md) {
        return -1;
    }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return -1;
    }
    int ok = 0;
    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    if (EVP_DigestInit_ex(ctx, md, NULL) != 1) {
        goto cleanup;
    }
    if (EVP_DigestUpdate(ctx, input, strlen(input)) != 1) {
        goto cleanup;
    }
    if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        goto cleanup;
    }
    if (digest_len < 16) {
        goto cleanup;
    }
    memcpy(out_id, digest, 16);
    ok = 1;

cleanup:
    EVP_MD_CTX_free(ctx);
    return ok ? 0 : -1;
}

int r_sha256_cookie(const char *secret, size_t secret_len, uint8_t out[32]) {
    if (!secret || !out) {
        return -1;
    }
    if (secret_len == 0) {
        secret_len = strlen(secret);
    }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return -1;
    }
    int ok = 0;
    unsigned int digest_len = 0;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        goto cleanup;
    }
    if (EVP_DigestUpdate(ctx, secret, secret_len) != 1) {
        goto cleanup;
    }
    if (EVP_DigestFinal_ex(ctx, out, &digest_len) != 1) {
        goto cleanup;
    }
    if (digest_len != 32) {
        goto cleanup;
    }
    ok = 1;

cleanup:
    EVP_MD_CTX_free(ctx);
    return ok ? 0 : -1;
}

int r_derive_aes_key(const char *secret, size_t secret_len, uint8_t out_key[32]) {
    if (!secret || !out_key) {
        return -1;
    }
    if (secret_len == 0) {
        secret_len = strlen(secret);
    }
    int ok = PKCS5_PBKDF2_HMAC(
        secret,
        (int)secret_len,
        (const unsigned char *)k_ratelimitly_salt,
        (int)strlen(k_ratelimitly_salt),
        R_PBKDF2_ITERS,
        EVP_sha256(),
        32,
        out_key
    );
    return ok == 1 ? 0 : -1;
}

int r_encrypt_pdu_aes_gcm(
    const uint8_t *pdu,
    size_t pdu_len,
    const uint8_t key[32],
    uint8_t *cipher,
    size_t cipher_cap,
    size_t *cipher_len,
    uint8_t nonce[12],
    uint8_t tag[16]
) {
    if (!pdu || !key || !cipher || !cipher_len || !nonce || !tag) {
        return -1;
    }
    if (cipher_cap < pdu_len) {
        return -1;
    }
    if (RAND_bytes(nonce, 12) != 1) {
        return -1;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return -1;
    }

    int len = 0;
    int out_len = 0;
    int ok = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
        goto cleanup;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) {
        goto cleanup;
    }
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) {
        goto cleanup;
    }
    if (EVP_EncryptUpdate(ctx, cipher, &len, pdu, (int)pdu_len) != 1) {
        goto cleanup;
    }
    out_len = len;
    if (EVP_EncryptFinal_ex(ctx, cipher + out_len, &len) != 1) {
        goto cleanup;
    }
    out_len += len;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) {
        goto cleanup;
    }

    *cipher_len = (size_t)out_len;
    ok = 1;

cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return ok ? 0 : -1;
}

int r_decrypt_pdu_aes_gcm(
    const uint8_t *cipher,
    size_t cipher_len,
    const uint8_t key[32],
    const uint8_t nonce[12],
    const uint8_t tag[16],
    uint8_t *out,
    size_t out_cap,
    size_t *out_len
) {
    if (!cipher || !key || !nonce || !tag || !out || !out_len) {
        return -1;
    }
    if (out_cap < cipher_len) {
        return -1;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return -1;
    }

    int len = 0;
    int out_total = 0;
    int ok = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
        goto cleanup;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) {
        goto cleanup;
    }
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) {
        goto cleanup;
    }
    if (EVP_DecryptUpdate(ctx, out, &len, cipher, (int)cipher_len) != 1) {
        goto cleanup;
    }
    out_total = len;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) != 1) {
        goto cleanup;
    }
    if (EVP_DecryptFinal_ex(ctx, out + out_total, &len) != 1) {
        goto cleanup;
    }
    out_total += len;
    *out_len = (size_t)out_total;
    ok = 1;

cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return ok ? 0 : -1;
}
