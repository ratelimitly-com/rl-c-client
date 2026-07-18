#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#elif !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include "r_test_responder.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET responder_socket_t;
typedef int responder_socklen_t;
#define R_TEST_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int responder_socket_t;
typedef socklen_t responder_socklen_t;
#define R_TEST_INVALID_SOCKET (-1)
#endif

#include "../src/r_protocol.h"

#ifdef _WIN32
static volatile LONG shutdown_requested = 0;

static bool is_shutdown_requested(void) {
    return InterlockedCompareExchange(&shutdown_requested, 0, 0) != 0;
}

static BOOL WINAPI on_console_control(DWORD control_type) {
    switch (control_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            (void)InterlockedExchange(&shutdown_requested, 1);
            return TRUE;
        default:
            return FALSE;
    }
}
#else
static volatile sig_atomic_t shutdown_requested = 0;

static bool is_shutdown_requested(void) {
    return shutdown_requested != 0;
}

static void request_shutdown(int signal_number) {
    (void)signal_number;
    shutdown_requested = 1;
}
#endif

typedef struct responder_options {
    const char *listen;
    const char *auth_name;
    const char *auth_key;
    r_test_scenario_t scenario;
    uint64_t server_id;
    uint64_t allow_count;
    uint64_t delay_ms;
    uint64_t max_packets;
    bool steering_keep_port;
    bool print_nginx_config;
} responder_options_t;

typedef struct listen_address {
    struct sockaddr_storage storage;
    responder_socklen_t len;
    int family;
    char host[INET6_ADDRSTRLEN];
    uint16_t port;
} listen_address_t;

static void usage(FILE *stream) {
    fprintf(stream,
        "Usage: r_test_responder --listen=<IPv4:port|[IPv6]:port> [options]\n"
        "\n"
        "Options:\n"
        "  --scenario=<name>       allow, deny, guard-pass, guard-deny, quota,\n"
        "                          drop, malformed-auth, malformed-truncated,\n"
        "                          malformed-request-id, count-empty,\n"
        "                          count-short, or count-extra\n"
        "  --auth=aes|cookie       Select a built-in synthetic credential\n"
        "  --server-id=<n>         Response server id (default: 1)\n"
        "  --steering=keep|rebind  Steering feedback (default: keep)\n"
        "  --allow-count=<n>       Per-bucket quota allowance (default: 1)\n"
        "  --delay-ms=<n>          Delay responses by n milliseconds\n"
        "  --max-packets=<n>       Exit after n authenticated packets\n"
        "  --print-nginx-config    Print synthetic test configuration and exit\n"
        "  --help                  Show this help\n");
}

static const char *option_value(const char *argument, const char *name) {
    size_t length = strlen(name);
    if (strncmp(argument, name, length) != 0 || argument[length] != '=') {
        return NULL;
    }
    return argument + length + 1u;
}

static int parse_u64(const char *text, uint64_t *out) {
    if (!text || text[0] == '\0' || !out || text[0] == '-') {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        return -1;
    }
    *out = (uint64_t)value;
    return 0;
}

static int parse_options(int argc, char **argv, responder_options_t *options) {
    memset(options, 0, sizeof(*options));
    options->auth_name = "aes";
    options->auth_key = R_TEST_RESPONDER_AES_KEY;
    options->scenario = R_TEST_SCENARIO_ALLOW;
    options->server_id = 1u;
    options->allow_count = 1u;
    options->steering_keep_port = true;

    for (int i = 1; i < argc; i++) {
        const char *value = NULL;
        if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            exit(0);
        } else if (strcmp(argv[i], "--print-nginx-config") == 0) {
            options->print_nginx_config = true;
        } else if ((value = option_value(argv[i], "--listen")) != NULL) {
            options->listen = value;
        } else if ((value = option_value(argv[i], "--scenario")) != NULL) {
            if (r_test_scenario_parse(value, &options->scenario) != RCLIENT_OK) {
                fprintf(stderr, "invalid scenario: %s\n", value);
                return -1;
            }
        } else if ((value = option_value(argv[i], "--auth")) != NULL) {
            if (strcmp(value, "aes") == 0) {
                options->auth_name = "aes";
                options->auth_key = R_TEST_RESPONDER_AES_KEY;
            } else if (strcmp(value, "cookie") == 0) {
                options->auth_name = "cookie";
                options->auth_key = R_TEST_RESPONDER_COOKIE_KEY;
            } else {
                fprintf(stderr, "invalid auth mode: %s\n", value);
                return -1;
            }
        } else if ((value = option_value(argv[i], "--server-id")) != NULL) {
            if (parse_u64(value, &options->server_id) != 0 || options->server_id == 0u) {
                fprintf(stderr, "invalid server id: %s\n", value);
                return -1;
            }
        } else if ((value = option_value(argv[i], "--allow-count")) != NULL) {
            if (parse_u64(value, &options->allow_count) != 0) {
                fprintf(stderr, "invalid allow count: %s\n", value);
                return -1;
            }
        } else if ((value = option_value(argv[i], "--delay-ms")) != NULL) {
            if (parse_u64(value, &options->delay_ms) != 0) {
                fprintf(stderr, "invalid delay: %s\n", value);
                return -1;
            }
        } else if ((value = option_value(argv[i], "--max-packets")) != NULL) {
            if (parse_u64(value, &options->max_packets) != 0) {
                fprintf(stderr, "invalid maximum packet count: %s\n", value);
                return -1;
            }
        } else if ((value = option_value(argv[i], "--steering")) != NULL) {
            if (strcmp(value, "keep") == 0) {
                options->steering_keep_port = true;
            } else if (strcmp(value, "rebind") == 0) {
                options->steering_keep_port = false;
            } else {
                fprintf(stderr, "invalid steering mode: %s\n", value);
                return -1;
            }
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return -1;
        }
    }

    if (!options->listen) {
        fprintf(stderr, "--listen is required\n");
        return -1;
    }
    return 0;
}

static int parse_listen_address(const char *text, listen_address_t *address) {
    if (!text || !address) {
        return -1;
    }
    char host[INET6_ADDRSTRLEN];
    const char *port_text = NULL;
    size_t host_len = 0u;
    if (text[0] == '[') {
        const char *close = strchr(text, ']');
        if (!close || close[1] != ':') {
            return -1;
        }
        host_len = (size_t)(close - text - 1);
        port_text = close + 2;
        text++;
    } else {
        const char *colon = strrchr(text, ':');
        if (!colon) {
            return -1;
        }
        host_len = (size_t)(colon - text);
        port_text = colon + 1;
    }
    if (host_len == 0u || host_len >= sizeof(host)) {
        return -1;
    }
    memcpy(host, text, host_len);
    host[host_len] = '\0';

    uint64_t port = 0u;
    if (parse_u64(port_text, &port) != 0 || port == 0u || port > 65535u) {
        return -1;
    }

    memset(address, 0, sizeof(*address));
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)&address->storage;
#ifdef _WIN32
    int ipv4_status = InetPtonA(AF_INET, host, &ipv4->sin_addr);
#else
    int ipv4_status = inet_pton(AF_INET, host, &ipv4->sin_addr);
#endif
    if (ipv4_status == 1) {
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons((uint16_t)port);
        address->family = AF_INET;
        address->len = (responder_socklen_t)sizeof(*ipv4);
    } else {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&address->storage;
#ifdef _WIN32
        int ipv6_status = InetPtonA(AF_INET6, host, &ipv6->sin6_addr);
#else
        int ipv6_status = inet_pton(AF_INET6, host, &ipv6->sin6_addr);
#endif
        if (ipv6_status != 1) {
            return -1;
        }
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons((uint16_t)port);
        address->family = AF_INET6;
        address->len = (responder_socklen_t)sizeof(*ipv6);
    }
    memcpy(address->host, host, host_len + 1u);
    address->port = (uint16_t)port;
    return 0;
}

static void print_nginx_config(
    const responder_options_t *options,
    const char *listen,
    uint16_t port
) {
    printf("# Synthetic rl-c-client test responder configuration; never use in production.\n");
    printf("ratelimitly_tenant rn-test.local;\n");
    printf("ratelimitly_auth_key %s;\n", options->auth_key);
    printf("# responder=%s\n", listen);
    printf("# SRV _ratelimitly._udp.rn-test.local 0 0 %u s-%" PRIu64 ".localhost.\n",
        (unsigned int)port,
        options->server_id);
}

static void print_json_string(const char *text) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        switch (*p) {
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\b': fputs("\\b", stdout); break;
            case '\f': fputs("\\f", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (*p < 0x20u) {
                    printf("\\u%04x", *p);
                } else {
                    putchar(*p);
                }
                break;
        }
    }
    putchar('"');
}

static void print_tracker(const r_test_tracker_observation_t *tracker) {
    if (!tracker->present) {
        fputs("null", stdout);
        return;
    }
    printf("{\"ttl_ms\":%" PRIu32
        ",\"max_samples\":%" PRIu32
        ",\"buffer_size\":%" PRIu32
        ",\"min_sample_threshold\":%" PRIu32 "}",
        tracker->ttl_ms,
        tracker->max_samples,
        tracker->buffer_size,
        tracker->min_sample_threshold);
}

static void print_event(const r_test_event_t *event, int status) {
    if (event->kind == R_TEST_EVENT_RATE_REQUEST) {
        printf("{\"event\":\"rate_request\",\"sequence\":%" PRIu64
            ",\"guards\":%zu,\"resources\":%zu,\"label\":",
            event->sequence,
            event->guard_count,
            event->resource_count);
        print_json_string(event->label);
        fputs(",\"tracker\":", stdout);
        print_tracker(&event->tracker);
        printf(",\"guard_threshold_ms\":%" PRIu32
            ",\"disposition\":\"%s\"}\n",
            event->guard_threshold_ms,
            event->disposition);
    } else if (event->kind == R_TEST_EVENT_LATENCY_REPORT) {
        printf("{\"event\":\"latency_report\",\"sequence\":%" PRIu64
            ",\"reports\":%zu,\"tracker\":",
            event->sequence,
            event->report_count);
        print_tracker(&event->tracker);
        printf(",\"observed_latency_ms\":%" PRIu32
            ",\"matches_previous_guard\":%s}\n",
            event->observed_latency_ms,
            event->tracker_matches_guard ? "true" : "false");
    } else {
        printf("{\"event\":\"input_rejected\",\"sequence\":%" PRIu64
            ",\"status\":%d}\n",
            event->sequence,
            status);
    }
    fflush(stdout);
}

static void delay_response(uint64_t delay_ms) {
#ifdef _WIN32
    while (!is_shutdown_requested() && delay_ms > 0u) {
        DWORD chunk_ms = delay_ms > 250u ? 250u : (DWORD)delay_ms;
        Sleep(chunk_ms);
        delay_ms -= chunk_ms;
    }
#else
    struct timespec remaining = {
        .tv_sec = (time_t)(delay_ms / 1000u),
        .tv_nsec = (long)((delay_ms % 1000u) * 1000000u),
    };
    while (!is_shutdown_requested() && nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) {
            break;
        }
    }
#endif
}

static int install_shutdown_handler(void) {
#ifdef _WIN32
    if (!SetConsoleCtrlHandler(on_console_control, TRUE)) {
        fprintf(stderr, "SetConsoleCtrlHandler failed: %lu\n",
            (unsigned long)GetLastError());
        return -1;
    }
#else
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_shutdown;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        perror("sigaction");
        return -1;
    }
#endif
    return 0;
}

static int network_start(void) {
#ifdef _WIN32
    WSADATA winsock_data;
    int status = WSAStartup(MAKEWORD(2, 2), &winsock_data);
    if (status != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", status);
        return -1;
    }
    if (LOBYTE(winsock_data.wVersion) != 2
        || HIBYTE(winsock_data.wVersion) != 2) {
        fprintf(stderr, "WinSock 2.2 is required; negotiated %u.%u\n",
            (unsigned int)LOBYTE(winsock_data.wVersion),
            (unsigned int)HIBYTE(winsock_data.wVersion));
        (void)WSACleanup();
        return -1;
    }
#endif
    return 0;
}

static void network_stop(void) {
#ifdef _WIN32
    (void)WSACleanup();
#endif
}

static void print_socket_error(const char *operation) {
#ifdef _WIN32
    fprintf(stderr, "%s failed: %d\n", operation, WSAGetLastError());
#else
    perror(operation);
#endif
}

static void close_responder_socket(responder_socket_t socket_value) {
#ifdef _WIN32
    (void)closesocket(socket_value);
#else
    (void)close(socket_value);
#endif
}

/* Return 1 when readable, 0 after a timeout/interruption, or -1 on error. */
static int wait_for_readable(responder_socket_t socket_value) {
#ifdef _WIN32
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_value, &read_set);
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
    int result = select(0, &read_set, NULL, NULL, &timeout);
    if (result == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAEINTR) {
            return 0;
        }
        print_socket_error("select");
        return -1;
    }
    if (result == 0) {
        return 0;
    }
    if (!FD_ISSET(socket_value, &read_set)) {
        fputs("select returned without socket readiness\n", stderr);
        return -1;
    }
    return 1;
#else
    struct pollfd poll_fd = {
        .fd = socket_value,
        .events = POLLIN,
        .revents = 0,
    };
    int result = poll(&poll_fd, 1u, 250);
    if (result < 0) {
        if (errno == EINTR) {
            return 0;
        }
        print_socket_error("poll");
        return -1;
    }
    if (result == 0) {
        return 0;
    }
    if ((poll_fd.revents & POLLIN) == 0) {
        fprintf(stderr, "poll returned unexpected socket events: 0x%x\n",
            poll_fd.revents);
        return -1;
    }
    return 1;
#endif
}

/* Return bytes received, -2 for a retryable condition, or -1 on error. */
static int receive_packet(
    responder_socket_t socket_value,
    uint8_t *buffer,
    size_t capacity,
    struct sockaddr_storage *peer,
    responder_socklen_t *peer_len
) {
    if (capacity > INT_MAX) {
        fputs("receive buffer exceeds socket length range\n", stderr);
        return -1;
    }
#ifdef _WIN32
    int received = recvfrom(
        socket_value,
        (char *)buffer,
        (int)capacity,
        0,
        (struct sockaddr *)peer,
        peer_len
    );
    if (received == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error == WSAEINTR || error == WSAEWOULDBLOCK) {
            return -2;
        }
        print_socket_error("recvfrom");
        return -1;
    }
    return received;
#else
    ssize_t received = recvfrom(
        socket_value,
        buffer,
        capacity,
        0,
        (struct sockaddr *)peer,
        peer_len
    );
    if (received < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return -2;
        }
        print_socket_error("recvfrom");
        return -1;
    }
    return (int)received;
#endif
}

static int send_packet(
    responder_socket_t socket_value,
    const uint8_t *buffer,
    size_t length,
    const struct sockaddr_storage *peer,
    responder_socklen_t peer_len
) {
    if (length > INT_MAX) {
        fputs("response exceeds socket length range\n", stderr);
        return -1;
    }
#ifdef _WIN32
    int sent = sendto(
        socket_value,
        (const char *)buffer,
        (int)length,
        0,
        (const struct sockaddr *)peer,
        peer_len
    );
    if (sent == SOCKET_ERROR) {
        print_socket_error("sendto");
        return -1;
    }
    if ((size_t)sent != length) {
        fputs("sendto completed with a short datagram\n", stderr);
        return -1;
    }
#else
    ssize_t sent = sendto(
        socket_value,
        buffer,
        length,
        0,
        (const struct sockaddr *)peer,
        peer_len
    );
    if (sent < 0) {
        print_socket_error("sendto");
        return -1;
    }
    if ((size_t)sent != length) {
        fputs("sendto completed with a short datagram\n", stderr);
        return -1;
    }
#endif
    return 0;
}

int main(int argc, char **argv) {
    responder_options_t options;
    if (parse_options(argc, argv, &options) != 0) {
        usage(stderr);
        return 2;
    }
    if (network_start() != 0) {
        return 1;
    }
    const char *listen = options.listen;
    listen_address_t address;
    if (parse_listen_address(listen, &address) != 0) {
        fprintf(stderr, "listen address must be an explicit numeric IP and nonzero port\n");
        network_stop();
        return 2;
    }
    if (options.print_nginx_config) {
        print_nginx_config(&options, listen, address.port);
        network_stop();
        return 0;
    }
    r_test_responder_state_t state;
    int rc = r_test_responder_init(
        &state,
        options.scenario,
        options.auth_key,
        options.server_id,
        options.steering_keep_port,
        options.allow_count
    );
    if (rc != RCLIENT_OK) {
        fprintf(stderr, "failed to initialize responder: %d\n", rc);
        network_stop();
        return 2;
    }
    if (install_shutdown_handler() != 0) {
        network_stop();
        OPENSSL_cleanse(&state, sizeof(state));
        return 1;
    }

    responder_socket_t fd = socket(address.family, SOCK_DGRAM, 0);
    if (fd == R_TEST_INVALID_SOCKET) {
        print_socket_error("socket");
        network_stop();
        OPENSSL_cleanse(&state, sizeof(state));
        return 1;
    }
    if (bind(fd, (struct sockaddr *)&address.storage, address.len) != 0) {
        print_socket_error("bind");
        close_responder_socket(fd);
        network_stop();
        OPENSSL_cleanse(&state, sizeof(state));
        return 1;
    }

    printf("{\"event\":\"ready\",\"address\":\"%s\",\"port\":%u,"
        "\"server_id\":%" PRIu64 ",\"auth\":\"%s\"}\n",
        address.host,
        address.port,
        options.server_id,
        options.auth_name);
    fflush(stdout);

    uint64_t authenticated_packets = 0u;
    int exit_status = 0;
    while (!is_shutdown_requested()) {
        int wait_status = wait_for_readable(fd);
        if (wait_status < 0) {
            exit_status = 1;
            break;
        }
        if (wait_status == 0 || is_shutdown_requested()) {
            continue;
        }

        uint8_t input[2048];
        struct sockaddr_storage peer;
        responder_socklen_t peer_len = (responder_socklen_t)sizeof(peer);
        int received = receive_packet(
            fd,
            input,
            sizeof(input),
            &peer,
            &peer_len
        );
        if (received == -2) {
            continue;
        }
        if (received < 0) {
            exit_status = 1;
            break;
        }

        uint8_t output[R_MAX_PACKET_SIZE];
        size_t output_len = 0u;
        bool send_response = false;
        r_test_event_t event;
        rc = r_test_responder_process(
            &state,
            input,
            (size_t)received,
            output,
            sizeof(output),
            &output_len,
            &send_response,
            &event
        );
        print_event(&event, rc);
        if (event.kind != R_TEST_EVENT_INPUT_REJECTED) {
            authenticated_packets++;
        }
        if (rc == RCLIENT_OK && send_response && !is_shutdown_requested()) {
            delay_response(options.delay_ms);
            if (!is_shutdown_requested()) {
                if (send_packet(
                    fd,
                    output,
                    output_len,
                    &peer,
                    peer_len
                ) != 0) {
                    exit_status = 1;
                    break;
                }
            }
        }
        if (options.max_packets > 0u && authenticated_packets >= options.max_packets) {
            break;
        }
    }

    close_responder_socket(fd);
    network_stop();
    OPENSSL_cleanse(&state, sizeof(state));
    return exit_status;
}
