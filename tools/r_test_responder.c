#define _POSIX_C_SOURCE 200809L

#include "r_test_responder.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../src/r_protocol.h"

static volatile sig_atomic_t shutdown_requested = 0;

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
    socklen_t len;
    int family;
    char host[INET6_ADDRSTRLEN];
    uint16_t port;
} listen_address_t;

static void request_shutdown(int signal_number) {
    (void)signal_number;
    shutdown_requested = 1;
}

static void usage(FILE *stream) {
    fprintf(stream,
        "Usage: r_test_responder --listen=<loopback:port> [options]\n"
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

    if (!options->print_nginx_config && !options->listen) {
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
    if (inet_pton(AF_INET, host, &ipv4->sin_addr) == 1) {
        uint32_t value = ntohl(ipv4->sin_addr.s_addr);
        if ((value >> 24) != 127u) {
            return -1;
        }
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons((uint16_t)port);
        address->family = AF_INET;
        address->len = sizeof(*ipv4);
    } else {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&address->storage;
        if (inet_pton(AF_INET6, host, &ipv6->sin6_addr) != 1
            || !IN6_IS_ADDR_LOOPBACK(&ipv6->sin6_addr)) {
            return -1;
        }
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons((uint16_t)port);
        address->family = AF_INET6;
        address->len = sizeof(*ipv6);
    }
    memcpy(address->host, host, host_len + 1u);
    address->port = (uint16_t)port;
    return 0;
}

static void print_nginx_config(const responder_options_t *options) {
    const char *listen = options->listen ? options->listen : "127.0.0.1:39080";
    printf("# Synthetic rl-c-client test responder configuration; never use in production.\n");
    printf("ratelimitly_tenant rn-test.local;\n");
    printf("ratelimitly_auth_key %s;\n", options->auth_key);
    printf("# responder=%s\n", listen);
    printf("# SRV _ratelimitly._udp.rn-test.local 0 0 %s s-%" PRIu64 ".localhost.\n",
        strrchr(listen, ':') ? strrchr(listen, ':') + 1 : "39080",
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

static void print_event(const r_test_event_t *event, int status) {
    if (event->kind == R_TEST_EVENT_RATE_REQUEST) {
        printf("{\"event\":\"rate_request\",\"sequence\":%" PRIu64
            ",\"guards\":%zu,\"resources\":%zu,\"label\":",
            event->sequence,
            event->guard_count,
            event->resource_count);
        print_json_string(event->label);
        printf(",\"disposition\":\"%s\"}\n", event->disposition);
    } else if (event->kind == R_TEST_EVENT_LATENCY_REPORT) {
        printf("{\"event\":\"latency_report\",\"sequence\":%" PRIu64
            ",\"reports\":%zu}\n",
            event->sequence,
            event->report_count);
    } else {
        printf("{\"event\":\"input_rejected\",\"sequence\":%" PRIu64
            ",\"status\":%d}\n",
            event->sequence,
            status);
    }
    fflush(stdout);
}

static void delay_response(uint64_t delay_ms) {
    struct timespec remaining = {
        .tv_sec = (time_t)(delay_ms / 1000u),
        .tv_nsec = (long)((delay_ms % 1000u) * 1000000u),
    };
    while (!shutdown_requested && nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) {
            break;
        }
    }
}

static int install_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_shutdown;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    responder_options_t options;
    if (parse_options(argc, argv, &options) != 0) {
        usage(stderr);
        return 2;
    }
    if (options.print_nginx_config) {
        print_nginx_config(&options);
        return 0;
    }

    listen_address_t address;
    if (parse_listen_address(options.listen, &address) != 0) {
        fprintf(stderr, "listen address must be an explicit loopback IP and nonzero port\n");
        return 2;
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
        return 2;
    }
    if (install_signal_handlers() != 0) {
        perror("sigaction");
        OPENSSL_cleanse(&state, sizeof(state));
        return 1;
    }

    int fd = socket(address.family, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        OPENSSL_cleanse(&state, sizeof(state));
        return 1;
    }
    if (bind(fd, (struct sockaddr *)&address.storage, address.len) != 0) {
        perror("bind");
        close(fd);
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
    while (!shutdown_requested) {
        struct pollfd poll_fd = {
            .fd = fd,
            .events = POLLIN,
            .revents = 0,
        };
        int poll_result = poll(&poll_fd, 1u, 250);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            exit_status = 1;
            break;
        }
        if (poll_result == 0 || shutdown_requested) {
            continue;
        }
        if ((poll_fd.revents & POLLIN) == 0) {
            fprintf(stderr, "poll returned unexpected socket events: 0x%x\n", poll_fd.revents);
            exit_status = 1;
            break;
        }

        uint8_t input[2048];
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t received = recvfrom(
            fd,
            input,
            sizeof(input),
            MSG_DONTWAIT,
            (struct sockaddr *)&peer,
            &peer_len
        );
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("recvfrom");
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
        if (rc == RCLIENT_OK && send_response && !shutdown_requested) {
            delay_response(options.delay_ms);
            if (!shutdown_requested) {
                ssize_t sent = sendto(
                    fd,
                    output,
                    output_len,
                    0,
                    (struct sockaddr *)&peer,
                    peer_len
                );
                if (sent < 0 || (size_t)sent != output_len) {
                    perror("sendto");
                    exit_status = 1;
                    break;
                }
            }
        }
        if (options.max_packets > 0u && authenticated_packets >= options.max_packets) {
            break;
        }
    }

    close(fd);
    OPENSSL_cleanse(&state, sizeof(state));
    return exit_status;
}
