#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../include/r_client.h"
#include "fixtures/api_key_v1_test_vectors.h"

static void test_parse_api_key_v1_vectors(void) {
    for (size_t i = 0;
         i < sizeof(R_API_KEY_V1_VALID_VECTORS) /
                 sizeof(R_API_KEY_V1_VALID_VECTORS[0]);
         i++) {
        const r_api_key_v1_valid_vector_t *vector =
            &R_API_KEY_V1_VALID_VECTORS[i];
        r_auth_key_info_t info;
        memset(&info, 0, sizeof(info));

        assert(r_client_parse_auth_key(vector->encoded, &info) == RCLIENT_OK);
        assert(info.format_version == 1u);
        assert(info.type == vector->auth_type);
        assert(info.key_id == vector->key_id);
        assert(info.secret_len == sizeof(vector->secret));
        assert(memcmp(info.secret, vector->secret, sizeof(vector->secret)) == 0);
        assert(info.rate_buckets_max == vector->rate_buckets_max);
        assert(info.latency_services_max == vector->latency_services_max);
        assert(info.metrics_labels_max == vector->metrics_labels_max);
        assert(info.latency_buffer_size_max == vector->latency_buffer_size_max);
        assert(info.dedup_ttl_ms_max == vector->dedup_ttl_ms_max);
        assert(info.rate_window_size_ms_max == vector->rate_window_size_ms_max);
    }

    for (size_t i = 0;
         i < sizeof(R_API_KEY_V1_INVALID_VECTORS) /
                 sizeof(R_API_KEY_V1_INVALID_VECTORS[0]);
         i++) {
        r_auth_key_info_t info;
        memset(&info, 0xff, sizeof(info));
        assert(r_client_parse_auth_key(
            R_API_KEY_V1_INVALID_VECTORS[i].encoded,
            &info
        ) == RCLIENT_ERR_CONFIG);
        assert(info.key_id == 0u);
        assert(info.secret_len == 0u);
    }
}

static void test_reject_invalid_key(void) {
    r_auth_key_info_t info;
    memset(&info, 0xff, sizeof(info));
    assert(r_client_parse_auth_key("rl-aes1not-valid", &info) == RCLIENT_ERR_CONFIG);
    assert(info.key_id == 0u);
    assert(info.secret_len == 0u);
    assert(r_client_parse_auth_key(NULL, &info) == RCLIENT_ERR_CONFIG);
    assert(r_client_parse_auth_key(R_API_KEY_V1_VALID_VECTORS[0].encoded, NULL)
        == RCLIENT_ERR_CONFIG);
}

static void test_canonical_id_known_vectors(void) {
    uint8_t id[16];
    memset(id, 0, sizeof(id));
    assert(r_client_derive_bucket_id(
        "checkout",
        strlen("checkout"),
        1000u,
        100u,
        id
    ) == RCLIENT_OK);

    const uint8_t expected_bucket[16] = {
        0xf5, 0xcf, 0x3a, 0xd8, 0xb8, 0x40, 0x68, 0x54,
        0xb5, 0x96, 0xba, 0x36, 0x14, 0xf1, 0x6e, 0xff,
    };
    assert(memcmp(id, expected_bucket, sizeof(expected_bucket)) == 0);

    assert(r_client_derive_latency_tracker_id(
        "inventory-backend",
        strlen("inventory-backend"),
        10000u,
        100u,
        32u,
        5u,
        id
    ) == RCLIENT_OK);
    const uint8_t expected_tracker[16] = {
        0x03, 0x20, 0xbf, 0x15, 0xb8, 0x84, 0xbd, 0xa3,
        0x67, 0xa1, 0x7e, 0x5f, 0xfb, 0x65, 0x04, 0x41,
    };
    assert(memcmp(id, expected_tracker, sizeof(expected_tracker)) == 0);

    const uint8_t binary_name[] = {
        'b', 'i', 'n', 'a', 'r', 'y', '\0', 't', 'r', 'a', 'c', 'k', 'e', 'r',
    };
    assert(r_client_derive_latency_tracker_id(
        binary_name,
        sizeof(binary_name),
        UINT32_MAX,
        UINT32_MAX,
        UINT32_MAX,
        UINT32_MAX,
        id
    ) == RCLIENT_OK);
    const uint8_t expected_binary_tracker[16] = {
        0x06, 0x96, 0xca, 0x52, 0xa5, 0xbf, 0xc5, 0xe9,
        0xc4, 0x6b, 0xa9, 0x0f, 0x31, 0x10, 0xb7, 0x28,
    };
    assert(memcmp(
        id,
        expected_binary_tracker,
        sizeof(expected_binary_tracker)
    ) == 0);
}

static void test_default_request_policy(void) {
    r_request_policy_t policy;
    memset(&policy, 0xff, sizeof(policy));
    r_client_default_request_policy(&policy);

    assert(policy.unit_ms == 20u);
    assert(policy.replay_count == 1u);
    assert(policy.replay_gap.kind == R_HA_SCHEDULE_FIXED);
    assert(policy.replay_gap.initial_units == 1u);
    assert(policy.replay_gap.max_units == 1u);
    assert(policy.final_receive_units == 1u);
    assert(policy.completion_delivery);
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
    test_parse_api_key_v1_vectors();
    test_reject_invalid_key();
    test_canonical_id_known_vectors();
    test_default_request_policy();
    test_format_default_tenant_dns();
    return 0;
}
