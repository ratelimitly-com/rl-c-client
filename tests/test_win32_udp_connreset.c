#ifndef _WIN32
#error "This regression test requires Win32"
#endif

#include <stdio.h>

#include "r_client_runtime.h"

#include "../src/r_win32_udp.h"

/*
 * Winsock reports an ICMP Port Unreachable response as WSAECONNRESET on a
 * later operation on the UDP socket. First prove this machine exhibits that
 * behavior with an ordinary control socket. Then exercise both the Win32 event
 * path and the runtime's receive/send paths with the same local ICMP response.
 */

static const char TEST_AES_KEY[] =
    "rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqhmzd8l";

enum {
    RESET_ATTEMPTS = 200,
    RESET_WAIT_MS = 10,
};

static int send_retry_policy_is_bounded(void) {
    bool reset_retried = false;
    if (!r_win32_udp_send_should_retry(WSAEINTR, &reset_retried)
        || reset_retried) {
        fputs("WSAEINTR did not request an ordinary retry\n", stderr);
        return -1;
    }
    if (!r_win32_udp_send_should_retry(WSAECONNRESET, &reset_retried)
        || !reset_retried) {
        fputs("first WSAECONNRESET did not request one retry\n", stderr);
        return -1;
    }
    if (r_win32_udp_send_should_retry(WSAECONNRESET, &reset_retried)) {
        fputs("second WSAECONNRESET requested an unbounded retry\n", stderr);
        return -1;
    }
    if (r_win32_udp_send_should_retry(WSAEWOULDBLOCK, &reset_retried)) {
        fputs("unrelated send error was treated as retryable\n", stderr);
        return -1;
    }
    return 0;
}

static int make_nonblocking(SOCKET socket_value) {
    u_long enabled = 1u;
    if (ioctlsocket(socket_value, FIONBIO, &enabled) == SOCKET_ERROR) {
        fprintf(stderr, "ioctlsocket failed: %d\n", WSAGetLastError());
        return -1;
    }
    return 0;
}

static SOCKET open_bound_control_socket(void) {
    SOCKET control = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (control == INVALID_SOCKET) {
        fprintf(stderr, "control socket failed: %d\n", WSAGetLastError());
        return INVALID_SOCKET;
    }

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    if (bind(
            control,
            (const struct sockaddr *)&local,
            (int)sizeof(local)) == SOCKET_ERROR) {
        fprintf(stderr, "control bind failed: %d\n", WSAGetLastError());
        (void)closesocket(control);
        return INVALID_SOCKET;
    }
    if (make_nonblocking(control) != 0) {
        (void)closesocket(control);
        return INVALID_SOCKET;
    }
    return control;
}

static int reserve_then_release_loopback_port(
    struct sockaddr_in *out_destination
) {
    SOCKET reservation = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (reservation == INVALID_SOCKET) {
        fprintf(stderr, "reservation socket failed: %d\n", WSAGetLastError());
        return -1;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(
            reservation,
            (const struct sockaddr *)&address,
            (int)sizeof(address)) == SOCKET_ERROR) {
        fprintf(stderr, "reservation bind failed: %d\n", WSAGetLastError());
        (void)closesocket(reservation);
        return -1;
    }

    int address_length = (int)sizeof(address);
    if (getsockname(
            reservation,
            (struct sockaddr *)&address,
            &address_length) == SOCKET_ERROR) {
        fprintf(stderr, "reservation getsockname failed: %d\n",
            WSAGetLastError());
        (void)closesocket(reservation);
        return -1;
    }
    (void)closesocket(reservation);

    /*
     * Nothing now owns this loopback port. A datagram sent to it causes the
     * local IP stack to return ICMP Port Unreachable without external network
     * timing or firewall dependencies.
     */
    *out_destination = address;
    return 0;
}

static int control_socket_observes_connreset(
    SOCKET control,
    const struct sockaddr_in *destination
) {
    const char probe = 'C';
    for (int attempt = 0; attempt < RESET_ATTEMPTS; attempt++) {
        int sent = sendto(
            control,
            &probe,
            1,
            0,
            (const struct sockaddr *)destination,
            (int)sizeof(*destination)
        );
        if (sent == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAECONNRESET) {
                return 0;
            }
            fprintf(stderr, "control sendto failed: %d\n", error);
            return -1;
        }

        Sleep(RESET_WAIT_MS);
        char received_byte = '\0';
        struct sockaddr_storage sender = {0};
        int sender_length = (int)sizeof(sender);
        int received = recvfrom(
            control,
            &received_byte,
            1,
            0,
            (struct sockaddr *)&sender,
            &sender_length
        );
        if (received == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAECONNRESET) {
                return 0;
            }
            if (error == WSAEWOULDBLOCK || error == WSAEINTR) {
                continue;
            }
            fprintf(stderr, "control recvfrom failed: %d\n", error);
            return -1;
        }

        fprintf(stderr, "control socket received an unexpected datagram\n");
        return -1;
    }

    fputs("control socket never observed WSAECONNRESET\n", stderr);
    return -1;
}

static SOCKET find_runtime_ipv4_socket(const r_runtime_client_t *runtime) {
    size_t count = r_runtime_socket_count(runtime);
    for (size_t index = 0; index < count; index++) {
        SOCKET socket_value = r_runtime_socket_at(runtime, index);
        struct sockaddr_storage local = {0};
        int local_length = (int)sizeof(local);
        if (getsockname(
                socket_value,
                (struct sockaddr *)&local,
                &local_length) == 0
            && local.ss_family == AF_INET) {
            return socket_value;
        }
    }
    return INVALID_SOCKET;
}

static int drain_runtime_event(
    r_runtime_client_t *runtime,
    SOCKET socket_value,
    WSAEVENT socket_event
) {
    DWORD wait_status = WSAWaitForMultipleEvents(
        1u,
        &socket_event,
        FALSE,
        RESET_WAIT_MS,
        FALSE
    );
    if (wait_status == WSA_WAIT_FAILED) {
        fprintf(stderr, "WSAWaitForMultipleEvents failed: %d\n",
            WSAGetLastError());
        return -1;
    }
    if (wait_status != WSA_WAIT_TIMEOUT) {
        if (wait_status != WSA_WAIT_EVENT_0) {
            fprintf(stderr, "unexpected Winsock wait result: %lu\n",
                (unsigned long)wait_status);
            return -1;
        }

        WSANETWORKEVENTS network_events;
        if (WSAEnumNetworkEvents(
                socket_value,
                socket_event,
                &network_events) == SOCKET_ERROR) {
            fprintf(stderr, "WSAEnumNetworkEvents failed: %d\n",
                WSAGetLastError());
            return -1;
        }
        if ((network_events.lNetworkEvents & FD_READ) == 0) {
            fputs("runtime event omitted FD_READ\n", stderr);
            return -1;
        }
        int read_error = network_events.iErrorCode[FD_READ_BIT];
        if (read_error != 0 && read_error != WSAECONNRESET) {
            fprintf(stderr, "runtime event surfaced Winsock error %d\n",
                read_error);
            return -1;
        }
    }

    int status = r_runtime_client_on_readable(runtime, socket_value);
    if (status != RCLIENT_OK) {
        fprintf(stderr,
            "runtime receive returned %s (%d), Winsock error %d\n",
            r_runtime_status_name(status),
            status,
            WSAGetLastError());
        return -1;
    }
    return 0;
}

static int runtime_socket_survives_connreset(
    r_runtime_client_t *runtime,
    SOCKET socket_value,
    const struct sockaddr_in *destination
) {
    WSAEVENT socket_event = WSACreateEvent();
    if (socket_event == WSA_INVALID_EVENT) {
        fprintf(stderr, "WSACreateEvent failed: %d\n", WSAGetLastError());
        return -1;
    }
    if (WSAEventSelect(socket_value, socket_event, FD_READ) == SOCKET_ERROR) {
        fprintf(stderr, "WSAEventSelect failed: %d\n", WSAGetLastError());
        (void)WSACloseEvent(socket_event);
        return -1;
    }

    int result = 0;
    const char probe = 'R';
    for (int attempt = 0; attempt < RESET_ATTEMPTS; attempt++) {
        int sent = sendto(
            socket_value,
            &probe,
            1,
            0,
            (const struct sockaddr *)destination,
            (int)sizeof(*destination)
        );
        if (sent == SOCKET_ERROR) {
            fprintf(stderr, "runtime sendto surfaced Winsock error %d\n",
                WSAGetLastError());
            result = -1;
            break;
        }
        if (drain_runtime_event(
                runtime,
                socket_value,
                socket_event) != 0) {
            result = -1;
            break;
        }
    }

    if (WSAEventSelect(socket_value, NULL, 0) == SOCKET_ERROR && result == 0) {
        fprintf(stderr, "WSAEventSelect cleanup failed: %d\n",
            WSAGetLastError());
        result = -1;
    }
    (void)WSACloseEvent(socket_event);
    return result;
}

static int set_connreset_reporting(SOCKET socket_value, BOOL enabled) {
    DWORD bytes_returned = 0u;
    if (WSAIoctl(
            socket_value,
            SIO_UDP_CONNRESET,
            &enabled,
            (DWORD)sizeof(enabled),
            NULL,
            0u,
            &bytes_returned,
            NULL,
            NULL) == SOCKET_ERROR) {
        fprintf(stderr, "SIO_UDP_CONNRESET failed: %d\n", WSAGetLastError());
        return -1;
    }
    return 0;
}

static int wait_for_socket_notification(SOCKET socket_value) {
    for (int attempt = 0; attempt < RESET_ATTEMPTS; attempt++) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket_value, &readable);
        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = RESET_WAIT_MS * 1000,
        };
        int ready = select(0, &readable, NULL, NULL, &timeout);
        if (ready > 0 && FD_ISSET(socket_value, &readable)) {
            return 0;
        }
        if (ready == SOCKET_ERROR && WSAGetLastError() != WSAEINTR) {
            fprintf(stderr, "select failed: %d\n", WSAGetLastError());
            return -1;
        }
    }
    fputs("runtime socket never received its ICMP notification\n", stderr);
    return -1;
}

static void record_unexpected_completion(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    (void)status;
    (void)outcome;
    bool *completed = user;
    *completed = true;
}

static int runtime_send_survives_connreset(
    r_runtime_client_t *runtime,
    SOCKET socket_value,
    const struct sockaddr_in *destination
) {
    if (set_connreset_reporting(socket_value, TRUE) != 0) {
        return -1;
    }

    const char probe = 'S';
    int sent = sendto(
        socket_value,
        &probe,
        1,
        0,
        (const struct sockaddr *)destination,
        (int)sizeof(*destination)
    );
    if (sent == SOCKET_ERROR) {
        fprintf(stderr, "send-fallback probe failed: %d\n", WSAGetLastError());
        return -1;
    }
    if (wait_for_socket_notification(socket_value) != 0) {
        return -1;
    }

    r_admission_config_t config;
    r_client_admission_config_defaults(&config);
    config.bucket_name = "win32-reset-test";
    config.service_name = "win32-reset-test";
    config.metrics_label = "win32-reset-test";

    bool completed = false;
    r_admission_request_t request;
    int status = r_client_admission_start(
        runtime->handle,
        &request,
        &config,
        record_unexpected_completion,
        &completed
    );
    if (status != RCLIENT_OK) {
        fprintf(stderr,
            "runtime send returned %s (%d), Winsock error %d\n",
            r_runtime_status_name(status),
            status,
            WSAGetLastError());
        return -1;
    }
    if (completed) {
        fputs("closed UDP endpoint completed admission synchronously\n", stderr);
        return -1;
    }

    r_runtime_admission_cancel(runtime, &request);
    return set_connreset_reporting(socket_value, FALSE);
}

int main(void) {
    if (send_retry_policy_is_bounded() != 0) {
        return 1;
    }

    r_runtime_options_t options = {
        .auth_key = TEST_AES_KEY,
        .server_host = "127.0.0.1",
        .server_port = 9u,
    };
    r_runtime_client_t runtime;
    int status = r_runtime_client_init(&runtime, &options);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "runtime initialization failed: %s (%d)\n",
            r_runtime_status_name(status), status);
        return 1;
    }

    int exit_status = 1;
    struct sockaddr_in destination = {0};
    SOCKET runtime_socket = find_runtime_ipv4_socket(&runtime);
    SOCKET control = INVALID_SOCKET;
    if (runtime_socket == INVALID_SOCKET) {
        fputs("runtime did not create an IPv4 UDP socket\n", stderr);
    } else if ((control = open_bound_control_socket()) == INVALID_SOCKET) {
        /* The helper already printed a precise socket diagnostic. */
    } else if (reserve_then_release_loopback_port(&destination) != 0) {
        /* The helper already printed a precise socket diagnostic. */
    } else if (control_socket_observes_connreset(
            control,
            &destination) != 0) {
        /* Prevent a false GREEN on a platform that did not exercise 10054. */
    } else if (runtime_socket_survives_connreset(
            &runtime,
            runtime_socket,
            &destination) != 0) {
        /* The helper already printed a precise socket diagnostic. */
    } else if (runtime_send_survives_connreset(
            &runtime,
            runtime_socket,
            &destination) == 0) {
        exit_status = 0;
    }

    if (control != INVALID_SOCKET) {
        (void)closesocket(control);
    }
    r_runtime_client_destroy(&runtime);
    if (exit_status == 0) {
        puts("test_win32_udp_connreset: PASS");
    }
    return exit_status;
}
