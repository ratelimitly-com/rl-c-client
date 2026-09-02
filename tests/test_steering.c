#include <stdio.h>
#include <stdint.h>

#include "r_client_steering.h"
#include "r_client_runtime.h"

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                  \
                __FILE__, __LINE__, #condition);                              \
            return 1;                                                         \
        }                                                                     \
    } while (0)

#ifdef _WIN32
static const char TEST_AES_KEY[] =
    "rl-aes1qypsqqqqqqqqqqqrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqdgrrulcvcn0x5";

static int test_windows_runtime_sockets_are_exclusive(void) {
    r_runtime_options_t options = {0};
    options.auth_key = TEST_AES_KEY;
    options.server_host = "127.0.0.1";
    options.server_port = 39080u;

    r_runtime_client_t runtime;
    CHECK(r_runtime_client_init(&runtime, &options) == RCLIENT_OK);
    CHECK(r_runtime_socket_count(&runtime) > 0u);
    for (size_t i = 0u; i < r_runtime_socket_count(&runtime); i++) {
        int exclusive = 0;
        int length = (int)sizeof(exclusive);
        CHECK(getsockopt(
            r_runtime_socket_at(&runtime, i),
            SOL_SOCKET,
            SO_EXCLUSIVEADDRUSE,
            (char *)&exclusive,
            &length
        ) == 0);
        CHECK(exclusive == 1);
    }
    r_runtime_client_destroy(&runtime);
    return 0;
}
#endif

typedef struct bind_fixture {
    uint16_t occupied;
    uint16_t selected;
    size_t calls;
} bind_fixture_t;

static r_steering_bind_result_t try_bind(void *user, uint16_t port) {
    bind_fixture_t *fixture = user;
    fixture->calls++;
    if (port == fixture->occupied) {
        return R_STEERING_BIND_OCCUPIED;
    }
    fixture->selected = port;
    return R_STEERING_BIND_OK;
}

int main(void) {
    CHECK(r_client_next_steering_port(R_STEERING_PORT_MIN)
        == R_STEERING_PORT_MIN + 1u);
    CHECK(r_client_next_steering_port(UINT16_MAX - 1u) == UINT16_MAX);
    CHECK(r_client_next_steering_port(UINT16_MAX) == R_STEERING_PORT_MIN);
    CHECK(r_client_next_steering_port(40000u) == R_STEERING_PORT_MIN);

    bind_fixture_t fixture = {
        .occupied = 60000u,
    };
    uint16_t selected = 0u;
    uint16_t next = 0u;
    CHECK(r_client_select_steering_port(
        60000u,
        try_bind,
        &fixture,
        &selected,
        &next
    ) == RCLIENT_OK);
    CHECK(fixture.calls == 2u);
    CHECK(fixture.selected == 60001u);
    CHECK(selected == 60001u);
    CHECK(next == 60002u);

    fixture = (bind_fixture_t){
        .occupied = UINT16_MAX,
    };
    CHECK(r_client_select_steering_port(
        UINT16_MAX,
        try_bind,
        &fixture,
        &selected,
        &next
    ) == RCLIENT_OK);
    CHECK(fixture.calls == 2u);
    CHECK(selected == R_STEERING_PORT_MIN);
    CHECK(next == R_STEERING_PORT_MIN + 1u);
#ifdef _WIN32
    CHECK(test_windows_runtime_sockets_are_exclusive() == 0);
#endif
    return 0;
}
