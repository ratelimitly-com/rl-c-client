#include <arpa/inet.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../include/r_client.h"
#include "../tools/r_test_responder.h"

typedef struct test_context {
    uint8_t packet[2048];
    size_t packet_len;
    size_t send_count;
    size_t callback_count;
    int callback_status;
    bool success;
    bool steering_feedback;
    size_t guard_count;
    size_t resource_count;
    bool first_guard_passed;
    uint16_t first_deficit;
    size_t steering_count;
    bool last_keep_port;
} test_context_t;

static int capture_send(void *user, const r_addr_t *to, const uint8_t *buf, size_t len) {
    (void)to;
    test_context_t *context = (test_context_t *)user;
    assert(len <= sizeof(context->packet));
    memcpy(context->packet, buf, len);
    context->packet_len = len;
    context->send_count++;
    return 0;
}

static uint64_t test_now_ms(void *user) {
    (void)user;
    return 1760000000000u;
}

static void steering_feedback(void *user, bool keep_port) {
    test_context_t *context = (test_context_t *)user;
    context->steering_count++;
    context->last_keep_port = keep_port;
}

static int resolve_srv(
    void *user,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_srv_cb cb,
    void *cb_user
) {
    (void)user;
    assert(strcmp(name, "_ratelimitly._udp.rn-test.local") == 0);
    if (out_req_id) {
        *out_req_id = 1u;
    }
    r_srv_record_t record = {
        .target = "s-1.localhost",
        .port = 39080u,
        .priority = 0u,
        .weight = 0u,
        .ttl_ms = 60000u,
    };
    cb(cb_user, 0, &record, 1u);
    return 0;
}

static int resolve_addrs(
    void *user,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_addr_cb cb,
    void *cb_user
) {
    (void)user;
    assert(strcmp(name, "s-1.localhost") == 0);
    if (out_req_id) {
        *out_req_id = 2u;
    }
    r_addr_t address;
    memset(&address, 0, sizeof(address));
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)&address.sa;
    ipv4->sin_family = AF_INET;
    ipv4->sin_port = htons(39080u);
    assert(inet_pton(AF_INET, "127.0.0.1", &ipv4->sin_addr) == 1);
    address.len = sizeof(*ipv4);
    cb(cb_user, 0, &address, 1u);
    return 0;
}

static void rate_callback(
    void *user,
    r_client_req_t *request,
    int status,
    const r_rate_limit_result_t *result
) {
    (void)request;
    test_context_t *context = (test_context_t *)user;
    context->callback_count++;
    context->callback_status = status;
    if (!result) {
        return;
    }
    context->success = result->success;
    context->steering_feedback = result->steering_feedback;
    context->guard_count = result->guard_count;
    context->resource_count = result->resource_count;
    if (result->guard_count > 0u) {
        context->first_guard_passed = result->guards[0].passed;
    }
    if (result->resource_count > 0u) {
        context->first_deficit = result->resources[0].tokens_deficit;
    }
}

static r_client_t *create_client(test_context_t *context, const char *auth_key) {
    r_auth_key_info_t key;
    memset(&key, 0, sizeof(key));
    assert(r_client_parse_auth_key(auth_key, &key) == RCLIENT_OK);

    r_io_ops_t io = {
        .ctx = context,
        .udp_send = capture_send,
        .now_ms = test_now_ms,
        .log = NULL,
        .on_steering_feedback = steering_feedback,
    };
    r_resolver_ops_t resolver = {
        .ctx = context,
        .resolve_srv = resolve_srv,
        .resolve_addrs = resolve_addrs,
        .cancel = NULL,
    };
    r_request_policy_t policy;
    r_client_default_request_policy(&policy);
    policy.wait = R_WAIT_RETURN_ON_FIRST_VALID;

    r_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.tenant.dns_name = "rn-test.local";
    config.tenant.key_id = key.key_id;
    config.tenant.auth.type = key.type;
    config.tenant.auth.secret = auth_key;
    config.request_policy = &policy;

    r_client_t *client = NULL;
    assert(r_client_create(&config, &io, &resolver, &client) == RCLIENT_OK);
    assert(client != NULL);
    return client;
}

static void fill_source_address(r_addr_t *address) {
    memset(address, 0, sizeof(*address));
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)&address->sa;
    ipv4->sin_family = AF_INET;
    ipv4->sin_port = htons(39080u);
    assert(inet_pton(AF_INET, "127.0.0.1", &ipv4->sin_addr) == 1);
    address->len = sizeof(*ipv4);
}

static r_client_req_t *submit_request(r_client_t *client, test_context_t *context) {
    r_resource_request_t resource;
    memset(&resource, 0, sizeof(resource));
    memcpy(resource.bucket_id, "bucket", 6u);
    resource.window_size_ms = 1000u;
    resource.rate_limit = 10u;
    resource.tokens_requested = 1u;

    r_latency_guard_t guard;
    memset(&guard, 0, sizeof(guard));
    memcpy(guard.service_id, "service", 7u);
    guard.threshold_ms = 50u;
    guard.ttl_ms = 1000u;
    guard.max_samples = 10u;
    guard.buffer_size = 16u;
    guard.min_sample_threshold = 1u;

    context->packet_len = 0u;
    r_client_req_t *request = NULL;
    assert(r_client_check_rate_limit_async(
        client,
        &resource,
        1u,
        &guard,
        1u,
        "api",
        0u,
        rate_callback,
        context,
        &request
    ) == RCLIENT_OK);
    assert(request != NULL);
    assert(context->packet_len > 0u);
    return request;
}

static int process_captured_request(
    r_test_responder_state_t *state,
    test_context_t *context,
    uint8_t response[1200],
    size_t *response_len,
    bool *send_response,
    r_test_event_t *event
) {
    return r_test_responder_process(
        state,
        context->packet,
        context->packet_len,
        response,
        1200u,
        response_len,
        send_response,
        event
    );
}

static void run_valid_response_case(
    const char *auth_key,
    r_test_scenario_t scenario,
    bool keep_port,
    size_t expected_guards,
    size_t expected_resources,
    bool expected_success
) {
    test_context_t context;
    memset(&context, 0, sizeof(context));
    r_client_t *client = create_client(&context, auth_key);
    r_client_req_t *request = submit_request(client, &context);

    r_test_responder_state_t state;
    assert(r_test_responder_init(
        &state,
        scenario,
        auth_key,
        1u,
        keep_port,
        1u
    ) == RCLIENT_OK);
    uint8_t response[1200];
    size_t response_len = 0u;
    bool send_response = false;
    r_test_event_t event;
    assert(process_captured_request(
        &state,
        &context,
        response,
        &response_len,
        &send_response,
        &event
    ) == RCLIENT_OK);
    assert(send_response);
    assert(response_len > 0u);
    assert(event.kind == R_TEST_EVENT_RATE_REQUEST);
    assert(event.guard_count == 1u);
    assert(event.resource_count == 1u);
    assert(strcmp(event.label, "api") == 0);

    r_addr_t source;
    fill_source_address(&source);
    assert(r_client_on_datagram(client, response, response_len, &source) == RCLIENT_OK);
    assert(context.callback_count == 1u);
    assert(context.callback_status == RCLIENT_OK);
    assert(context.guard_count == expected_guards);
    assert(context.resource_count == expected_resources);
    assert(context.success == expected_success);
    assert(context.steering_feedback == keep_port);
    if (!keep_port) {
        assert(context.steering_count == 1u);
        assert(!context.last_keep_port);
    }

    (void)request;
    r_client_destroy(client);
}

static void test_valid_scenarios(void) {
    run_valid_response_case(
        R_TEST_RESPONDER_AES_KEY,
        R_TEST_SCENARIO_ALLOW,
        true,
        1u,
        1u,
        true
    );
    run_valid_response_case(
        R_TEST_RESPONDER_COOKIE_KEY,
        R_TEST_SCENARIO_ALLOW,
        true,
        1u,
        1u,
        true
    );
    run_valid_response_case(
        R_TEST_RESPONDER_AES_KEY,
        R_TEST_SCENARIO_DENY,
        true,
        1u,
        1u,
        false
    );
    run_valid_response_case(
        R_TEST_RESPONDER_AES_KEY,
        R_TEST_SCENARIO_GUARD_PASS,
        true,
        1u,
        1u,
        true
    );
    run_valid_response_case(
        R_TEST_RESPONDER_AES_KEY,
        R_TEST_SCENARIO_GUARD_DENY,
        true,
        1u,
        1u,
        false
    );
    run_valid_response_case(
        R_TEST_RESPONDER_AES_KEY,
        R_TEST_SCENARIO_COUNT_EMPTY,
        true,
        0u,
        0u,
        true
    );
    run_valid_response_case(
        R_TEST_RESPONDER_AES_KEY,
        R_TEST_SCENARIO_COUNT_SHORT,
        true,
        1u,
        0u,
        true
    );
    run_valid_response_case(
        R_TEST_RESPONDER_AES_KEY,
        R_TEST_SCENARIO_COUNT_EXTRA,
        true,
        1u,
        2u,
        false
    );
    run_valid_response_case(
        R_TEST_RESPONDER_AES_KEY,
        R_TEST_SCENARIO_ALLOW,
        false,
        1u,
        1u,
        true
    );
}

static void test_quota_scenario(void) {
    test_context_t context;
    memset(&context, 0, sizeof(context));
    r_client_t *client = create_client(&context, R_TEST_RESPONDER_AES_KEY);
    r_test_responder_state_t state;
    assert(r_test_responder_init(
        &state,
        R_TEST_SCENARIO_QUOTA,
        R_TEST_RESPONDER_AES_KEY,
        1u,
        true,
        1u
    ) == RCLIENT_OK);

    for (size_t attempt = 0u; attempt < 2u; attempt++) {
        context.callback_count = 0u;
        r_client_req_t *request = submit_request(client, &context);
        uint8_t response[1200];
        size_t response_len = 0u;
        bool send_response = false;
        r_test_event_t event;
        assert(process_captured_request(
            &state,
            &context,
            response,
            &response_len,
            &send_response,
            &event
        ) == RCLIENT_OK);
        assert(send_response);
        r_addr_t source;
        fill_source_address(&source);
        assert(r_client_on_datagram(client, response, response_len, &source) == RCLIENT_OK);
        assert(context.callback_count == 1u);
        assert(context.success == (attempt == 0u));
        (void)request;
    }
    r_client_destroy(client);
}

static void test_no_response_and_malformed_scenarios(void) {
    const r_test_scenario_t scenarios[] = {
        R_TEST_SCENARIO_DROP,
        R_TEST_SCENARIO_MALFORMED_AUTH,
        R_TEST_SCENARIO_MALFORMED_TRUNCATED,
        R_TEST_SCENARIO_MALFORMED_REQUEST_ID,
    };
    for (size_t i = 0u; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        test_context_t context;
        memset(&context, 0, sizeof(context));
        r_client_t *client = create_client(&context, R_TEST_RESPONDER_AES_KEY);
        r_client_req_t *request = submit_request(client, &context);
        r_test_responder_state_t state;
        assert(r_test_responder_init(
            &state,
            scenarios[i],
            R_TEST_RESPONDER_AES_KEY,
            1u,
            true,
            1u
        ) == RCLIENT_OK);
        uint8_t response[1200];
        size_t response_len = 0u;
        bool send_response = false;
        r_test_event_t event;
        assert(process_captured_request(
            &state,
            &context,
            response,
            &response_len,
            &send_response,
            &event
        ) == RCLIENT_OK);
        assert(event.kind == R_TEST_EVENT_RATE_REQUEST);
        if (scenarios[i] == R_TEST_SCENARIO_DROP) {
            assert(!send_response);
        } else {
            assert(send_response);
            r_addr_t source;
            fill_source_address(&source);
            int rc = r_client_on_datagram(client, response, response_len, &source);
            if (scenarios[i] == R_TEST_SCENARIO_MALFORMED_REQUEST_ID) {
                assert(rc == RCLIENT_OK);
            } else {
                assert(rc == RCLIENT_ERR_AUTH || rc == RCLIENT_ERR_PROTOCOL);
            }
        }
        assert(context.callback_count == 0u);
        r_client_cancel_request(client, request);
        r_client_destroy(client);
    }
}

static void test_latency_report_observation(void) {
    test_context_t context;
    memset(&context, 0, sizeof(context));
    r_client_t *client = create_client(&context, R_TEST_RESPONDER_AES_KEY);
    r_service_latency_report_t report;
    memset(&report, 0, sizeof(report));
    memcpy(report.service_id, "service", 7u);
    report.observed_latency = 25u;
    report.ttl_ms = 1000u;
    report.max_samples = 10u;
    report.buffer_size = 16u;
    report.min_sample_threshold = 1u;
    assert(r_client_report_latency(client, &report, 1u) == RCLIENT_OK);

    r_test_responder_state_t state;
    assert(r_test_responder_init(
        &state,
        R_TEST_SCENARIO_ALLOW,
        R_TEST_RESPONDER_AES_KEY,
        1u,
        true,
        1u
    ) == RCLIENT_OK);
    uint8_t response[1200];
    size_t response_len = 0u;
    bool send_response = true;
    r_test_event_t event;
    assert(process_captured_request(
        &state,
        &context,
        response,
        &response_len,
        &send_response,
        &event
    ) == RCLIENT_OK);
    assert(event.kind == R_TEST_EVENT_LATENCY_REPORT);
    assert(event.report_count == 1u);
    assert(!send_response);
    assert(response_len == 0u);
    r_client_destroy(client);
}

static void test_rejects_bad_input(void) {
    test_context_t context;
    memset(&context, 0, sizeof(context));
    r_client_t *client = create_client(&context, R_TEST_RESPONDER_AES_KEY);
    r_client_req_t *request = submit_request(client, &context);
    context.packet[55] ^= 0x01u;

    r_test_responder_state_t state;
    assert(r_test_responder_init(
        &state,
        R_TEST_SCENARIO_ALLOW,
        R_TEST_RESPONDER_AES_KEY,
        1u,
        true,
        1u
    ) == RCLIENT_OK);
    uint8_t response[1200];
    size_t response_len = 0u;
    bool send_response = true;
    r_test_event_t event;
    assert(process_captured_request(
        &state,
        &context,
        response,
        &response_len,
        &send_response,
        &event
    ) == RCLIENT_ERR_AUTH);
    assert(event.kind == R_TEST_EVENT_INPUT_REJECTED);
    assert(!send_response);
    r_client_cancel_request(client, request);
    r_client_destroy(client);
}

static void test_scenario_parser(void) {
    for (int value = R_TEST_SCENARIO_ALLOW; value <= R_TEST_SCENARIO_COUNT_EXTRA; value++) {
        r_test_scenario_t parsed = R_TEST_SCENARIO_ALLOW;
        const char *name = r_test_scenario_name((r_test_scenario_t)value);
        assert(r_test_scenario_parse(name, &parsed) == RCLIENT_OK);
        assert(parsed == (r_test_scenario_t)value);
    }
    r_test_scenario_t parsed = R_TEST_SCENARIO_ALLOW;
    assert(r_test_scenario_parse("invalid", &parsed) == RCLIENT_ERR_CONFIG);
}

int main(void) {
    test_scenario_parser();
    test_valid_scenarios();
    test_quota_scenario();
    test_no_response_and_malformed_scenarios();
    test_latency_report_observation();
    test_rejects_bad_input();
    return 0;
}
