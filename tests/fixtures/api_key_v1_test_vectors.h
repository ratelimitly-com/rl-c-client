/* Generated from docs/spec/api_key_v1_test_vectors.json. Do not edit. */
#ifndef R_API_KEY_V1_TEST_VECTORS_H
#define R_API_KEY_V1_TEST_VECTORS_H

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
    uint32_t rate_window_size_ms_max;
} r_api_key_v1_valid_vector_t;

typedef struct {
    const char *name;
    const char *encoded;
} r_api_key_v1_invalid_vector_t;

static const r_api_key_v1_valid_vector_t R_API_KEY_V1_VALID_VECTORS[] = {
    {
        "minimum_cookie",
        "rl-cookie1qyqsqqqqqqqqqqqpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqqqzqqfs0x84",
        R_AUTH_COOKIE, UINT64_C(1),
        {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01},
        UINT32_C(1), UINT32_C(1), UINT32_C(1), UINT32_C(1), UINT32_C(10), UINT32_C(1)
    },
    {
        "free_plan_aes",
        "rl-aes1qypqqqqqqqqqqqqzqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqghjmu4scddmk3",
        R_AUTH_AES_GCM, UINT64_C(2),
        {0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02},
        UINT32_C(32768), UINT32_C(512), UINT32_C(2048), UINT32_C(16), UINT32_C(300), UINT32_C(4194304)
    },
    {
        "pro_plan_cookie",
        "rl-cookie1qypsqqqqqqqqqqqrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqdgtrukcen35ze",
        R_AUTH_COOKIE, UINT64_C(3),
        {0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03},
        UINT32_C(65536), UINT32_C(1024), UINT32_C(4096), UINT32_C(32), UINT32_C(300), UINT32_C(134217728)
    },
    {
        "business_plan_aes",
        "rl-aes1qyzqqqqqqqqqqqqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqjfrnulg8ky0wz",
        R_AUTH_AES_GCM, UINT64_C(4),
        {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
        UINT32_C(262144), UINT32_C(4096), UINT32_C(16384), UINT32_C(64), UINT32_C(300), UINT32_C(536870912)
    },
    {
        "enterprise_plan_cookie",
        "rl-cookie1qyzsqqqqqqqqqqq9q5zs2pg9q5zs2pg9q5zs2pg9q5zs2pg9q5zs2pg9q5zs2pg9qh2vrulclr47kf",
        R_AUTH_COOKIE, UINT64_C(5),
        {0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05},
        UINT32_C(1048576), UINT32_C(16384), UINT32_C(65536), UINT32_C(128), UINT32_C(300), UINT32_C(4294967295)
    },
    {
        "maximum_aes",
        "rl-aes1q8llllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllluv073l7r88xf4",
        R_AUTH_AES_GCM, UINT64_C(18446744073709551615),
        {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
        UINT32_C(16777216), UINT32_C(16777216), UINT32_C(2147483648), UINT32_C(32768), UINT32_C(2000), UINT32_C(4294967295)
    },
    {
        "maximum_finite_window_cookie",
        "rl-cookie1qyrqqqqqqqqqqqqxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqegtruhselxhvj",
        R_AUTH_COOKIE, UINT64_C(6),
        {0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06},
        UINT32_C(65536), UINT32_C(1024), UINT32_C(4096), UINT32_C(32), UINT32_C(300), UINT32_C(1073741824)
    },
};

static const r_api_key_v1_invalid_vector_t R_API_KEY_V1_INVALID_VECTORS[] = {
    {"reserved_rate_exponent_25", "rl-aes1qyysqqqqqqqqqqqfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyvsqzqqs45cew"},
    {"reserved_rate_exponent_31", "rl-aes1qyysqqqqqqqqqqqfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpy0sqzqqzpcatf"},
    {"reserved_latency_exponent_25", "rl-aes1qyysqqqqqqqqqqqfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpysqxzqqnetqym"},
    {"reserved_latency_exponent_31", "rl-aes1qyysqqqqqqqqqqqfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfp8sqxzqqp38f99"},
    {"invalid_dedup_code_0", "rl-aes1qyysqqqqqqqqqqqfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyqqqqqqurys6m"},
    {"invalid_dedup_code_201", "rl-aes1qyysqqqqqqqqqqqfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyqqqjqxdpgr6z"},
    {"invalid_dedup_code_255", "rl-aes1qyysqqqqqqqqqqqfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyqqp7q8era7ye"},
    {"format_version_0", "rl-aes1qqpqqqqqqqqqqqqzqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqghjmuhccp9vn9"},
    {"format_version_2", "rl-aes1qgpqqqqqqqqqqqqzqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqghjmuhcchgqf0"},
    {"secret_payload_too_short", "rl-aes1qypqqqqqqqqqqqqzqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqghjmusgf2mxj"},
    {"secret_payload_too_long", "rl-aes1qypqqqqqqqqqqqqzqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqghjmuhcqqls7lnn"},
    {"legacy_unversioned_aes", "rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqemrljn"},
};

#endif
