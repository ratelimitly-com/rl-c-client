/* Generated from docs/spec/api_key_v2_test_vectors.json. Do not edit. */
#ifndef R_API_KEY_V2_TEST_VECTORS_H
#define R_API_KEY_V2_TEST_VECTORS_H

#include <stdbool.h>

typedef struct {
    const char *name;
    const char *encoded;
    r_auth_type_t auth_type;
    uint64_t key_id;
    uint8_t secret[32];
    uint32_t rate_buckets_max;
    uint32_t latency_services_max;
    uint32_t metrics_labels_max;
    uint32_t latency_buffer_size_max;
    uint32_t dedup_ttl_ms_max;
    uint64_t rate_window_size_ms_max;
    bool rate_window_unlimited;
} r_api_key_v2_valid_vector_t;

typedef struct {
    const char *name;
    const char *encoded;
} r_api_key_v2_invalid_vector_t;

static const r_api_key_v2_valid_vector_t R_API_KEY_V2_VALID_VECTORS[] = {
    {
        "minimum_cookie",
        "rl-cookie1qgqsqqqqqqqqqqqpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqqqzqghe5qw8",
        R_AUTH_COOKIE, UINT64_C(1),
        {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01},
        UINT32_C(1), UINT32_C(1), UINT32_C(1), UINT32_C(1), UINT32_C(10), UINT64_C(1000), false
    },
    {
        "free_plan_aes",
        "rl-aes1qgpqqqqqqqqqqqqzqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqghjmu3sdl8zyx",
        R_AUTH_AES_GCM, UINT64_C(2),
        {0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02},
        UINT32_C(32768), UINT32_C(512), UINT32_C(2048), UINT32_C(16), UINT32_C(300), UINT64_C(60000), false
    },
    {
        "pro_plan_cookie",
        "rl-cookie1qgpsqqqqqqqqqqqrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqdgtrunsagsm94",
        R_AUTH_COOKIE, UINT64_C(3),
        {0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03},
        UINT32_C(65536), UINT32_C(1024), UINT32_C(4096), UINT32_C(32), UINT32_C(300), UINT64_C(3600000), false
    },
    {
        "business_plan_aes",
        "rl-aes1qgzqqqqqqqqqqqqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqjfrnuagec9c0p",
        R_AUTH_AES_GCM, UINT64_C(4),
        {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
        UINT32_C(262144), UINT32_C(4096), UINT32_C(16384), UINT32_C(64), UINT32_C(300), UINT64_C(86400000), false
    },
    {
        "enterprise_plan_cookie",
        "rl-cookie1qgzsqqqqqqqqqqq9q5zs2pg9q5zs2pg9q5zs2pg9q5zs2pg9q5zs2pg9q5zs2pg9qh2vrulcl725px",
        R_AUTH_COOKIE, UINT64_C(5),
        {0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05},
        UINT32_C(1048576), UINT32_C(16384), UINT32_C(65536), UINT32_C(128), UINT32_C(300), UINT64_C(0), true
    },
    {
        "maximum_aes",
        "rl-aes1qtllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllluv073l7r6cv76",
        R_AUTH_AES_GCM, UINT64_C(18446744073709551615),
        {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
        UINT32_C(16777216), UINT32_C(16777216), UINT32_C(2147483648), UINT32_C(32768), UINT32_C(2000), UINT64_C(0), true
    },
    {
        "maximum_finite_window_cookie",
        "rl-cookie1qgrqqqqqqqqqqqqxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqegtruhsezeama",
        R_AUTH_COOKIE, UINT64_C(6),
        {0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06},
        UINT32_C(65536), UINT32_C(1024), UINT32_C(4096), UINT32_C(32), UINT32_C(300), UINT64_C(31536000000), false
    },
};

static const r_api_key_v2_invalid_vector_t R_API_KEY_V2_INVALID_VECTORS[] = {
    {"reserved_rate_exponent_25", "rl-aes1qgysqqqqqqqqqqqfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyvsqzqgwu07su"},
    {"reserved_rate_exponent_31", "rl-aes1qgysqqqqqqqqqqqfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpy0sqzqgugrmzm"},
    {"reserved_latency_exponent_25", "rl-aes1qgysqqqqqqqqqqqfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpysqxzqgdssxdf"},
    {"reserved_latency_exponent_31", "rl-aes1qgysqqqqqqqqqqqfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfp8sqxzqglcu0vh"},
    {"invalid_dedup_code_0", "rl-aes1qgysqqqqqqqqqqqfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyqqqqqgz2lknf"},
    {"invalid_dedup_code_201", "rl-aes1qgysqqqqqqqqqqqfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyqqqjqwngn9ns"},
    {"invalid_dedup_code_255", "rl-aes1qgysqqqqqqqqqqqfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyqqp7q082xcdt"},
    {"invalid_window_code_0", "rl-aes1qgysqqqqqqqqqqqfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyqqqzqqc6fakd"},
    {"format_version_0", "rl-aes1qqpqqqqqqqqqqqqzqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqghjmuhccp9vn9"},
    {"format_version_1", "rl-aes1qypqqqqqqqqqqqqzqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqghjmuhcc2h27q"},
    {"format_version_3", "rl-aes1qvpqqqqqqqqqqqqzqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqghjmuhccu6xy2"},
    {"secret_payload_too_short", "rl-aes1qgpqqqqqqqqqqqqzqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqghjmusp2454x"},
    {"secret_payload_too_long", "rl-aes1qgpqqqqqqqqqqqqzqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqghjmuhcqqennh24"},
    {"legacy_unversioned_aes", "rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqemrljn"},
};

#endif
