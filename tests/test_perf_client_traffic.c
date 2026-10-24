/* Exercise the CLI's real socket hooks, without DNS or production credentials. */
#define main perf_client_program_main
#include "../bin/perf_client.c"
#undef main

#include <assert.h>
#include "../tools/r_test_responder.h"

static void test_socket_traffic(int family) {
    int server = socket(family, SOCK_DGRAM, 0);
    assert(server >= 0);
    r_addr_t address = {0};
    if (family == AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&address.sa;
        v4->sin_family = AF_INET;
        v4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.len = sizeof(*v4);
    } else {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&address.sa;
        v6->sin6_family = AF_INET6;
        v6->sin6_addr = in6addr_loopback;
        address.len = sizeof(*v6);
    }
    assert(bind(server, (struct sockaddr *)&address.sa, address.len) == 0);
    assert(getsockname(server, (struct sockaddr *)&address.sa, &address.len) == 0);

    perf_traffic_t traffic = {0};
    io_ctx_t io = {0};
    io.sockfd = perf_setup_socket(0);
    io.traffic = &traffic;
    assert(io.sockfd >= 0);
    uint8_t request[112] = {0};
    const uint64_t overhead = family == AF_INET ? 28u : 48u;

    /* Every successful send counts, including physical replays / HA sends. */
    for (unsigned i = 0; i < 2; i++) {
        assert(perf_udp_send(&io, &address, request, sizeof(request)) == 0);
        struct pollfd server_ready = {.fd = server, .events = POLLIN};
        assert(poll(&server_ready, 1, 1000) == 1);
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        uint8_t received[112];
        assert(recvfrom(server, received, sizeof(received), 0,
                        (struct sockaddr *)&peer, &peer_len) == 112);
        /* Deliberately invalid protocol data: count before client validation. */
        uint8_t reply[6000] = {0};
        size_t reply_len = i == 0 ? sizeof(reply) : 0u;
        assert(sendto(server, reply, reply_len, 0,
                      (struct sockaddr *)&peer, peer_len) == (ssize_t)reply_len);
        struct pollfd pfd = {.fd = io.sockfd, .events = POLLIN};
        assert(poll(&pfd, 1, 1000) == 1);
        r_addr_t from = {0};
        assert(perf_udp_receive(&io, &from) == (ssize_t)reply_len);
    }
    assert(traffic.sent.datagrams == 2);
    assert(traffic.sent.payload_bytes == 224);
    assert(traffic.sent.ip_bytes == 224 + 2 * overhead);
    assert(traffic.received.datagrams == 2);
    assert(traffic.received.payload_bytes == 6000);
    assert(traffic.received.ip_bytes == 6000 + 2 * overhead);
    assert(!traffic.incomplete);
    r_addr_t from = {0};
    assert(perf_udp_receive(&io, &from) == -1); /* EAGAIN adds no traffic. */
    assert(traffic.received.datagrams == 2);

    close(io.sockfd);
    assert(perf_udp_send(&io, &address, request, sizeof(request)) == -1);
    assert(traffic.send_failures == 1);
    assert(traffic.sent.datagrams == 2);
    assert(traffic.sent.ip_bytes == 224 + 2 * overhead);
    close(server);
}

static void test_worker_aggregation_and_summary(void) {
    perf_traffic_t first = {.sent = {2, 200, 256}, .received = {1, 50, 78}};
    perf_traffic_t second = {.sent = {3, 300, 444}, .received = {2, 100, 196}, .send_failures = 1};
    perf_traffic_merge(&first, &second);
    assert(first.sent.datagrams == 5 && first.sent.payload_bytes == 500 && first.sent.ip_bytes == 700);
    assert(first.received.datagrams == 3 && first.received.payload_bytes == 150 && first.received.ip_bytes == 274);
    assert(first.send_failures == 1 && !first.incomplete);

    FILE *capture = tmpfile();
    assert(capture);
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    assert(saved >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
    perf_print_traffic(&first);
    fflush(stdout);
    assert(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    rewind(capture);
    char output[2048] = {0};
    assert(fread(output, 1, sizeof(output) - 1, capture) > 0);
    fclose(capture);
    assert(strstr(output, "Sent bytes (IP estimate): 700\n"));
    assert(strstr(output, "Received bytes (IP estimate): 274\n"));
    assert(strstr(output, "UDP datagrams sent: 5\n"));
    assert(strstr(output, "UDP datagrams received: 3\n"));
    assert(strstr(output, "UDP send failures: 1\n"));

    first.sent.ip_bytes = UINT64_MAX;
    perf_traffic_merge(&first, &second);
    assert(first.sent.ip_bytes == UINT64_MAX && first.incomplete);

    perf_traffic_t unknown = {0};
    struct sockaddr unsupported = {0};
    unsupported.sa_family = AF_UNSPEC;
    perf_record_traffic(&unknown, false, &unsupported, sizeof(unsupported), 100u);
    assert(unknown.incomplete && unknown.received.ip_bytes == 0);
    perf_traffic_t merged = {0};
    perf_traffic_merge(&merged, &unknown);
    assert(merged.incomplete);
}

typedef struct fixture_server {
    int fd;
    atomic_bool stop;
    r_test_responder_state_t responder;
    uint64_t received_packets, received_ip_bytes;
    uint64_t sent_packets, sent_ip_bytes;
} fixture_server_t;

static void *serve_worker_fixture(void *arg) {
    fixture_server_t *server = arg;
    while (!atomic_load_explicit(&server->stop, memory_order_relaxed)) {
        struct pollfd pfd = {.fd = server->fd, .events = POLLIN};
        int ready = poll(&pfd, 1, 10);
        assert(ready >= 0);
        if (!ready) continue;
        uint8_t request[1400], reply[1400];
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t bytes = recvfrom(server->fd, request, sizeof(request), 0,
                                 (struct sockaddr *)&peer, &peer_len);
        assert(bytes >= 0);
        server->received_packets++;
        server->received_ip_bytes += (uint64_t)bytes + 28u;
        size_t reply_len = 0;
        bool send_response = false;
        r_test_event_t event;
        assert(r_test_responder_process(&server->responder, request, (size_t)bytes,
                                        reply, sizeof(reply), &reply_len,
                                        &send_response, &event) == RCLIENT_OK);
        assert(send_response);
        assert(sendto(server->fd, reply, reply_len, 0,
                      (struct sockaddr *)&peer, peer_len) == (ssize_t)reply_len);
        server->sent_packets++;
        server->sent_ip_bytes += reply_len + 28u;
    }
    return NULL;
}

static void test_completed_workers_match_independent_responder_counts(void) {
    fixture_server_t server = {0};
    atomic_init(&server.stop, false);
    server.fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(server.fd >= 0);
    r_addr_t address = {0};
    struct sockaddr_in *v4 = (struct sockaddr_in *)&address.sa;
    v4->sin_family = AF_INET;
    v4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.len = sizeof(*v4);
    assert(bind(server.fd, (struct sockaddr *)&address.sa, address.len) == 0);
    assert(getsockname(server.fd, (struct sockaddr *)&address.sa, &address.len) == 0);
    assert(r_test_responder_init(&server.responder, R_TEST_SCENARIO_ALLOW,
                                 R_TEST_RESPONDER_AES_KEY, 1u, true, 0u) == RCLIENT_OK);
    char target[] = "s-1.localhost";
    r_srv_record_t record = {.target = target, .port = ntohs(v4->sin_port), .ttl_ms = 60000};
    perf_dns_addr_entry_t entry = {.name = target, .items = &address, .count = 1};
    perf_dns_cache_t cache = {
        .srv_name = "_ratelimitly._udp.rn-test.local", .srv_records = &record, .srv_count = 1,
        .addr_entries = &entry, .addr_entry_count = 1,
    };
    perf_config_t config = {
        .srv_domain = "rn-test.local", .auth_bech32 = R_TEST_RESPONDER_AES_KEY,
        .requests_per_client = 3, .bucket_prefix = "traffic-test", .rate_limit = 1000,
        .unit_ms = 100, .replay_count = 0, .ignore_steering = true, .dns_cache = &cache,
    };
    assert(perf_parse_auth_bech32(config.auth_bech32, &config.auth_type, &config.tenant_id) == 0);
    perf_stats_t stats = {0};
    pthread_t responder;
    assert(pthread_create(&responder, NULL, serve_worker_fixture, &server) == 0);
    perf_traffic_t total = {0};
    for (size_t i = 0; i < 2; i++) {
        worker_ctx_t worker = {.client_id = i, .config = &config, .stats = &stats};
        worker.latency_hist = calloc(PERF_LATENCY_BUCKETS, sizeof(uint64_t));
        assert(worker.latency_hist);
        assert((intptr_t)perf_worker(&worker) == 0);
        assert(worker.worker_error == 0 && !worker.traffic.incomplete);
        perf_traffic_merge(&total, &worker.traffic);
        free(worker.latency_hist);
    }
    atomic_store_explicit(&server.stop, true, memory_order_relaxed);
    assert(pthread_join(responder, NULL) == 0);
    close(server.fd);
    assert(atomic_load(&stats.errors) == 0 && atomic_load(&stats.responses_received) == 6);
    assert(total.sent.datagrams == 6 && total.received.datagrams == 6);
    assert(total.sent.datagrams == server.received_packets);
    assert(total.sent.ip_bytes == server.received_ip_bytes);
    assert(total.received.datagrams == server.sent_packets);
    assert(total.received.ip_bytes == server.sent_ip_bytes);
}

int main(void) {
    test_socket_traffic(AF_INET); /* The perf socket returns IPv4-mapped IPv6 peers. */
    test_socket_traffic(AF_INET6);
    test_worker_aggregation_and_summary();
    test_completed_workers_match_independent_responder_counts();
    puts("test_perf_client_traffic: ok");
    return 0;
}
