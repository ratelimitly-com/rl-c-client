#ifndef R_WIN32_UDP_H
#define R_WIN32_UDP_H

#ifndef _WIN32
#error "r_win32_udp.h is only available on Win32"
#endif

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
static void r_win32_udp_disable_connreset(SOCKET socket_value) {
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

#endif
