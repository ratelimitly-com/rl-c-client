#ifndef _WIN32
#error "This regression test requires Win32"
#endif

#include <stdio.h>

#include "r_client_runtime.h"

/*
 * Winsock reports an ICMP Port Unreachable response as WSAECONNRESET on a
 * later operation on the UDP socket. First prove this machine exhibits that
 * behavior with an ordinary control socket. Then send the same probes from a
 * runtime-owned socket and require its public receive path to remain usable.
 */

static const char TEST_AES_KEY[] =
    "rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqhmzd8l";

enum {
    RESET_ATTEMPTS = 200,
    RESET_WAIT_MS = 10,
};

static int make_nonblocking(SOCKET socket_value) {
    u_long enabled = 1u;
    if (ioctlsocket(socket_value, FIONBIO, &enabled) == SOCKET_ERROR) {
        fprintf(stderr, "ioctlsocket failed: %d\n", WSAGetLastError());
        return -1;
    }
    return 0;
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
    const struct sockaddr_in *destination
) {
    SOCKET control = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (control == INVALID_SOCKET) {
        fprintf(stderr, "control socket failed: %d\n", WSAGetLastError());
        return -1;
    }
    if (make_nonblocking(control) != 0) {
        (void)closesocket(control);
        return -1;
    }

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
                (void)closesocket(control);
                return 0;
            }
            fprintf(stderr, "control sendto failed: %d\n", error);
            (void)closesocket(control);
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
                (void)closesocket(control);
                return 0;
            }
            if (error == WSAEWOULDBLOCK || error == WSAEINTR) {
                continue;
            }
            fprintf(stderr, "control recvfrom failed: %d\n", error);
            (void)closesocket(control);
            return -1;
        }

        fprintf(stderr, "control socket received an unexpected datagram\n");
        (void)closesocket(control);
        return -1;
    }

    fputs("control socket never observed WSAECONNRESET\n", stderr);
    (void)closesocket(control);
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

static int runtime_socket_survives_connreset(
    r_runtime_client_t *runtime,
    SOCKET socket_value,
    const struct sockaddr_in *destination
) {
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
            return -1;
        }

        Sleep(RESET_WAIT_MS);
        int status = r_runtime_client_on_readable(runtime, socket_value);
        if (status != RCLIENT_OK) {
            fprintf(stderr,
                "runtime receive returned %s (%d), Winsock error %d\n",
                r_runtime_status_name(status),
                status,
                WSAGetLastError());
            return -1;
        }
    }
    return 0;
}

int main(void) {
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
    if (runtime_socket == INVALID_SOCKET) {
        fputs("runtime did not create an IPv4 UDP socket\n", stderr);
    } else if (reserve_then_release_loopback_port(&destination) != 0) {
        /* The helper already printed a precise socket diagnostic. */
    } else if (control_socket_observes_connreset(&destination) != 0) {
        /* Prevent a false GREEN on a platform that did not exercise 10054. */
    } else if (runtime_socket_survives_connreset(
            &runtime,
            runtime_socket,
            &destination) == 0) {
        exit_status = 0;
    }

    r_runtime_client_destroy(&runtime);
    if (exit_status == 0) {
        puts("test_win32_udp_connreset: PASS");
    }
    return exit_status;
}
