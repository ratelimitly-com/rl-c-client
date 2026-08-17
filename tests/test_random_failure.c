#include <assert.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "r_client.h"
#include "r_protocol.h"

static const char TEST_COOKIE_KEY[] =
    "rl-cookie1qypqqqqqqqqqqqqzqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqfgrrulczhg30p";

static bool fail_random;
static size_t send_count;
static uint8_t last_packet[R_MAX_PACKET_SIZE];
static size_t last_packet_length;

static r_client_t *make_client(void);
static void completion(
    void *user,
    r_client_req_t *request,
    int status,
    const r_rate_limit_result_t *result
);

/* Override libcrypto's dynamically linked symbol for deterministic fault injection. */
int RAND_bytes(unsigned char *buffer, int length) {
    if (fail_random) {
        return 0;
    }
    for (int i = 0; i < length; i++) {
        buffer[i] = (unsigned char)(i + 1);
    }
    return 1;
}

static int send_packet(
    void *context,
    const r_addr_t *to,
    const uint8_t *buffer,
    size_t length
) {
    (void)context;
    (void)to;
    assert(length > 0u);
    assert(length <= sizeof(last_packet));
    memcpy(last_packet, buffer, length);
    last_packet_length = length;
    send_count++;
    return 0;
}

static r_resource_request_t resource_request(void) {
    r_resource_request_t resource = {
        .window_size_ms = 1000u,
        .rate_limit = 10u,
        .tokens_requested = 1u,
    };
    memset(resource.bucket_id, 1, sizeof(resource.bucket_id));
    return resource;
}

static void test_successful_id_is_uuid_v4_shaped(void) {
    r_client_t *client = make_client();
    r_resource_request_t resource = resource_request();
    fail_random = false;
    send_count = 0u;
    last_packet_length = 0u;
    r_client_req_t *request = NULL;
    assert(r_client_check_rate_limit_async(
        client,
        &resource,
        1u,
        NULL,
        0u,
        NULL,
        0u,
        completion,
        NULL,
        &request
    ) == RCLIENT_OK);
    assert(request != NULL);
    assert(send_count == 1u);
    r_tenant_header_t tenant = {0};
    size_t position = 0u;
    assert(r_parse_tenant_header(
        last_packet,
        last_packet_length,
        &tenant,
        &position
    ) == RCLIENT_OK);
    assert((tenant.unique_id[6] & 0xf0u) == 0x40u);
    assert((tenant.unique_id[8] & 0xc0u) == 0x80u);
    r_client_cancel_request(client, request);
    r_client_destroy(client);
}

static uint64_t now_ms(void *context) {
    (void)context;
    return 1000u;
}

static int resolve_srv(
    void *context,
    const char *name,
    r_dns_req_id_t *out_request_id,
    r_dns_srv_cb callback,
    void *user
) {
    (void)context;
    (void)name;
    if (out_request_id) {
        *out_request_id = 1u;
    }
    const r_srv_record_t record = {
        .target = "s-1.local",
        .port = 8080u,
        .ttl_ms = 60000u,
    };
    callback(user, 0, &record, 1u);
    return 0;
}

static int resolve_addrs(
    void *context,
    const char *name,
    r_dns_req_id_t *out_request_id,
    r_dns_addr_cb callback,
    void *user
) {
    (void)context;
    (void)name;
    if (out_request_id) {
        *out_request_id = 2u;
    }
    r_addr_t address = {0};
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)&address.sa;
    ipv4->sin_family = AF_INET;
    assert(inet_pton(AF_INET, "127.0.0.1", &ipv4->sin_addr) == 1);
    address.len = sizeof(*ipv4);
    callback(user, 0, &address, 1u);
    return 0;
}

static void completion(
    void *user,
    r_client_req_t *request,
    int status,
    const r_rate_limit_result_t *result
) {
    (void)user;
    (void)request;
    (void)status;
    (void)result;
    assert(false && "RNG failure must not complete an unsubmitted request");
}

static r_client_t *make_client(void) {
    r_client_config_t config = {0};
    config.tenant.dns_name = "example.local";
    config.tenant.auth.secret = TEST_COOKIE_KEY;
    r_io_ops_t io = {
        .udp_send = send_packet,
        .now_ms = now_ms,
    };
    r_resolver_ops_t resolver = {
        .resolve_srv = resolve_srv,
        .resolve_addrs = resolve_addrs,
    };
    r_client_t *client = NULL;
    assert(r_client_create(&config, &io, &resolver, &client) == RCLIENT_OK);
    assert(client != NULL);
    return client;
}

static void test_resource_request_fails_closed(void) {
    r_client_t *client = make_client();
    r_resource_request_t resource = resource_request();

    fail_random = true;
    send_count = 0u;
    r_client_req_t *request = NULL;
    assert(r_client_check_rate_limit_async(
        client,
        &resource,
        1u,
        NULL,
        0u,
        NULL,
        0u,
        completion,
        NULL,
        &request
    ) == RCLIENT_ERR_AUTH);
    assert(request == NULL);
    assert(send_count == 0u);
    fail_random = false;
    r_client_destroy(client);
}

static void test_latency_report_fails_closed(void) {
    r_client_t *client = make_client();
    r_service_latency_report_t report = {
        .observed_latency = 5u,
        .ttl_ms = 1000u,
        .max_samples = 10u,
        .buffer_size = 10u,
        .min_sample_threshold = 1u,
    };
    memset(report.latency_tracker_id, 2, sizeof(report.latency_tracker_id));

    fail_random = true;
    send_count = 0u;
    assert(r_client_report_latency(client, &report, 1u) == RCLIENT_ERR_AUTH);
    assert(send_count == 0u);
    fail_random = false;
    r_client_destroy(client);
}

int main(void) {
    test_successful_id_is_uuid_v4_shaped();
    test_resource_request_fails_closed();
    test_latency_report_fails_closed();
    return 0;
}
