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
    r_auth_type_t auth_type;
    const char *aes_secret;
    const char *cookie_secret;
    uint64_t tenant_id;
    size_t concurrent_clients;
    uint64_t requests_per_client;
    bool has_duration;
    uint64_t duration_secs;
    const char *bucket_prefix;
    bool ignore_steering;
    bool debug_steering;
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

typedef struct worker_ctx {
    size_t client_id;
    const perf_config_t *config;
    perf_stats_t *stats;
    int worker_error;
} worker_ctx_t;

static atomic_bool g_stop_progress = false;

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

static int perf_resolve_addrs(
    void *ctx,
    const char *name,
    r_dns_req_id_t *out_req_id,
    r_dns_addr_cb cb,
    void *user
) {
    (void)out_req_id;
    dns_resolver_ctx_t *resolver = (dns_resolver_ctx_t *)ctx;
    if (!resolver || !resolver->initialized || !name || !cb) {
        return -1;
    }

    addr_list_t list;
    memset(&list, 0, sizeof(list));

    unsigned char answer[4096];
    int len_a = res_nquery(&resolver->state, name, ns_c_in, ns_t_a, answer, sizeof(answer));
    if (len_a > 0) {
        (void)dns_parse_addrs(answer, len_a, ns_t_a, &list);
    }

    int len_aaaa = res_nquery(&resolver->state, name, ns_c_in, ns_t_aaaa, answer, sizeof(answer));
    if (len_aaaa > 0) {
        (void)dns_parse_addrs(answer, len_aaaa, ns_t_aaaa, &list);
    }

    int status = list.count > 0 ? 0 : -1;
    cb(user, status, list.items, list.count);
    addr_list_free(&list);
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
    dns_resolver_ctx_t *resolver = (dns_resolver_ctx_t *)ctx;
    if (!resolver || !resolver->initialized || !name || !cb) {
        return -1;
    }

    unsigned char answer[4096];
    int len = res_nquery(&resolver->state, name, ns_c_in, ns_t_srv, answer, sizeof(answer));
    if (len <= 0) {
        cb(user, -1, NULL, 0);
        return 0;
    }

    ns_msg handle;
    if (ns_initparse(answer, len, &handle) < 0) {
        cb(user, -1, NULL, 0);
        return 0;
    }

    int count = ns_msg_count(handle, ns_s_an);
    r_srv_record_t *records = (r_srv_record_t *)calloc((size_t)count, sizeof(r_srv_record_t));
    if (!records) {
        cb(user, -1, NULL, 0);
        return 0;
    }

    size_t out_count = 0;
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

        records[out_count].priority = priority;
        records[out_count].weight = weight;
        records[out_count].port = port;
        records[out_count].ttl_ms = (uint32_t)ns_rr_ttl(rr) * 1000u;
        records[out_count].target = perf_strdup(target);
        if (!records[out_count].target) {
            continue;
        }
        out_count++;
    }

    int status = out_count > 0 ? 0 : -1;
    cb(user, status, records, out_count);

    for (size_t i = 0; i < out_count; i++) {
        free((char *)records[i].target);
    }
    free(records);
    return 0;
}

static void perf_resolver_cancel(void *ctx, r_dns_req_id_t req_id) {
    (void)ctx;
    (void)req_id;
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

    dns_resolver_ctx_t resolver;
    if (dns_resolver_init(&resolver) != 0) {
        close(io.sockfd);
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
    resolver_ops.ctx = &resolver;
    resolver_ops.resolve_srv = perf_resolve_srv;
    resolver_ops.resolve_addrs = perf_resolve_addrs;
    resolver_ops.cancel = perf_resolver_cancel;

    r_request_policy_t policy;
    r_client_default_request_policy(&policy);
    policy.attempt_timeout_ms = 500;
    policy.retry.retry_attempts = 0;
    policy.retry.retry_on = R_RETRY_NEVER;
    policy.dns_resync.refresh_interval_ms = 3600u * 1000u;

    r_auth_config_t auth;
    memset(&auth, 0, sizeof(auth));
    auth.type = worker->config->auth_type;
    if (auth.type == R_AUTH_AES_GCM) {
        auth.secret = worker->config->aes_secret;
    } else if (auth.type == R_AUTH_COOKIE) {
        auth.secret = worker->config->cookie_secret;
    }

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
        dns_resolver_close(&resolver);
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
    dns_resolver_close(&resolver);
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

static perf_config_t perf_config_from_args(int argc, char **argv) {
    perf_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.srv_domain = "rl.glar.com";
    cfg.auth_type = R_AUTH_NONE;
    cfg.aes_secret = "secret";
    cfg.cookie_secret = "secret";
    cfg.tenant_id = 1;
    cfg.concurrent_clients = 10;
    cfg.requests_per_client = 1000;
    cfg.has_duration = false;
    cfg.duration_secs = 0;
    cfg.bucket_prefix = "perf_bucket";
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

    const char *aes_secret = perf_find_arg(argc, argv, "--aes-secret");
    if (aes_secret) {
        cfg.aes_secret = aes_secret;
    }

    const char *cookie_secret = perf_find_arg(argc, argv, "--cookie-secret");
    if (cookie_secret) {
        cfg.cookie_secret = cookie_secret;
    }

    const char *auth = perf_find_arg(argc, argv, "--auth");
    if (auth) {
        if (strcmp(auth, "cookie") == 0) {
            cfg.auth_type = R_AUTH_COOKIE;
        } else if (strcmp(auth, "aes") == 0) {
            cfg.auth_type = R_AUTH_AES_GCM;
        } else {
            cfg.auth_type = R_AUTH_NONE;
        }
    }

    const char *tenant = perf_find_arg(argc, argv, "--tenant");
    if (tenant) {
        cfg.tenant_id = perf_parse_u64(tenant, cfg.tenant_id);
    }

    const char *bucket = perf_find_arg(argc, argv, "--bucket-prefix");
    if (bucket) {
        cfg.bucket_prefix = bucket;
    }

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
    printf("  --auth=<none|cookie|aes> Auth method (default: none)\n");
    printf("  --aes-secret=<value>    AES secret (default: secret)\n");
    printf("  --cookie-secret=<value> Cookie secret (default: secret)\n");
    printf("  --tenant=<id>           Tenant ID (default: 1)\n");
    printf("  --bucket-prefix=<name>  Bucket name prefix (default: perf_bucket)\n");
    printf("  --ignore-steering       Ignore steering_feedback from server\n");
    printf("  --debug-steering        Show steering_feedback values\n\n");
    printf("Examples:\n");
    printf("  perf_client --clients=50 --requests=10000\n");
    printf("  perf_client --duration=60 --auth=aes\n");
    printf("  perf_client --srv=rl1.glar.com --duration=30 --clients=50\n");
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

    printf("RateLimitly Performance Client\n");
    printf("==============================\n");
    printf("SRV domain: %s\n", cfg.srv_domain);
    printf("Auth: %s\n", cfg.auth_type == R_AUTH_COOKIE ? "Cookie" : cfg.auth_type == R_AUTH_AES_GCM ? "AES" : "None");
    printf("Tenant: %llu\n", (unsigned long long)cfg.tenant_id);
    printf("Concurrent clients: %zu\n", cfg.concurrent_clients);
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
    return 0;
}
