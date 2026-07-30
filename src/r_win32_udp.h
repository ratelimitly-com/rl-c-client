#ifndef R_WIN32_UDP_H
#define R_WIN32_UDP_H

#ifndef _WIN32
#error "r_win32_udp.h is only available on Win32"
#endif

#include <stdbool.h>
#include <winsock2.h>
#include <mswsock.h>

/*
 * Winsock normally turns an ICMP Port Unreachable response into
 * WSAECONNRESET on a later UDP operation. The client handles an unreachable
 * endpoint through its request deadline and replay policy, so that asynchronous
 * ICMP report must not abort the event loop.
 *
 * SIO_UDP_CONNRESET is supported by the Microsoft UDP provider. Keep this
 * best-effort for alternate providers such as Wine; callers also treat a reset
 * returned by recvfrom() as a transient UDP notification.
 */
static inline void r_win32_udp_disable_connreset(SOCKET socket_value) {
    BOOL report_port_unreachable = FALSE;
    DWORD bytes_returned = 0u;
    (void)WSAIoctl(
        socket_value,
        SIO_UDP_CONNRESET,
        &report_port_unreachable,
        (DWORD)sizeof(report_port_unreachable),
        NULL,
        0u,
        &bytes_returned,
        NULL,
        NULL
    );
}

/*
 * WSAECONNRESET describes a previously sent datagram; it does not mean the
 * current datagram was transmitted. Permit one retry to consume that stale
 * notification without allowing a broken provider to spin forever.
 */
static inline bool r_win32_udp_send_should_retry(
    int error,
    bool *reset_retried
) {
    if (error == WSAEINTR) {
        return true;
    }
    if (error == WSAECONNRESET && !*reset_retried) {
        *reset_retried = true;
        return true;
    }
    return false;
}

static inline int r_win32_udp_sendto(
    SOCKET socket_value,
    const char *buffer,
    int length,
    const struct sockaddr *destination,
    int destination_length
) {
    bool reset_retried = false;
    for (;;) {
        int sent = sendto(
            socket_value,
            buffer,
            length,
            0,
            destination,
            destination_length
        );
        if (sent != SOCKET_ERROR) {
            return sent;
        }

        int error = WSAGetLastError();
        if (r_win32_udp_send_should_retry(error, &reset_retried)) {
            continue;
        }
        WSASetLastError(error);
        return SOCKET_ERROR;
    }
}

#endif
