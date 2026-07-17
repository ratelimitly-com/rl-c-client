#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

static const char TEST_AES_KEY[] =
    "rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqhmzd8l";

typedef struct test_completion {
    bool done;
    int status;
    r_admission_outcome_t outcome;
} test_completion_t;

static void on_admission(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    test_completion_t *completion = user;
    completion->done = true;
    completion->status = status;
    completion->outcome = *outcome;
}

static void drive_request(
    r_runtime_client_t *runtime,
    r_admission_request_t *request,
    test_completion_t *completion
) {
    while (!completion->done) {
        struct pollfd descriptors[2] = {0};
        size_t count = r_runtime_socket_count(runtime);
        assert(count > 0u && count <= 2u);
        for (size_t i = 0; i < count; i++) {
            descriptors[i].fd = (int)r_runtime_socket_at(runtime, i);
            descriptors[i].events = POLLIN;
        }

        uint64_t deadline_ms = 0u;
        assert(r_client_admission_deadline_ms(request, &deadline_ms) == RCLIENT_OK);
        uint64_t now_ms = r_runtime_wall_time_ms();
        uint64_t delay_ms = deadline_ms > now_ms ? deadline_ms - now_ms : 0u;
        int timeout_ms = delay_ms > INT_MAX ? INT_MAX : (int)delay_ms;
        int ready = poll(descriptors, (nfds_t)count, timeout_ms);
        assert(ready >= 0);
        if (ready == 0) {
            assert(r_client_admission_on_timeout(runtime->handle, request,
                r_runtime_wall_time_ms()) == RCLIENT_OK);
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            assert((descriptors[i].revents & (POLLERR | POLLHUP | POLLNVAL)) == 0);
            if ((descriptors[i].revents & POLLIN) != 0) {
                assert(r_runtime_client_on_readable(
                    runtime,
                    r_runtime_socket_at(runtime, i)
                ) == RCLIENT_OK);
            }
        }
    }
}

static uint32_t perform_measured_work(void) {
    uint64_t started_ms = 0u;
    assert(r_runtime_monotonic_time_ms(&started_ms) == RCLIENT_OK);
    struct timespec duration = {.tv_nsec = 1000000L};
    assert(nanosleep(&duration, NULL) == 0);
    uint64_t finished_ms = 0u;
    assert(r_runtime_monotonic_time_ms(&finished_ms) == RCLIENT_OK);
    assert(finished_ms >= started_ms);
    uint64_t elapsed_ms = finished_ms - started_ms;
    return elapsed_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_ms;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    long port = strtol(argv[1], NULL, 10);
    assert(port > 0 && port <= UINT16_MAX);
    assert(r_runtime_wall_time_ms() > 0u);

    r_runtime_options_t options = {
        .tenant_dns_name = "rn-test.local",
        .auth_key = TEST_AES_KEY,
        .server_host = "127.0.0.1",
        .server_port = (uint16_t)port,
    };
    r_runtime_client_t runtime;
    assert(r_runtime_client_init(&runtime, &options) == RCLIENT_OK);

    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "runtime-test-bucket";
    config.service_name = "runtime-test-service";
    config.metrics_label = "runtime-test";

    test_completion_t completion = {0};
    r_admission_request_t request;
    assert(r_client_admission_start(
        runtime.handle,
        &request,
        &config,
        on_admission,
        &completion
    ) == RCLIENT_OK);
    drive_request(&runtime, &request, &completion);

    assert(completion.status == RCLIENT_OK);
    assert(completion.outcome.decision == R_ADMISSION_ALLOWED);
    uint32_t observed_ms = perform_measured_work();
    assert(r_client_admission_report_latency(
        runtime.handle,
        &request,
        observed_ms
    ) == RCLIENT_OK);

    r_runtime_client_destroy(&runtime);
    return 0;
}
