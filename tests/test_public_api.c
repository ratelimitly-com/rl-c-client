#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../include/r_client.h"

static const char *SAMPLE_COOKIE_KEY_TENANT_2 =
    "rl-cookie1qgqqqqqqqqqqqqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqfn54mv";
static const char *SAMPLE_AES_KEY_TENANT_3 =
    "rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqhmzd8l";

static void assert_default_quotas(const r_auth_key_info_t *info) {
    assert(info->rate_buckets_max == 65536u);
    assert(info->latency_services_max == 1024u);
    assert(info->metrics_labels_max == 4096u);
    assert(info->latency_buffer_size_max == 64u);
    assert(info->dedup_ttl_ms_max == 300u);
}

static void test_parse_cookie_key(void) {
    r_auth_key_info_t info;
    memset(&info, 0, sizeof(info));

    int rc = r_client_parse_auth_key(SAMPLE_COOKIE_KEY_TENANT_2, &info);
    assert(rc == RCLIENT_OK);
    assert(info.type == R_AUTH_COOKIE);
    assert(info.key_id == 2u);
    assert(info.secret_len == 32u);
    for (size_t i = 0; i < info.secret_len; i++) {
        assert(info.secret[i] == 2u);
    }
    assert_default_quotas(&info);
}

static void test_parse_aes_key(void) {
    r_auth_key_info_t info;
    memset(&info, 0, sizeof(info));

    int rc = r_client_parse_auth_key(SAMPLE_AES_KEY_TENANT_3, &info);
    assert(rc == RCLIENT_OK);
    assert(info.type == R_AUTH_AES_GCM);
    assert(info.key_id == 3u);
    assert(info.secret_len == 32u);
    for (size_t i = 0; i < info.secret_len; i++) {
        assert(info.secret[i] == 3u);
    }
    assert_default_quotas(&info);
}

static void test_reject_invalid_key(void) {
    r_auth_key_info_t info;
    memset(&info, 0xff, sizeof(info));
    assert(r_client_parse_auth_key("rl-aes1not-valid", &info) == RCLIENT_ERR_CONFIG);
    assert(info.key_id == 0u);
    assert(info.secret_len == 0u);
    assert(r_client_parse_auth_key(NULL, &info) == RCLIENT_ERR_CONFIG);
    assert(r_client_parse_auth_key(SAMPLE_AES_KEY_TENANT_3, NULL) == RCLIENT_ERR_CONFIG);
}

static void test_hash_id_known_vector(void) {
    uint8_t id[16];
    memset(id, 0, sizeof(id));
    r_client_hash_id("api_calls", id);

    const uint8_t expected[16] = {
        0x09, 0x50, 0x5f, 0x75, 0x2a, 0xf0, 0x41, 0x5e,
        0xae, 0x22, 0xe0, 0x7b, 0xdf, 0xea, 0x83, 0xab,
    };
    assert(memcmp(id, expected, sizeof(expected)) == 0);
}

static void test_format_default_tenant_dns(void) {
    char dns_name[R_CLIENT_DEFAULT_TENANT_DNS_CAPACITY];
    assert(r_client_format_default_tenant_dns(
        UINT64_C(2213169720275691601),
        dns_name,
        sizeof(dns_name)
    ) == RCLIENT_OK);
    assert(strcmp(
        dns_name,
        "c-2213169720275691601.p0.ratelimitly.com"
    ) == 0);

    assert(r_client_format_default_tenant_dns(
        UINT64_MAX,
        dns_name,
        sizeof(dns_name)
    ) == RCLIENT_OK);
    assert(strcmp(
        dns_name,
        "c-18446744073709551615.p0.ratelimitly.com"
    ) == 0);

    char too_small[4] = "bad";
    assert(r_client_format_default_tenant_dns(
        3u,
        too_small,
        sizeof(too_small)
    ) == RCLIENT_ERR_CONFIG);
    assert(too_small[0] == '\0');
    assert(r_client_format_default_tenant_dns(3u, NULL, 0u)
        == RCLIENT_ERR_CONFIG);
}

int main(void) {
    test_parse_cookie_key();
    test_parse_aes_key();
    test_reject_invalid_key();
    test_hash_id_known_vector();
    test_format_default_tenant_dns();
    return 0;
}
