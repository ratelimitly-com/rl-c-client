#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "rl_example.h"

#include <sys/types.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RL_EXAMPLE_MAX_SRV_RECORDS 32
#define RL_EXAMPLE_DNS_PACKET_SIZE 65536
#define RL_EXAMPLE_FIXED_TARGET "s-1.ratelimitly-example.invalid"

/* rl-c-client deadlines use milliseconds on the same clock supplied here. */
static uint64_t example_now_ms(void *context) {
    (void)context;
    struct timespec now;
    /* Monotonic time cannot jump when wall-clock time is corrected. */
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

const char *rl_example_status_name(int status) {
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

static void example_log(void *context, r_log_level_t level, const char *message) {
    (void)context;
    static const char *names[] = {"error", "warn", "info", "debug"};
    const char *name = level >= R_LOG_ERROR && level <= R_LOG_DEBUG
        ? names[level]
        : "unknown";
    fprintf(stderr, "rl-c-client[%s]: %s\n", name, message ? message : "");
}

static int set_nonblocking(int socket_fd) {
    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return -1;
    }
    return 0;
}

static int open_udp_socket(int family) {
    int socket_fd = socket(family, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        return -1;
    }
    if (set_nonblocking(socket_fd) != 0) {
        close(socket_fd);
        return -1;
    }

    if (family == AF_INET) {
        struct sockaddr_in address = {0};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(socket_fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
            close(socket_fd);
            return -1;
        }
    } else {
        struct sockaddr_in6 address = {0};
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_any;
        int ipv6_only = 1;
        (void)setsockopt(socket_fd, IPPROTO_IPV6, IPV6_V6ONLY,
            &ipv6_only, sizeof(ipv6_only));
        if (bind(socket_fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
            close(socket_fd);
            return -1;
        }
    }
    return socket_fd;
}

/*
 * rl-c-client gives the application a destination address, but never sends on
 * its behalf. Choose the socket with the matching address family and preserve
 * datagram boundaries by requiring sendto() to accept the complete payload.
 */
static int example_udp_send(
    void *context,
    const r_addr_t *to,
    const uint8_t *buffer,
    size_t length
) {
    rl_example_client_t *client = context;
    if (!client || !to || !buffer || length == 0) {
        return -1;
    }

    int socket_fd = -1;
    for (size_t i = 0; i < client->socket_count; i++) {
        struct sockaddr_storage local;
        socklen_t local_length = sizeof(local);
        if (getsockname(client->sockets[i], (struct sockaddr *)&local, &local_length) == 0
            && local.ss_family == to->sa.ss_family) {
            socket_fd = client->sockets[i];
            break;
        }
    }
    if (socket_fd < 0) {
        return -1;
    }

    ssize_t sent;
    do {
        sent = sendto(socket_fd, buffer, length, 0,
            (const struct sockaddr *)&to->sa, to->len);
    } while (sent < 0 && errno == EINTR);
    return sent == (ssize_t)length ? 0 : -1;
}

static int copy_fixed_host(
    rl_example_client_t *client,
    const rl_example_options_t *options
) {
    if (!options->server_host && options->server_port == 0) {
        return RCLIENT_OK;
    }
    if (!options->server_host || options->server_port == 0) {
        return RCLIENT_ERR_CONFIG;
    }
    size_t length = strlen(options->server_host);
    if (length == 0 || length >= sizeof(client->server_host)) {
        return RCLIENT_ERR_CONFIG;
    }
    memcpy(client->server_host, options->server_host, length + 1);
    client->server_port = options->server_port;
    return RCLIENT_OK;
}

/*
 * These resolver callbacks complete synchronously. That is legal in the
 * public API and useful for readable examples, but it can re-enter client code
 * before the initiating call returns. Production loops will usually replace
 * this section with their asynchronous DNS integration.
 */
static int resolve_addresses(
    const char *host,
    r_dns_addr_cb callback,
    void *user
) {
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *addresses = NULL;
    int status = getaddrinfo(host, NULL, &hints, &addresses);
    if (status != 0) {
        callback(user, RCLIENT_ERR_DNS, NULL, 0);
        return 0;
    }

    size_t count = 0;
    for (const struct addrinfo *item = addresses; item; item = item->ai_next) {
        if ((item->ai_family == AF_INET || item->ai_family == AF_INET6)
            && item->ai_addrlen <= sizeof(struct sockaddr_storage)) {
            count++;
        }
    }
    r_addr_t *result = count ? calloc(count, sizeof(*result)) : NULL;
    if (count && !result) {
        freeaddrinfo(addresses);
        callback(user, RCLIENT_ERR_NOMEM, NULL, 0);
        return 0;
    }

    size_t index = 0;
    for (const struct addrinfo *item = addresses; item; item = item->ai_next) {
        if ((item->ai_family != AF_INET && item->ai_family != AF_INET6)
            || item->ai_addrlen > sizeof(struct sockaddr_storage)) {
            continue;
        }
        memcpy(&result[index].sa, item->ai_addr, item->ai_addrlen);
        result[index].len = (socklen_t)item->ai_addrlen;
        index++;
    }
    freeaddrinfo(addresses);
    callback(user, count ? RCLIENT_OK : RCLIENT_ERR_DNS, result, count);
    free(result);
    return 0;
}

static int example_resolve_addresses(
    void *context,
    const char *name,
    r_dns_req_id_t *request_id,
    r_dns_addr_cb callback,
    void *user
) {
    rl_example_client_t *client = context;
    if (!client || !name || !request_id || !callback) {
        return -1;
    }
    *request_id = 0;
    const char *host = client->server_host[0] != '\0'
        && strcmp(name, RL_EXAMPLE_FIXED_TARGET) == 0
        ? client->server_host
        : name;
    return resolve_addresses(host, callback, user);
}

static int example_resolve_srv(
    void *context,
    const char *name,
    r_dns_req_id_t *request_id,
    r_dns_srv_cb callback,
    void *user
) {
    rl_example_client_t *client = context;
    if (!client || !name || !request_id || !callback) {
        return -1;
    }
    *request_id = 0;

    if (client->server_host[0] != '\0') {
        r_srv_record_t record = {
            .target = RL_EXAMPLE_FIXED_TARGET,
            .port = client->server_port,
            .ttl_ms = 60000,
        };
        callback(user, RCLIENT_OK, &record, 1);
        return 0;
    }

    unsigned char *answer = malloc(RL_EXAMPLE_DNS_PACKET_SIZE);
    if (!answer) {
        callback(user, RCLIENT_ERR_NOMEM, NULL, 0);
        return 0;
    }
    int answer_length = res_query(name, ns_c_in, ns_t_srv,
        answer, RL_EXAMPLE_DNS_PACKET_SIZE);
    ns_msg message;
    if (answer_length < 0 || ns_initparse(answer, answer_length, &message) != 0) {
        free(answer);
        callback(user, RCLIENT_ERR_DNS, NULL, 0);
        return 0;
    }

    int answer_count = ns_msg_count(message, ns_s_an);
    if (answer_count > RL_EXAMPLE_MAX_SRV_RECORDS) {
        answer_count = RL_EXAMPLE_MAX_SRV_RECORDS;
    }
    r_srv_record_t records[RL_EXAMPLE_MAX_SRV_RECORDS];
    char targets[RL_EXAMPLE_MAX_SRV_RECORDS][NS_MAXDNAME];
    size_t count = 0;
    for (int i = 0; i < answer_count; i++) {
        ns_rr record;
        if (ns_parserr(&message, ns_s_an, i, &record) != 0
            || ns_rr_type(record) != ns_t_srv
            || ns_rr_rdlen(record) < 7) {
            continue;
        }
        const unsigned char *data = ns_rr_rdata(record);
        int target_length = dn_expand(answer, answer + answer_length, data + 6,
            targets[count], sizeof(targets[count]));
        if (target_length < 0) {
            continue;
        }
        uint64_t ttl_ms = (uint64_t)ns_rr_ttl(record) * 1000u;
        records[count].target = targets[count];
        records[count].priority = ns_get16(data);
        records[count].weight = ns_get16(data + 2);
        records[count].port = ns_get16(data + 4);
        records[count].ttl_ms = ttl_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ttl_ms;
        count++;
    }
    callback(user, count ? RCLIENT_OK : RCLIENT_ERR_DNS, records, count);
    free(answer);
    return 0;
}

static void example_cancel_dns(void *context, r_dns_req_id_t request_id) {
    (void)context;
    (void)request_id;
}

static void example_request_complete(
    void *user,
    r_client_req_t *client_request,
    int status,
    const r_rate_limit_result_t *result
) {
    (void)client_request;
    rl_example_request_t *request = user;
    request->handle = NULL;
    request->active = false;
    bool allowed = status == RCLIENT_OK && result && result->success;
    request->callback(request->user, status, allowed);
}

int rl_example_options_from_env(rl_example_options_t *options) {
    if (!options) {
        return RCLIENT_ERR_CONFIG;
    }
    memset(options, 0, sizeof(*options));
    options->tenant_dns_name = getenv("RATELIMITLY_TENANT");
    options->auth_key = getenv("RATELIMITLY_AUTH_KEY");
    options->server_host = getenv("RATELIMITLY_EXAMPLE_SERVER_HOST");
    const char *port_text = getenv("RATELIMITLY_EXAMPLE_SERVER_PORT");
    if (port_text && port_text[0] != '\0') {
        char *end = NULL;
        errno = 0;
        unsigned long port = strtoul(port_text, &end, 10);
        if (errno != 0 || !end || *end != '\0' || port == 0 || port > UINT16_MAX) {
            return RCLIENT_ERR_CONFIG;
        }
        options->server_port = (uint16_t)port;
    }
    if (!options->tenant_dns_name || !options->auth_key) {
        return RCLIENT_ERR_CONFIG;
    }
    return (options->server_host == NULL) == (options->server_port == 0)
        ? RCLIENT_OK
        : RCLIENT_ERR_CONFIG;
}

int rl_example_client_init(
    rl_example_client_t *client,
    const rl_example_options_t *options
) {
    if (!client || !options || !options->tenant_dns_name || !options->auth_key) {
        return RCLIENT_ERR_CONFIG;
    }
    memset(client, 0, sizeof(*client));
    client->sockets[0] = -1;
    client->sockets[1] = -1;

    int status = copy_fixed_host(client, options);
    if (status != RCLIENT_OK) {
        return status;
    }

    int ipv4 = open_udp_socket(AF_INET);
    if (ipv4 >= 0) {
        client->sockets[client->socket_count++] = ipv4;
    }
    int ipv6 = open_udp_socket(AF_INET6);
    if (ipv6 >= 0) {
        client->sockets[client->socket_count++] = ipv6;
    }
    if (client->socket_count == 0) {
        return RCLIENT_ERR_IO;
    }

    r_auth_key_info_t key;
    status = r_client_parse_auth_key(options->auth_key, &key);
    if (status != RCLIENT_OK) {
        rl_example_client_destroy(client);
        return status;
    }

    /* One short attempt keeps failures and demonstrations deterministic. */
    r_request_policy_t policy;
    r_client_default_request_policy(&policy);
    policy.attempt_timeout_ms = 1000;
    policy.retry.retry_attempts = 0;

    r_client_config_t config = {0};
    config.tenant.dns_name = options->tenant_dns_name;
    config.tenant.key_id = key.key_id;
    config.tenant.auth.type = key.type;
    config.tenant.auth.secret = options->auth_key;
    config.request_policy = &policy;

    r_io_ops_t io = {
        .ctx = client,
        .udp_send = example_udp_send,
        .now_ms = example_now_ms,
        .log = example_log,
    };
    r_resolver_ops_t resolver = {
        .ctx = client,
        .resolve_srv = example_resolve_srv,
        .resolve_addrs = example_resolve_addresses,
        .cancel = example_cancel_dns,
    };
    status = r_client_create(&config, &io, &resolver, &client->handle);
    if (status != RCLIENT_OK) {
        rl_example_client_destroy(client);
    }
    return status;
}

void rl_example_client_destroy(rl_example_client_t *client) {
    if (!client) {
        return;
    }
    if (client->handle) {
        r_client_destroy(client->handle);
        client->handle = NULL;
    }
    for (size_t i = 0; i < client->socket_count; i++) {
        if (client->sockets[i] >= 0) {
            close(client->sockets[i]);
            client->sockets[i] = -1;
        }
    }
    client->socket_count = 0;
}

size_t rl_example_socket_count(const rl_example_client_t *client) {
    return client ? client->socket_count : 0;
}

int rl_example_socket_at(const rl_example_client_t *client, size_t index) {
    return client && index < client->socket_count ? client->sockets[index] : -1;
}

int rl_example_client_on_readable(rl_example_client_t *client, int socket_fd) {
    if (!client || !client->handle || socket_fd < 0) {
        return RCLIENT_ERR_CONFIG;
    }
    /* Read until EAGAIN: readiness APIs are edge-safe only when fully drained. */
    for (;;) {
        uint8_t buffer[65536];
        r_addr_t from = {0};
        from.len = sizeof(from.sa);
        ssize_t length = recvfrom(socket_fd, buffer, sizeof(buffer), 0,
            (struct sockaddr *)&from.sa, &from.len);
        if (length < 0 && errno == EINTR) {
            continue;
        }
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return RCLIENT_OK;
        }
        if (length < 0) {
            return RCLIENT_ERR_IO;
        }
        int status = r_client_on_datagram(client->handle, buffer, (size_t)length, &from);
        if (status != RCLIENT_OK) {
            return status;
        }
    }
}

int rl_example_check(
    rl_example_client_t *client,
    rl_example_request_t *request,
    const char *bucket,
    rl_example_result_cb callback,
    void *user
) {
    if (!client || !client->handle || !request || !bucket || !callback) {
        return RCLIENT_ERR_CONFIG;
    }
    /* The borrowed resource storage lives inside request for the whole check. */
    memset(request, 0, sizeof(*request));
    r_client_hash_id(bucket, request->resource.bucket_id);
    request->resource.window_size_ms = 1000;
    request->resource.rate_limit = 100;
    request->resource.tokens_requested = 1;
    request->callback = callback;
    request->user = user;
    request->active = true;

    int status = r_client_check_rate_limit_async_borrowed(
        client->handle,
        &request->resource,
        1,
        NULL,
        0,
        "example",
        0,
        example_request_complete,
        request,
        &request->handle
    );
    if (status != RCLIENT_OK) {
        request->handle = NULL;
        request->active = false;
    }
    return status;
}

int rl_example_request_delay_ms(
    const rl_example_request_t *request,
    uint64_t *delay_ms
) {
    if (!request || !request->active || !request->handle || !delay_ms) {
        return RCLIENT_ERR_CONFIG;
    }
    uint64_t deadline = 0;
    int status = r_client_request_deadline_ms(request->handle, &deadline);
    if (status != RCLIENT_OK) {
        return status;
    }
    uint64_t now = example_now_ms(NULL);
    *delay_ms = deadline > now ? deadline - now : 0;
    return RCLIENT_OK;
}

int rl_example_request_on_timeout(
    rl_example_client_t *client,
    rl_example_request_t *request
) {
    if (!client || !client->handle || !request || !request->active || !request->handle) {
        return RCLIENT_ERR_CONFIG;
    }
    return r_client_on_timeout(client->handle, request->handle, example_now_ms(NULL));
}

void rl_example_request_cancel(
    rl_example_client_t *client,
    rl_example_request_t *request
) {
    if (!client || !client->handle || !request || !request->active || !request->handle) {
        return;
    }
    r_client_cancel_request(client->handle, request->handle);
    request->handle = NULL;
    request->active = false;
}
