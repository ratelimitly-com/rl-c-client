#include <assert.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rl_example.h"

static const char TEST_AES_KEY[] =
    "rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqhmzd8l";

typedef struct test_result {
    int done;
    int status;
    bool allowed;
} test_result_t;

static void on_result(void *user, int status, bool allowed) {
    test_result_t *result = user;
    result->done = 1;
    result->status = status;
    result->allowed = allowed;
}

int main(int argc, char **argv) {
    assert(strcmp(rl_example_status_name(RCLIENT_OK), "ok") == 0);
    assert(strcmp(rl_example_status_name(RCLIENT_ERR_IO), "I/O error") == 0);
    assert(strcmp(rl_example_status_name(RCLIENT_ERR_TIMEOUT), "timeout") == 0);
    assert(strcmp(rl_example_status_name(RCLIENT_ERR_PROTOCOL), "protocol error") == 0);
    assert(strcmp(rl_example_status_name(RCLIENT_ERR_AUTH), "authentication error") == 0);
    assert(strcmp(rl_example_status_name(RCLIENT_ERR_DNS), "DNS error") == 0);
    assert(strcmp(rl_example_status_name(RCLIENT_ERR_CONFIG), "configuration error") == 0);
    assert(strcmp(rl_example_status_name(RCLIENT_ERR_NOMEM), "out of memory") == 0);
    assert(strcmp(rl_example_status_name(-999), "unknown error") == 0);

    assert(argc == 2);
    long port = strtol(argv[1], NULL, 10);
    assert(port > 0 && port <= UINT16_MAX);

    rl_example_options_t options = {
        .tenant_dns_name = "rn-test.local",
        .auth_key = TEST_AES_KEY,
        .server_host = "127.0.0.1",
        .server_port = (uint16_t)port,
    };
    rl_example_client_t client;
    assert(rl_example_client_init(&client, &options) == RCLIENT_OK);

    test_result_t result = {0};
    rl_example_request_t request;
    assert(rl_example_check(&client, &request, "example-test", on_result, &result)
        == RCLIENT_OK);

    while (!result.done) {
        struct pollfd fds[2];
        size_t count = rl_example_socket_count(&client);
        assert(count > 0 && count <= 2);
        for (size_t i = 0; i < count; i++) {
            fds[i].fd = rl_example_socket_at(&client, i);
            fds[i].events = POLLIN;
            fds[i].revents = 0;
        }

        uint64_t wait_ms = 0;
        assert(rl_example_request_delay_ms(&request, &wait_ms) == RCLIENT_OK);
        int timeout = wait_ms > INT32_MAX ? INT32_MAX : (int)wait_ms;
        int ready = poll(fds, (nfds_t)count, timeout);
        assert(ready >= 0);
        if (ready == 0) {
            assert(rl_example_request_on_timeout(&client, &request) == RCLIENT_OK);
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            if ((fds[i].revents & POLLIN) != 0) {
                assert(rl_example_client_on_readable(&client, fds[i].fd) == RCLIENT_OK);
            }
        }
    }

    assert(result.status == RCLIENT_OK);
    assert(result.allowed);
    rl_example_client_destroy(&client);
    return 0;
}
