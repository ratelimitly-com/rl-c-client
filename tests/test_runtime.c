#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <arpa/inet.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <unistd.h>

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

        uint64_t delay_ms = 0u;
        assert(r_runtime_admission_delay_ms(request, &delay_ms) == RCLIENT_OK);
        int timeout_ms = delay_ms > INT_MAX ? INT_MAX : (int)delay_ms;
        int ready = poll(descriptors, (nfds_t)count, timeout_ms);
        assert(ready >= 0);
        if (ready == 0) {
            assert(r_runtime_admission_on_timeout(runtime, request) == RCLIENT_OK);
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

static int perform_protected_work(void *user) {
    int *calls = user;
    *calls += 1;
    struct timespec duration = {.tv_nsec = 1000000L};
    assert(nanosleep(&duration, NULL) == 0);
    return RCLIENT_OK;
}

static void inject_invalid_datagram(r_runtime_client_t *runtime) {
    for (size_t i = 0; i < r_runtime_socket_count(runtime); i++) {
        int target_socket = (int)r_runtime_socket_at(runtime, i);
        struct sockaddr_storage target = {0};
        socklen_t target_length = sizeof(target);
        assert(getsockname(
            target_socket,
            (struct sockaddr *)&target,
            &target_length
        ) == 0);
        if (target.ss_family != AF_INET) {
            continue;
        }
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)&target;
        ipv4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int sender = socket(AF_INET, SOCK_DGRAM, 0);
        assert(sender >= 0);
        const uint8_t invalid[] = {0xffu};
        assert(sendto(
            sender,
            invalid,
            sizeof(invalid),
            0,
            (const struct sockaddr *)&target,
            target_length
        ) == (ssize_t)sizeof(invalid));
        assert(close(sender) == 0);
        return;
    }
    assert(false && "runtime did not expose an IPv4 socket");
}

int main(int argc, char **argv) {
    assert(argc == 2);
    long port = strtol(argv[1], NULL, 10);
    assert(port > 0 && port <= UINT16_MAX);
    assert(r_runtime_wall_time_ms() > 0u);

    assert(unsetenv("RATELIMITLY_TENANT") == 0);
    assert(unsetenv("RATELIMITLY_AUTH_KEY") == 0);
    assert(unsetenv("RATELIMITLY_REQUEST_UNIT_MS") == 0);
    assert(unsetenv("RATELIMITLY_REQUEST_REPLAY_COUNT") == 0);
    assert(unsetenv("RATELIMITLY_REQUEST_PROFILE") == 0);
    r_runtime_options_t environment_options;
    assert(r_runtime_options_from_env(&environment_options)
        == RCLIENT_ERR_CONFIG);

    assert(setenv("RATELIMITLY_AUTH_KEY", TEST_AES_KEY, 1) == 0);
    assert(unsetenv("RATELIMITLY_EXAMPLE_SERVER_HOST") == 0);
    assert(unsetenv("RATELIMITLY_EXAMPLE_SERVER_PORT") == 0);
    assert(r_runtime_options_from_env(&environment_options) == RCLIENT_OK);
    assert(environment_options.tenant_dns_name == NULL);
    assert(environment_options.auth_key == TEST_AES_KEY
        || strcmp(environment_options.auth_key, TEST_AES_KEY) == 0);
    assert(!environment_options.has_request_policy);
    assert(!environment_options.profile_requests);

    assert(setenv("RATELIMITLY_REQUEST_UNIT_MS", "25", 1) == 0);
    assert(setenv("RATELIMITLY_REQUEST_REPLAY_COUNT", "3", 1) == 0);
    assert(setenv("RATELIMITLY_REQUEST_PROFILE", "1", 1) == 0);
    assert(r_runtime_options_from_env(&environment_options) == RCLIENT_OK);
    assert(environment_options.has_request_policy);
    assert(environment_options.request_policy.unit_ms == 25u);
    assert(environment_options.request_policy.replay_count == 3u);
    assert(environment_options.profile_requests);

    assert(setenv("RATELIMITLY_REQUEST_REPLAY_COUNT", "0", 1) == 0);
    assert(r_runtime_options_from_env(&environment_options) == RCLIENT_OK);
    assert(environment_options.has_request_policy);
    assert(environment_options.request_policy.unit_ms == 25u);
    assert(environment_options.request_policy.replay_count == 0u);
    assert(setenv("RATELIMITLY_REQUEST_REPLAY_COUNT", "3", 1) == 0);

    assert(setenv("RATELIMITLY_REQUEST_UNIT_MS", "0", 1) == 0);
    assert(r_runtime_options_from_env(&environment_options)
        == RCLIENT_ERR_CONFIG);
    assert(setenv("RATELIMITLY_REQUEST_UNIT_MS", "-1", 1) == 0);
    assert(r_runtime_options_from_env(&environment_options)
        == RCLIENT_ERR_CONFIG);
    assert(setenv("RATELIMITLY_REQUEST_UNIT_MS", "+25", 1) == 0);
    assert(r_runtime_options_from_env(&environment_options)
        == RCLIENT_ERR_CONFIG);
    assert(setenv("RATELIMITLY_REQUEST_UNIT_MS", "25", 1) == 0);

    assert(setenv(
        "RATELIMITLY_REQUEST_REPLAY_COUNT",
        "65536",
        1
    ) == 0);
    assert(r_runtime_options_from_env(&environment_options)
        == RCLIENT_ERR_CONFIG);
    assert(setenv("RATELIMITLY_REQUEST_REPLAY_COUNT", "3", 1) == 0);

    assert(setenv("RATELIMITLY_REQUEST_PROFILE", "true", 1) == 0);
    assert(r_runtime_options_from_env(&environment_options)
        == RCLIENT_ERR_CONFIG);
    assert(setenv("RATELIMITLY_REQUEST_PROFILE", "1", 1) == 0);

    assert(setenv("RATELIMITLY_TENANT", "custom.example", 1) == 0);
    assert(r_runtime_options_from_env(&environment_options) == RCLIENT_OK);
    assert(strcmp(environment_options.tenant_dns_name, "custom.example") == 0);

    assert(setenv("RATELIMITLY_TENANT", "", 1) == 0);
    assert(r_runtime_options_from_env(&environment_options) == RCLIENT_OK);
    assert(environment_options.tenant_dns_name == NULL);

    environment_options.server_host = "127.0.0.1";
    environment_options.server_port = (uint16_t)port;
    r_runtime_client_t runtime;
    assert(r_runtime_client_init(&runtime, &environment_options) == RCLIENT_OK);
    inject_invalid_datagram(&runtime);

    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "runtime-test-bucket";
    config.latency_tracker_name = "runtime-test-service";
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
    int work_calls = 0;
    uint32_t observed_ms = 0u;
    assert(r_runtime_admission_run_and_report(
        &runtime,
        &request,
        perform_protected_work,
        &work_calls,
        &observed_ms
    ) == RCLIENT_OK);
    assert(work_calls == 1);
    assert(r_runtime_admission_run_and_report(
        &runtime,
        &request,
        perform_protected_work,
        &work_calls,
        &observed_ms
    ) == RCLIENT_ERR_CONFIG);
    assert(work_calls == 1);

    test_completion_t failed_report_completion = {0};
    r_admission_request_t failed_report_request;
    assert(r_client_admission_start(
        runtime.handle,
        &failed_report_request,
        &config,
        on_admission,
        &failed_report_completion
    ) == RCLIENT_OK);
    drive_request(&runtime, &failed_report_request, &failed_report_completion);
    assert(failed_report_completion.status == RCLIENT_OK);
    assert(failed_report_completion.outcome.allowed);

    size_t socket_count = runtime.socket_count;
    runtime.socket_count = 0u;
    int failed_report_work_calls = 0;
    assert(r_runtime_admission_run_and_report(
        &runtime,
        &failed_report_request,
        perform_protected_work,
        &failed_report_work_calls,
        &observed_ms
    ) == RCLIENT_ERR_IO);
    runtime.socket_count = socket_count;
    assert(failed_report_work_calls == 1);
    assert(r_runtime_admission_run_and_report(
        &runtime,
        &failed_report_request,
        perform_protected_work,
        &failed_report_work_calls,
        &observed_ms
    ) == RCLIENT_ERR_CONFIG);
    assert(failed_report_work_calls == 1);

    r_runtime_client_destroy(&runtime);
    assert(unsetenv("RATELIMITLY_REQUEST_UNIT_MS") == 0);
    assert(unsetenv("RATELIMITLY_REQUEST_REPLAY_COUNT") == 0);
    assert(unsetenv("RATELIMITLY_REQUEST_PROFILE") == 0);
    return 0;
}
