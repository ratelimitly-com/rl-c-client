#ifndef _WIN32
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../include/r_client_runtime.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <windns.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <fcntl.h>
#include <netdb.h>
#include <resolv.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

enum {
    R_RUNTIME_MAX_SRV_RECORDS = 32,
    R_RUNTIME_DNS_PACKET_SIZE = 65536,
};

static const char R_RUNTIME_FIXED_TARGET[] =
    "s-1.ratelimitly-example.invalid";

const char *r_runtime_status_name(int status) {
    switch (status) {
        case RCLIENT_OK:
            return "ok";
        case RCLIENT_ERR_IO:
            return "I/O error";
        case RCLIENT_ERR_TIMEOUT:
            return "timeout";
        case RCLIENT_ERR_PROTOCOL:
            return "protocol error";
        case RCLIENT_ERR_AUTH:
            return "authentication error";
        case RCLIENT_ERR_DNS:
            return "DNS error";
        case RCLIENT_ERR_CONFIG:
            return "configuration error";
        case RCLIENT_ERR_NOMEM:
            return "out of memory";
        default:
            return "unknown error";
    }
}

uint64_t r_runtime_wall_time_ms(void) {
#ifdef _WIN32
    FILETIME file_time;
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER ticks = {
        .LowPart = file_time.dwLowDateTime,
        .HighPart = file_time.dwHighDateTime,
    };
    const uint64_t windows_to_unix_epoch_ms = 11644473600000ULL;
    return ticks.QuadPart / 10000ULL - windows_to_unix_epoch_ms;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return 0u;
    }
    return (uint64_t)now.tv_sec * 1000u
        + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

static uint64_t runtime_wall_time(void *context) {
    (void)context;
    return r_runtime_wall_time_ms();
}

int r_runtime_monotonic_time_ms(uint64_t *out_milliseconds) {
    if (!out_milliseconds) {
        return RCLIENT_ERR_CONFIG;
    }
#ifdef _WIN32
    *out_milliseconds = GetTickCount64();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return RCLIENT_ERR_IO;
    }
    *out_milliseconds = (uint64_t)now.tv_sec * 1000u
        + (uint64_t)now.tv_nsec / 1000000u;
#endif
    return RCLIENT_OK;
}

static void runtime_log(void *context, r_log_level_t level, const char *message) {
    (void)context;
    static const char *names[] = {"error", "warn", "info", "debug"};
    const char *name = level >= R_LOG_ERROR && level <= R_LOG_DEBUG
        ? names[level]
        : "unknown";
    fprintf(stderr, "rl-c-client[%s]: %s\n", name, message ? message : "");
}

static void close_socket(r_runtime_socket_t socket_value) {
#ifdef _WIN32
    closesocket(socket_value);
#else
    close(socket_value);
#endif
}

static int set_nonblocking(r_runtime_socket_t socket_value) {
#ifdef _WIN32
    u_long enabled = 1u;
    return ioctlsocket(socket_value, FIONBIO, &enabled) == 0 ? 0 : -1;
#else
    int flags = fcntl(socket_value, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_value, F_SETFL, flags | O_NONBLOCK) != 0) {
        return -1;
    }
    return 0;
#endif
}

static r_runtime_socket_t open_udp_socket(int family) {
    r_runtime_socket_t socket_value = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_value == R_RUNTIME_INVALID_SOCKET) {
        return R_RUNTIME_INVALID_SOCKET;
    }
    if (set_nonblocking(socket_value) != 0) {
        close_socket(socket_value);
        return R_RUNTIME_INVALID_SOCKET;
    }

    if (family == AF_INET) {
        struct sockaddr_in address = {0};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(socket_value, (const struct sockaddr *)&address,
                (r_socklen_t)sizeof(address)) != 0) {
            close_socket(socket_value);
            return R_RUNTIME_INVALID_SOCKET;
        }
    } else {
        struct sockaddr_in6 address = {0};
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_any;
        int ipv6_only = 1;
#ifdef _WIN32
        (void)setsockopt(socket_value, IPPROTO_IPV6, IPV6_V6ONLY,
            (const char *)&ipv6_only, (int)sizeof(ipv6_only));
#else
        (void)setsockopt(socket_value, IPPROTO_IPV6, IPV6_V6ONLY,
            &ipv6_only, (socklen_t)sizeof(ipv6_only));
#endif
        if (bind(socket_value, (const struct sockaddr *)&address,
                (r_socklen_t)sizeof(address)) != 0) {
            close_socket(socket_value);
            return R_RUNTIME_INVALID_SOCKET;
        }
    }
    return socket_value;
}

static int runtime_udp_send(
    void *context,
    const r_addr_t *to,
    const uint8_t *buffer,
    size_t length
) {
    r_runtime_client_t *runtime = context;
    if (!runtime || !to || !buffer || length == 0u) {
        return -1;
    }

    r_runtime_socket_t socket_value = R_RUNTIME_INVALID_SOCKET;
    for (size_t i = 0; i < runtime->socket_count; i++) {
        struct sockaddr_storage local;
        r_socklen_t local_length = (r_socklen_t)sizeof(local);
        if (getsockname(runtime->sockets[i], (struct sockaddr *)&local,
                &local_length) == 0
            && local.ss_family == to->sa.ss_family) {
            socket_value = runtime->sockets[i];
            break;
        }
    }
    if (socket_value == R_RUNTIME_INVALID_SOCKET) {
        return -1;
    }

#ifdef _WIN32
    if (length > INT_MAX) {
        return -1;
    }
    int sent;
    do {
        sent = sendto(socket_value, (const char *)buffer, (int)length, 0,
            (const struct sockaddr *)&to->sa, to->len);
    } while (sent == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);
    return sent == (int)length ? 0 : -1;
#else
    ssize_t sent;
    do {
        sent = sendto(socket_value, buffer, length, 0,
            (const struct sockaddr *)&to->sa, to->len);
    } while (sent < 0 && errno == EINTR);
    return sent == (ssize_t)length ? 0 : -1;
#endif
}

static int copy_fixed_host(
    r_runtime_client_t *runtime,
    const r_runtime_options_t *options
) {
    if (!options->server_host && options->server_port == 0u) {
        return RCLIENT_OK;
    }
    if (!options->server_host || options->server_port == 0u) {
        return RCLIENT_ERR_CONFIG;
    }
    size_t length = strlen(options->server_host);
    if (length == 0u || length >= sizeof(runtime->server_host)) {
        return RCLIENT_ERR_CONFIG;
    }
    memcpy(runtime->server_host, options->server_host, length + 1u);
    runtime->server_port = options->server_port;
    return RCLIENT_OK;
}

static int resolve_addresses(
    const char *host,
    r_dns_addr_cb callback,
    void *user
) {
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, NULL, &hints, &addresses) != 0) {
        callback(user, RCLIENT_ERR_DNS, NULL, 0u);
        return 0;
    }

    size_t count = 0u;
    for (const struct addrinfo *item = addresses; item; item = item->ai_next) {
        if ((item->ai_family == AF_INET || item->ai_family == AF_INET6)
            && (size_t)item->ai_addrlen <= sizeof(struct sockaddr_storage)) {
            count++;
        }
    }
    r_addr_t *result = count ? calloc(count, sizeof(*result)) : NULL;
    if (count && !result) {
        freeaddrinfo(addresses);
        callback(user, RCLIENT_ERR_NOMEM, NULL, 0u);
        return 0;
    }

    size_t index = 0u;
    for (const struct addrinfo *item = addresses; item; item = item->ai_next) {
        if ((item->ai_family != AF_INET && item->ai_family != AF_INET6)
            || (size_t)item->ai_addrlen > sizeof(struct sockaddr_storage)) {
            continue;
        }
        memcpy(&result[index].sa, item->ai_addr, (size_t)item->ai_addrlen);
        result[index].len = (r_socklen_t)item->ai_addrlen;
        index++;
    }
    freeaddrinfo(addresses);
    callback(user, count ? RCLIENT_OK : RCLIENT_ERR_DNS, result, count);
    free(result);
    return 0;
}

static int runtime_resolve_addresses(
    void *context,
    const char *name,
    r_dns_req_id_t *request_id,
    r_dns_addr_cb callback,
    void *user
) {
    r_runtime_client_t *runtime = context;
    if (!runtime || !name || !request_id || !callback) {
        return -1;
    }
    *request_id = 0u;
    const char *host = runtime->server_host[0] != '\0'
        && strcmp(name, R_RUNTIME_FIXED_TARGET) == 0
        ? runtime->server_host
        : name;
    return resolve_addresses(host, callback, user);
}

#ifdef _WIN32
static int resolve_srv_records(
    const char *name,
    r_dns_srv_cb callback,
    void *user
) {
    PDNS_RECORD records = NULL;
    DNS_STATUS query_status = DnsQuery_A(
        name,
        DNS_TYPE_SRV,
        DNS_QUERY_STANDARD,
        NULL,
        &records,
        NULL
    );
    if (query_status != ERROR_SUCCESS) {
        callback(user, RCLIENT_ERR_DNS, NULL, 0u);
        return 0;
    }

    r_srv_record_t result[R_RUNTIME_MAX_SRV_RECORDS];
    size_t count = 0u;
    for (PDNS_RECORD item = records;
         item && count < R_RUNTIME_MAX_SRV_RECORDS;
         item = item->pNext) {
        if (item->wType != DNS_TYPE_SRV || !item->Data.SRV.pNameTarget) {
            continue;
        }
        result[count].target = item->Data.SRV.pNameTarget;
        result[count].port = item->Data.SRV.wPort;
        result[count].priority = item->Data.SRV.wPriority;
        result[count].weight = item->Data.SRV.wWeight;
        uint64_t ttl_ms = (uint64_t)item->dwTtl * 1000u;
        result[count].ttl_ms = ttl_ms > UINT32_MAX
            ? UINT32_MAX
            : (uint32_t)ttl_ms;
        count++;
    }
    callback(user, count ? RCLIENT_OK : RCLIENT_ERR_DNS, result, count);
    DnsRecordListFree(records, DnsFreeRecordList);
    return 0;
}
#else
static int resolve_srv_records(
    const char *name,
    r_dns_srv_cb callback,
    void *user
) {
    unsigned char *answer = malloc(R_RUNTIME_DNS_PACKET_SIZE);
    if (!answer) {
        callback(user, RCLIENT_ERR_NOMEM, NULL, 0u);
        return 0;
    }
    int answer_length = res_query(
        name,
        ns_c_in,
        ns_t_srv,
        answer,
        R_RUNTIME_DNS_PACKET_SIZE
    );
    ns_msg message;
    if (answer_length < 0 || ns_initparse(answer, answer_length, &message) != 0) {
        free(answer);
        callback(user, RCLIENT_ERR_DNS, NULL, 0u);
        return 0;
    }

    int answer_count = ns_msg_count(message, ns_s_an);
    if (answer_count > R_RUNTIME_MAX_SRV_RECORDS) {
        answer_count = R_RUNTIME_MAX_SRV_RECORDS;
    }
    r_srv_record_t records[R_RUNTIME_MAX_SRV_RECORDS];
    char targets[R_RUNTIME_MAX_SRV_RECORDS][NS_MAXDNAME];
    size_t count = 0u;
    for (int i = 0; i < answer_count; i++) {
        ns_rr record;
        if (ns_parserr(&message, ns_s_an, i, &record) != 0
            || ns_rr_type(record) != ns_t_srv
            || ns_rr_rdlen(record) < 7) {
            continue;
        }
        const unsigned char *data = ns_rr_rdata(record);
        if (dn_expand(answer, answer + answer_length, data + 6,
                targets[count], sizeof(targets[count])) < 0) {
            continue;
        }
        uint64_t ttl_ms = (uint64_t)ns_rr_ttl(record) * 1000u;
        records[count].target = targets[count];
        records[count].priority = ns_get16(data);
        records[count].weight = ns_get16(data + 2);
        records[count].port = ns_get16(data + 4);
        records[count].ttl_ms = ttl_ms > UINT32_MAX
            ? UINT32_MAX
            : (uint32_t)ttl_ms;
        count++;
    }
    callback(user, count ? RCLIENT_OK : RCLIENT_ERR_DNS, records, count);
    free(answer);
    return 0;
}
#endif

static int runtime_resolve_srv(
    void *context,
    const char *name,
    r_dns_req_id_t *request_id,
    r_dns_srv_cb callback,
    void *user
) {
    r_runtime_client_t *runtime = context;
    if (!runtime || !name || !request_id || !callback) {
        return -1;
    }
    *request_id = 0u;
    if (runtime->server_host[0] != '\0') {
        r_srv_record_t record = {
            .target = R_RUNTIME_FIXED_TARGET,
            .port = runtime->server_port,
            .ttl_ms = 60000u,
        };
        callback(user, RCLIENT_OK, &record, 1u);
        return 0;
    }
    return resolve_srv_records(name, callback, user);
}

static void runtime_cancel_dns(void *context, r_dns_req_id_t request_id) {
    (void)context;
    (void)request_id;
}

int r_runtime_options_from_env(r_runtime_options_t *out_options) {
    if (!out_options) {
        return RCLIENT_ERR_CONFIG;
    }
    memset(out_options, 0, sizeof(*out_options));
    out_options->tenant_dns_name = getenv("RATELIMITLY_TENANT");
    out_options->auth_key = getenv("RATELIMITLY_AUTH_KEY");
    out_options->server_host = getenv("RATELIMITLY_EXAMPLE_SERVER_HOST");
    const char *port_text = getenv("RATELIMITLY_EXAMPLE_SERVER_PORT");
    if (port_text && port_text[0] != '\0') {
        char *end = NULL;
        errno = 0;
        unsigned long port = strtoul(port_text, &end, 10);
        if (errno != 0 || !end || *end != '\0' || port == 0u
            || port > UINT16_MAX) {
            return RCLIENT_ERR_CONFIG;
        }
        out_options->server_port = (uint16_t)port;
    }
    if (!out_options->tenant_dns_name || !out_options->auth_key) {
        return RCLIENT_ERR_CONFIG;
    }
    return (out_options->server_host == NULL)
            == (out_options->server_port == 0u)
        ? RCLIENT_OK
        : RCLIENT_ERR_CONFIG;
}

int r_runtime_client_init(
    r_runtime_client_t *runtime,
    const r_runtime_options_t *options
) {
    if (!runtime || !options || !options->tenant_dns_name || !options->auth_key) {
        return RCLIENT_ERR_CONFIG;
    }
    memset(runtime, 0, sizeof(*runtime));
    runtime->sockets[0] = R_RUNTIME_INVALID_SOCKET;
    runtime->sockets[1] = R_RUNTIME_INVALID_SOCKET;

#ifdef _WIN32
    WSADATA winsock_data;
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
        return RCLIENT_ERR_IO;
    }
    runtime->network_started = true;
#endif

    int status = copy_fixed_host(runtime, options);
    if (status != RCLIENT_OK) {
        r_runtime_client_destroy(runtime);
        return status;
    }

    r_runtime_socket_t ipv4 = open_udp_socket(AF_INET);
    if (ipv4 != R_RUNTIME_INVALID_SOCKET) {
        runtime->sockets[runtime->socket_count++] = ipv4;
    }
    r_runtime_socket_t ipv6 = open_udp_socket(AF_INET6);
    if (ipv6 != R_RUNTIME_INVALID_SOCKET) {
        runtime->sockets[runtime->socket_count++] = ipv6;
    }
    if (runtime->socket_count == 0u) {
        r_runtime_client_destroy(runtime);
        return RCLIENT_ERR_IO;
    }

    r_auth_key_info_t key;
    status = r_client_parse_auth_key(options->auth_key, &key);
    if (status != RCLIENT_OK) {
        r_runtime_client_destroy(runtime);
        return status;
    }

    r_request_policy_t policy;
    r_client_default_request_policy(&policy);
    policy.attempt_timeout_ms = 1000u;
    policy.retry.retry_attempts = 0u;

    r_client_config_t config = {0};
    config.tenant.dns_name = options->tenant_dns_name;
    config.tenant.key_id = key.key_id;
    config.tenant.auth.type = key.type;
    config.tenant.auth.secret = options->auth_key;
    config.request_policy = &policy;

    r_io_ops_t io = {
        .ctx = runtime,
        .udp_send = runtime_udp_send,
        .now_ms = runtime_wall_time,
        .log = runtime_log,
    };
    r_resolver_ops_t resolver = {
        .ctx = runtime,
        .resolve_srv = runtime_resolve_srv,
        .resolve_addrs = runtime_resolve_addresses,
        .cancel = runtime_cancel_dns,
    };
    status = r_client_create(&config, &io, &resolver, &runtime->handle);
    if (status != RCLIENT_OK) {
        r_runtime_client_destroy(runtime);
    }
    return status;
}

void r_runtime_client_destroy(r_runtime_client_t *runtime) {
    if (!runtime) {
        return;
    }
    if (runtime->handle) {
        r_client_destroy(runtime->handle);
        runtime->handle = NULL;
    }
    for (size_t i = 0; i < runtime->socket_count; i++) {
        if (runtime->sockets[i] != R_RUNTIME_INVALID_SOCKET) {
            close_socket(runtime->sockets[i]);
            runtime->sockets[i] = R_RUNTIME_INVALID_SOCKET;
        }
    }
    runtime->socket_count = 0u;
#ifdef _WIN32
    if (runtime->network_started) {
        WSACleanup();
        runtime->network_started = false;
    }
#endif
}

size_t r_runtime_socket_count(const r_runtime_client_t *runtime) {
    return runtime ? runtime->socket_count : 0u;
}

r_runtime_socket_t r_runtime_socket_at(
    const r_runtime_client_t *runtime,
    size_t index
) {
    return runtime && index < runtime->socket_count
        ? runtime->sockets[index]
        : R_RUNTIME_INVALID_SOCKET;
}

int r_runtime_client_on_readable(
    r_runtime_client_t *runtime,
    r_runtime_socket_t socket_value
) {
    if (!runtime || !runtime->handle
        || socket_value == R_RUNTIME_INVALID_SOCKET) {
        return RCLIENT_ERR_CONFIG;
    }

    for (;;) {
        uint8_t buffer[65536];
        r_addr_t from = {0};
        from.len = (r_socklen_t)sizeof(from.sa);
#ifdef _WIN32
        int length = recvfrom(
            socket_value,
            (char *)buffer,
            (int)sizeof(buffer),
            0,
            (struct sockaddr *)&from.sa,
            &from.len
        );
        if (length == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAEINTR) {
                continue;
            }
            if (error == WSAEWOULDBLOCK) {
                return RCLIENT_OK;
            }
            return RCLIENT_ERR_IO;
        }
#else
        ssize_t length = recvfrom(
            socket_value,
            buffer,
            sizeof(buffer),
            0,
            (struct sockaddr *)&from.sa,
            &from.len
        );
        if (length < 0 && errno == EINTR) {
            continue;
        }
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return RCLIENT_OK;
        }
        if (length < 0) {
            return RCLIENT_ERR_IO;
        }
#endif
        int status = r_client_on_datagram(
            runtime->handle,
            buffer,
            (size_t)length,
            &from
        );
        if (status != RCLIENT_OK) {
            return status;
        }
    }
}

int r_runtime_admission_delay_ms(
    const r_admission_request_t *request,
    uint64_t *out_delay_ms
) {
    if (!out_delay_ms) {
        return RCLIENT_ERR_CONFIG;
    }
    uint64_t deadline_ms = 0u;
    int status = r_client_admission_deadline_ms(request, &deadline_ms);
    if (status != RCLIENT_OK) {
        return status;
    }
    uint64_t now_ms = r_runtime_wall_time_ms();
    *out_delay_ms = deadline_ms > now_ms ? deadline_ms - now_ms : 0u;
    return RCLIENT_OK;
}

int r_runtime_admission_on_timeout(
    r_runtime_client_t *runtime,
    r_admission_request_t *request
) {
    if (!runtime || !runtime->handle) {
        return RCLIENT_ERR_CONFIG;
    }
    return r_client_admission_on_timeout(
        runtime->handle,
        request,
        r_runtime_wall_time_ms()
    );
}

void r_runtime_admission_cancel(
    r_runtime_client_t *runtime,
    r_admission_request_t *request
) {
    if (runtime) {
        r_client_admission_cancel(runtime->handle, request);
    }
}

int r_runtime_admission_run_and_report(
    r_runtime_client_t *runtime,
    r_admission_request_t *request,
    r_runtime_protected_work_cb protected_work,
    void *user,
    uint32_t *out_observed_latency_ms
) {
    if (!runtime || !runtime->handle || !request || !request->admitted
        || !protected_work) {
        return RCLIENT_ERR_CONFIG;
    }

    uint64_t started_ms = 0u;
    int status = r_runtime_monotonic_time_ms(&started_ms);
    if (status != RCLIENT_OK) {
        return status;
    }
    status = protected_work(user);
    if (status != RCLIENT_OK) {
        return status;
    }
    uint64_t finished_ms = 0u;
    status = r_runtime_monotonic_time_ms(&finished_ms);
    if (status != RCLIENT_OK || finished_ms < started_ms) {
        return RCLIENT_ERR_IO;
    }

    uint64_t elapsed_ms = finished_ms - started_ms;
    uint32_t observed_ms = elapsed_ms > UINT32_MAX
        ? UINT32_MAX
        : (uint32_t)elapsed_ms;
    if (out_observed_latency_ms) {
        *out_observed_latency_ms = observed_ms;
    }
    return r_client_admission_report_latency(
        runtime->handle,
        request,
        observed_ms
    );
}
