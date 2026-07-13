#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <resolv.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <arpa/nameser.h>

#include "../include/r_client.h"

#define PERF_MAX_PACKET 1400

typedef struct perf_config {
    const char *srv_domain;
    const char *auth_bech32;
    r_auth_type_t auth_type;
    uint64_t tenant_id;
    size_t concurrent_clients;
    uint64_t requests_per_client;
    bool has_duration;
    uint64_t duration_secs;
    const char *bucket_prefix;
    uint64_t attempt_timeout_ms;
    uint32_t retry_attempts;
    r_retry_on_t retry_on;
    r_resend_policy_t retry_resend;
    bool retry_refresh_dns_on_retry;
    uint64_t retry_total_timeout_ms;
    bool ignore_steering;
    bool debug_steering;
    const struct perf_dns_cache *dns_cache;
} perf_config_t;

typedef struct perf_stats {
    atomic_uint_fast64_t requests_sent;
    atomic_uint_fast64_t responses_received;
    atomic_uint_fast64_t errors;
    atomic_uint_fast64_t total_latency_ns;
} perf_stats_t;

typedef struct request_ctx {
    bool done;
    int status;
    bool steering_feedback;
} request_ctx_t;

typedef struct io_ctx {
    int sockfd;
    bool ignore_steering;
} io_ctx_t;

typedef struct dns_resolver_ctx {
    struct __res_state state;
    bool initialized;
} dns_resolver_ctx_t;

typedef struct perf_dns_addr_entry {
    char *name;
    r_addr_t *items;
    size_t count;
} perf_dns_addr_entry_t;

typedef struct perf_dns_cache {
    char *tenant_name;
    char *srv_name;
    r_srv_record_t *srv_records;
    size_t srv_count;
    perf_dns_addr_entry_t *addr_entries;
    size_t addr_entry_count;
} perf_dns_cache_t;

typedef struct worker_ctx {
    size_t client_id;
    const perf_config_t *config;
    perf_stats_t *stats;
    int worker_error;
} worker_ctx_t;

static atomic_bool g_stop_progress = false;

static const char *perf_auth_label(r_auth_type_t auth_type) {
    switch (auth_type) {
        case R_AUTH_COOKIE: return "Cookie";
        case R_AUTH_AES_GCM: return "AES";
        default: return "Unknown";
    }
}

static const char *perf_retry_on_label(r_retry_on_t retry_on) {
    switch (retry_on) {
        case R_RETRY_TIMEOUT_ONLY: return "timeout";
        case R_RETRY_QUORUM_NOT_MET: return "quorum";
        case R_RETRY_INCONSISTENT: return "inconsistent";
        case R_RETRY_NEVER:
        default: return "never";
    }
}

static const char *perf_retry_resend_label(r_resend_policy_t resend) {
    switch (resend) {
        case R_RESEND_MISSING_ONLY: return "missing";
        case R_RESEND_ALL:
        default: return "all";
    }
}

static int perf_parse_auth_bech32(const char *auth_bech32, r_auth_type_t *out_type, uint64_t *out_tenant_id) {
    if (!auth_bech32 || !out_type || !out_tenant_id) {
        return -1;
    }
    r_auth_key_info_t info;
    if (r_client_parse_auth_key(auth_bech32, &info) != RCLIENT_OK) {
        return -1;
    }
    *out_type = info.type;
    *out_tenant_id = info.key_id;
    return 0;
}

static uint64_t perf_now_ms(void *ctx) {
    (void)ctx;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

static uint64_t perf_now_ns_monotonic(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static char *perf_strdup(const char *src) {
    if (!src) {
        return NULL;
    }
    size_t len = strlen(src);
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, src, len + 1);
    return out;
}

static const char *perf_err_str(int status) {
    switch (status) {
    case RCLIENT_OK:
        return "ok";
    case RCLIENT_ERR_IO:
        return "io error";
    case RCLIENT_ERR_TIMEOUT:
        return "timeout";
    case RCLIENT_ERR_PROTOCOL:
        return "protocol error";
    case RCLIENT_ERR_AUTH:
        return "auth error";
    case RCLIENT_ERR_DNS:
        return "dns error";
    case RCLIENT_ERR_CONFIG:
        return "config error";
    case RCLIENT_ERR_NOMEM:
        return "out of memory";
    default:
        return "unknown";
    }
}

static void perf_maybe_print_dns_hint(int status) {
    if (status == RCLIENT_ERR_DNS) {
        fprintf(stderr, "[HINT] Set RCLIENT_DNS_SERVER=127.0.0.1[:port] to use local dnsmasq.\n");
    }
}

static int perf_setup_socket(void) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    int off = 0;
    (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return fd;
}

static bool perf_parse_dns_server(const char *value, struct sockaddr_in *out) {
    if (!value || !out) {
        return false;
    }
    const char *colon = strrchr(value, ':');
    size_t host_len = colon ? (size_t)(colon - value) : strlen(value);
    if (host_len == 0 || host_len >= 64) {
        return false;
    }
    char host[64];
    memcpy(host, value, host_len);
    host[host_len] = '\0';

    int port = 53;
    if (colon && colon[1] != '\0') {
        char *end = NULL;
        long parsed = strtol(colon + 1, &end, 10);
        if (!end || end == colon + 1 || parsed <= 0 || parsed > 65535) {
            return false;
        }
        port = (int)parsed;
    }

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &out->sin_addr) != 1) {
        return false;
    }
    return true;
}

static int perf_udp_send(void *ctx, const r_addr_t *to, const uint8_t *buf, size_t len) {
    io_ctx_t *io = (io_ctx_t *)ctx;
    if (!io || io->sockfd < 0 || !to || !buf) {
        return -1;
    }
    const struct sockaddr *sa = (const struct sockaddr *)&to->sa;
    socklen_t sa_len = to->len;
    struct sockaddr_in6 mapped;

    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        memset(&mapped, 0, sizeof(mapped));
        mapped.sin6_family = AF_INET6;
        mapped.sin6_port = sin->sin_port;
        mapped.sin6_addr.s6_addr[10] = 0xff;
        mapped.sin6_addr.s6_addr[11] = 0xff;
        memcpy(&mapped.sin6_addr.s6_addr[12], &sin->sin_addr, 4);
        sa = (const struct sockaddr *)&mapped;
        sa_len = sizeof(mapped);
    }

    ssize_t rc = sendto(io->sockfd, buf, len, 0, sa, sa_len);
    if (rc < 0 || (size_t)rc != len) {
        return -1;
    }
    return 0;
}

static void perf_on_steering_feedback(void *ctx, bool keep_port) {
    io_ctx_t *io = (io_ctx_t *)ctx;
    if (!io || io->ignore_steering || keep_port) {
        return;
    }
    int new_fd = perf_setup_socket();
    if (new_fd < 0) {
        return;
    }
    int old_fd = io->sockfd;
    io->sockfd = new_fd;
    if (old_fd >= 0) {
        close(old_fd);
    }
}

static void perf_log(void *ctx, r_log_level_t level, const char *msg) {
    (void)ctx;
    const char *prefix = "INFO";
    if (level == R_LOG_ERROR) {
        prefix = "ERROR";
    } else if (level == R_LOG_WARN) {
        prefix = "WARN";
    } else if (level == R_LOG_DEBUG) {
        prefix = "DEBUG";
    }
    fprintf(stderr, "[%s] %s\n", prefix, msg ? msg : "(null)");
}

static int dns_resolver_init(dns_resolver_ctx_t *resolver) {
    if (!resolver) {
        return -1;
    }
    memset(resolver, 0, sizeof(*resolver));
    if (res_ninit(&resolver->state) != 0) {
        return -1;
    }
    const char *dns_server = getenv("RCLIENT_DNS_SERVER");
    if (dns_server && dns_server[0] != '\0') {
        struct sockaddr_in ns;
        if (perf_parse_dns_server(dns_server, &ns)) {
            resolver->state.nscount = 1;
            resolver->state.nsaddr_list[0] = ns;
        } else {
            fprintf(stderr, "[WARN] Invalid RCLIENT_DNS_SERVER: %s\n", dns_server);
        }
    }
    resolver->initialized = true;
    return 0;
}

static void dns_resolver_close(dns_resolver_ctx_t *resolver) {
    if (!resolver || !resolver->initialized) {
        return;
    }
    res_nclose(&resolver->state);
    resolver->initialized = false;
}

typedef struct addr_list {
    r_addr_t *items;
    size_t count;
    size_t cap;
} addr_list_t;

static int addr_list_push(addr_list_t *list, const struct sockaddr *sa, socklen_t len) {
    if (!list || !sa || len == 0) {
        return -1;
    }
    if (list->count == list->cap) {
        size_t next_cap = list->cap == 0 ? 8 : list->cap * 2;
        r_addr_t *next = (r_addr_t *)realloc(list->items, next_cap * sizeof(r_addr_t));
        if (!next) {
            return -1;
        }
        list->items = next;
        list->cap = next_cap;
    }
    r_addr_t *out = &list->items[list->count++];
    memset(out, 0, sizeof(*out));
    if (len > sizeof(out->sa)) {
        return -1;
    }
    memcpy(&out->sa, sa, len);
    out->len = len;
    return 0;
}

static void addr_list_free(addr_list_t *list) {
    if (!list) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static int addr_list_clone(const addr_list_t *list, r_addr_t **out_items, size_t *out_count) {
    if (!list || !out_items || !out_count) {
        return -1;
    }
    *out_items = NULL;
    *out_count = 0;
    if (list->count == 0) {
        return 0;
    }
    r_addr_t *items = (r_addr_t *)calloc(list->count, sizeof(r_addr_t));
    if (!items) {
        return -1;
    }
    memcpy(items, list->items, list->count * sizeof(r_addr_t));
    *out_items = items;
    *out_count = list->count;
    return 0;
}

static int dns_parse_addrs(unsigned char *answer, int len, int qtype, addr_list_t *list) {
    ns_msg handle;
    if (ns_initparse(answer, len, &handle) < 0) {
        return -1;
    }
    int count = ns_msg_count(handle, ns_s_an);
    for (int i = 0; i < count; i++) {
        ns_rr rr;
        if (ns_parserr(&handle, ns_s_an, i, &rr) != 0) {
            continue;
        }
        if ((int)ns_rr_type(rr) != qtype || ns_rr_class(rr) != ns_c_in) {
            continue;
        }
        const unsigned char *rdata = ns_rr_rdata(rr);
        if (qtype == ns_t_a) {
            if (ns_rr_rdlen(rr) != 4) {
                continue;
            }
            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            memcpy(&sin.sin_addr, rdata, 4);
            (void)addr_list_push(list, (const struct sockaddr *)&sin, sizeof(sin));
        } else if (qtype == ns_t_aaaa) {
            if (ns_rr_rdlen(rr) != 16) {
                continue;
            }
            struct sockaddr_in6 sin6;
            memset(&sin6, 0, sizeof(sin6));
            sin6.sin6_family = AF_INET6;
            memcpy(&sin6.sin6_addr, rdata, 16);
            (void)addr_list_push(list, (const struct sockaddr *)&sin6, sizeof(sin6));
        }
    }
    return 0;
}

static int dns_query_addrs(dns_resolver_ctx_t *resolver, const char *name, addr_list_t *list) {
    if (!resolver || !resolver->initialized || !name || !list) {
        return -1;
    }
    memset(list, 0, sizeof(*list));

    unsigned char answer[4096];
    int len_a = res_nquery(&resolver->state, name, ns_c_in, ns_t_a, answer, sizeof(answer));
    if (len_a > 0) {
        (void)dns_parse_addrs(answer, len_a, ns_t_a, list);
    }

    int len_aaaa = res_nquery(&resolver->state, name, ns_c_in, ns_t_aaaa, answer, sizeof(answer));
    if (len_aaaa > 0) {
        (void)dns_parse_addrs(answer, len_aaaa, ns_t_aaaa, list);
    }
    return list->count > 0 ? 0 : -1;
}

static int dns_query_srv(
    dns_resolver_ctx_t *resolver,
    const char *name,
    r_srv_record_t **out_records,
    size_t *out_count
) {
    if (!resolver || !resolver->initialized || !name || !out_records || !out_count) {
        return -1;
    }
    *out_records = NULL;
    *out_count = 0;
    unsigned char answer[4096];
    int len = res_nquery(&resolver->state, name, ns_c_in, ns_t_srv, answer, sizeof(answer));
    if (len <= 0) {
        return -1;
    }

    ns_msg handle;
    if (ns_initparse(answer, len, &handle) < 0) {
        return -1;
    }

    int count = ns_msg_count(handle, ns_s_an);
    if (count <= 0) {
        return -1;
    }
    r_srv_record_t *records = (r_srv_record_t *)calloc((size_t)count, sizeof(r_srv_record_t));
    if (!records) {
        return -1;
    }

    size_t record_count = 0;
    for (int i = 0; i < count; i++) {
        ns_rr rr;
        if (ns_parserr(&handle, ns_s_an, i, &rr) != 0) {
            continue;
        }
        if (ns_rr_type(rr) != ns_t_srv || ns_rr_class(rr) != ns_c_in) {
            continue;
        }
        const unsigned char *rdata = ns_rr_rdata(rr);
        if (ns_rr_rdlen(rr) < 7) {
            continue;
        }
        uint16_t priority = ns_get16(rdata);
        uint16_t weight = ns_get16(rdata + 2);
        uint16_t port = ns_get16(rdata + 4);

        char target[256];
        int expanded = dn_expand(ns_msg_base(handle), ns_msg_end(handle), rdata + 6, target, sizeof(target));
        if (expanded < 0) {
            continue;
        }

        records[record_count].priority = priority;
        records[record_count].weight = weight;
        records[record_count].port = port;
        records[record_count].ttl_ms = (uint32_t)ns_rr_ttl(rr) * 1000u;
        records[record_count].target = perf_strdup(target);
        if (!records[record_count].target) {
            continue;
        }
        record_count++;
    }
    if (record_count == 0) {
        free(records);
        return -1;
    }
    *out_records = records;
    *out_count = record_count;
    return 0;
}

static void perf_resolver_cancel(void *ctx, r_dns_req_id_t req_id) {
    (void)ctx;
    (void)req_id;
}

static void perf_dns_cache_reset(perf_dns_cache_t *cache) {
    if (!cache) {
        return;
    }
    for (size_t i = 0; i < cache->srv_count; i++) {
        free((char *)cache->srv_records[i].target);
    }
    free(cache->srv_records);
    for (size_t i = 0; i < cache->addr_entry_count; i++) {
        free(cache->addr_entries[i].name);
        free(cache->addr_entries[i].items);
    }
    free(cache->addr_entries);
    free(cache->srv_name);
    free(cache->tenant_name);
    memset(cache, 0, sizeof(*cache));
}

static const perf_dns_addr_entry_t *perf_dns_cache_find_addrs(const perf_dns_cache_t *cache, const char *name) {
    if (!cache || !name) {
        return NULL;
    }
    for (size_t i = 0; i < cache->addr_entry_count; i++) {
        if (strcmp(cache->addr_entries[i].name, name) == 0) {
            return &cache->addr_entries[i];
        }
    }
    return NULL;
}

static int perf_dns_cache_add_addrs(perf_dns_cache_t *cache, const char *name, const addr_list_t *list) {
    if (!cache || !name || !list || list->count == 0) {
        return -1;
    }
    if (perf_dns_cache_find_addrs(cache, name)) {
        return 0;
    }
    perf_dns_addr_entry_t *next = (perf_dns_addr_entry_t *)realloc(
        cache->addr_entries,
        (cache->addr_entry_count + 1) * sizeof(*cache->addr_entries)
    );
    if (!next) {
        return -1;
    }
    cache->addr_entries = next;
    perf_dns_addr_entry_t *entry = &cache->addr_entries[cache->addr_entry_count];
    memset(entry, 0, sizeof(*entry));
    entry->name = perf_strdup(name);
    if (!entry->name) {
        return -1;
    }
    if (addr_list_clone(list, &entry->items, &entry->count) != 0) {
        free(entry->name);
        memset(entry, 0, sizeof(*entry));
        return -1;
    }
    cache->addr_entry_count++;
    return 0;
}

static int perf_dns_cache_init(perf_dns_cache_t *cache, const char *tenant_name) {
    if (!cache || !tenant_name) {
        return -1;
    }
    memset(cache, 0, sizeof(*cache));
    cache->tenant_name = perf_strdup(tenant_name);
    if (!cache->tenant_name) {
        perf_dns_cache_reset(cache);
        return -1;
    }

    dns_resolver_ctx_t resolver;
    if (dns_resolver_init(&resolver) != 0) {
        perf_dns_cache_reset(cache);
        return -1;
    }

    char srv_name[512];
    int n = snprintf(srv_name, sizeof(srv_name), "_ratelimitly._udp.%s", tenant_name);
    if (n < 0 || (size_t)n >= sizeof(srv_name)) {
        dns_resolver_close(&resolver);
        perf_dns_cache_reset(cache);
        return -1;
    }
    cache->srv_name = perf_strdup(srv_name);
    if (!cache->srv_name) {
        dns_resolver_close(&resolver);
        perf_dns_cache_reset(cache);
        return -1;
    }

    (void)dns_query_srv(&resolver, cache->srv_name, &cache->srv_records, &cache->srv_count);

    for (size_t i = 0; i < cache->srv_count; i++) {
        addr_list_t list;
        memset(&list, 0, sizeof(list));
        if (dns_query_addrs(&resolver, cache->srv_records[i].target, &list) == 0) {
            if (perf_dns_cache_add_addrs(cache, cache->srv_records[i].target, &list) != 0) {
                addr_list_free(&list);
                dns_resolver_close(&resolver);
                perf_dns_cache_reset(cache);
                return -1;
            }
        }
        addr_list_free(&list);
    }

    addr_list_t fallback;
    memset(&fallback, 0, sizeof(fallback));
    if (dns_query_addrs(&resolver, tenant_name, &fallback) == 0) {
        if (perf_dns_cache_add_addrs(cache, tenant_name, &fallback) != 0) {
            addr_list_free(&fallback);
            dns_resolver_close(&resolver);
            perf_dns_cache_reset(cache);
            return -1;
        }
    }
    addr_list_free(&fallback);
    dns_resolver_close(&resolver);
    return 0;
}

static int perf_resolve_addrs(
    void *ctx,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_addr_cb cb,
    void *user
) {
    (void)out_req_id;
    const perf_dns_cache_t *cache = (const perf_dns_cache_t *)ctx;
    if (!cache || !name || !cb) {
        return -1;
    }
    const perf_dns_addr_entry_t *entry = perf_dns_cache_find_addrs(cache, name);
    if (!entry || entry->count == 0) {
        cb(user, -1, NULL, 0);
        return 0;
    }
    cb(user, 0, entry->items, entry->count);
    return 0;
}

static int perf_resolve_srv(
    void *ctx,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_srv_cb cb,
    void *user
) {
    (void)out_req_id;
    const perf_dns_cache_t *cache = (const perf_dns_cache_t *)ctx;
    if (!cache || !name || !cb) {
        return -1;
    }
    if (!cache->srv_name || strcmp(cache->srv_name, name) != 0 || cache->srv_count == 0) {
        cb(user, -1, NULL, 0);
        return 0;
    }
    cb(user, 0, cache->srv_records, cache->srv_count);
    return 0;
}

static void perf_record_stats(perf_stats_t *stats, uint64_t latency_ns, bool success) {
    atomic_fetch_add_explicit(&stats->requests_sent, 1, memory_order_relaxed);
    if (success) {
        atomic_fetch_add_explicit(&stats->responses_received, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&stats->total_latency_ns, latency_ns, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&stats->errors, 1, memory_order_relaxed);
    }
}

static void on_rate_limit(void *user, r_client_req_t *req, int status, const r_rate_limit_result_t *result) {
    (void)req;
    request_ctx_t *ctx = (request_ctx_t *)user;
    if (!ctx) {
        return;
    }
    ctx->status = status;
    ctx->steering_feedback = result ? result->steering_feedback : false;
    ctx->done = true;
}

static int perf_wait_for_request(io_ctx_t *io, r_client_t *client, r_client_req_t *req, request_ctx_t *ctx) {
    if (!io || !client || !req || !ctx) {
        return RCLIENT_ERR_CONFIG;
    }

    uint8_t buf[PERF_MAX_PACKET];
    while (!ctx->done) {
        uint64_t deadline_ms = 0;
        if (r_client_request_deadline_ms(req, &deadline_ms) != RCLIENT_OK) {
            ctx->status = RCLIENT_ERR_CONFIG;
            ctx->done = true;
            break;
        }

        uint64_t now_ms = perf_now_ms(NULL);
        int timeout_ms = 0;
        if (deadline_ms > now_ms) {
            uint64_t diff = deadline_ms - now_ms;
            timeout_ms = diff > (uint64_t)INT32_MAX ? INT32_MAX : (int)diff;
        }

        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = io->sockfd;
        pfd.events = POLLIN;

        int prc = poll(&pfd, 1, timeout_ms);
        if (prc > 0 && (pfd.revents & POLLIN)) {
            for (;;) {
                struct sockaddr_storage from;
                socklen_t from_len = sizeof(from);
                ssize_t n = recvfrom(io->sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    break;
                }
                r_addr_t addr;
                memset(&addr, 0, sizeof(addr));
                addr.len = from_len;
                memcpy(&addr.sa, &from, from_len);
                (void)r_client_on_datagram(client, buf, (size_t)n, &addr);
                if (ctx->done) {
                    break;
                }
            }
        } else if (prc < 0 && errno != EINTR) {
            ctx->status = RCLIENT_ERR_IO;
            ctx->done = true;
            break;
        }

        now_ms = perf_now_ms(NULL);
        if (!ctx->done && deadline_ms <= now_ms) {
            (void)r_client_on_timeout(client, req, now_ms);
        }
    }

    return ctx->status;
}

static void *perf_worker(void *arg) {
    worker_ctx_t *worker = (worker_ctx_t *)arg;
    if (!worker || !worker->config || !worker->stats) {
        return (void *)(intptr_t)1;
    }

    io_ctx_t io;
    memset(&io, 0, sizeof(io));
    io.ignore_steering = worker->config->ignore_steering;
    io.sockfd = perf_setup_socket();
    if (io.sockfd < 0) {
        worker->worker_error = 1;
        return (void *)(intptr_t)1;
    }

    r_io_ops_t io_ops;
    memset(&io_ops, 0, sizeof(io_ops));
    io_ops.ctx = &io;
    io_ops.udp_send = perf_udp_send;
    io_ops.now_ms = perf_now_ms;
    io_ops.log = perf_log;
    io_ops.on_steering_feedback = worker->config->ignore_steering ? NULL : perf_on_steering_feedback;

    r_resolver_ops_t resolver_ops;
    memset(&resolver_ops, 0, sizeof(resolver_ops));
    resolver_ops.ctx = (void *)worker->config->dns_cache;
    resolver_ops.resolve_srv = perf_resolve_srv;
    resolver_ops.resolve_addrs = perf_resolve_addrs;
    resolver_ops.cancel = perf_resolver_cancel;

    r_request_policy_t policy;
    r_client_default_request_policy(&policy);
    policy.attempt_timeout_ms = worker->config->attempt_timeout_ms;
    policy.retry.retry_attempts = worker->config->retry_attempts;
    policy.retry.retry_on = worker->config->retry_on;
    policy.retry.resend = worker->config->retry_resend;
    policy.retry.backoff.kind = R_BACKOFF_NONE;
    policy.retry.backoff.delay_ms = 0;
    policy.retry.backoff.base_delay_ms = 0;
    policy.retry.backoff.max_delay_ms = 0;
    policy.retry.backoff.jitter_ms = 0;
    policy.retry.refresh_dns_on_retry = worker->config->retry_refresh_dns_on_retry;
    policy.retry.total_timeout_ms = worker->config->retry_total_timeout_ms;
    policy.dns_resync.refresh_interval_ms = 3600u * 1000u;

    r_auth_config_t auth;
    memset(&auth, 0, sizeof(auth));
    auth.type = worker->config->auth_type;
    auth.secret = worker->config->auth_bech32;

    r_tenant_config_t tenant;
    memset(&tenant, 0, sizeof(tenant));
    tenant.dns_name = worker->config->srv_domain;
    tenant.key_id = worker->config->tenant_id;
    tenant.auth = auth;

    r_client_config_t client_cfg;
    memset(&client_cfg, 0, sizeof(client_cfg));
    client_cfg.tenant = tenant;
    client_cfg.server_stability_threshold_ms = 0;
    client_cfg.request_policy = &policy;

    r_client_t *client = NULL;
    int rc = r_client_create(&client_cfg, &io_ops, &resolver_ops, &client);
    if (rc != RCLIENT_OK) {
        fprintf(stderr, "[ERROR] client_%zu: %s (%d)\n", worker->client_id, perf_err_str(rc), rc);
        perf_maybe_print_dns_hint(rc);
        close(io.sockfd);
        worker->worker_error = 1;
        return (void *)(intptr_t)1;
    }

    char bucket_name[256];
    snprintf(bucket_name, sizeof(bucket_name), "%s_%zu", worker->config->bucket_prefix, worker->client_id);

    r_resource_request_t resource;
    memset(&resource, 0, sizeof(resource));
    r_client_hash_id(bucket_name, resource.bucket_id);
    resource.window_size_ms = 60000;
    resource.rate_limit = 10000;
    resource.tokens_requested = 1;

    uint64_t start_ns = perf_now_ns_monotonic();
    uint64_t request_count = 0;
    bool printed_error = false;
    bool printed_steering = false;

    for (;;) {
        if (worker->config->has_duration) {
            uint64_t elapsed_ns = perf_now_ns_monotonic() - start_ns;
            if (elapsed_ns >= worker->config->duration_secs * 1000000000ull) {
                break;
            }
        } else if (request_count >= worker->config->requests_per_client) {
            break;
        }

        request_ctx_t req_ctx;
        memset(&req_ctx, 0, sizeof(req_ctx));

        uint64_t req_start = perf_now_ns_monotonic();
        r_client_req_t *req = NULL;
        rc = r_client_check_rate_limit_async_borrowed(
            client,
            &resource,
            1,
            NULL,
            0,
            NULL,
            0,
            on_rate_limit,
            &req_ctx,
            &req
        );

        if (rc != RCLIENT_OK) {
            uint64_t latency_ns = perf_now_ns_monotonic() - req_start;
            perf_record_stats(worker->stats, latency_ns, false);
            if (!printed_error) {
                fprintf(stderr, "[ERROR] client_%zu: %s (%d)\n", worker->client_id, perf_err_str(rc), rc);
                perf_maybe_print_dns_hint(rc);
                printed_error = true;
            }
            request_count++;
            continue;
        }

        (void)perf_wait_for_request(&io, client, req, &req_ctx);
        uint64_t latency_ns = perf_now_ns_monotonic() - req_start;
        bool ok = req_ctx.status == RCLIENT_OK;
        perf_record_stats(worker->stats, latency_ns, ok);

        if (!ok && !printed_error) {
            fprintf(stderr, "[ERROR] client_%zu: %s (%d)\n", worker->client_id, perf_err_str(req_ctx.status), req_ctx.status);
            perf_maybe_print_dns_hint(req_ctx.status);
            printed_error = true;
        }

        if (ok && worker->config->debug_steering && !printed_steering) {
            fprintf(stderr, "[DEBUG] client_%zu: steering_feedback=%s\n", worker->client_id, req_ctx.steering_feedback ? "true" : "false");
            printed_steering = true;
        }

        request_count++;
    }

    r_client_destroy(client);
    close(io.sockfd);
    return (void *)(intptr_t)0;
}

static const char *perf_find_arg(int argc, char **argv, const char *name) {
    size_t name_len = strlen(name);
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strncmp(arg, name, name_len) == 0) {
            if (arg[name_len] == '=') {
                return arg + name_len + 1;
            }
            if (arg[name_len] == '\0' && i + 1 < argc) {
                return argv[i + 1];
            }
        }
    }
    return NULL;
}

static bool perf_has_flag(int argc, char **argv, const char *name) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static uint64_t perf_parse_u64(const char *value, uint64_t fallback) {
    if (!value || value[0] == '\0') {
        return fallback;
    }
    char *end = NULL;
    unsigned long long out = strtoull(value, &end, 10);
    if (!end || end == value) {
        return fallback;
    }
    return (uint64_t)out;
}

static bool perf_parse_retry_on(const char *value, r_retry_on_t *out) {
    if (!value || !out) {
        return false;
    }
    if (strcmp(value, "timeout") == 0) {
        *out = R_RETRY_TIMEOUT_ONLY;
        return true;
    }
    if (strcmp(value, "quorum") == 0) {
        *out = R_RETRY_QUORUM_NOT_MET;
        return true;
    }
    if (strcmp(value, "inconsistent") == 0) {
        *out = R_RETRY_INCONSISTENT;
        return true;
    }
    if (strcmp(value, "never") == 0) {
        *out = R_RETRY_NEVER;
        return true;
    }
    return false;
}

static bool perf_parse_retry_resend(const char *value, r_resend_policy_t *out) {
    if (!value || !out) {
        return false;
    }
    if (strcmp(value, "all") == 0) {
        *out = R_RESEND_ALL;
        return true;
    }
    if (strcmp(value, "missing") == 0) {
        *out = R_RESEND_MISSING_ONLY;
        return true;
    }
    return false;
}

static perf_config_t perf_config_from_args(int argc, char **argv) {
    perf_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.srv_domain = "rl.glar.com";
    cfg.auth_bech32 = NULL;
    cfg.auth_type = (r_auth_type_t)0;
    cfg.tenant_id = 0;
    cfg.concurrent_clients = 10;
    cfg.requests_per_client = 1000;
    cfg.has_duration = false;
    cfg.duration_secs = 0;
    cfg.bucket_prefix = "perf_bucket";
    cfg.attempt_timeout_ms = 500;
    cfg.retry_attempts = 0;
    cfg.retry_on = R_RETRY_NEVER;
    cfg.retry_resend = R_RESEND_ALL;
    cfg.retry_refresh_dns_on_retry = false;
    cfg.retry_total_timeout_ms = 0;
    cfg.ignore_steering = false;
    cfg.debug_steering = false;

    const char *srv = perf_find_arg(argc, argv, "--srv");
    if (srv) {
        cfg.srv_domain = srv;
    }

    const char *clients = perf_find_arg(argc, argv, "--clients");
    if (clients) {
        cfg.concurrent_clients = (size_t)perf_parse_u64(clients, cfg.concurrent_clients);
    }

    const char *requests = perf_find_arg(argc, argv, "--requests");
    if (requests) {
        cfg.requests_per_client = perf_parse_u64(requests, cfg.requests_per_client);
    }

    const char *duration = perf_find_arg(argc, argv, "--duration");
    if (duration) {
        cfg.has_duration = true;
        cfg.duration_secs = perf_parse_u64(duration, 0);
    }

    const char *auth = perf_find_arg(argc, argv, "--auth");
    if (auth) {
        cfg.auth_bech32 = auth;
    }
    if (!cfg.auth_bech32) {
        fprintf(stderr, "Missing required --auth value (expected rl-cookie... or rl-aes...)\n");
        exit(2);
    }
    if (perf_parse_auth_bech32(cfg.auth_bech32, &cfg.auth_type, &cfg.tenant_id) != 0) {
        fprintf(stderr, "Invalid --auth value '%s' (expected rl-cookie... or rl-aes...)\n",
                cfg.auth_bech32 ? cfg.auth_bech32 : "");
        exit(2);
    }

    const char *bucket = perf_find_arg(argc, argv, "--bucket-prefix");
    if (bucket) {
        cfg.bucket_prefix = bucket;
    }

    const char *attempt_timeout = perf_find_arg(argc, argv, "--attempt-timeout-ms");
    if (attempt_timeout) {
        cfg.attempt_timeout_ms = perf_parse_u64(attempt_timeout, cfg.attempt_timeout_ms);
    }

    const char *retry_attempts = perf_find_arg(argc, argv, "--retry-attempts");
    if (retry_attempts) {
        cfg.retry_attempts = (uint32_t)perf_parse_u64(retry_attempts, cfg.retry_attempts);
    }

    const char *retry_on = perf_find_arg(argc, argv, "--retry-on");
    if (retry_on && !perf_parse_retry_on(retry_on, &cfg.retry_on)) {
        fprintf(stderr, "Invalid --retry-on value '%s' (expected timeout / quorum / inconsistent / never)\n",
                retry_on);
        exit(2);
    }

    const char *retry_resend = perf_find_arg(argc, argv, "--retry-resend");
    if (retry_resend && !perf_parse_retry_resend(retry_resend, &cfg.retry_resend)) {
        fprintf(stderr, "Invalid --retry-resend value '%s' (expected all / missing)\n",
                retry_resend);
        exit(2);
    }

    const char *retry_total_timeout = perf_find_arg(argc, argv, "--retry-total-timeout-ms");
    if (retry_total_timeout) {
        cfg.retry_total_timeout_ms = perf_parse_u64(retry_total_timeout, cfg.retry_total_timeout_ms);
    }

    cfg.retry_refresh_dns_on_retry = perf_has_flag(argc, argv, "--retry-refresh-dns");

    cfg.ignore_steering = perf_has_flag(argc, argv, "--ignore-steering");
    cfg.debug_steering = perf_has_flag(argc, argv, "--debug-steering");

    return cfg;
}

static void perf_print_help(void) {
    printf("RateLimitly Performance Client\n\n");
    printf("Usage: perf_client [OPTIONS]\n\n");
    printf("Options:\n");
    printf("  --srv=<domain>          DNS SRV lookup domain (default: rl.glar.com)\n");
    printf("  --clients=<n>           Concurrent clients (default: 10)\n");
    printf("  --requests=<n>          Requests per client (default: 1000)\n");
    printf("  --duration=<secs>       Run for duration instead of request count\n");
    printf("  --auth=<bech32>         Tenant auth Bech32 key (rl-cookie... or rl-aes...)\n");
    printf("                         Tenant ID is derived from the embedded key_id\n");
    printf("  --bucket-prefix=<name>  Bucket name prefix (default: perf_bucket)\n");
    printf("  --attempt-timeout-ms=<n> Per-attempt UDP reply deadline (default: 500)\n");
    printf("  --retry-attempts=<n>    Retry count after the first attempt (default: 0)\n");
    printf("  --retry-on=<mode>       Retry trigger: timeout | quorum | inconsistent | never\n");
    printf("  --retry-resend=<mode>   Retry resend policy: all | missing\n");
    printf("  --retry-total-timeout-ms=<n> Overall timeout cap across retries (0 disables cap)\n");
    printf("  --retry-refresh-dns     Refresh DNS before retry attempts\n");
    printf("  --ignore-steering       Ignore steering_feedback from server\n");
    printf("  --debug-steering        Show steering_feedback values\n\n");
    printf("Examples:\n");
    printf("  perf_client --clients=50 --requests=10000 --auth=rl-aes1...\n");
    printf("  perf_client --duration=60 --auth=rl-aes1...\n");
    printf("  perf_client --srv=rl1.glar.com --duration=30 --clients=50 --auth=rl-aes1...\n");
    printf("  perf_client --attempt-timeout-ms=750 --retry-attempts=2 --retry-on=timeout --auth=rl-aes1...\n");
}

static void *perf_progress_thread(void *arg) {
    perf_stats_t *stats = (perf_stats_t *)arg;
    uint64_t last_sent = 0;
    while (!atomic_load_explicit(&g_stop_progress, memory_order_relaxed)) {
        // Check stop flag at a finer granularity so short benchmarks don't
        // inherit a fixed 1s tail while waiting for this thread to exit.
        for (int i = 0; i < 10; i++) {
            if (atomic_load_explicit(&g_stop_progress, memory_order_relaxed)) {
                break;
            }
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 100000000L;
            nanosleep(&ts, NULL);
        }
        if (atomic_load_explicit(&g_stop_progress, memory_order_relaxed)) {
            break;
        }
        uint64_t sent = atomic_load_explicit(&stats->requests_sent, memory_order_relaxed);
        uint64_t rps = sent - last_sent;
        printf("Requests: %llu (+%llu/s)\n", (unsigned long long)sent, (unsigned long long)rps);
        last_sent = sent;
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (perf_has_flag(argc, argv, "--help") || perf_has_flag(argc, argv, "-h")) {
        perf_print_help();
        return 0;
    }

    perf_config_t cfg = perf_config_from_args(argc, argv);
    perf_dns_cache_t dns_cache;
    if (perf_dns_cache_init(&dns_cache, cfg.srv_domain) != 0) {
        fprintf(stderr, "[ERROR] Failed to pre-resolve DNS for %s\n", cfg.srv_domain);
        perf_maybe_print_dns_hint(RCLIENT_ERR_DNS);
        return 1;
    }
    cfg.dns_cache = &dns_cache;

    printf("RateLimitly Performance Client\n");
    printf("==============================\n");
    printf("SRV domain: %s\n", cfg.srv_domain);
    printf("Auth: %s\n", perf_auth_label(cfg.auth_type));
    printf("Tenant: %llu\n", (unsigned long long)cfg.tenant_id);
    printf("Concurrent clients: %zu\n", cfg.concurrent_clients);
    printf("Attempt timeout: %llu ms\n", (unsigned long long)cfg.attempt_timeout_ms);
    printf("Retry policy: attempts=%u on=%s resend=%s backoff=none",
           cfg.retry_attempts,
           perf_retry_on_label(cfg.retry_on),
           perf_retry_resend_label(cfg.retry_resend));
    if (cfg.retry_total_timeout_ms > 0) {
        printf(" total_timeout=%llu ms", (unsigned long long)cfg.retry_total_timeout_ms);
    }
    if (cfg.retry_refresh_dns_on_retry) {
        printf(" refresh_dns=true");
    }
    printf("\n");
    if (cfg.has_duration) {
        printf("Duration: %llu s\n", (unsigned long long)cfg.duration_secs);
    } else {
        printf("Requests per client: %llu\n", (unsigned long long)cfg.requests_per_client);
        printf("Total requests: %llu\n", (unsigned long long)(cfg.requests_per_client * cfg.concurrent_clients));
    }
    printf("\nStarting benchmark...\n");

    perf_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    atomic_store_explicit(&g_stop_progress, false, memory_order_relaxed);

    pthread_t *threads = (pthread_t *)calloc(cfg.concurrent_clients, sizeof(pthread_t));
    worker_ctx_t *workers = (worker_ctx_t *)calloc(cfg.concurrent_clients, sizeof(worker_ctx_t));
    bool *started = (bool *)calloc(cfg.concurrent_clients, sizeof(bool));
    if (!threads || !workers || !started) {
        fprintf(stderr, "[ERROR] Out of memory\n");
        free(threads);
        free(workers);
        free(started);
        perf_dns_cache_reset(&dns_cache);
        return 1;
    }

    uint64_t start_ns = perf_now_ns_monotonic();

    for (size_t i = 0; i < cfg.concurrent_clients; i++) {
        workers[i].client_id = i;
        workers[i].config = &cfg;
        workers[i].stats = &stats;
        workers[i].worker_error = 0;
        if (pthread_create(&threads[i], NULL, perf_worker, &workers[i]) != 0) {
            fprintf(stderr, "[ERROR] Failed to spawn worker %zu\n", i);
            workers[i].worker_error = 1;
            started[i] = false;
        } else {
            started[i] = true;
        }
    }

    pthread_t progress_thread;
    bool progress_started = false;
    if (pthread_create(&progress_thread, NULL, perf_progress_thread, &stats) != 0) {
        fprintf(stderr, "[WARN] Failed to start progress thread\n");
    } else {
        progress_started = true;
    }

    uint64_t worker_errors = 0;
    for (size_t i = 0; i < cfg.concurrent_clients; i++) {
        if (started[i]) {
            void *ret = NULL;
            if (pthread_join(threads[i], &ret) == 0) {
                if ((intptr_t)ret != 0 || workers[i].worker_error) {
                    worker_errors++;
                }
            } else {
                worker_errors++;
            }
        } else if (workers[i].worker_error) {
            worker_errors++;
        }
    }

    atomic_store_explicit(&g_stop_progress, true, memory_order_relaxed);
    if (progress_started) {
        pthread_join(progress_thread, NULL);
    }

    uint64_t elapsed_ns = perf_now_ns_monotonic() - start_ns;
    double elapsed_secs = (double)elapsed_ns / 1000000000.0;

    uint64_t sent = atomic_load_explicit(&stats.requests_sent, memory_order_relaxed);
    uint64_t received = atomic_load_explicit(&stats.responses_received, memory_order_relaxed);
    uint64_t errors = atomic_load_explicit(&stats.errors, memory_order_relaxed);
    uint64_t total_latency = atomic_load_explicit(&stats.total_latency_ns, memory_order_relaxed);

    double rps = elapsed_secs > 0 ? (double)sent / elapsed_secs : 0.0;
    double success_rate = sent > 0 ? ((double)received / (double)sent) * 100.0 : 0.0;
    double avg_latency_us = received > 0 ? ((double)total_latency / (double)received) / 1000.0 : 0.0;

    printf("=== Performance Results ===\n");
    printf("Duration: %.2fs\n", elapsed_secs);
    printf("Requests sent: %llu\n", (unsigned long long)sent);
    printf("Responses received: %llu\n", (unsigned long long)received);
    printf("Errors: %llu\n", (unsigned long long)errors);
    printf("Success rate: %.2f%%\n", success_rate);
    printf("Requests/sec: %.0f\n", rps);
    printf("Average latency: %.1fus\n", avg_latency_us);
    if (sent > 0 && received == 0 && errors > 0) {
        printf("Note: if the server has no tenant registered (or auth mismatches), it will drop requests without responding.\n");
    }

    if (worker_errors > 0) {
        fprintf(stderr, "[ERROR] %llu worker(s) failed\n", (unsigned long long)worker_errors);
    }

    free(threads);
    free(workers);
    free(started);
    perf_dns_cache_reset(&dns_cache);
    return 0;
}
