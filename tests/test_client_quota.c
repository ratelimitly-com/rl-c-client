#include <assert.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../include/r_client.h"
#include "../src/r_protocol.h"

static const char *SAMPLE_COOKIE_KEY_TENANT_2 =
    "rl-cookie1qgqqqqqqqqqqqqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqfn54mv";

typedef struct test_ctx {
    uint8_t last_packet[R_MAX_PACKET_SIZE];
    size_t last_packet_len;
    size_t send_count;
    r_dns_srv_cb pending_srv_cb;
    void *pending_srv_user;
    r_dns_addr_cb pending_addr_cb;
    void *pending_addr_user;
    r_dns_req_id_t cancelled_ids[8];
    size_t cancel_count;
} test_ctx_t;

static int test_udp_send(void *ctx, const r_addr_t *to, const uint8_t *buf, size_t len) {
    (void)to;
    test_ctx_t *test = (test_ctx_t *)ctx;
    assert(len <= sizeof(test->last_packet));
    memcpy(test->last_packet, buf, len);
    test->last_packet_len = len;
    test->send_count += 1;
    return 0;
}

static uint64_t test_now_ms(void *ctx) {
    (void)ctx;
    return 123456789u;
}

static int test_resolve_srv(
    void *ctx,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_srv_cb cb,
    void *user
) {
    (void)ctx;
    (void)name;
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
    memcpy(guard.service_id, "guard", 5);
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
    memcpy(reports[0].service_id, "ok", 2);
    reports[0].observed_latency = 10;
    reports[0].ttl_ms = 1000;
    reports[0].max_samples = 10;
    reports[0].buffer_size = 64;
    reports[0].min_sample_threshold = 1;

    memcpy(reports[1].service_id, "drop", 4);
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
    uint16_t service_count = (uint16_t)pdu[8] | ((uint16_t)pdu[9] << 8);
    assert(pdu_type == R_PDU_LATENCY_REPORT);
    assert(service_count == 1u);

    ctx.send_count = 0;
    rc = r_client_report_latency(client, &reports[1], 1);
    assert(rc == RCLIENT_OK);
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
    memcpy(report.service_id, "ok", 2);
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

int main(void) {
    test_check_rate_limit_rejects_oversized_guard();
    test_report_latency_filters_oversized_reports();
    test_report_latency_requires_udp_send();
    test_destroy_ignores_late_dns_srv_callback();
    test_destroy_ignores_late_dns_addr_callback();
    test_destroy_handles_dns_cancel_callback();
    return 0;
}
