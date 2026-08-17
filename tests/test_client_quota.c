#include <assert.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../include/r_client.h"
#include "../src/r_protocol.h"

static const char *SAMPLE_COOKIE_KEY_TENANT_2 =
    "rl-cookie1qypqqqqqqqqqqqqzqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqfgrrulczhg30p";
static const char *SAMPLE_AES_KEY_TENANT_3 =
    "rl-aes1qypsqqqqqqqqqqqrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqdgrrulcvcn0x5";

typedef struct test_ctx {
    uint8_t last_packet[R_MAX_PACKET_SIZE];
    size_t last_packet_len;
    size_t send_count;
    r_addr_t sent_to[64];
    size_t fail_send_number;
    uint64_t now_ms;
    size_t resolve_srv_count;
    char last_srv_name[256];
    r_dns_srv_cb pending_srv_cb;
    void *pending_srv_user;
    r_dns_addr_cb pending_addr_cb;
    void *pending_addr_user;
    r_dns_req_id_t cancelled_ids[8];
    size_t cancel_count;
} test_ctx_t;

typedef struct cancel_cb_ctx {
    r_client_t *client;
    int calls;
    int status;
} cancel_cb_ctx_t;

typedef struct result_cb_ctx {
    int calls;
    int status;
    bool has_result;
    uint64_t server_id;
    bool success;
    size_t guard_count;
    size_t resource_count;
    bool guards_present;
    bool resources_present;
    bool first_guard_passed;
} result_cb_ctx_t;

typedef struct profile_cb_ctx {
    int calls;
    r_request_profile_t profile;
} profile_cb_ctx_t;

typedef struct empty_cb_ctx {
    int state;
    int calls;
    r_client_req_t **out_request;
    profile_cb_ctx_t *profile;
    int expected_profile_calls;
} empty_cb_ctx_t;

typedef struct recursive_empty_cb_ctx {
    r_client_t *client;
    int calls;
} recursive_empty_cb_ctx_t;

static void record_request_profile(
    void *user,
    const r_request_profile_t *profile
) {
    profile_cb_ctx_t *context = user;
    assert(context != NULL);
    assert(profile != NULL);
    context->calls += 1;
    context->profile = *profile;
}

static void fill_ipv4_addr(r_addr_t *addr, const char *ip);

static int test_udp_send(void *ctx, const r_addr_t *to, const uint8_t *buf, size_t len) {
    test_ctx_t *test = (test_ctx_t *)ctx;
    assert(len <= sizeof(test->last_packet));
    assert(test->send_count < sizeof(test->sent_to) / sizeof(test->sent_to[0]));
    memcpy(test->last_packet, buf, len);
    test->last_packet_len = len;
    test->sent_to[test->send_count] = *to;
    test->send_count += 1;
    if (test->fail_send_number == test->send_count) {
        return -1;
    }
    return 0;
}

static uint64_t test_now_ms(void *ctx) {
    test_ctx_t *test = (test_ctx_t *)ctx;
    return test && test->now_ms != 0u ? test->now_ms : 123456789u;
}

static int test_resolve_srv(
    void *ctx,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_srv_cb cb,
    void *user
) {
    test_ctx_t *test = (test_ctx_t *)ctx;
    test->resolve_srv_count += 1u;
    assert(strlen(name) < sizeof(test->last_srv_name));
    strcpy(test->last_srv_name, name);
    if (out_req_id) {
        *out_req_id = 1u;
    }
    r_srv_record_t record = {
        .target = "s-1.local",
        .port = 8080,
        .priority = 0,
        .weight = 0,
        .ttl_ms = 60000,
    };
    cb(user, 0, &record, 1);
    return 0;
}

static int test_resolve_srv_two(
    void *ctx,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_srv_cb cb,
    void *user
) {
    (void)ctx;
    (void)name;
    if (out_req_id) {
        *out_req_id = 3u;
    }
    r_srv_record_t records[2] = {
        {.target = "s-1.local", .port = 8080, .ttl_ms = 60000},
        {.target = "s-2.local", .port = 8080, .ttl_ms = 60000},
    };
    cb(user, 0, records, 2);
    return 0;
}

static int test_resolve_addrs(
    void *ctx,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_addr_cb cb,
    void *user
) {
    (void)ctx;
    (void)name;
    if (out_req_id) {
        *out_req_id = 2u;
    }

    r_addr_t addr;
    memset(&addr, 0, sizeof(addr));
    struct sockaddr_in *sin = (struct sockaddr_in *)&addr.sa;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &sin->sin_addr);
    addr.len = sizeof(*sin);

    cb(user, 0, &addr, 1);
    return 0;
}

static int test_resolve_addrs_two(
    void *ctx,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_addr_cb cb,
    void *user
) {
    (void)ctx;
    if (out_req_id) {
        *out_req_id = 4u;
    }
    r_addr_t addr;
    memset(&addr, 0, sizeof(addr));
    struct sockaddr_in *sin = (struct sockaddr_in *)&addr.sa;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, strcmp(name, "s-1.local") == 0
        ? "127.0.0.1" : "127.0.0.2", &sin->sin_addr);
    addr.len = sizeof(*sin);
    cb(user, 0, &addr, 1);
    return 0;
}

static int test_resolve_addrs_multiple_per_server(
    void *ctx,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_addr_cb cb,
    void *user
) {
    (void)ctx;
    if (out_req_id) {
        *out_req_id = 5u;
    }
    r_addr_t addrs[2];
    fill_ipv4_addr(&addrs[0], strcmp(name, "s-1.local") == 0
        ? "127.0.0.1" : "127.0.0.2");
    if (strcmp(name, "s-1.local") == 0) {
        fill_ipv4_addr(&addrs[1], "127.0.0.11");
        cb(user, 0, addrs, 2u);
    } else {
        cb(user, 0, addrs, 1u);
    }
    return 0;
}

static int test_resolve_srv_async(
    void *ctx,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_srv_cb cb,
    void *user
) {
    (void)name;
    test_ctx_t *test = (test_ctx_t *)ctx;
    if (out_req_id) {
        *out_req_id = 101u;
    }
    test->pending_srv_cb = cb;
    test->pending_srv_user = user;
    return 0;
}

static int test_resolve_srv_failure(
    void *ctx,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_srv_cb cb,
    void *user
) {
    test_ctx_t *test = (test_ctx_t *)ctx;
    test->resolve_srv_count += 1u;
    (void)name;
    (void)out_req_id;
    (void)cb;
    (void)user;
    return -1;
}

static int test_resolve_srv_empty(
    void *ctx,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_srv_cb cb,
    void *user
) {
    (void)ctx;
    (void)name;
    if (out_req_id) {
        *out_req_id = 303u;
    }
    cb(user, 0, NULL, 0u);
    return 0;
}

static int test_resolve_srv_nonconforming(
    void *ctx,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_srv_cb cb,
    void *user
) {
    (void)ctx;
    (void)name;
    if (out_req_id) {
        *out_req_id = 404u;
    }
    const r_srv_record_t record = {
        .target = "legacy.local",
        .port = 8080u,
        .ttl_ms = 60000u,
    };
    cb(user, 0, &record, 1u);
    return 0;
}

static int test_resolve_addrs_async(
    void *ctx,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_addr_cb cb,
    void *user
) {
    (void)name;
    test_ctx_t *test = (test_ctx_t *)ctx;
    if (out_req_id) {
        *out_req_id = 202u;
    }
    test->pending_addr_cb = cb;
    test->pending_addr_user = user;
    return 0;
}

static int test_resolve_addrs_unexpected(
    void *ctx,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_addr_cb cb,
    void *user
) {
    (void)ctx;
    (void)name;
    (void)out_req_id;
    (void)cb;
    (void)user;
    assert(0 && "address resolution should not be scheduled");
    return -1;
}

static void test_cancel(void *ctx, r_dns_req_id_t req_id) {
    test_ctx_t *test = (test_ctx_t *)ctx;
    assert(test->cancel_count < sizeof(test->cancelled_ids) / sizeof(test->cancelled_ids[0]));
    test->cancelled_ids[test->cancel_count++] = req_id;
}

static void test_cancel_calls_addr_cb(void *ctx, r_dns_req_id_t req_id) {
    test_ctx_t *test = (test_ctx_t *)ctx;
    test_cancel(ctx, req_id);
    if (test->pending_addr_cb) {
        r_dns_addr_cb cb = test->pending_addr_cb;
        void *user = test->pending_addr_user;
        test->pending_addr_cb = NULL;
        test->pending_addr_user = NULL;

        r_addr_t addr;
        memset(&addr, 0, sizeof(addr));
        struct sockaddr_in *sin = (struct sockaddr_in *)&addr.sa;
        sin->sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &sin->sin_addr);
        addr.len = sizeof(*sin);
        cb(user, 0, &addr, 1);
    }
}

static void noop_rate_limit_cb(
    void *user,
    r_client_req_t *req,
    int status,
    const r_rate_limit_result_t *result
) {
    (void)user;
    (void)req;
    (void)status;
    (void)result;
}

static void record_rate_limit_cb(
    void *user,
    r_client_req_t *req,
    int status,
    const r_rate_limit_result_t *result
) {
    result_cb_ctx_t *ctx = (result_cb_ctx_t *)user;
    ctx->calls += 1;
    ctx->status = status;
    (void)req;
    ctx->has_result = result != NULL;
    ctx->server_id = result ? result->server_id : 0u;
    ctx->success = result && result->success;
    ctx->guard_count = result ? result->guard_count : 0u;
    ctx->resource_count = result ? result->resource_count : 0u;
    ctx->guards_present = result && result->guards;
    ctx->resources_present = result && result->resources;
    ctx->first_guard_passed = result
        && result->guard_count > 0u
        && result->guards[0].passed;
}

static void record_empty_rate_limit_cb(
    void *user,
    r_client_req_t *req,
    int status,
    const r_rate_limit_result_t *result
) {
    empty_cb_ctx_t *ctx = (empty_cb_ctx_t *)user;
    assert(ctx != NULL);
    assert(ctx->state == 1);
    assert(ctx->out_request != NULL);
    assert(*ctx->out_request == NULL);
    assert(ctx->profile != NULL);
    assert(ctx->profile->calls == ctx->expected_profile_calls);
    assert(req == NULL);
    assert(status == RCLIENT_OK);
    assert(result != NULL);
    assert(result->success);
    assert(result->server_id == 0u);
    assert(!result->steering_feedback);
    assert(result->guards == NULL);
    assert(result->guard_count == 0u);
    assert(result->resources == NULL);
    assert(result->resource_count == 0u);
    ctx->calls += 1;
    ctx->state = 2;
}

static void recurse_empty_rate_limit_cb(
    void *user,
    r_client_req_t *req,
    int status,
    const r_rate_limit_result_t *result
) {
    recursive_empty_cb_ctx_t *ctx = (recursive_empty_cb_ctx_t *)user;
    assert(ctx != NULL);
    assert(req == NULL);
    assert(status == RCLIENT_OK);
    assert(result != NULL);
    assert(result->success);
    assert(result->guard_count == 0u);
    assert(result->resource_count == 0u);
    ctx->calls += 1;
    if (ctx->calls == 1) {
        r_client_req_t *nested_request = (r_client_req_t *)(uintptr_t)1u;
        assert(r_client_check_rate_limit_async(
            ctx->client,
            NULL,
            0u,
            NULL,
            0u,
            NULL,
            0u,
            recurse_empty_rate_limit_cb,
            ctx,
            &nested_request
        ) == RCLIENT_OK);
        assert(nested_request == NULL);
    }
}

static void cancel_same_request_cb(
    void *user,
    r_client_req_t *req,
    int status,
    const r_rate_limit_result_t *result
) {
    (void)result;
    cancel_cb_ctx_t *ctx = (cancel_cb_ctx_t *)user;
    ctx->calls += 1;
    ctx->status = status;
    r_client_cancel_request(ctx->client, req);
}

static void fill_ipv4_addr(r_addr_t *addr, const char *ip) {
    memset(addr, 0, sizeof(*addr));
    struct sockaddr_in *sin = (struct sockaddr_in *)&addr->sa;
    sin->sin_family = AF_INET;
    assert(inet_pton(AF_INET, ip, &sin->sin_addr) == 1);
    addr->len = sizeof(*sin);
}

static void fill_loopback_addr(r_addr_t *addr) {
    fill_ipv4_addr(addr, "127.0.0.1");
}

static void assert_ipv4_addr(const r_addr_t *addr, const char *expected_ip) {
    assert(addr != NULL);
    assert(addr->sa.ss_family == AF_INET);
    struct in_addr expected;
    assert(inet_pton(AF_INET, expected_ip, &expected) == 1);
    const struct sockaddr_in *sin = (const struct sockaddr_in *)&addr->sa;
    assert(memcmp(&sin->sin_addr, &expected, sizeof(expected)) == 0);
}

static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static r_client_t *make_client(test_ctx_t *ctx) {
    r_io_ops_t io = {
        .ctx = ctx,
        .udp_send = test_udp_send,
        .now_ms = test_now_ms,
        .log = NULL,
        .on_steering_feedback = NULL,
    };
    r_resolver_ops_t resolver = {
        .ctx = ctx,
        .resolve_srv = test_resolve_srv,
        .resolve_addrs = test_resolve_addrs,
        .cancel = test_cancel,
    };
    r_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.tenant.dns_name = "example.local";
    config.tenant.key_id = 2;
    config.tenant.auth.type = R_AUTH_COOKIE;
    config.tenant.auth.secret = SAMPLE_COOKIE_KEY_TENANT_2;
    config.tenant.auth.secret_len = 0;

    r_client_t *client = NULL;
    int rc = r_client_create(&config, &io, &resolver, &client);
    assert(rc == RCLIENT_OK);
    assert(client != NULL);
    return client;
}

static r_client_t *make_two_server_client_with_policy(
    test_ctx_t *ctx,
    const r_request_policy_t *policy
) {
    r_io_ops_t io = {
        .ctx = ctx,
        .udp_send = test_udp_send,
        .now_ms = test_now_ms,
    };
    r_resolver_ops_t resolver = {
        .ctx = ctx,
        .resolve_srv = test_resolve_srv_two,
        .resolve_addrs = test_resolve_addrs_two,
        .cancel = test_cancel,
    };
    r_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.tenant.dns_name = "example.local";
    config.tenant.key_id = 2;
    config.tenant.auth.type = R_AUTH_COOKIE;
    config.tenant.auth.secret = SAMPLE_COOKIE_KEY_TENANT_2;
    config.request_policy = policy;

    r_client_t *client = NULL;
    assert(r_client_create(&config, &io, &resolver, &client) == RCLIENT_OK);
    assert(client != NULL);
    return client;
}

static r_client_t *make_two_server_client(test_ctx_t *ctx) {
    return make_two_server_client_with_policy(ctx, NULL);
}

static r_client_t *make_multiple_address_client(test_ctx_t *ctx) {
    r_io_ops_t io = {
        .ctx = ctx,
        .udp_send = test_udp_send,
        .now_ms = test_now_ms,
    };
    r_resolver_ops_t resolver = {
        .ctx = ctx,
        .resolve_srv = test_resolve_srv_two,
        .resolve_addrs = test_resolve_addrs_multiple_per_server,
        .cancel = test_cancel,
    };
    r_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.tenant.dns_name = "example.local";
    config.tenant.key_id = 2;
    config.tenant.auth.type = R_AUTH_COOKIE;
    config.tenant.auth.secret = SAMPLE_COOKIE_KEY_TENANT_2;

    r_client_t *client = NULL;
    assert(r_client_create(&config, &io, &resolver, &client) == RCLIENT_OK);
    assert(client != NULL);
    return client;
}

static r_client_t *make_client_with_policy(
    test_ctx_t *ctx,
    const r_request_policy_t *policy
) {
    r_io_ops_t io = {
        .ctx = ctx,
        .udp_send = test_udp_send,
        .now_ms = test_now_ms,
    };
    r_resolver_ops_t resolver = {
        .ctx = ctx,
        .resolve_srv = test_resolve_srv,
        .resolve_addrs = test_resolve_addrs,
        .cancel = test_cancel,
    };
    r_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.tenant.dns_name = "example.local";
    config.tenant.key_id = 2;
    config.tenant.auth.type = R_AUTH_COOKIE;
    config.tenant.auth.secret = SAMPLE_COOKIE_KEY_TENANT_2;
    config.request_policy = policy;
    r_client_t *client = NULL;
    assert(r_client_create(&config, &io, &resolver, &client) == RCLIENT_OK);
    return client;
}

static r_client_t *make_client_with_policy_and_profile(
    test_ctx_t *ctx,
    const r_request_policy_t *policy,
    profile_cb_ctx_t *profile
) {
    r_io_ops_t io = {
        .ctx = ctx,
        .udp_send = test_udp_send,
        .now_ms = test_now_ms,
    };
    r_resolver_ops_t resolver = {
        .ctx = ctx,
        .resolve_srv = test_resolve_srv,
        .resolve_addrs = test_resolve_addrs,
        .cancel = test_cancel,
    };
    r_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.tenant.dns_name = "example.local";
    config.tenant.key_id = 2;
    config.tenant.auth.type = R_AUTH_COOKIE;
    config.tenant.auth.secret = SAMPLE_COOKIE_KEY_TENANT_2;
    config.request_policy = policy;
    config.request_profile_cb = record_request_profile;
    config.request_profile_user = profile;

    r_client_t *client = NULL;
    assert(r_client_create(&config, &io, &resolver, &client) == RCLIENT_OK);
    return client;
}

static r_client_t *make_client_with_ops(
    test_ctx_t *ctx,
    const r_io_ops_t *io,
    const r_resolver_ops_t *resolver
) {
    r_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.tenant.dns_name = "example.local";
    config.tenant.key_id = 2;
    config.tenant.auth.type = R_AUTH_COOKIE;
    config.tenant.auth.secret = SAMPLE_COOKIE_KEY_TENANT_2;
    config.tenant.auth.secret_len = 0;

    r_client_t *client = NULL;
    int rc = r_client_create(&config, io, resolver, &client);
    assert(rc == RCLIENT_OK);
    assert(client != NULL);
    (void)ctx;
    return client;
}

static r_client_t *make_aes_client(test_ctx_t *ctx) {
    r_io_ops_t io = {
        .ctx = ctx,
        .udp_send = test_udp_send,
        .now_ms = test_now_ms,
        .log = NULL,
        .on_steering_feedback = NULL,
    };
    r_resolver_ops_t resolver = {
        .ctx = ctx,
        .resolve_srv = test_resolve_srv,
        .resolve_addrs = test_resolve_addrs,
        .cancel = test_cancel,
    };
    r_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.tenant.dns_name = "example.local";
    config.tenant.key_id = 3;
    config.tenant.auth.type = R_AUTH_AES_GCM;
    config.tenant.auth.secret = SAMPLE_AES_KEY_TENANT_3;
    config.tenant.auth.secret_len = 0;

    r_client_t *client = NULL;
    int rc = r_client_create(&config, &io, &resolver, &client);
    assert(rc == RCLIENT_OK);
    assert(client != NULL);
    return client;
}

static void test_client_derives_production_tenant_from_key(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_io_ops_t io = {
        .ctx = &ctx,
        .udp_send = test_udp_send,
        .now_ms = test_now_ms,
        .log = NULL,
        .on_steering_feedback = NULL,
    };
    r_resolver_ops_t resolver = {
        .ctx = &ctx,
        .resolve_srv = test_resolve_srv,
        .resolve_addrs = test_resolve_addrs,
        .cancel = test_cancel,
    };
    r_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.tenant.auth.secret = SAMPLE_AES_KEY_TENANT_3;

    r_client_t *client = NULL;
    assert(r_client_create(&config, &io, &resolver, &client) == RCLIENT_OK);
    assert(client != NULL);
    assert(strcmp(
        ctx.last_srv_name,
        "_ratelimitly._udp.c-3.p0.ratelimitly.com"
    ) == 0);

    r_service_latency_report_t report;
    memset(&report, 0, sizeof(report));
    memcpy(report.latency_tracker_id, "default-tenant", 14);
    report.observed_latency = 10;
    report.ttl_ms = 1000;
    report.max_samples = 10;
    report.buffer_size = 10;
    report.min_sample_threshold = 1;
    assert(r_client_report_latency(client, &report, 1) == RCLIENT_OK);

    r_tenant_header_t tenant;
    size_t auth_pos = 0;
    assert(r_parse_tenant_header(
        ctx.last_packet,
        ctx.last_packet_len,
        &tenant,
        &auth_pos
    ) == RCLIENT_OK);
    assert(tenant.key_id == 3u);

    uint16_t auth_type = 0;
    size_t auth_size = 0;
    const uint8_t *auth_body = NULL;
    size_t auth_body_len = 0;
    size_t pdu_pos = 0;
    assert(r_parse_auth_tlv_header(
        ctx.last_packet,
        ctx.last_packet_len,
        auth_pos,
        &auth_type,
        &auth_size,
        &auth_body,
        &auth_body_len,
        &pdu_pos
    ) == RCLIENT_OK);
    assert(auth_type == R_TLV_AUTH_AES);

    r_client_destroy(client);
}

static void test_client_preserves_explicit_tenant_override(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_io_ops_t io = {
        .ctx = &ctx,
        .udp_send = test_udp_send,
        .now_ms = test_now_ms,
        .log = NULL,
        .on_steering_feedback = NULL,
    };
    r_resolver_ops_t resolver = {
        .ctx = &ctx,
        .resolve_srv = test_resolve_srv,
        .resolve_addrs = test_resolve_addrs,
        .cancel = test_cancel,
    };
    r_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.tenant.dns_name = "custom.example";
    config.tenant.auth.secret = SAMPLE_AES_KEY_TENANT_3;

    r_client_t *client = NULL;
    assert(r_client_create(&config, &io, &resolver, &client) == RCLIENT_OK);
    assert(client != NULL);
    assert(strcmp(ctx.last_srv_name, "_ratelimitly._udp.custom.example") == 0);
    r_client_destroy(client);
}

static void test_client_rejects_explicit_key_metadata_mismatch(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_io_ops_t io = {
        .ctx = &ctx,
        .udp_send = test_udp_send,
        .now_ms = test_now_ms,
        .log = NULL,
        .on_steering_feedback = NULL,
    };
    r_resolver_ops_t resolver = {
        .ctx = &ctx,
        .resolve_srv = test_resolve_srv,
        .resolve_addrs = test_resolve_addrs,
        .cancel = test_cancel,
    };
    r_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.tenant.auth.secret = SAMPLE_AES_KEY_TENANT_3;
    config.tenant.key_id = 2u;

    r_client_t *client = NULL;
    assert(r_client_create(&config, &io, &resolver, &client)
        == RCLIENT_ERR_CONFIG);
    assert(client == NULL);

    config.tenant.key_id = 0u;
    config.tenant.auth.type = R_AUTH_COOKIE;
    assert(r_client_create(&config, &io, &resolver, &client)
        == RCLIENT_ERR_CONFIG);
    assert(client == NULL);
}

static r_resource_request_t sample_resource(void) {
    r_resource_request_t resource;
    memset(&resource, 0, sizeof(resource));
    memcpy(resource.bucket_id, "bucket", 6);
    resource.window_size_ms = 1000;
    resource.rate_limit = 100;
    resource.tokens_requested = 1;
    return resource;
}

static void assert_srv_only_discovery_failure(
    int (*resolve_srv)(
        void *,
        const char *,
        r_dns_req_id_t *,
        r_dns_srv_cb,
        void *
    )
) {
    test_ctx_t ctx = {0};
    r_io_ops_t io = {
        .ctx = &ctx,
        .udp_send = test_udp_send,
        .now_ms = test_now_ms,
    };
    r_resolver_ops_t resolver = {
        .ctx = &ctx,
        .resolve_srv = resolve_srv,
        .resolve_addrs = test_resolve_addrs_unexpected,
        .cancel = test_cancel,
    };
    r_client_t *client = make_client_with_ops(&ctx, &io, &resolver);
    r_resource_request_t resource = sample_resource();
    r_client_req_t *request = NULL;
    assert(r_client_check_rate_limit_async(
        client,
        &resource,
        1u,
        NULL,
        0u,
        NULL,
        0u,
        noop_rate_limit_cb,
        NULL,
        &request
    ) == RCLIENT_ERR_DNS);
    assert(request == NULL);
    assert(ctx.send_count == 0u);
    r_client_destroy(client);
}

static void test_discovery_remains_srv_only(void) {
    assert_srv_only_discovery_failure(test_resolve_srv_failure);
    assert_srv_only_discovery_failure(test_resolve_srv_empty);
    assert_srv_only_discovery_failure(test_resolve_srv_nonconforming);
}

static r_client_req_t *start_sample_request(
    r_client_t *client,
    result_cb_ctx_t *result
) {
    r_resource_request_t resource = sample_resource();
    r_client_req_t *req = NULL;
    assert(r_client_check_rate_limit_async(
        client,
        &resource,
        1,
        NULL,
        0,
        NULL,
        0,
        record_rate_limit_cb,
        result,
        &req
    ) == RCLIENT_OK);
    assert(req != NULL);
    return req;
}

static void copy_last_request_id(
    const test_ctx_t *ctx,
    uint8_t out_request_id[16]
) {
    r_tenant_header_t tenant;
    size_t pos = 0;
    assert(r_parse_tenant_header(
        ctx->last_packet,
        ctx->last_packet_len,
        &tenant,
        &pos
    ) == RCLIENT_OK);
    memcpy(out_request_id, tenant.unique_id, 16);
}

static size_t build_cookie_success_response_from(
    uint64_t server_id,
    const uint8_t unique_id[16],
    uint8_t *out,
    size_t out_cap
) {
    assert(out_cap >= R_MAX_PACKET_SIZE);
    r_tenant_header_t tenant;
    memset(&tenant, 0, sizeof(tenant));
    tenant.tlv_type = R_TLV_TENANT;
    tenant.tlv_size = R_TENANT_TLV_LEN;
    tenant.key_id = server_id;
    memcpy(tenant.unique_id, unique_id, 16);
    tenant.time_stamp = test_now_ms(NULL);
    tenant.steering_feedback = 1;

    size_t pos = 0;
    r_tenant_header_write(&tenant, out, out_cap);
    pos += R_TENANT_TLV_LEN;
    write_le16(out + pos, R_TLV_AUTH_COOKIE);
    write_le16(out + pos + 2, 36);
    pos += 4;
    memset(out + pos, 2, 32);
    pos += 32;

    const uint8_t body[4] = {0, 0, 0, 0};
    size_t pdu_len = 0;
    int rc = r_build_pdu(
        R_PDU_RATE_RESPONSE,
        body,
        sizeof(body),
        out + pos,
        out_cap - pos,
        &pdu_len
    );
    assert(rc == RCLIENT_OK);
    return pos + pdu_len;
}

static size_t build_cookie_denied_response_from(
    uint64_t server_id,
    const uint8_t unique_id[16],
    uint8_t *out,
    size_t out_cap
) {
    assert(out_cap >= R_MAX_PACKET_SIZE);
    r_tenant_header_t tenant;
    memset(&tenant, 0, sizeof(tenant));
    tenant.tlv_type = R_TLV_TENANT;
    tenant.tlv_size = R_TENANT_TLV_LEN;
    tenant.key_id = server_id;
    memcpy(tenant.unique_id, unique_id, 16);
    tenant.time_stamp = test_now_ms(NULL);
    tenant.steering_feedback = 1;

    size_t pos = 0;
    r_tenant_header_write(&tenant, out, out_cap);
    pos += R_TENANT_TLV_LEN;
    write_le16(out + pos, R_TLV_AUTH_COOKIE);
    write_le16(out + pos + 2, 36);
    pos += 4;
    memset(out + pos, 2, 32);
    pos += 32;

    uint8_t body[4 + R_RESOURCE_BLOCK_WIRE_LEN];
    memset(body, 0, sizeof(body));
    write_le16(body, 0u);
    write_le16(body + 2, 1u);
    memcpy(body + 4, "bucket", 6);
    write_le16(body + 4 + 24, 1u);

    size_t pdu_len = 0;
    int rc = r_build_pdu(
        R_PDU_RATE_RESPONSE,
        body,
        sizeof(body),
        out + pos,
        out_cap - pos,
        &pdu_len
    );
    assert(rc == RCLIENT_OK);
    return pos + pdu_len;
}

static size_t build_cookie_guard_response_from(
    uint64_t server_id,
    const uint8_t unique_id[16],
    uint32_t threshold_ms,
    uint32_t current_latency_ms,
    uint8_t *out,
    size_t out_cap
) {
    assert(out_cap >= R_MAX_PACKET_SIZE);
    r_tenant_header_t tenant;
    memset(&tenant, 0, sizeof(tenant));
    tenant.tlv_type = R_TLV_TENANT;
    tenant.tlv_size = R_TENANT_TLV_LEN;
    tenant.key_id = server_id;
    memcpy(tenant.unique_id, unique_id, 16);
    tenant.time_stamp = test_now_ms(NULL);
    tenant.steering_feedback = 1;

    size_t pos = 0;
    r_tenant_header_write(&tenant, out, out_cap);
    pos += R_TENANT_TLV_LEN;
    write_le16(out + pos, R_TLV_AUTH_COOKIE);
    write_le16(out + pos + 2, 36u);
    pos += 4;
    memset(out + pos, 2, 32);
    pos += 32;

    uint8_t body[4 + R_GUARD_BLOCK_WIRE_LEN];
    memset(body, 0, sizeof(body));
    write_le16(body, 1u);
    write_le16(body + 2, 0u);
    memcpy(body + 4, "guard", 5);
    write_le32(body + 4 + 32, threshold_ms);
    write_le32(body + 4 + 36, current_latency_ms);

    size_t pdu_len = 0;
    int rc = r_build_pdu(
        R_PDU_RATE_RESPONSE,
        body,
        sizeof(body),
        out + pos,
        out_cap - pos,
        &pdu_len
    );
    assert(rc == RCLIENT_OK);
    return pos + pdu_len;
}

static size_t build_cookie_success_response(
    const uint8_t unique_id[16],
    uint8_t *out,
    size_t out_cap
) {
    return build_cookie_success_response_from(1u, unique_id, out, out_cap);
}

static size_t build_aes_empty_response(
    const uint8_t unique_id[16],
    uint8_t *out,
    size_t out_cap
) {
    assert(out_cap >= R_TENANT_TLV_LEN + 4 + 28);
    r_tenant_header_t tenant;
    memset(&tenant, 0, sizeof(tenant));
    tenant.tlv_type = R_TLV_TENANT;
    tenant.tlv_size = R_TENANT_TLV_LEN;
    tenant.key_id = 1;
    memcpy(tenant.unique_id, unique_id, 16);
    tenant.time_stamp = test_now_ms(NULL);
    tenant.steering_feedback = 1;

    size_t pos = 0;
    r_tenant_header_write(&tenant, out, out_cap);
    pos += R_TENANT_TLV_LEN;
    write_le16(out + pos, R_TLV_AUTH_AES);
    write_le16(out + pos + 2, 32);
    pos += 4;
    memset(out + pos, 0, 12 + 16);
    pos += 12 + 16;
    return pos;
}

static r_latency_guard_t sample_guard(void) {
    r_latency_guard_t guard;
    memset(&guard, 0, sizeof(guard));
    memcpy(guard.latency_tracker_id, "guard", 5);
    guard.threshold_ms = 50u;
    guard.ttl_ms = 1000u;
    guard.max_samples = 10u;
    guard.buffer_size = 64u;
    guard.min_sample_threshold = 1u;
    return guard;
}

static void assert_last_request_shape(
    const test_ctx_t *ctx,
    uint16_t expected_guard_count,
    uint16_t expected_resource_count
) {
    r_tenant_header_t tenant;
    size_t auth_pos = 0;
    assert(r_parse_tenant_header(
        ctx->last_packet,
        ctx->last_packet_len,
        &tenant,
        &auth_pos
    ) == RCLIENT_OK);

    uint16_t auth_type = 0;
    size_t auth_size = 0;
    const uint8_t *auth_body = NULL;
    size_t auth_body_len = 0;
    size_t pdu_pos = 0;
    assert(r_parse_auth_tlv_header(
        ctx->last_packet,
        ctx->last_packet_len,
        auth_pos,
        &auth_type,
        &auth_size,
        &auth_body,
        &auth_body_len,
        &pdu_pos
    ) == RCLIENT_OK);
    assert(auth_type == R_TLV_AUTH_COOKIE);
    assert(pdu_pos + 12u <= ctx->last_packet_len);

    const uint8_t *pdu = ctx->last_packet + pdu_pos;
    uint16_t pdu_type = (uint16_t)pdu[0] | ((uint16_t)pdu[1] << 8);
    uint16_t guard_count = (uint16_t)pdu[8] | ((uint16_t)pdu[9] << 8);
    uint16_t resource_count = (uint16_t)pdu[10] | ((uint16_t)pdu[11] << 8);
    assert(pdu_type == R_PDU_RATE_REQUEST);
    assert(guard_count == expected_guard_count);
    assert(resource_count == expected_resource_count);
}

static void test_guard_only_requests_allow_and_deny(void) {
    test_ctx_t ctx = {0};
    r_client_t *client = make_client(&ctx);
    r_latency_guard_t guard = sample_guard();
    r_addr_t from;
    fill_loopback_addr(&from);

    result_cb_ctx_t allowed = {0};
    r_client_req_t *request = NULL;
    assert(r_client_check_rate_limit_async(
        client,
        NULL,
        0u,
        &guard,
        1u,
        NULL,
        0u,
        record_rate_limit_cb,
        &allowed,
        &request
    ) == RCLIENT_OK);
    assert(request != NULL);
    assert(ctx.send_count == 1u);
    assert_last_request_shape(&ctx, 1u, 0u);

    uint8_t request_id[16];
    copy_last_request_id(&ctx, request_id);
    uint8_t response[R_MAX_PACKET_SIZE];
    size_t response_len = build_cookie_guard_response_from(
        1u,
        request_id,
        guard.threshold_ms,
        20u,
        response,
        sizeof(response)
    );
    assert(r_client_on_datagram(
        client,
        response,
        response_len,
        &from
    ) == RCLIENT_OK);
    assert(allowed.calls == 1);
    assert(allowed.status == RCLIENT_OK);
    assert(allowed.has_result);
    assert(allowed.success);
    assert(allowed.guard_count == 1u);
    assert(allowed.resource_count == 0u);
    assert(allowed.guards_present);
    assert(!allowed.resources_present);
    assert(allowed.first_guard_passed);

    result_cb_ctx_t denied = {0};
    request = NULL;
    assert(r_client_check_rate_limit_async_borrowed(
        client,
        NULL,
        0u,
        &guard,
        1u,
        NULL,
        0u,
        record_rate_limit_cb,
        &denied,
        &request
    ) == RCLIENT_OK);
    assert(request != NULL);
    assert(ctx.send_count == 2u);
    assert_last_request_shape(&ctx, 1u, 0u);

    copy_last_request_id(&ctx, request_id);
    response_len = build_cookie_guard_response_from(
        1u,
        request_id,
        guard.threshold_ms,
        75u,
        response,
        sizeof(response)
    );
    assert(r_client_on_datagram(
        client,
        response,
        response_len,
        &from
    ) == RCLIENT_OK);
    assert(denied.calls == 1);
    assert(denied.status == RCLIENT_OK);
    assert(denied.has_result);
    assert(!denied.success);
    assert(denied.guard_count == 1u);
    assert(denied.resource_count == 0u);
    assert(denied.guards_present);
    assert(!denied.resources_present);
    assert(!denied.first_guard_passed);

    r_client_destroy(client);
}

static void test_empty_requests_complete_locally(void) {
    test_ctx_t ctx = {0};
    r_io_ops_t io = {
        .ctx = &ctx,
        .udp_send = test_udp_send,
        .now_ms = test_now_ms,
    };
    r_resolver_ops_t resolver = {
        .ctx = &ctx,
        .resolve_srv = test_resolve_srv_failure,
        .resolve_addrs = test_resolve_addrs_unexpected,
        .cancel = test_cancel,
    };
    profile_cb_ctx_t profile = {0};
    r_request_policy_t invalid_policy;
    r_client_default_request_policy(&invalid_policy);
    invalid_policy.unit_ms = 0u;
    r_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.tenant.dns_name = "example.local";
    config.tenant.key_id = 2u;
    config.tenant.auth.type = R_AUTH_COOKIE;
    config.tenant.auth.secret = SAMPLE_COOKIE_KEY_TENANT_2;
    config.request_policy = &invalid_policy;
    config.request_profile_cb = record_request_profile;
    config.request_profile_user = &profile;

    r_client_t *client = NULL;
    assert(r_client_create(&config, &io, &resolver, &client) == RCLIENT_OK);
    assert(client != NULL);
    assert(ctx.resolve_srv_count == 1u);

    r_client_req_t *request = (r_client_req_t *)(uintptr_t)1u;
    empty_cb_ctx_t copied = {
        .state = 1,
        .out_request = &request,
        .profile = &profile,
        .expected_profile_calls = 1,
    };
    assert(r_client_check_rate_limit_async(
        client,
        NULL,
        0u,
        NULL,
        0u,
        "ignored-empty-label",
        0u,
        record_empty_rate_limit_cb,
        &copied,
        &request
    ) == RCLIENT_OK);
    assert(copied.state == 2);
    assert(copied.calls == 1);
    assert(request == NULL);
    assert(ctx.send_count == 0u);
    assert(ctx.resolve_srv_count == 1u);
    assert(profile.calls == 1);
    assert(profile.profile.wait_ms == 0u);
    assert(profile.profile.round == 0u);
    assert(profile.profile.phase == R_REQUEST_COMPLETION_ROUND);
    assert(profile.profile.status == RCLIENT_OK);
    assert(!profile.profile.response_selected);

    request = (r_client_req_t *)(uintptr_t)1u;
    empty_cb_ctx_t borrowed = {
        .state = 1,
        .out_request = &request,
        .profile = &profile,
        .expected_profile_calls = 2,
    };
    assert(r_client_check_rate_limit_async_borrowed(
        client,
        NULL,
        0u,
        NULL,
        0u,
        NULL,
        0u,
        record_empty_rate_limit_cb,
        &borrowed,
        &request
    ) == RCLIENT_OK);
    assert(borrowed.state == 2);
    assert(borrowed.calls == 1);
    assert(request == NULL);
    assert(ctx.send_count == 0u);
    assert(ctx.resolve_srv_count == 1u);
    assert(profile.calls == 2);

    r_client_destroy(client);
}

static void test_request_shape_pointer_validation(void) {
    test_ctx_t ctx = {0};
    r_client_t *client = make_client(&ctx);
    r_resource_request_t resource = sample_resource();
    r_latency_guard_t guard = sample_guard();

    assert(r_client_check_rate_limit_async(
        client, NULL, 1u, NULL, 0u, NULL, 0u,
        noop_rate_limit_cb, NULL, NULL
    ) == RCLIENT_ERR_CONFIG);
    assert(r_client_check_rate_limit_async(
        client, &resource, 1u, NULL, 1u, NULL, 0u,
        noop_rate_limit_cb, NULL, NULL
    ) == RCLIENT_ERR_CONFIG);
    assert(r_client_check_rate_limit_async_borrowed(
        client, NULL, 1u, &guard, 1u, NULL, 0u,
        noop_rate_limit_cb, NULL, NULL
    ) == RCLIENT_ERR_CONFIG);
    assert(ctx.send_count == 0u);

    r_client_destroy(client);
}

static void test_empty_request_callback_may_submit_recursively(void) {
    test_ctx_t ctx = {0};
    r_client_t *client = make_client(&ctx);
    recursive_empty_cb_ctx_t callback = {
        .client = client,
        .calls = 0,
    };
    r_client_req_t *request = (r_client_req_t *)(uintptr_t)1u;

    assert(r_client_check_rate_limit_async(
        client,
        NULL,
        0u,
        NULL,
        0u,
        NULL,
        0u,
        recurse_empty_rate_limit_cb,
        &callback,
        &request
    ) == RCLIENT_OK);
    assert(request == NULL);
    assert(callback.calls == 2);
    assert(ctx.send_count == 0u);

    r_client_destroy(client);
}

static void test_check_rate_limit_rejects_oversized_guard(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_client_t *client = make_client(&ctx);

    r_resource_request_t resource;
    memset(&resource, 0, sizeof(resource));
    memcpy(resource.bucket_id, "bucket", 6);
    resource.window_size_ms = 1000;
    resource.rate_limit = 100;
    resource.tokens_requested = 1;

    r_latency_guard_t guard;
    memset(&guard, 0, sizeof(guard));
    memcpy(guard.latency_tracker_id, "guard", 5);
    guard.threshold_ms = 50;
    guard.ttl_ms = 1000;
    guard.max_samples = 10;
    guard.buffer_size = 65;
    guard.min_sample_threshold = 1;

    int rc = r_client_check_rate_limit_async(
        client,
        &resource,
        1,
        &guard,
        1,
        NULL,
        0,
        noop_rate_limit_cb,
        NULL,
        NULL
    );
    assert(rc == RCLIENT_ERR_PROTOCOL);
    assert(ctx.send_count == 0);

    r_client_destroy(client);
}

static void test_report_latency_filters_oversized_reports(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_client_t *client = make_client(&ctx);

    r_service_latency_report_t reports[2];
    memset(reports, 0, sizeof(reports));
    memcpy(reports[0].latency_tracker_id, "ok", 2);
    reports[0].observed_latency = 10;
    reports[0].ttl_ms = 1000;
    reports[0].max_samples = 10;
    reports[0].buffer_size = 64;
    reports[0].min_sample_threshold = 1;

    memcpy(reports[1].latency_tracker_id, "drop", 4);
    reports[1].observed_latency = 20;
    reports[1].ttl_ms = 1000;
    reports[1].max_samples = 10;
    reports[1].buffer_size = 65;
    reports[1].min_sample_threshold = 1;

    int rc = r_client_report_latency(client, reports, 2);
    assert(rc == RCLIENT_OK);
    assert(ctx.send_count >= 1);

    r_tenant_header_t tenant;
    size_t pos = 0;
    rc = r_parse_tenant_header(ctx.last_packet, ctx.last_packet_len, &tenant, &pos);
    assert(rc == RCLIENT_OK);
    assert(tenant.key_id == 2u);

    uint16_t auth_type = 0;
    size_t auth_size = 0;
    const uint8_t *auth_body = NULL;
    size_t auth_body_len = 0;
    size_t pdu_pos = 0;
    rc = r_parse_auth_tlv_header(
        ctx.last_packet,
        ctx.last_packet_len,
        pos,
        &auth_type,
        &auth_size,
        &auth_body,
        &auth_body_len,
        &pdu_pos
    );
    assert(rc == RCLIENT_OK);
    assert(auth_type == R_TLV_AUTH_COOKIE);
    assert(auth_size == 36u);
    assert(auth_body_len == 32u);
    for (size_t i = 0; i < auth_body_len; i++) {
        assert(auth_body[i] == 2u);
    }
    assert(pdu_pos + 10 <= ctx.last_packet_len);

    const uint8_t *pdu = ctx.last_packet + pdu_pos;
    uint16_t pdu_type = (uint16_t)pdu[0] | ((uint16_t)pdu[1] << 8);
    uint16_t pdu_size = (uint16_t)pdu[2] | ((uint16_t)pdu[3] << 8);
    uint16_t service_count = (uint16_t)pdu[8] | ((uint16_t)pdu[9] << 8);
    assert(pdu_type == R_PDU_LATENCY_REPORT);
    assert(pdu_size == 48u);
    assert(service_count == 1u);
    assert(ctx.last_packet_len == pdu_pos + pdu_size);

    ctx.send_count = 0;
    rc = r_client_report_latency(client, &reports[1], 1);
    assert(rc == RCLIENT_OK);
    assert(ctx.send_count == 0);

    r_client_destroy(client);
}

static void fill_latency_reports(r_service_latency_report_t *reports, size_t count) {
    memset(reports, 0, count * sizeof(*reports));
    for (size_t i = 0; i < count; i++) {
        memcpy(reports[i].latency_tracker_id, "svc", 3);
        reports[i].observed_latency = 10;
        reports[i].ttl_ms = 1000;
        reports[i].max_samples = 10;
        reports[i].buffer_size = 64;
        reports[i].min_sample_threshold = 1;
    }
}

/*
 * Latency reports are framed into a fixed R_MAX_PACKET_SIZE buffer:
 *   cookie: 40 (tenant) + 4 (auth TLV) + 32 (cookie) + 8 (PDU) + 4 + 36 * n
 *   aes:    40 (tenant) + 4 (auth TLV) + 12 (nonce) + 16 (tag) + 8 + 4 + 36 * n
 * so 30 reports fit under cookie auth and 31 under AES. r_build_latency_report_body
 * only rejects at n >= 34, leaving a window where the framing memcpys would run past
 * the packet buffer. Oversized batches must be rejected before any copy happens.
 */
#define LATENCY_COOKIE_MAX_REPORTS 30u
#define LATENCY_AES_MAX_REPORTS 31u

static void test_report_latency_accepts_largest_cookie_batch(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_client_t *client = make_client(&ctx);

    r_service_latency_report_t reports[LATENCY_COOKIE_MAX_REPORTS];
    fill_latency_reports(reports, LATENCY_COOKIE_MAX_REPORTS);

    int rc = r_client_report_latency(client, reports, LATENCY_COOKIE_MAX_REPORTS);
    assert(rc == RCLIENT_OK);
    assert(ctx.send_count == 1);
    /* Every report survived the buffer_size filter and landed in one packet. */
    assert(ctx.last_packet_len == 88u + 36u * LATENCY_COOKIE_MAX_REPORTS);
    assert(ctx.last_packet_len <= R_MAX_PACKET_SIZE);

    r_client_destroy(client);
}

static void test_report_latency_rejects_oversized_cookie_batch(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_client_t *client = make_client(&ctx);

    r_service_latency_report_t reports[LATENCY_COOKIE_MAX_REPORTS + 1u];
    fill_latency_reports(reports, LATENCY_COOKIE_MAX_REPORTS + 1u);

    int rc = r_client_report_latency(client, reports, LATENCY_COOKIE_MAX_REPORTS + 1u);
    assert(rc == RCLIENT_ERR_PROTOCOL);
    assert(ctx.send_count == 0);

    r_client_destroy(client);
}

static void test_report_latency_rejects_oversized_batch_after_filtering(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_client_t *client = make_client(&ctx);

    /* Reports above the buffer_size quota are dropped into a heap-allocated
     * filtered copy, so the capacity rejection must release it too. Three of
     * these 34 are filtered out, leaving 31 - one past cookie capacity. */
    enum { TOTAL = LATENCY_COOKIE_MAX_REPORTS + 4u };
    r_service_latency_report_t reports[TOTAL];
    fill_latency_reports(reports, TOTAL);
    reports[0].buffer_size = 65;
    reports[1].buffer_size = 65;
    reports[2].buffer_size = 65;

    int rc = r_client_report_latency(client, reports, TOTAL);
    assert(rc == RCLIENT_ERR_PROTOCOL);
    assert(ctx.send_count == 0);

    r_client_destroy(client);
}

static void test_report_latency_accepts_largest_aes_batch(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_client_t *client = make_aes_client(&ctx);

    r_service_latency_report_t reports[LATENCY_AES_MAX_REPORTS];
    fill_latency_reports(reports, LATENCY_AES_MAX_REPORTS);

    int rc = r_client_report_latency(client, reports, LATENCY_AES_MAX_REPORTS);
    assert(rc == RCLIENT_OK);
    assert(ctx.send_count == 1);
    assert(ctx.last_packet_len == 84u + 36u * LATENCY_AES_MAX_REPORTS);
    assert(ctx.last_packet_len <= R_MAX_PACKET_SIZE);

    r_client_destroy(client);
}

static void test_report_latency_rejects_oversized_aes_batch(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_client_t *client = make_aes_client(&ctx);

    r_service_latency_report_t reports[LATENCY_AES_MAX_REPORTS + 1u];
    fill_latency_reports(reports, LATENCY_AES_MAX_REPORTS + 1u);

    int rc = r_client_report_latency(client, reports, LATENCY_AES_MAX_REPORTS + 1u);
    assert(rc == RCLIENT_ERR_PROTOCOL);
    assert(ctx.send_count == 0);

    r_client_destroy(client);
}

static void test_report_latency_requires_udp_send(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_io_ops_t io = {
        .ctx = &ctx,
        .udp_send = NULL,
        .now_ms = test_now_ms,
        .log = NULL,
        .on_steering_feedback = NULL,
    };
    r_resolver_ops_t resolver = {
        .ctx = &ctx,
        .resolve_srv = test_resolve_srv,
        .resolve_addrs = test_resolve_addrs,
        .cancel = test_cancel,
    };
    r_client_t *client = make_client_with_ops(&ctx, &io, &resolver);

    r_service_latency_report_t report;
    memset(&report, 0, sizeof(report));
    memcpy(report.latency_tracker_id, "ok", 2);
    report.observed_latency = 10;
    report.ttl_ms = 1000;
    report.max_samples = 10;
    report.buffer_size = 64;
    report.min_sample_threshold = 1;

    int rc = r_client_report_latency(client, &report, 1);
    assert(rc == RCLIENT_ERR_IO);
    assert(ctx.send_count == 0);

    r_client_destroy(client);
}

static void test_empty_aes_response_is_rejected_explicitly(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_client_t *client = make_aes_client(&ctx);

    r_resource_request_t resource = sample_resource();
    r_client_req_t *req = NULL;
    int rc = r_client_check_rate_limit_async(
        client,
        &resource,
        1,
        NULL,
        0,
        NULL,
        0,
        noop_rate_limit_cb,
        NULL,
        &req
    );
    assert(rc == RCLIENT_OK);
    assert(req != NULL);

    r_tenant_header_t request_tenant;
    size_t pos = 0;
    rc = r_parse_tenant_header(ctx.last_packet, ctx.last_packet_len, &request_tenant, &pos);
    assert(rc == RCLIENT_OK);

    uint8_t response[R_MAX_PACKET_SIZE];
    size_t response_len =
        build_aes_empty_response(request_tenant.unique_id, response, sizeof(response));

    r_addr_t from;
    fill_loopback_addr(&from);
    rc = r_client_on_datagram(client, response, response_len, &from);
    assert(rc == RCLIENT_ERR_PROTOCOL);

    r_client_cancel_request(client, req);
    r_client_destroy(client);
}

static void test_callback_can_cancel_same_request_without_double_free(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_client_t *client = make_client(&ctx);

    cancel_cb_ctx_t cb_ctx;
    memset(&cb_ctx, 0, sizeof(cb_ctx));
    cb_ctx.client = client;
    cb_ctx.status = 999;

    r_resource_request_t resource = sample_resource();
    r_client_req_t *req = NULL;
    int rc = r_client_check_rate_limit_async(
        client,
        &resource,
        1,
        NULL,
        0,
        NULL,
        0,
        cancel_same_request_cb,
        &cb_ctx,
        &req
    );
    assert(rc == RCLIENT_OK);
    assert(req != NULL);

    r_tenant_header_t request_tenant;
    size_t pos = 0;
    rc = r_parse_tenant_header(ctx.last_packet, ctx.last_packet_len, &request_tenant, &pos);
    assert(rc == RCLIENT_OK);

    uint8_t response[R_MAX_PACKET_SIZE];
    size_t response_len =
        build_cookie_success_response(request_tenant.unique_id, response, sizeof(response));

    r_addr_t from;
    fill_loopback_addr(&from);
    rc = r_client_on_datagram(client, response, response_len, &from);
    assert(rc == RCLIENT_OK);
    assert(cb_ctx.calls == 1);
    assert(cb_ctx.status == RCLIENT_OK);

    r_client_destroy(client);
}

static void test_destroy_ignores_late_dns_srv_callback(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_io_ops_t io = {
        .ctx = &ctx,
        .udp_send = test_udp_send,
        .now_ms = test_now_ms,
        .log = NULL,
        .on_steering_feedback = NULL,
    };
    r_resolver_ops_t resolver = {
        .ctx = &ctx,
        .resolve_srv = test_resolve_srv_async,
        .resolve_addrs = test_resolve_addrs_unexpected,
        .cancel = test_cancel,
    };
    r_client_t *client = make_client_with_ops(&ctx, &io, &resolver);
    assert(ctx.pending_srv_cb != NULL);
    assert(ctx.pending_srv_user != NULL);

    r_client_destroy(client);
    assert(ctx.cancel_count == 1u);
    assert(ctx.cancelled_ids[0] == 101u);

    ctx.pending_srv_cb(ctx.pending_srv_user, -1, NULL, 0);
}

static void test_destroy_ignores_late_dns_addr_callback(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_io_ops_t io = {
        .ctx = &ctx,
        .udp_send = test_udp_send,
        .now_ms = test_now_ms,
        .log = NULL,
        .on_steering_feedback = NULL,
    };
    r_resolver_ops_t resolver = {
        .ctx = &ctx,
        .resolve_srv = test_resolve_srv,
        .resolve_addrs = test_resolve_addrs_async,
        .cancel = test_cancel,
    };
    r_client_t *client = make_client_with_ops(&ctx, &io, &resolver);
    assert(ctx.pending_addr_cb != NULL);
    assert(ctx.pending_addr_user != NULL);

    r_client_destroy(client);
    assert(ctx.cancel_count == 1u);
    assert(ctx.cancelled_ids[0] == 202u);

    r_addr_t addr;
    memset(&addr, 0, sizeof(addr));
    struct sockaddr_in *sin = (struct sockaddr_in *)&addr.sa;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &sin->sin_addr);
    addr.len = sizeof(*sin);
    ctx.pending_addr_cb(ctx.pending_addr_user, 0, &addr, 1);
}

static void test_destroy_handles_dns_cancel_callback(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_io_ops_t io = {
        .ctx = &ctx,
        .udp_send = test_udp_send,
        .now_ms = test_now_ms,
        .log = NULL,
        .on_steering_feedback = NULL,
    };
    r_resolver_ops_t resolver = {
        .ctx = &ctx,
        .resolve_srv = test_resolve_srv,
        .resolve_addrs = test_resolve_addrs_async,
        .cancel = test_cancel_calls_addr_cb,
    };
    r_client_t *client = make_client_with_ops(&ctx, &io, &resolver);
    assert(ctx.pending_addr_cb != NULL);
    assert(ctx.pending_addr_user != NULL);

    r_client_destroy(client);
    assert(ctx.cancel_count == 1u);
    assert(ctx.cancelled_ids[0] == 202u);
    assert(ctx.pending_addr_cb == NULL);
    assert(ctx.pending_addr_user == NULL);
}

static void test_default_policy_replays_once_then_enters_receive_only_phase(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.now_ms = 123456789u;
    r_client_t *client = make_client(&ctx);
    result_cb_ctx_t result;
    memset(&result, 0, sizeof(result));

    r_resource_request_t resource = sample_resource();
    r_client_req_t *req = NULL;
    assert(r_client_check_rate_limit_async(
        client, &resource, 1, NULL, 0, NULL, 0,
        record_rate_limit_cb, &result, &req
    ) == RCLIENT_OK);
    assert(ctx.send_count == 1u);

    uint64_t deadline = 0;
    assert(r_client_request_deadline_ms(req, &deadline) == RCLIENT_OK);
    assert(deadline == 123456789u + 20u);
    ctx.now_ms = deadline;
    assert(r_client_on_timeout(client, req, deadline) == RCLIENT_OK);
    assert(ctx.send_count == 2u);
    assert(result.calls == 0);

    assert(r_client_request_deadline_ms(req, &deadline) == RCLIENT_OK);
    assert(deadline == 123456789u + 40u);
    ctx.now_ms = deadline;
    assert(r_client_on_timeout(client, req, deadline) == RCLIENT_OK);
    assert(ctx.send_count == 2u);
    assert(result.calls == 0);

    assert(r_client_request_deadline_ms(req, &deadline) == RCLIENT_OK);
    assert(deadline == 123456789u + 60u);
    ctx.now_ms = deadline;
    assert(r_client_on_timeout(client, req, deadline) == RCLIENT_OK);
    assert(result.calls == 1);
    assert(result.status == RCLIENT_ERR_TIMEOUT);

    r_client_destroy(client);
}

static void test_default_policy_late_timers_remain_bounded_by_dedup_ttl(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.now_ms = 123456789u;
    r_client_t *client = make_client(&ctx);
    result_cb_ctx_t result;
    memset(&result, 0, sizeof(result));

    r_resource_request_t resource = sample_resource();
    r_client_req_t *req = NULL;
    assert(r_client_check_rate_limit_async(
        client, &resource, 1, NULL, 0, NULL, 0,
        record_rate_limit_cb, &result, &req
    ) == RCLIENT_OK);

    uint64_t deadline = 0;
    assert(r_client_request_deadline_ms(req, &deadline) == RCLIENT_OK);
    assert(deadline == 123456789u + 20u);

    ctx.now_ms = 123456789u + 35u;
    assert(r_client_on_timeout(client, req, ctx.now_ms) == RCLIENT_OK);
    assert(ctx.send_count == 2u);
    assert(r_client_request_deadline_ms(req, &deadline) == RCLIENT_OK);
    assert(deadline == 123456789u + 40u);

    ctx.now_ms = 123456789u + 45u;
    assert(r_client_on_timeout(client, req, ctx.now_ms) == RCLIENT_OK);
    assert(ctx.send_count == 2u);
    assert(r_client_request_deadline_ms(req, &deadline) == RCLIENT_OK);
    assert(deadline == 123456789u + 60u);

    ctx.now_ms = deadline;
    assert(r_client_on_timeout(client, req, deadline) == RCLIENT_OK);
    assert(result.calls == 1);
    assert(result.status == RCLIENT_ERR_TIMEOUT);

    r_client_destroy(client);
}

static void test_default_policy_final_phase_returns_first_valid_without_replay(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_client_t *client = make_client(&ctx);
    result_cb_ctx_t result;
    memset(&result, 0, sizeof(result));
    r_resource_request_t resource = sample_resource();
    r_client_req_t *req = NULL;

    assert(r_client_check_rate_limit_async(
        client, &resource, 1, NULL, 0, NULL, 0,
        record_rate_limit_cb, &result, &req
    ) == RCLIENT_OK);

    r_tenant_header_t request_tenant;
    size_t pos = 0;
    assert(r_parse_tenant_header(
        ctx.last_packet, ctx.last_packet_len, &request_tenant, &pos
    ) == RCLIENT_OK);

    uint64_t deadline = 0;
    assert(r_client_request_deadline_ms(req, &deadline) == RCLIENT_OK);
    assert(r_client_on_timeout(client, req, deadline) == RCLIENT_OK);
    assert(ctx.send_count == 2u);
    assert(r_client_request_deadline_ms(req, &deadline) == RCLIENT_OK);
    assert(r_client_on_timeout(client, req, deadline) == RCLIENT_OK);
    assert(ctx.send_count == 2u);

    uint8_t response[R_MAX_PACKET_SIZE];
    size_t response_len = build_cookie_success_response(
        request_tenant.unique_id, response, sizeof(response));
    r_addr_t from;
    fill_loopback_addr(&from);
    assert(r_client_on_datagram(client, response, response_len, &from) == RCLIENT_OK);
    assert(result.calls == 1);
    assert(result.status == RCLIENT_OK);
    assert(result.server_id == 1u);
    assert(ctx.send_count == 2u);

    r_client_destroy(client);
}

static void test_default_policy_waits_for_oldest_and_returns_best_at_deadline(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_client_t *client = make_two_server_client(&ctx);
    result_cb_ctx_t result;
    memset(&result, 0, sizeof(result));
    r_resource_request_t resource = sample_resource();
    r_client_req_t *req = NULL;
    assert(r_client_check_rate_limit_async(
        client, &resource, 1, NULL, 0, NULL, 0,
        record_rate_limit_cb, &result, &req
    ) == RCLIENT_OK);
    assert(ctx.send_count == 2u);

    r_tenant_header_t request_tenant;
    size_t pos = 0;
    assert(r_parse_tenant_header(
        ctx.last_packet, ctx.last_packet_len, &request_tenant, &pos
    ) == RCLIENT_OK);
    uint8_t response[R_MAX_PACKET_SIZE];
    size_t response_len = build_cookie_success_response_from(
        2u, request_tenant.unique_id, response, sizeof(response));
    r_addr_t from;
    fill_loopback_addr(&from);
    assert(r_client_on_datagram(client, response, response_len, &from) == RCLIENT_OK);
    assert(result.calls == 0);

    uint64_t deadline = 0;
    assert(r_client_request_deadline_ms(req, &deadline) == RCLIENT_OK);
    assert(r_client_on_timeout(client, req, deadline) == RCLIENT_OK);
    assert(result.calls == 1);
    assert(result.status == RCLIENT_OK);
    assert(result.server_id == 2u);
    assert(ctx.send_count == 3u);
    assert_ipv4_addr(&ctx.sent_to[2], "127.0.0.1");
    r_client_destroy(client);
}

static void test_default_policy_returns_oldest_immediately(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_client_t *client = make_two_server_client(&ctx);
    result_cb_ctx_t result;
    memset(&result, 0, sizeof(result));
    r_resource_request_t resource = sample_resource();
    r_client_req_t *req = NULL;
    assert(r_client_check_rate_limit_async(
        client, &resource, 1, NULL, 0, NULL, 0,
        record_rate_limit_cb, &result, &req
    ) == RCLIENT_OK);
    r_tenant_header_t request_tenant;
    size_t pos = 0;
    assert(r_parse_tenant_header(
        ctx.last_packet, ctx.last_packet_len, &request_tenant, &pos
    ) == RCLIENT_OK);
    r_addr_t from;
    fill_loopback_addr(&from);
    uint8_t response[R_MAX_PACKET_SIZE];
    size_t response_len = build_cookie_success_response_from(
        2u, request_tenant.unique_id, response, sizeof(response));
    assert(r_client_on_datagram(client, response, response_len, &from) == RCLIENT_OK);
    assert(result.calls == 0);
    response_len = build_cookie_success_response_from(
        1u, request_tenant.unique_id, response, sizeof(response));
    assert(r_client_on_datagram(client, response, response_len, &from) == RCLIENT_OK);
    assert(result.calls == 1);
    assert(result.status == RCLIENT_OK);
    assert(result.server_id == 1u);
    r_client_destroy(client);
}

static void test_default_policy_rejects_ttl_above_credential_limit(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_request_policy_t policy;
    r_client_default_request_policy(&policy);
    policy.unit_ms = 101u;
    r_client_t *client = make_client_with_policy(&ctx, &policy);
    r_resource_request_t resource = sample_resource();
    assert(r_client_check_rate_limit_async(
        client, &resource, 1, NULL, 0, NULL, 0,
        noop_rate_limit_cb, NULL, NULL
    ) == RCLIENT_ERR_CONFIG);
    assert(ctx.send_count == 0u);
    r_client_destroy(client);
}

static void set_ha_schedule(
    r_ha_schedule_t *schedule,
    r_ha_schedule_kind_t kind,
    uint32_t initial_units,
    uint32_t max_units,
    uint32_t growth
) {
    memset(schedule, 0, sizeof(*schedule));
    schedule->kind = kind;
    schedule->initial_units = initial_units;
    schedule->max_units = max_units;
    if (kind == R_HA_SCHEDULE_LINEAR) {
        schedule->growth.linear_step_units = growth;
    } else if (kind == R_HA_SCHEDULE_EXPONENTIAL) {
        schedule->growth.exponential_factor = growth;
    }
}

static uint32_t last_cookie_request_ttl(const test_ctx_t *ctx) {
    size_t pdu_pos = R_TENANT_TLV_LEN + 4u + 32u;
    assert(ctx->last_packet_len >= pdu_pos + R_PDU_HEADER_LEN);
    assert(
        ((uint16_t)ctx->last_packet[pdu_pos]
            | ((uint16_t)ctx->last_packet[pdu_pos + 1] << 8))
        == R_PDU_RATE_REQUEST
    );
    return read_le32(ctx->last_packet + pdu_pos + 4u);
}

static void test_exponential_schedule_uses_absolute_deadlines(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.now_ms = 1000u;

    r_request_policy_t policy;
    r_client_default_request_policy(&policy);
    policy.unit_ms = 10u;
    policy.replay_count = 2u;
    set_ha_schedule(
        &policy.replay_gap,
        R_HA_SCHEDULE_EXPONENTIAL,
        1u,
        4u,
        2u
    );
    policy.final_receive_units = 1u;
    policy.completion_delivery = false;

    r_client_t *client = make_client_with_policy(&ctx, &policy);
    result_cb_ctx_t result = {0};
    r_client_req_t *req = start_sample_request(client, &result);
    assert(ctx.send_count == 1u);
    assert(last_cookie_request_ttl(&ctx) == 80u);

    const uint64_t expected_deadlines[] = {1010u, 1030u, 1070u, 1080u};
    for (size_t i = 0; i < 4u; i++) {
        uint64_t deadline = 0;
        assert(r_client_request_deadline_ms(req, &deadline) == RCLIENT_OK);
        assert(deadline == expected_deadlines[i]);
        ctx.now_ms = deadline;
        assert(r_client_on_timeout(client, req, deadline) == RCLIENT_OK);
        if (i < 2u) {
            assert(ctx.send_count == i + 2u);
        } else {
            assert(ctx.send_count == 3u);
        }
    }
    assert(result.calls == 1);
    assert(result.status == RCLIENT_ERR_TIMEOUT);
    r_client_destroy(client);
}

static void test_request_profile_reports_wait_round_and_final_phase(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.now_ms = 1000u;

    r_request_policy_t policy;
    r_client_default_request_policy(&policy);
    policy.unit_ms = 25u;
    policy.replay_count = 3u;
    policy.completion_delivery = false;

    profile_cb_ctx_t profile = {0};
    r_client_t *client = make_client_with_policy_and_profile(
        &ctx,
        &policy,
        &profile
    );
    result_cb_ctx_t result = {0};
    r_client_req_t *request = start_sample_request(client, &result);

    const uint64_t expected_deadlines[] = {
        1025u,
        1050u,
        1075u,
        1100u,
        1125u,
    };
    for (size_t index = 0u;
            index < sizeof(expected_deadlines) / sizeof(expected_deadlines[0]);
            index++) {
        uint64_t deadline = 0u;
        assert(r_client_request_deadline_ms(request, &deadline) == RCLIENT_OK);
        assert(deadline == expected_deadlines[index]);
        ctx.now_ms = deadline;
        assert(r_client_on_timeout(client, request, deadline) == RCLIENT_OK);
    }

    assert(result.calls == 1);
    assert(result.status == RCLIENT_ERR_TIMEOUT);
    assert(profile.calls == 1);
    assert(profile.profile.wait_ms == 125u);
    assert(profile.profile.round == 3u);
    assert(profile.profile.phase == R_REQUEST_COMPLETION_FINAL_RECEIVE);
    assert(profile.profile.status == RCLIENT_ERR_TIMEOUT);
    assert(!profile.profile.response_selected);
    r_client_destroy(client);
}

static void test_linear_schedule_has_distinct_gaps(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.now_ms = 2000u;

    r_request_policy_t policy;
    r_client_default_request_policy(&policy);
    policy.unit_ms = 10u;
    policy.replay_count = 2u;
    set_ha_schedule(
        &policy.replay_gap,
        R_HA_SCHEDULE_LINEAR,
        1u,
        3u,
        1u
    );
    policy.final_receive_units = 0u;
    policy.completion_delivery = false;

    r_client_t *client = make_client_with_policy(&ctx, &policy);
    result_cb_ctx_t result = {0};
    r_client_req_t *req = start_sample_request(client, &result);
    assert(last_cookie_request_ttl(&ctx) == 60u);

    const uint64_t expected_deadlines[] = {2010u, 2030u, 2060u};
    for (size_t i = 0; i < 3u; i++) {
        uint64_t deadline = 0;
        assert(r_client_request_deadline_ms(req, &deadline) == RCLIENT_OK);
        assert(deadline == expected_deadlines[i]);
        ctx.now_ms = deadline;
        assert(r_client_on_timeout(client, req, deadline) == RCLIENT_OK);
    }
    assert(ctx.send_count == 3u);
    assert(result.calls == 1);
    assert(result.status == RCLIENT_ERR_TIMEOUT);
    r_client_destroy(client);
}

static void test_round_zero_waits_for_oldest_server_until_round_deadline(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.now_ms = 3000u;

    r_request_policy_t policy;
    r_client_default_request_policy(&policy);
    policy.unit_ms = 10u;
    policy.replay_count = 0u;
    set_ha_schedule(
        &policy.replay_gap,
        R_HA_SCHEDULE_FIXED,
        5u,
        5u,
        0u
    );
    policy.final_receive_units = 0u;
    policy.completion_delivery = false;

    r_client_t *client = make_two_server_client_with_policy(&ctx, &policy);
    result_cb_ctx_t result = {0};
    r_client_req_t *req = start_sample_request(client, &result);
    uint8_t request_id[16];
    copy_last_request_id(&ctx, request_id);

    ctx.now_ms = 3005u;
    uint8_t response[R_MAX_PACKET_SIZE];
    size_t response_len = build_cookie_success_response_from(
        2u, request_id, response, sizeof(response)
    );
    r_addr_t from;
    fill_ipv4_addr(&from, "127.0.0.2");
    assert(r_client_on_datagram(
        client, response, response_len, &from
    ) == RCLIENT_OK);
    assert(result.calls == 0);

    uint64_t deadline = 0;
    assert(r_client_request_deadline_ms(req, &deadline) == RCLIENT_OK);
    assert(deadline == 3050u);
    ctx.now_ms = deadline;
    assert(r_client_on_timeout(client, req, deadline) == RCLIENT_OK);
    assert(result.calls == 1);
    assert(result.server_id == 2u);
    r_client_destroy(client);
}

static void test_replay_round_any_server_completes_immediately(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.now_ms = 4000u;

    r_request_policy_t policy;
    r_client_default_request_policy(&policy);
    policy.unit_ms = 10u;
    policy.replay_count = 1u;
    set_ha_schedule(
        &policy.replay_gap,
        R_HA_SCHEDULE_FIXED,
        2u,
        2u,
        0u
    );
    policy.final_receive_units = 0u;
    policy.completion_delivery = false;

    r_client_t *client = make_two_server_client_with_policy(&ctx, &policy);
    result_cb_ctx_t result = {0};
    r_client_req_t *req = start_sample_request(client, &result);

    ctx.now_ms = 4020u;
    assert(r_client_on_timeout(client, req, 4020u) == RCLIENT_OK);
    assert(result.calls == 0);

    uint8_t request_id[16];
    copy_last_request_id(&ctx, request_id);

    ctx.now_ms = 4025u;
    uint8_t response[R_MAX_PACKET_SIZE];
    size_t response_len = build_cookie_success_response_from(
        2u, request_id, response, sizeof(response)
    );
    r_addr_t from;
    fill_ipv4_addr(&from, "127.0.0.2");
    assert(r_client_on_datagram(
        client, response, response_len, &from
    ) == RCLIENT_OK);
    assert(result.calls == 1);
    assert(result.server_id == 2u);
    r_client_destroy(client);
}

static void test_completion_delivery_covers_allow_and_deny(void) {
    for (size_t denied = 0; denied < 2u; denied++) {
        test_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.now_ms = 5000u + denied * 100u;
        r_client_t *client = make_two_server_client(&ctx);
        result_cb_ctx_t result = {0};
        (void)start_sample_request(client, &result);
        uint8_t request_id[16];
        copy_last_request_id(&ctx, request_id);

        uint8_t response[R_MAX_PACKET_SIZE];
        size_t response_len = denied
            ? build_cookie_denied_response_from(
                1u, request_id, response, sizeof(response))
            : build_cookie_success_response_from(
                1u, request_id, response, sizeof(response));
        r_addr_t from;
        fill_ipv4_addr(&from, "127.0.0.1");
        assert(r_client_on_datagram(
            client, response, response_len, &from
        ) == RCLIENT_OK);
        assert(result.calls == 1);
        assert(result.server_id == 1u);
        assert(result.success == !denied);
        assert(ctx.send_count == 3u);
        assert_ipv4_addr(&ctx.sent_to[2], "127.0.0.2");

        uint8_t resent_id[16];
        copy_last_request_id(&ctx, resent_id);
        assert(memcmp(resent_id, request_id, sizeof(request_id)) == 0);
        r_client_destroy(client);
    }
}

static void test_completion_delivery_can_be_disabled(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_request_policy_t policy;
    r_client_default_request_policy(&policy);
    policy.completion_delivery = false;
    r_client_t *client = make_two_server_client_with_policy(&ctx, &policy);
    result_cb_ctx_t result = {0};
    (void)start_sample_request(client, &result);
    uint8_t request_id[16];
    copy_last_request_id(&ctx, request_id);

    uint8_t response[R_MAX_PACKET_SIZE];
    size_t response_len = build_cookie_success_response_from(
        1u, request_id, response, sizeof(response)
    );
    r_addr_t from;
    fill_ipv4_addr(&from, "127.0.0.1");
    assert(r_client_on_datagram(
        client, response, response_len, &from
    ) == RCLIENT_OK);
    assert(result.calls == 1);
    assert(ctx.send_count == 2u);
    r_client_destroy(client);
}

static void test_final_phase_completion_delivery(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.now_ms = 6000u;
    r_client_t *client = make_two_server_client(&ctx);
    result_cb_ctx_t result = {0};
    r_client_req_t *req = start_sample_request(client, &result);
    uint8_t request_id[16];
    copy_last_request_id(&ctx, request_id);
    assert(ctx.send_count == 2u);

    uint64_t deadline = 0;
    assert(r_client_request_deadline_ms(req, &deadline) == RCLIENT_OK);
    ctx.now_ms = deadline;
    assert(r_client_on_timeout(client, req, deadline) == RCLIENT_OK);
    assert(ctx.send_count == 4u);
    assert(r_client_request_deadline_ms(req, &deadline) == RCLIENT_OK);
    ctx.now_ms = deadline;
    assert(r_client_on_timeout(client, req, deadline) == RCLIENT_OK);
    assert(ctx.send_count == 4u);

    ctx.now_ms += 1u;
    uint8_t response[R_MAX_PACKET_SIZE];
    size_t response_len = build_cookie_success_response_from(
        2u, request_id, response, sizeof(response)
    );
    r_addr_t from;
    fill_ipv4_addr(&from, "127.0.0.2");
    assert(r_client_on_datagram(
        client, response, response_len, &from
    ) == RCLIENT_OK);
    assert(result.calls == 1);
    assert(result.server_id == 2u);
    assert(ctx.send_count == 5u);
    assert_ipv4_addr(&ctx.sent_to[4], "127.0.0.1");
    r_client_destroy(client);
}

static void test_completion_delivery_uses_server_identity(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_client_t *client = make_multiple_address_client(&ctx);
    result_cb_ctx_t result = {0};
    (void)start_sample_request(client, &result);
    assert(ctx.send_count == 3u);
    uint8_t request_id[16];
    copy_last_request_id(&ctx, request_id);

    uint8_t response[R_MAX_PACKET_SIZE];
    size_t response_len = build_cookie_success_response_from(
        1u, request_id, response, sizeof(response)
    );
    r_addr_t from;
    fill_ipv4_addr(&from, "127.0.0.11");
    assert(r_client_on_datagram(
        client, response, response_len, &from
    ) == RCLIENT_OK);
    assert(result.calls == 1);
    assert(ctx.send_count == 4u);
    assert_ipv4_addr(&ctx.sent_to[3], "127.0.0.2");
    r_client_destroy(client);
}

static void test_completion_send_failure_keeps_result(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_client_t *client = make_two_server_client(&ctx);
    result_cb_ctx_t result = {0};
    (void)start_sample_request(client, &result);
    uint8_t request_id[16];
    copy_last_request_id(&ctx, request_id);
    ctx.fail_send_number = 3u;

    uint8_t response[R_MAX_PACKET_SIZE];
    size_t response_len = build_cookie_success_response_from(
        1u, request_id, response, sizeof(response)
    );
    r_addr_t from;
    fill_ipv4_addr(&from, "127.0.0.1");
    assert(r_client_on_datagram(
        client, response, response_len, &from
    ) == RCLIENT_OK);
    assert(ctx.send_count == 3u);
    assert(result.calls == 1);
    assert(result.status == RCLIENT_OK);
    assert(result.success);
    r_client_destroy(client);
}

static void test_late_response_cannot_change_timeout(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.now_ms = 8000u;
    r_request_policy_t policy;
    r_client_default_request_policy(&policy);
    policy.unit_ms = 10u;
    policy.replay_count = 0u;
    policy.final_receive_units = 0u;

    r_client_t *client = make_two_server_client_with_policy(&ctx, &policy);
    result_cb_ctx_t result = {0};
    r_client_req_t *req = start_sample_request(client, &result);
    uint8_t request_id[16];
    copy_last_request_id(&ctx, request_id);

    ctx.now_ms = 8011u;
    uint8_t response[R_MAX_PACKET_SIZE];
    size_t response_len = build_cookie_success_response_from(
        1u, request_id, response, sizeof(response)
    );
    r_addr_t from;
    fill_ipv4_addr(&from, "127.0.0.1");
    assert(r_client_on_datagram(
        client, response, response_len, &from
    ) == RCLIENT_OK);
    assert(result.calls == 0);
    assert(ctx.send_count == 2u);

    assert(r_client_on_timeout(client, req, ctx.now_ms) == RCLIENT_OK);
    assert(result.calls == 1);
    assert(result.status == RCLIENT_ERR_TIMEOUT);
    assert(ctx.send_count == 2u);
    r_client_destroy(client);
}

static void test_completion_does_not_send_at_deadline(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.now_ms = 9000u;
    r_request_policy_t policy;
    r_client_default_request_policy(&policy);
    policy.unit_ms = 10u;
    policy.replay_count = 0u;
    policy.final_receive_units = 0u;

    r_client_t *client = make_two_server_client_with_policy(&ctx, &policy);
    result_cb_ctx_t result = {0};
    (void)start_sample_request(client, &result);
    uint8_t request_id[16];
    copy_last_request_id(&ctx, request_id);

    ctx.now_ms = 9010u;
    uint8_t response[R_MAX_PACKET_SIZE];
    size_t response_len = build_cookie_success_response_from(
        1u, request_id, response, sizeof(response)
    );
    r_addr_t from;
    fill_ipv4_addr(&from, "127.0.0.1");
    assert(r_client_on_datagram(
        client, response, response_len, &from
    ) == RCLIENT_OK);
    assert(result.calls == 1);
    assert(result.status == RCLIENT_OK);
    assert(ctx.send_count == 2u);
    r_client_destroy(client);
}

static void test_late_timer_does_not_replay_at_dedup_deadline(void) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.now_ms = 10000u;
    r_client_t *client = make_client(&ctx);
    result_cb_ctx_t result = {0};
    r_client_req_t *req = start_sample_request(client, &result);
    assert(ctx.send_count == 1u);

    ctx.now_ms = 10060u;
    assert(r_client_on_timeout(client, req, ctx.now_ms) == RCLIENT_OK);
    assert(ctx.send_count == 1u);
    assert(result.calls == 1);
    assert(result.status == RCLIENT_ERR_TIMEOUT);
    r_client_destroy(client);
}

static void assert_policy_rejected(
    r_request_policy_t *policy
) {
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    r_client_t *client = make_client_with_policy(&ctx, policy);
    r_resource_request_t resource = sample_resource();
    assert(r_client_check_rate_limit_async(
        client,
        &resource,
        1,
        NULL,
        0,
        NULL,
        0,
        noop_rate_limit_cb,
        NULL,
        NULL
    ) == RCLIENT_ERR_CONFIG);
    assert(ctx.send_count == 0u);
    r_client_destroy(client);
}

static void test_policy_rejects_invalid_schedules(void) {
    r_request_policy_t policy;

    r_client_default_request_policy(&policy);
    policy.unit_ms = 0u;
    assert_policy_rejected(&policy);

    r_client_default_request_policy(&policy);
    policy.replay_gap.initial_units = 0u;
    assert_policy_rejected(&policy);

    r_client_default_request_policy(&policy);
    set_ha_schedule(
        &policy.replay_gap,
        R_HA_SCHEDULE_EXPONENTIAL,
        1u,
        2u,
        0u
    );
    assert_policy_rejected(&policy);

    r_client_default_request_policy(&policy);
    policy.unit_ms = 101u;
    assert_policy_rejected(&policy);
}

int main(void) {
    test_client_derives_production_tenant_from_key();
    test_client_preserves_explicit_tenant_override();
    test_client_rejects_explicit_key_metadata_mismatch();
    test_guard_only_requests_allow_and_deny();
    test_empty_requests_complete_locally();
    test_empty_request_callback_may_submit_recursively();
    test_request_shape_pointer_validation();
    test_check_rate_limit_rejects_oversized_guard();
    test_report_latency_filters_oversized_reports();
    test_report_latency_accepts_largest_cookie_batch();
    test_report_latency_rejects_oversized_cookie_batch();
    test_report_latency_rejects_oversized_batch_after_filtering();
    test_report_latency_accepts_largest_aes_batch();
    test_report_latency_rejects_oversized_aes_batch();
    test_report_latency_requires_udp_send();
    test_empty_aes_response_is_rejected_explicitly();
    test_callback_can_cancel_same_request_without_double_free();
    test_destroy_ignores_late_dns_srv_callback();
    test_destroy_ignores_late_dns_addr_callback();
    test_destroy_handles_dns_cancel_callback();
    test_discovery_remains_srv_only();
    test_default_policy_replays_once_then_enters_receive_only_phase();
    test_default_policy_late_timers_remain_bounded_by_dedup_ttl();
    test_default_policy_final_phase_returns_first_valid_without_replay();
    test_default_policy_waits_for_oldest_and_returns_best_at_deadline();
    test_default_policy_returns_oldest_immediately();
    test_default_policy_rejects_ttl_above_credential_limit();
    test_exponential_schedule_uses_absolute_deadlines();
    test_request_profile_reports_wait_round_and_final_phase();
    test_linear_schedule_has_distinct_gaps();
    test_round_zero_waits_for_oldest_server_until_round_deadline();
    test_replay_round_any_server_completes_immediately();
    test_completion_delivery_covers_allow_and_deny();
    test_completion_delivery_can_be_disabled();
    test_final_phase_completion_delivery();
    test_completion_delivery_uses_server_identity();
    test_completion_send_failure_keeps_result();
    test_late_response_cannot_change_timeout();
    test_completion_does_not_send_at_deadline();
    test_late_timer_does_not_replay_at_dedup_deadline();
    test_policy_rejects_invalid_schedules();
    return 0;
}
