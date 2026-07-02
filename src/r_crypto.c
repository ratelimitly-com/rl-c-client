#include "r_crypto.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

static const uint32_t k_bech32_gen[5] = {
    0x3B6A57B2u, 0x26508E6Du, 0x1EA119FAu, 0x3D4233DDu, 0x2A1462B3u
};

static int r_bech32_char_value(char c, uint8_t *out) {
    if (!out) {
        return -1;
    }
    switch (c) {
        case 'q': *out = 0; return 0;
        case 'p': *out = 1; return 0;
        case 'z': *out = 2; return 0;
        case 'r': *out = 3; return 0;
        case 'y': *out = 4; return 0;
        case '9': *out = 5; return 0;
        case 'x': *out = 6; return 0;
        case '8': *out = 7; return 0;
        case 'g': *out = 8; return 0;
        case 'f': *out = 9; return 0;
        case '2': *out = 10; return 0;
        case 't': *out = 11; return 0;
        case 'v': *out = 12; return 0;
        case 'd': *out = 13; return 0;
        case 'w': *out = 14; return 0;
        case '0': *out = 15; return 0;
        case 's': *out = 16; return 0;
        case '3': *out = 17; return 0;
        case 'j': *out = 18; return 0;
        case 'n': *out = 19; return 0;
        case '5': *out = 20; return 0;
        case '4': *out = 21; return 0;
        case 'k': *out = 22; return 0;
        case 'h': *out = 23; return 0;
        case 'c': *out = 24; return 0;
        case 'e': *out = 25; return 0;
        case '6': *out = 26; return 0;
        case 'm': *out = 27; return 0;
        case 'u': *out = 28; return 0;
        case 'a': *out = 29; return 0;
        case '7': *out = 30; return 0;
        case 'l': *out = 31; return 0;
        default: return -1;
    }
}

static uint32_t r_bech32_polymod(const uint8_t *values, size_t len) {
    uint32_t chk = 1;
    for (size_t i = 0; i < len; i++) {
        uint32_t v = values[i];
        uint32_t b = chk >> 25;
        chk = ((chk & 0x1FFFFFFu) << 5) ^ v;
        for (size_t j = 0; j < 5; j++) {
            if (((b >> j) & 1u) != 0) {
                chk ^= k_bech32_gen[j];
            }
        }
    }
    return chk;
}

static int r_bech32_verify_checksum(const char *hrp, const uint8_t *data, size_t data_len) {
    if (!hrp || !data) {
        return 0;
    }
    size_t hrp_len = strlen(hrp);
    size_t values_len = hrp_len * 2u + 1u + data_len;
    uint8_t *values = (uint8_t *)malloc(values_len);
    if (!values) {
        return 0;
    }
    size_t pos = 0;
    for (size_t i = 0; i < hrp_len; i++) {
        values[pos++] = (uint8_t)(((unsigned char)hrp[i]) >> 5);
    }
    values[pos++] = 0;
    for (size_t i = 0; i < hrp_len; i++) {
        values[pos++] = (uint8_t)(((unsigned char)hrp[i]) & 31u);
    }
    memcpy(values + pos, data, data_len);
    int ok = (r_bech32_polymod(values, values_len) == 1u) ? 1 : 0;
    free(values);
    return ok;
}

static int r_convert_bits_5_to_8_no_pad(
    const uint8_t *in,
    size_t in_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len
) {
    if (!in || !out || !out_len) {
        return -1;
    }
    uint32_t acc = 0;
    unsigned bits = 0;
    size_t pos = 0;
    for (size_t i = 0; i < in_len; i++) {
        uint8_t value = in[i];
        if ((value >> 5) != 0) {
            return -1;
        }
        acc = ((acc << 5) | (uint32_t)value) & 0x0FFFu;
        bits += 5;
        while (bits >= 8) {
            bits -= 8;
            if (pos >= out_cap) {
                return -1;
            }
            out[pos++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }
    if (bits >= 5) {
        return -1;
    }
    if (((acc << (8u - bits)) & 0xFFu) != 0u) {
        return -1;
    }
    *out_len = pos;
    return 0;
}

static uint64_t r_read_le64_local(const uint8_t *p) {
    return ((uint64_t)p[0]) |
           ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static uint32_t r_read_le32_local(const uint8_t *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

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

int r_decode_api_key_bech32(
    const char *encoded,
    r_auth_type_t *out_type,
    uint64_t *out_key_id,
    uint8_t *out_secret,
    size_t out_secret_cap,
    size_t *out_secret_len
) {
    return r_decode_api_key_bech32_with_quotas(
        encoded,
        out_type,
        out_key_id,
        out_secret,
        out_secret_cap,
        out_secret_len,
        NULL
    );
}

int r_decode_api_key_bech32_with_quotas(
    const char *encoded,
    r_auth_type_t *out_type,
    uint64_t *out_key_id,
    uint8_t *out_secret,
    size_t out_secret_cap,
    size_t *out_secret_len,
    r_bech32_quotas_t *out_quotas
) {
    if (!encoded || !out_type || !out_key_id || !out_secret || !out_secret_len) {
        return -1;
    }

    size_t enc_len = strlen(encoded);
    if (enc_len == 0) {
        return -1;
    }

    int has_lower = 0;
    int has_upper = 0;
    for (size_t i = 0; i < enc_len; i++) {
        unsigned char c = (unsigned char)encoded[i];
        if (c < 33 || c > 126) {
            return -1;
        }
        if (islower(c)) has_lower = 1;
        if (isupper(c)) has_upper = 1;
    }
    if (has_lower && has_upper) {
        return -1;
    }

    char *s = (char *)malloc(enc_len + 1);
    if (!s) {
        return -1;
    }
    for (size_t i = 0; i < enc_len; i++) {
        s[i] = (char)tolower((unsigned char)encoded[i]);
    }
    s[enc_len] = '\0';

    char *sep = strrchr(s, '1');
    if (!sep) {
        free(s);
        return -1;
    }
    size_t hrp_len = (size_t)(sep - s);
    if (hrp_len < 1) {
        free(s);
        return -1;
    }
    size_t data_chars_len = enc_len - hrp_len - 1u;
    if (data_chars_len < 6u) {
        free(s);
        return -1;
    }
    // Split in-place so checksum and HRP comparisons see only the hrp prefix.
    sep[0] = '\0';

    uint8_t *data = (uint8_t *)malloc(data_chars_len);
    if (!data) {
        free(s);
        return -1;
    }
    for (size_t i = 0; i < data_chars_len; i++) {
        if (r_bech32_char_value(sep[1 + i], &data[i]) != 0) {
            free(data);
            free(s);
            return -1;
        }
    }
    if (!r_bech32_verify_checksum(s, data, data_chars_len)) {
        free(data);
        free(s);
        return -1;
    }

    size_t payload5_len = data_chars_len - 6u;
    size_t payload_cap = (payload5_len * 5u) / 8u + 2u;
    uint8_t *payload = (uint8_t *)malloc(payload_cap);
    if (!payload) {
        free(data);
        free(s);
        return -1;
    }
    size_t payload_len = 0;
    if (r_convert_bits_5_to_8_no_pad(data, payload5_len, payload, payload_cap, &payload_len) != 0) {
        free(payload);
        free(data);
        free(s);
        return -1;
    }

    r_auth_type_t auth_type;
    if (strcmp(s, "rl-cookie") == 0) {
        auth_type = R_AUTH_COOKIE;
    } else if (strcmp(s, "rl-aes") == 0) {
        auth_type = R_AUTH_AES_GCM;
    } else {
        free(payload);
        free(data);
        free(s);
        return -1;
    }

    uint64_t key_id = 0;
    size_t secret_len = 0;

    if (payload_len != 60u) {
        free(payload);
        free(data);
        free(s);
        return -1;
    }
    key_id = r_read_le64_local(payload);
    secret_len = 32u;
    if (secret_len > out_secret_cap) {
        free(payload);
        free(data);
        free(s);
        return -1;
    }
    memcpy(out_secret, payload + 8, secret_len);

    *out_type = auth_type;
    *out_key_id = key_id;
    *out_secret_len = secret_len;
    if (out_quotas) {
        size_t quota_offset = 40u;
        out_quotas->rate_buckets_max = r_read_le32_local(payload + quota_offset);
        out_quotas->latency_services_max = r_read_le32_local(payload + quota_offset + 4u);
        out_quotas->metrics_labels_max = r_read_le32_local(payload + quota_offset + 8u);
        out_quotas->latency_buffer_size_max = r_read_le32_local(payload + quota_offset + 12u);
        out_quotas->dedup_ttl_ms_max = r_read_le32_local(payload + quota_offset + 16u);
    }

    free(payload);
    free(data);
    free(s);
    return 0;
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
