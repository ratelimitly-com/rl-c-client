#include "../include/r_client.h"

#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <stdio.h>

#include <openssl/rand.h>

#include "r_crypto.h"
#include "r_protocol.h"

typedef struct r_server_endpoint {
    r_addr_t addr;
    uint64_t server_id;
    bool has_server_id;
} r_server_endpoint_t;

typedef struct r_server_stat {
    uint64_t server_id;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    uint64_t valid_responses;
    uint64_t timeouts;
    uint64_t decrypt_fail;
    uint64_t auth_fail;
    uint64_t id_mismatch;
} r_server_stat_t;

typedef struct r_server_stats {
    r_server_stat_t *items;
    size_t count;
    size_t cap;
} r_server_stats_t;

typedef struct r_candidate {
    bool has;
    r_rate_limit_result_t result;
    r_guard_result_t *guards;
    r_resource_result_t *resources;
} r_candidate_t;

struct r_client_req {
    struct r_client_req *next;
    struct r_client *client;

    uint8_t request_id[16];
    r_rate_limit_cb cb;
    void *user;

    r_resource_request_t *resources;
    size_t resource_count;
    r_latency_guard_t *guards;
    size_t guard_count;
    char *metrics_label;
    size_t metrics_label_len;
    bool owns_resources;
    bool owns_guards;
    bool owns_metrics_label;

    r_addr_t *targets;
    size_t target_count;
    uint64_t *allowed_ids;
    size_t allowed_id_count;
    size_t replica_count;

    uint64_t start_ms;
    uint64_t attempt_deadline_ms;
    uint64_t total_deadline_ms;
    uint64_t dedup_deadline_ms;
    uint32_t attempt;
    uint32_t total_attempts;
    uint32_t dedup_ttl_ms;

    r_addr_t *seen_addrs;
    size_t seen_addr_count;
    size_t seen_addr_cap;
    uint64_t *seen_server_ids;
    size_t seen_server_id_count;
    size_t seen_server_id_cap;

    bool any_success;
    bool any_failure;
    bool steering_rebind;

    r_candidate_t best;
    r_candidate_t best_allow;
    r_candidate_t best_deny;
};

struct r_client {
    r_client_config_t config;
    r_request_policy_t policy;
    r_io_ops_t io;
    r_resolver_ops_t resolver;
    char *dns_name;
    char *auth_secret;
    bool has_quotas;
    r_bech32_quotas_t quotas;
    uint8_t cookie[32];
    bool has_cookie;
    uint8_t aes_key[32];
    bool has_aes_key;

    r_server_endpoint_t *servers;
    size_t server_count;
    size_t server_cap;
    uint64_t last_dns_refresh_ms;
    uint64_t dns_refresh_ttl_ms;
    bool dns_refresh_inflight;

    r_server_stats_t stats;
    r_client_req_t *inflight;
    struct r_dns_refresh *dns_refresh;
};

typedef struct r_dns_refresh {
    r_client_t *client;
    bool fallback;
    bool detached;
    size_t pending;
    size_t scheduling;
    struct r_dns_lookup_ctx *lookups;
    r_server_endpoint_t *endpoints;
    size_t endpoint_count;
    size_t endpoint_cap;
    uint64_t min_ttl_ms;
} r_dns_refresh_t;

typedef struct r_dns_lookup_ctx {
    r_dns_refresh_t *refresh;
    r_dns_req_id_t req_id;
    bool has_req_id;
    bool completed;
    bool in_resolver_call;
    uint16_t port;
    uint64_t server_id;
    bool has_server_id;
    struct r_dns_lookup_ctx *next;
} r_dns_lookup_ctx_t;

#define R_SERVER_ID_EPOCH_S_2025 1735689600ULL

static char *r_strdup_n(const char *src, size_t len) {
    if (!src) {
        return NULL;
    }
    if (len == 0) {
        len = strlen(src);
    }
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, src, len);
    out[len] = '\0';
    return out;
}

static int r_addr_equal(const r_addr_t *a, const r_addr_t *b) {
    if (!a || !b) {
        return 0;
    }
    if (a->len != b->len) {
        return 0;
    }
    return memcmp(&a->sa, &b->sa, a->len) == 0;
}

static uint16_t r_read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void r_write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static uint64_t r_rand_range(uint64_t max);

static uint64_t r_server_start_s_from_id(uint64_t server_id) {
    return R_SERVER_ID_EPOCH_S_2025 + (server_id >> 23);
}

static uint64_t r_server_age_ms(uint64_t server_id, uint64_t now_ms) {
    uint64_t start_ms = r_server_start_s_from_id(server_id) * 1000ULL;
    if (now_ms <= start_ms) {
        return 0;
    }
    return now_ms - start_ms;
}

static uint64_t r_now_ms(r_client_t *client) {
    if (!client || !client->io.now_ms) {
        return 0;
    }
    return client->io.now_ms(client->io.ctx);
}

static bool r_latency_buffer_size_quota(const r_client_t *client, uint32_t *out_limit) {
    if (!client || !client->has_quotas || !out_limit) {
        return false;
    }
    *out_limit = client->quotas.latency_buffer_size_max;
    return true;
}

static uint32_t r_effective_dedup_ttl_ms(const r_client_t *client) {
    if (!client) {
        return 0;
    }
    if (client->has_quotas
        && client->quotas.dedup_ttl_ms_max > 0
        && client->policy.dedup_ttl_ms > client->quotas.dedup_ttl_ms_max) {
        return client->quotas.dedup_ttl_ms_max;
    }
    return client->policy.dedup_ttl_ms;
}

static int r_validate_latency_guards(
    const r_client_t *client,
    const r_latency_guard_t *guards,
    size_t guard_count
) {
    uint32_t limit = 0;
    if (!r_latency_buffer_size_quota(client, &limit)) {
        return RCLIENT_OK;
    }
    for (size_t i = 0; i < guard_count; i++) {
        if (guards[i].buffer_size > limit) {
            return RCLIENT_ERR_PROTOCOL;
        }
    }
    return RCLIENT_OK;
}

static int r_filter_latency_reports(
    const r_client_t *client,
    const r_service_latency_report_t *reports,
    size_t report_count,
    r_service_latency_report_t **out_reports,
    size_t *out_count,
    bool *out_owned
) {
    if (!reports || !out_reports || !out_count || !out_owned) {
        return RCLIENT_ERR_CONFIG;
    }

    *out_reports = (r_service_latency_report_t *)reports;
    *out_count = report_count;
    *out_owned = false;

    uint32_t limit = 0;
    if (!r_latency_buffer_size_quota(client, &limit)) {
        return RCLIENT_OK;
    }

    size_t kept = 0;
    for (size_t i = 0; i < report_count; i++) {
        if (reports[i].buffer_size <= limit) {
            kept += 1;
        }
    }
    if (kept == report_count) {
        return RCLIENT_OK;
    }

    r_service_latency_report_t *filtered = NULL;
    if (kept > 0) {
        filtered = (r_service_latency_report_t *)calloc(kept, sizeof(*filtered));
        if (!filtered) {
            return RCLIENT_ERR_NOMEM;
        }
        size_t pos = 0;
        for (size_t i = 0; i < report_count; i++) {
            if (reports[i].buffer_size <= limit) {
                filtered[pos++] = reports[i];
            }
        }
    }

    *out_reports = filtered;
    *out_count = kept;
    *out_owned = true;
    return RCLIENT_OK;
}


static int r_stats_grow(r_server_stats_t *stats, size_t need) {
    if (stats->count + need <= stats->cap) {
        return 0;
    }
    size_t next_cap = stats->cap == 0 ? 8 : stats->cap * 2;
    while (next_cap < stats->count + need) {
        next_cap *= 2;
    }
    r_server_stat_t *items = (r_server_stat_t *)realloc(stats->items, next_cap * sizeof(r_server_stat_t));
    if (!items) {
        return -1;
    }
    stats->items = items;
    stats->cap = next_cap;
    return 0;
}

static r_server_stat_t *r_stats_get_or_add(r_client_t *client, uint64_t server_id, uint64_t now_ms) {
    if (!client) {
        return NULL;
    }
    for (size_t i = 0; i < client->stats.count; i++) {
        if (client->stats.items[i].server_id == server_id) {
            return &client->stats.items[i];
        }
    }
    if (r_stats_grow(&client->stats, 1) != 0) {
        return NULL;
    }
    r_server_stat_t *entry = &client->stats.items[client->stats.count++];
    memset(entry, 0, sizeof(*entry));
    entry->server_id = server_id;
    entry->first_seen_ms = now_ms;
    entry->last_seen_ms = now_ms;
    return entry;
}

static r_server_stat_t *r_stats_get(r_client_t *client, uint64_t server_id) {
    if (!client) {
        return NULL;
    }
    for (size_t i = 0; i < client->stats.count; i++) {
        if (client->stats.items[i].server_id == server_id) {
            return &client->stats.items[i];
        }
    }
    return NULL;
}

static void r_stats_timeout_all(r_client_t *client, uint64_t now_ms) {
    if (!client) {
        return;
    }
    for (size_t i = 0; i < client->stats.count; i++) {
        client->stats.items[i].last_seen_ms = now_ms;
        client->stats.items[i].timeouts += 1;
    }
}

static bool r_server_stable(r_client_t *client, uint64_t server_id, uint64_t now_ms) {
    if (!client) {
        return false;
    }
    if (client->config.server_stability_threshold_ms == 0) {
        return true;
    }
    return r_server_age_ms(server_id, now_ms) >= client->config.server_stability_threshold_ms;
}

static int r_seen_addr_add(r_client_req_t *req, const r_addr_t *addr) {
    if (!req || !addr) {
        return -1;
    }
    for (size_t i = 0; i < req->seen_addr_count; i++) {
        if (r_addr_equal(&req->seen_addrs[i], addr)) {
            return 0;
        }
    }
    if (req->seen_addr_count == req->seen_addr_cap) {
        size_t next_cap = req->seen_addr_cap == 0 ? 4 : req->seen_addr_cap * 2;
        r_addr_t *next = (r_addr_t *)realloc(req->seen_addrs, next_cap * sizeof(r_addr_t));
        if (!next) {
            return -1;
        }
        req->seen_addrs = next;
        req->seen_addr_cap = next_cap;
    }
    req->seen_addrs[req->seen_addr_count++] = *addr;
    return 0;
}

static int r_seen_server_add(r_client_req_t *req, uint64_t server_id) {
    if (!req) {
        return -1;
    }
    for (size_t i = 0; i < req->seen_server_id_count; i++) {
        if (req->seen_server_ids[i] == server_id) {
            return 0;
        }
    }
    if (req->seen_server_id_count == req->seen_server_id_cap) {
        size_t next_cap = req->seen_server_id_cap == 0 ? 4 : req->seen_server_id_cap * 2;
        uint64_t *next = (uint64_t *)realloc(req->seen_server_ids, next_cap * sizeof(uint64_t));
        if (!next) {
            return -1;
        }
        req->seen_server_ids = next;
        req->seen_server_id_cap = next_cap;
    }
    req->seen_server_ids[req->seen_server_id_count++] = server_id;
    return 0;
}

static void r_candidate_clear(r_candidate_t *cand) {
    if (!cand) {
        return;
    }
    free((void *)cand->guards);
    free((void *)cand->resources);
    memset(cand, 0, sizeof(*cand));
}

static bool r_is_better_candidate(
    r_client_t *client,
    uint64_t cand_id,
    uint64_t best_id,
    uint64_t now_ms
) {
    (void)now_ms;
    r_server_stat_t *cand_stats = r_stats_get(client, cand_id);
    r_server_stat_t *best_stats = r_stats_get(client, best_id);

    uint64_t cand_start_s = r_server_start_s_from_id(cand_id);
    uint64_t best_start_s = r_server_start_s_from_id(best_id);
    if (cand_start_s != best_start_s) {
        return cand_start_s < best_start_s;
    }

    uint64_t cand_valid = cand_stats ? cand_stats->valid_responses : 0;
    uint64_t best_valid = best_stats ? best_stats->valid_responses : 0;
    if (cand_valid != best_valid) {
        return cand_valid > best_valid;
    }

    uint64_t cand_penalty = cand_stats
        ? (cand_stats->auth_fail + cand_stats->decrypt_fail + cand_stats->timeouts + cand_stats->id_mismatch)
        : 0;
    uint64_t best_penalty = best_stats
        ? (best_stats->auth_fail + best_stats->decrypt_fail + best_stats->timeouts + best_stats->id_mismatch)
        : 0;
    return cand_penalty < best_penalty;
}

static int r_candidate_set(
    r_candidate_t *cand,
    uint64_t server_id,
    bool steering_feedback,
    bool success,
    r_guard_result_t *guards,
    size_t guard_count,
    r_resource_result_t *resources,
    size_t resource_count
) {
    if (!cand) {
        return -1;
    }
    r_candidate_clear(cand);
    cand->guards = guards;
    cand->resources = resources;
    cand->result.success = success;
    cand->result.server_id = server_id;
    cand->result.steering_feedback = steering_feedback;
    cand->result.guards = guards;
    cand->result.guard_count = guard_count;
    cand->result.resources = resources;
    cand->result.resource_count = resource_count;
    cand->has = true;
    return 0;
}


static bool r_candidate_try_update_best(
    r_client_t *client,
    r_candidate_t *best,
    r_rate_limit_result_t *result,
    r_guard_result_t *guards,
    size_t guard_count,
    r_resource_result_t *resources,
    size_t resource_count
) {
    uint64_t now_ms = r_now_ms(client);
    if (!best->has) {
        r_candidate_set(best, result->server_id, result->steering_feedback, result->success,
            guards, guard_count, resources, resource_count);
        return true;
    }
    if (r_is_better_candidate(client, result->server_id, best->result.server_id, now_ms)) {
        r_candidate_set(best, result->server_id, result->steering_feedback, result->success,
            guards, guard_count, resources, resource_count);
        return true;
    }
    return false;
}

static size_t r_quorum_required(r_response_quorum_t quorum, size_t replica_count) {
    switch (quorum.kind) {
    case R_QUORUM_ONE:
        return replica_count < 1 ? replica_count : 1;
    case R_QUORUM_MAJORITY:
        return replica_count == 0 ? 0 : (replica_count / 2) + 1;
    case R_QUORUM_ALL:
        return replica_count;
    case R_QUORUM_COUNT:
        return quorum.count < replica_count ? quorum.count : replica_count;
    default:
        return 0;
    }
}

static bool r_request_inconsistent(r_client_req_t *req) {
    return req->any_success && req->any_failure;
}

static int r_allowed_server_id(r_client_req_t *req, uint64_t server_id) {
    if (!req || req->allowed_id_count == 0) {
        return 1;
    }
    for (size_t i = 0; i < req->allowed_id_count; i++) {
        if (req->allowed_ids[i] == server_id) {
            return 1;
        }
    }
    return 0;
}

static void r_request_remove(r_client_t *client, r_client_req_t *req) {
    if (!client || !req) {
        return;
    }
    r_client_req_t **cur = &client->inflight;
    while (*cur) {
        if (*cur == req) {
            *cur = req->next;
            break;
        }
        cur = &(*cur)->next;
    }
}

static void r_request_free(r_client_req_t *req) {
    if (!req) {
        return;
    }
    if (req->owns_resources) {
        free(req->resources);
    }
    if (req->owns_guards) {
        free(req->guards);
    }
    if (req->owns_metrics_label) {
        free(req->metrics_label);
    }
    free(req->targets);
    free(req->allowed_ids);
    free(req->seen_addrs);
    free(req->seen_server_ids);
    r_candidate_clear(&req->best);
    r_candidate_clear(&req->best_allow);
    r_candidate_clear(&req->best_deny);
    free(req);
}

static void r_generate_request_id(uint8_t out_id[16]) {
    if (!out_id) {
        return;
    }
    if (RAND_bytes(out_id, 16) != 1) {
        memset(out_id, 0, 16);
        return;
    }
    out_id[6] = (uint8_t)((out_id[6] & 0x0F) | 0x40);
    out_id[8] = (uint8_t)((out_id[8] & 0x3F) | 0x80);
}

static uint64_t r_parse_server_id_from_target(const char *target, bool *ok) {
    if (ok) {
        *ok = false;
    }
    if (!target) {
        return 0;
    }
    const char *dot = strchr(target, '.');
    size_t len = dot ? (size_t)(dot - target) : strlen(target);
    if (len < 3) {
        return 0;
    }
    if (target[0] != 's' || target[1] != '-') {
        return 0;
    }
    uint64_t value = 0;
    for (size_t i = 2; i < len; i++) {
        char c = target[i];
        if (c < '0' || c > '9') {
            return 0;
        }
        value = value * 10 + (uint64_t)(c - '0');
    }
    if (ok) {
        *ok = true;
    }
    return value;
}

static void r_addr_set_port(r_addr_t *addr, uint16_t port) {
    if (!addr) {
        return;
    }
    if (addr->sa.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&addr->sa;
        sin->sin_port = htons(port);
        addr->len = sizeof(*sin);
    } else if (addr->sa.ss_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&addr->sa;
        sin6->sin6_port = htons(port);
        addr->len = sizeof(*sin6);
    }
}

static int r_dns_refresh_add_endpoint(r_dns_refresh_t *refresh, const r_addr_t *addr, uint64_t server_id, bool has_server_id) {
    if (!refresh || !addr) {
        return -1;
    }
    if (refresh->endpoint_count == refresh->endpoint_cap) {
        size_t next_cap = refresh->endpoint_cap == 0 ? 4 : refresh->endpoint_cap * 2;
        r_server_endpoint_t *next = (r_server_endpoint_t *)realloc(refresh->endpoints, next_cap * sizeof(r_server_endpoint_t));
        if (!next) {
            return -1;
        }
        refresh->endpoints = next;
        refresh->endpoint_cap = next_cap;
    }
    r_server_endpoint_t *entry = &refresh->endpoints[refresh->endpoint_count++];
    memset(entry, 0, sizeof(*entry));
    entry->addr = *addr;
    entry->server_id = server_id;
    entry->has_server_id = has_server_id;
    return 0;
}

static void r_dns_lookup_free_all(r_dns_lookup_ctx_t *lookup) {
    while (lookup) {
        r_dns_lookup_ctx_t *next = lookup->next;
        free(lookup);
        lookup = next;
    }
}

static void r_dns_lookup_link(r_dns_refresh_t *refresh, r_dns_lookup_ctx_t *lookup) {
    if (!refresh || !lookup) {
        return;
    }
    lookup->next = refresh->lookups;
    refresh->lookups = lookup;
}

static void r_dns_lookup_unlink(r_dns_lookup_ctx_t *lookup) {
    if (!lookup || !lookup->refresh) {
        return;
    }
    r_dns_lookup_ctx_t **cur = &lookup->refresh->lookups;
    while (*cur) {
        if (*cur == lookup) {
            *cur = lookup->next;
            lookup->next = NULL;
            return;
        }
        cur = &(*cur)->next;
    }
}

static void r_dns_refresh_free(r_dns_refresh_t *refresh) {
    if (!refresh) {
        return;
    }
    r_dns_lookup_free_all(refresh->lookups);
    free(refresh->endpoints);
    free(refresh);
}

static int r_dns_start_fallback(r_client_t *client, r_dns_refresh_t *refresh);

static void r_dns_refresh_finish(r_dns_refresh_t *refresh) {
    if (!refresh) {
        return;
    }
    r_client_t *client = refresh->client;
    if (!client || refresh->detached) {
        r_dns_refresh_free(refresh);
        return;
    }
    if (refresh->endpoint_count > 0) {
        free(client->servers);
        client->servers = refresh->endpoints;
        client->server_count = refresh->endpoint_count;
        client->server_cap = refresh->endpoint_cap;
        client->last_dns_refresh_ms = r_now_ms(client);
        client->dns_refresh_ttl_ms = refresh->min_ttl_ms;
        refresh->endpoints = NULL;
        refresh->endpoint_count = 0;
        refresh->endpoint_cap = 0;
    }
    client->dns_refresh_inflight = false;
    if (client->dns_refresh == refresh) {
        client->dns_refresh = NULL;
    }
    r_dns_refresh_free(refresh);
}

static void r_dns_refresh_maybe_complete(r_dns_refresh_t *refresh) {
    if (!refresh || refresh->pending != 0 || refresh->scheduling != 0) {
        return;
    }
    if (refresh->detached || !refresh->client) {
        r_dns_refresh_free(refresh);
        return;
    }
    if (refresh->endpoint_count == 0 && !refresh->fallback) {
        if (r_dns_start_fallback(refresh->client, refresh) == RCLIENT_OK) {
            return;
        }
    }
    r_dns_refresh_finish(refresh);
}

static void r_dns_lookup_finish(r_dns_lookup_ctx_t *lookup) {
    if (!lookup) {
        return;
    }
    r_dns_refresh_t *refresh = lookup->refresh;
    lookup->completed = true;
    if (refresh && refresh->pending > 0) {
        refresh->pending--;
    }
    if (!lookup->in_resolver_call) {
        r_dns_lookup_unlink(lookup);
        lookup->refresh = NULL;
        free(lookup);
    }
    r_dns_refresh_maybe_complete(refresh);
}

static void r_dns_refresh_cancel_lookups(r_dns_refresh_t *refresh) {
    if (!refresh || !refresh->client || !refresh->client->resolver.cancel) {
        return;
    }
    r_client_t *client = refresh->client;
    for (r_dns_lookup_ctx_t *lookup = refresh->lookups; lookup; lookup = lookup->next) {
        if (!lookup->completed && lookup->has_req_id) {
            client->resolver.cancel(client->resolver.ctx, lookup->req_id);
        }
    }
}

static void r_dns_refresh_detach(r_dns_refresh_t *refresh) {
    if (!refresh) {
        return;
    }
    r_client_t *client = refresh->client;
    r_dns_refresh_cancel_lookups(refresh);
    if (client) {
        if (client->dns_refresh == refresh) {
            client->dns_refresh = NULL;
        }
        client->dns_refresh_inflight = false;
    }
    refresh->client = NULL;
    refresh->detached = true;
    r_dns_refresh_maybe_complete(refresh);
}

static void r_dns_addr_cb_fn(void *user, int status, const r_addr_t *addrs, size_t addr_count);
static void r_dns_srv_cb_fn(void *user, int status, const r_srv_record_t *records, size_t record_count);

static int r_dns_schedule_addr_lookup(
    r_client_t *client,
    r_dns_refresh_t *refresh,
    const char *name,
    uint16_t port,
    uint64_t server_id,
    bool has_server_id
) {
    if (!client || !refresh || !name || refresh->detached) {
        return RCLIENT_ERR_DNS;
    }
    r_dns_lookup_ctx_t *lookup = (r_dns_lookup_ctx_t *)calloc(1, sizeof(*lookup));
    if (!lookup) {
        return RCLIENT_ERR_NOMEM;
    }
    lookup->refresh = refresh;
    lookup->port = port;
    lookup->server_id = server_id;
    lookup->has_server_id = has_server_id;
    lookup->in_resolver_call = true;
    r_dns_lookup_link(refresh, lookup);
    refresh->pending++;
    refresh->scheduling++;

    r_dns_req_id_t req_id = 0;
    int rc = client->resolver.resolve_addrs(client->resolver.ctx, name, &req_id, r_dns_addr_cb_fn, lookup);
    if (rc == 0 && !lookup->completed && req_id != 0) {
        lookup->req_id = req_id;
        lookup->has_req_id = true;
    }
    if (rc != 0 && !lookup->completed) {
        lookup->completed = true;
        if (refresh->pending > 0) {
            refresh->pending--;
        }
    }

    lookup->in_resolver_call = false;
    if (rc != 0 || lookup->completed) {
        r_dns_lookup_unlink(lookup);
        lookup->refresh = NULL;
        free(lookup);
    }
    if (refresh->scheduling > 0) {
        refresh->scheduling--;
    }
    r_dns_refresh_maybe_complete(refresh);
    return rc == 0 ? RCLIENT_OK : RCLIENT_ERR_DNS;
}

static int r_dns_start_fallback(r_client_t *client, r_dns_refresh_t *refresh) {
    if (!client || !refresh || refresh->detached) {
        return RCLIENT_ERR_DNS;
    }
    refresh->fallback = true;
    refresh->scheduling++;
    int rc = r_dns_schedule_addr_lookup(client, refresh, client->dns_name, 8080, 0, false);
    if (refresh->scheduling > 0) {
        refresh->scheduling--;
    }
    if (rc == RCLIENT_OK) {
        r_dns_refresh_maybe_complete(refresh);
    }
    return rc;
}

static int r_dns_schedule_srv_lookup(r_client_t *client, r_dns_refresh_t *refresh, const char *name) {
    if (!client || !refresh || !name || refresh->detached) {
        return RCLIENT_ERR_DNS;
    }
    r_dns_lookup_ctx_t *lookup = (r_dns_lookup_ctx_t *)calloc(1, sizeof(*lookup));
    if (!lookup) {
        return RCLIENT_ERR_NOMEM;
    }
    lookup->refresh = refresh;
    lookup->in_resolver_call = true;
    r_dns_lookup_link(refresh, lookup);
    refresh->pending++;
    refresh->scheduling++;

    r_dns_req_id_t req_id = 0;
    int rc = client->resolver.resolve_srv(client->resolver.ctx, name, &req_id, r_dns_srv_cb_fn, lookup);
    if (rc == 0 && !lookup->completed && req_id != 0) {
        lookup->req_id = req_id;
        lookup->has_req_id = true;
    }
    if (rc != 0 && !lookup->completed) {
        lookup->completed = true;
        if (refresh->pending > 0) {
            refresh->pending--;
        }
    }

    lookup->in_resolver_call = false;
    if (rc != 0 || lookup->completed) {
        r_dns_lookup_unlink(lookup);
        lookup->refresh = NULL;
        free(lookup);
    }
    if (refresh->scheduling > 0) {
        refresh->scheduling--;
    }
    r_dns_refresh_maybe_complete(refresh);
    return rc == 0 ? RCLIENT_OK : RCLIENT_ERR_DNS;
}

static int r_dns_start_refresh(r_client_t *client) {
    if (!client || client->dns_refresh_inflight) {
        return RCLIENT_OK;
    }
    r_dns_refresh_t *refresh = (r_dns_refresh_t *)calloc(1, sizeof(*refresh));
    if (!refresh) {
        return RCLIENT_ERR_NOMEM;
    }
    refresh->client = client;
    client->dns_refresh_inflight = true;
    client->dns_refresh = refresh;

    char name_buf[512];
    int n = snprintf(name_buf, sizeof(name_buf), "_ratelimitly._udp.%s", client->dns_name);
    if (n < 0 || (size_t)n >= sizeof(name_buf)) {
        client->dns_refresh_inflight = false;
        client->dns_refresh = NULL;
        r_dns_refresh_free(refresh);
        return RCLIENT_ERR_CONFIG;
    }
    int rc = r_dns_schedule_srv_lookup(client, refresh, name_buf);
    if (rc != RCLIENT_OK && client->dns_refresh == refresh && refresh->pending == 0 && refresh->scheduling == 0) {
        client->dns_refresh_inflight = false;
        client->dns_refresh = NULL;
        r_dns_refresh_free(refresh);
    }
    return rc;
}

static int r_dns_maybe_refresh(r_client_t *client, bool force) {
    if (!client) {
        return RCLIENT_ERR_DNS;
    }
    uint64_t now_ms = r_now_ms(client);
    uint64_t refresh_interval = client->policy.dns_resync.refresh_interval_ms;
    if (refresh_interval == 0) {
        refresh_interval = 300000;
    }
    if (client->dns_refresh_ttl_ms > 0 && client->dns_refresh_ttl_ms < refresh_interval) {
        refresh_interval = client->dns_refresh_ttl_ms;
    }

    if (!force) {
        if (client->last_dns_refresh_ms == 0
            || now_ms - client->last_dns_refresh_ms >= refresh_interval) {
            return r_dns_start_refresh(client);
        }
        return RCLIENT_OK;
    }

    uint64_t min_interval = client->policy.dns_resync.min_interval_ms;
    if (client->policy.dns_resync.jitter_ms > 0) {
        min_interval += r_rand_range(client->policy.dns_resync.jitter_ms);
    }
    if (client->last_dns_refresh_ms != 0 && now_ms - client->last_dns_refresh_ms < min_interval) {
        return RCLIENT_OK;
    }
    return r_dns_start_refresh(client);
}

static void r_dns_srv_cb_fn(void *user, int status, const r_srv_record_t *records, size_t record_count) {
    r_dns_lookup_ctx_t *lookup = (r_dns_lookup_ctx_t *)user;
    if (!lookup || !lookup->refresh) {
        return;
    }
    r_dns_refresh_t *refresh = lookup->refresh;
    if (refresh->detached || !refresh->client) {
        r_dns_lookup_finish(lookup);
        return;
    }
    r_client_t *client = refresh->client;
    if (status == 0 && records && record_count > 0) {
        uint64_t min_ttl_ms = 0;
        for (size_t i = 0; i < record_count; i++) {
            bool ok = false;
            uint64_t server_id = r_parse_server_id_from_target(records[i].target, &ok);
            if (!ok) {
                continue;
            }
            if (records[i].ttl_ms > 0) {
                if (min_ttl_ms == 0 || records[i].ttl_ms < min_ttl_ms) {
                    min_ttl_ms = records[i].ttl_ms;
                }
            }
            (void)r_dns_schedule_addr_lookup(
                client,
                refresh,
                records[i].target,
                records[i].port,
                server_id,
                true
            );
        }
        refresh->min_ttl_ms = min_ttl_ms;
    }
    r_dns_lookup_finish(lookup);
}

static void r_dns_addr_cb_fn(void *user, int status, const r_addr_t *addrs, size_t addr_count) {
    r_dns_lookup_ctx_t *lookup = (r_dns_lookup_ctx_t *)user;
    if (!lookup || !lookup->refresh) {
        return;
    }
    r_dns_refresh_t *refresh = lookup->refresh;
    if (!refresh->detached && refresh->client && status == 0 && addrs && addr_count > 0) {
        for (size_t i = 0; i < addr_count; i++) {
            r_addr_t addr = addrs[i];
            r_addr_set_port(&addr, lookup->port);
            r_dns_refresh_add_endpoint(refresh, &addr, lookup->server_id, lookup->has_server_id);
        }
    }
    r_dns_lookup_finish(lookup);
}

static int r_request_snapshot_targets(r_client_t *client, r_client_req_t *req) {
    if (!client || !req || client->server_count == 0) {
        return RCLIENT_ERR_DNS;
    }
    req->targets = (r_addr_t *)calloc(client->server_count, sizeof(r_addr_t));
    if (!req->targets) {
        return RCLIENT_ERR_NOMEM;
    }
    req->target_count = client->server_count;
    bool has_unknown = false;
    for (size_t i = 0; i < client->server_count; i++) {
        req->targets[i] = client->servers[i].addr;
        if (!client->servers[i].has_server_id) {
            has_unknown = true;
        }
    }
    if (!has_unknown) {
        req->allowed_ids = (uint64_t *)calloc(client->server_count, sizeof(uint64_t));
        if (!req->allowed_ids) {
            return RCLIENT_ERR_NOMEM;
        }
        size_t count = 0;
        for (size_t i = 0; i < client->server_count; i++) {
            uint64_t id = client->servers[i].server_id;
            bool seen = false;
            for (size_t j = 0; j < count; j++) {
                if (req->allowed_ids[j] == id) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                req->allowed_ids[count++] = id;
            }
        }
        req->allowed_id_count = count;
        req->replica_count = count;
    } else {
        req->allowed_id_count = 0;
        req->replica_count = req->target_count;
    }
    return RCLIENT_OK;
}

static int r_build_rate_request_packet(
    r_client_t *client,
    r_client_req_t *req,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len
) {
    if (!client || !req || !out || !out_len) {
        return RCLIENT_ERR_PROTOCOL;
    }
    uint8_t body[R_MAX_PACKET_SIZE];
    size_t body_len = 0;
    int rc = r_build_rate_request_body(
        req->resources,
        req->resource_count,
        req->guards,
        req->guard_count,
        req->metrics_label,
        req->metrics_label_len,
        body,
        sizeof(body),
        &body_len
    );
    if (rc != RCLIENT_OK) {
        return rc;
    }

    uint8_t pdu[R_MAX_PACKET_SIZE];
    size_t pdu_len = 0;
    rc = r_build_rate_request_pdu(req->dedup_ttl_ms, body, body_len, pdu, sizeof(pdu), &pdu_len);
    if (rc != RCLIENT_OK) {
        return rc;
    }

    r_tenant_header_t tenant;
    memset(&tenant, 0, sizeof(tenant));
    tenant.tlv_type = R_TLV_TENANT;
    tenant.tlv_size = R_TENANT_TLV_LEN;
    tenant.key_id = client->config.tenant.key_id;
    memcpy(tenant.unique_id, req->request_id, 16);
    tenant.time_stamp = r_now_ms(client);
    tenant.steering_feedback = 0;
    tenant.tenant_mgmt_flag = 0;
    tenant.padding[0] = 0;
    tenant.padding[1] = 0;

    size_t pos = 0;
    if (out_cap < R_TENANT_TLV_LEN) {
        return RCLIENT_ERR_PROTOCOL;
    }
    r_tenant_header_write(&tenant, out, R_TENANT_TLV_LEN);
    pos += R_TENANT_TLV_LEN;

    if (client->config.tenant.auth.type == R_AUTH_COOKIE) {
        if (!client->has_cookie) {
            return RCLIENT_ERR_AUTH;
        }
        if (pos + 4 + 32 + pdu_len > out_cap) {
            return RCLIENT_ERR_PROTOCOL;
        }
        r_write_le16(out + pos, R_TLV_AUTH_COOKIE);
        r_write_le16(out + pos + 2, 36);
        pos += 4;
        memcpy(out + pos, client->cookie, 32);
        pos += 32;
        memcpy(out + pos, pdu, pdu_len);
        pos += pdu_len;
    } else if (client->config.tenant.auth.type == R_AUTH_AES_GCM) {
        if (!client->has_aes_key) {
            return RCLIENT_ERR_AUTH;
        }
        uint8_t cipher[R_MAX_PACKET_SIZE];
        size_t cipher_len = 0;
        uint8_t nonce[12];
        uint8_t tag[16];
        if (r_encrypt_pdu_aes_gcm(pdu, pdu_len, client->aes_key, cipher, sizeof(cipher), &cipher_len, nonce, tag) != 0) {
            return RCLIENT_ERR_AUTH;
        }
        if (pos + 4 + 12 + 16 + cipher_len > out_cap) {
            return RCLIENT_ERR_PROTOCOL;
        }
        r_write_le16(out + pos, R_TLV_AUTH_AES);
        r_write_le16(out + pos + 2, 32);
        pos += 4;
        memcpy(out + pos, nonce, 12);
        pos += 12;
        memcpy(out + pos, tag, 16);
        pos += 16;
        memcpy(out + pos, cipher, cipher_len);
        pos += cipher_len;
    } else {
        return RCLIENT_ERR_CONFIG;
    }

    if (pos > R_MAX_PACKET_SIZE) {
        return RCLIENT_ERR_PROTOCOL;
    }
    *out_len = pos;
    return RCLIENT_OK;
}

static int r_send_packet_to_targets(r_client_t *client, r_client_req_t *req, const uint8_t *packet, size_t packet_len) {
    if (!client || !req || !packet || packet_len == 0) {
        return RCLIENT_ERR_PROTOCOL;
    }
    if (!client->io.udp_send) {
        return RCLIENT_ERR_IO;
    }
    for (size_t i = 0; i < req->target_count; i++) {
        int rc = client->io.udp_send(client->io.ctx, &req->targets[i], packet, packet_len);
        if (rc != 0) {
            return RCLIENT_ERR_IO;
        }
    }
    return RCLIENT_OK;
}

static int r_extract_pdu_data(
    r_client_t *client,
    const uint8_t *buf,
    size_t len,
    size_t pos,
    uint8_t *pdu_buf,
    size_t pdu_cap,
    const uint8_t **out_pdu,
    size_t *out_pdu_len
) {
    if (!client || !buf || !out_pdu || !out_pdu_len) {
        return RCLIENT_ERR_PROTOCOL;
    }
    uint16_t auth_type = 0;
    size_t auth_size = 0;
    const uint8_t *auth_body = NULL;
    size_t auth_body_len = 0;
    size_t pdu_pos = 0;
    int rc = r_parse_auth_tlv_header(buf, len, pos, &auth_type, &auth_size, &auth_body, &auth_body_len, &pdu_pos);
    if (rc != RCLIENT_OK) {
        return rc;
    }

    uint16_t expected_type = 0;
    if (client->config.tenant.auth.type == R_AUTH_COOKIE) {
        expected_type = R_TLV_AUTH_COOKIE;
    } else if (client->config.tenant.auth.type == R_AUTH_AES_GCM) {
        expected_type = R_TLV_AUTH_AES;
    } else {
        return RCLIENT_ERR_CONFIG;
    }
    if (auth_type != expected_type) {
        return RCLIENT_ERR_AUTH;
    }

    if (auth_type == R_TLV_AUTH_COOKIE) {
        if (auth_size != 36 || auth_body_len != 32) {
            return RCLIENT_ERR_PROTOCOL;
        }
        if (!client->has_cookie || memcmp(auth_body, client->cookie, 32) != 0) {
            return RCLIENT_ERR_AUTH;
        }
        *out_pdu = buf + pdu_pos;
        *out_pdu_len = len - pdu_pos;
        return RCLIENT_OK;
    }
    if (auth_type == R_TLV_AUTH_AES) {
        if (auth_size != 32 || auth_body_len != 28) {
            return RCLIENT_ERR_PROTOCOL;
        }
        if (!client->has_aes_key) {
            return RCLIENT_ERR_AUTH;
        }
        if (!pdu_buf || pdu_cap == 0) {
            return RCLIENT_ERR_PROTOCOL;
        }
        const uint8_t *nonce = auth_body;
        const uint8_t *tag = auth_body + 12;
        size_t cipher_len = len - pdu_pos;
        size_t out_len = 0;
        if (r_decrypt_pdu_aes_gcm(buf + pdu_pos, cipher_len, client->aes_key, nonce, tag, pdu_buf, pdu_cap, &out_len) != 0) {
            return RCLIENT_ERR_PROTOCOL;
        }
        *out_pdu = pdu_buf;
        *out_pdu_len = out_len;
        return RCLIENT_OK;
    }

    return RCLIENT_ERR_PROTOCOL;
}

static int r_parse_rate_response_alloc(
    const uint8_t *pdu,
    size_t pdu_len,
    r_guard_result_t **out_guards,
    size_t *out_guard_count,
    r_resource_result_t **out_resources,
    size_t *out_res_count,
    bool *out_success
) {
    if (!pdu || pdu_len < R_PDU_HEADER_LEN) {
        return RCLIENT_ERR_PROTOCOL;
    }
    uint16_t pdu_type = r_read_le16(pdu);
    if (pdu_type != R_PDU_RATE_RESPONSE) {
        return RCLIENT_ERR_PROTOCOL;
    }
    size_t pdu_size = r_read_le16(pdu + 2);
    if (pdu_size < R_PDU_HEADER_LEN || pdu_len < pdu_size) {
        return RCLIENT_ERR_PROTOCOL;
    }
    const uint8_t *body = pdu + R_PDU_HEADER_LEN;
    size_t body_len = pdu_size - R_PDU_HEADER_LEN;
    if (body_len < 4) {
        return RCLIENT_ERR_PROTOCOL;
    }
    uint16_t guard_count = r_read_le16(body);
    uint16_t res_count = r_read_le16(body + 2);

    r_guard_result_t *guards = NULL;
    r_resource_result_t *resources = NULL;
    if (guard_count > 0) {
        guards = (r_guard_result_t *)calloc(guard_count, sizeof(r_guard_result_t));
        if (!guards) {
            return RCLIENT_ERR_NOMEM;
        }
    }
    if (res_count > 0) {
        resources = (r_resource_result_t *)calloc(res_count, sizeof(r_resource_result_t));
        if (!resources) {
            free(guards);
            return RCLIENT_ERR_NOMEM;
        }
    }

    size_t parsed_guard_count = 0;
    size_t parsed_res_count = 0;
    bool success = false;
    int rc = r_parse_rate_response_pdu(
        pdu,
        pdu_size,
        guards,
        guard_count,
        &parsed_guard_count,
        resources,
        res_count,
        &parsed_res_count,
        &success
    );
    if (rc != RCLIENT_OK) {
        free(guards);
        free(resources);
        return rc;
    }
    if (out_guards) {
        *out_guards = guards;
    } else {
        free(guards);
    }
    if (out_guard_count) {
        *out_guard_count = parsed_guard_count;
    }
    if (out_resources) {
        *out_resources = resources;
    } else {
        free(resources);
    }
    if (out_res_count) {
        *out_res_count = parsed_res_count;
    }
    if (out_success) {
        *out_success = success;
    }
    return RCLIENT_OK;
}

static uint64_t r_rand_range(uint64_t max) {
    if (max == 0) {
        return 0;
    }
    uint64_t value = 0;
    if (RAND_bytes((unsigned char *)&value, sizeof(value)) != 1) {
        return 0;
    }
    return value % (max + 1);
}

static void r_request_reset_attempt_state(r_client_req_t *req) {
    if (!req) {
        return;
    }
    req->seen_addr_count = 0;
    req->seen_server_id_count = 0;
    req->any_success = false;
    req->any_failure = false;
    r_candidate_clear(&req->best);
    r_candidate_clear(&req->best_allow);
    r_candidate_clear(&req->best_deny);
}

static int r_request_start_attempt(r_client_t *client, r_client_req_t *req) {
    if (!client || !req) {
        return RCLIENT_ERR_CONFIG;
    }
    r_request_reset_attempt_state(req);

    uint8_t packet[R_MAX_PACKET_SIZE];
    size_t packet_len = 0;
    int rc = r_build_rate_request_packet(client, req, packet, sizeof(packet), &packet_len);
    if (rc != RCLIENT_OK) {
        return rc;
    }
    rc = r_send_packet_to_targets(client, req, packet, packet_len);
    if (rc != RCLIENT_OK) {
        return rc;
    }

    uint64_t now = r_now_ms(client);
    uint64_t attempt_timeout = client->policy.attempt_timeout_ms;
    req->attempt_deadline_ms = now + attempt_timeout;
    if (req->total_deadline_ms > 0 && req->attempt_deadline_ms > req->total_deadline_ms) {
        req->attempt_deadline_ms = req->total_deadline_ms;
    }
    return RCLIENT_OK;
}

static void r_request_complete(r_client_t *client, r_client_req_t *req, int status, r_candidate_t *selected);

static int r_request_retry_now(r_client_t *client, r_client_req_t *req, uint64_t now_ms) {
    if (!client || !req) {
        return RCLIENT_ERR_CONFIG;
    }
    if (now_ms >= req->dedup_deadline_ms) {
        r_request_complete(client, req, RCLIENT_ERR_TIMEOUT, NULL);
        return RCLIENT_OK;
    }
    if (req->total_deadline_ms > 0 && now_ms >= req->total_deadline_ms) {
        r_request_complete(client, req, RCLIENT_ERR_TIMEOUT, NULL);
        return RCLIENT_OK;
    }

    req->attempt++;
    int rc = r_request_start_attempt(client, req);
    if (rc != RCLIENT_OK) {
        r_request_complete(client, req, rc, NULL);
    }
    return RCLIENT_OK;
}

static void r_request_complete(r_client_t *client, r_client_req_t *req, int status, r_candidate_t *selected) {
    if (!client || !req) {
        return;
    }
    r_request_remove(client, req);
    if (req->cb) {
        const r_rate_limit_result_t *result = selected ? &selected->result : NULL;
        req->cb(req->user, req, status, result);
    }
    if (req->steering_rebind && client->io.on_steering_feedback) {
        client->io.on_steering_feedback(client->io.ctx, false);
    }
    r_request_free(req);
}

int r_client_create(
    const r_client_config_t *config,
    const r_io_ops_t *io_ops,
    const r_resolver_ops_t *resolver_ops,
    r_client_t **out_client
) {
    if (!config || !io_ops || !resolver_ops || !out_client) {
        return RCLIENT_ERR_CONFIG;
    }
    if (!config->tenant.dns_name) {
        return RCLIENT_ERR_CONFIG;
    }

    r_client_t *client = (r_client_t *)calloc(1, sizeof(r_client_t));
    if (!client) {
        return RCLIENT_ERR_NOMEM;
    }

    client->io = *io_ops;
    client->resolver = *resolver_ops;
    client->config = *config;

    client->dns_name = r_strdup_n(config->tenant.dns_name, 0);
    if (!client->dns_name) {
        r_client_destroy(client);
        return RCLIENT_ERR_NOMEM;
    }
    client->config.tenant.dns_name = client->dns_name;

    if (config->tenant.auth.type != R_AUTH_COOKIE &&
        config->tenant.auth.type != R_AUTH_AES_GCM) {
        r_client_destroy(client);
        return RCLIENT_ERR_CONFIG;
    }
    if (!config->tenant.auth.secret) {
        r_client_destroy(client);
        return RCLIENT_ERR_CONFIG;
    }

    if (config->tenant.auth.secret) {
        client->auth_secret = r_strdup_n(config->tenant.auth.secret, config->tenant.auth.secret_len);
        if (!client->auth_secret) {
            r_client_destroy(client);
            return RCLIENT_ERR_NOMEM;
        }
        client->config.tenant.auth.secret = client->auth_secret;
        client->config.tenant.auth.secret_len = 0;
    }

    if (config->request_policy) {
        client->policy = *config->request_policy;
    } else {
        r_client_default_request_policy(&client->policy);
    }

    if (client->auth_secret) {
        uint8_t decoded_secret[64];
        size_t decoded_secret_len = 0;
        r_auth_type_t decoded_type = (r_auth_type_t)0;
        uint64_t decoded_key_id = 0;
        if (r_decode_api_key_bech32_with_quotas(
                client->auth_secret,
                &decoded_type,
                &decoded_key_id,
                decoded_secret,
                sizeof(decoded_secret),
                &decoded_secret_len,
                &client->quotas) != 0) {
            r_client_destroy(client);
            return RCLIENT_ERR_CONFIG;
        }
        client->has_quotas = true;
        if (decoded_type != config->tenant.auth.type) {
            r_client_destroy(client);
            return RCLIENT_ERR_CONFIG;
        }
        if (decoded_key_id != config->tenant.key_id) {
            r_client_destroy(client);
            return RCLIENT_ERR_CONFIG;
        }
        if (decoded_type == R_AUTH_COOKIE) {
            if (decoded_secret_len != 32u) {
                r_client_destroy(client);
                return RCLIENT_ERR_CONFIG;
            }
            memcpy(client->cookie, decoded_secret, 32u);
            client->has_cookie = true;
        } else if (decoded_type == R_AUTH_AES_GCM) {
            if (decoded_secret_len != 32u) {
                r_client_destroy(client);
                return RCLIENT_ERR_CONFIG;
            }
            memcpy(client->aes_key, decoded_secret, 32u);
            client->has_aes_key = true;
        } else {
            r_client_destroy(client);
            return RCLIENT_ERR_CONFIG;
        }
    }

    client->last_dns_refresh_ms = 0;
    client->dns_refresh_inflight = false;
    client->servers = NULL;
    client->server_count = 0;
    client->server_cap = 0;
    client->stats.items = NULL;
    client->stats.count = 0;
    client->stats.cap = 0;
    client->inflight = NULL;

    (void)r_dns_maybe_refresh(client, true);

    *out_client = client;
    return RCLIENT_OK;
}

void r_client_destroy(r_client_t *client) {
    if (!client) {
        return;
    }
    r_client_req_t *req = client->inflight;
    while (req) {
        r_client_req_t *next = req->next;
        r_request_free(req);
        req = next;
    }
    free(client->servers);
    free(client->stats.items);
    if (client->dns_refresh) {
        r_dns_refresh_detach(client->dns_refresh);
    }
    free(client->dns_name);
    free(client->auth_secret);
    free(client);
}

static int r_client_check_rate_limit_async_impl(
    r_client_t *client,
    const r_resource_request_t *resources,
    size_t resource_count,
    const r_latency_guard_t *guards,
    size_t guard_count,
    const char *metrics_label,
    size_t metrics_label_len,
    r_rate_limit_cb cb,
    void *user,
    r_client_req_t **out_req,
    bool borrowed
) {
    if (!client || !resources || resource_count == 0 || !cb) {
        return RCLIENT_ERR_CONFIG;
    }
    if (guard_count > 0 && !guards) {
        return RCLIENT_ERR_CONFIG;
    }
    int rc = r_validate_latency_guards(client, guards, guard_count);
    if (rc != RCLIENT_OK) {
        return rc;
    }
    (void)r_dns_maybe_refresh(client, false);
    if (client->server_count == 0) {
        (void)r_dns_maybe_refresh(client, true);
        return RCLIENT_ERR_DNS;
    }

    r_client_req_t *req = (r_client_req_t *)calloc(1, sizeof(r_client_req_t));
    if (!req) {
        return RCLIENT_ERR_NOMEM;
    }
    req->client = client;
    req->cb = cb;
    req->user = user;

    if (borrowed) {
        req->resources = (r_resource_request_t *)resources;
        req->resource_count = resource_count;
        req->owns_resources = false;
    } else {
        req->resources = (r_resource_request_t *)calloc(resource_count, sizeof(r_resource_request_t));
        if (!req->resources) {
            r_request_free(req);
            return RCLIENT_ERR_NOMEM;
        }
        memcpy(req->resources, resources, resource_count * sizeof(r_resource_request_t));
        req->resource_count = resource_count;
        req->owns_resources = true;
    }

    if (guard_count > 0) {
        if (borrowed) {
            req->guards = (r_latency_guard_t *)guards;
            req->guard_count = guard_count;
            req->owns_guards = false;
        } else {
            req->guards = (r_latency_guard_t *)calloc(guard_count, sizeof(r_latency_guard_t));
            if (!req->guards) {
                r_request_free(req);
                return RCLIENT_ERR_NOMEM;
            }
            memcpy(req->guards, guards, guard_count * sizeof(r_latency_guard_t));
            req->guard_count = guard_count;
            req->owns_guards = true;
        }
    }

    if (metrics_label && metrics_label[0] != '\0') {
        size_t label_len = metrics_label_len == 0 ? strlen(metrics_label) : metrics_label_len;
        if (borrowed) {
            req->metrics_label = (char *)metrics_label;
            req->metrics_label_len = label_len;
            req->owns_metrics_label = false;
        } else {
            req->metrics_label = r_strdup_n(metrics_label, label_len);
            if (!req->metrics_label) {
                r_request_free(req);
                return RCLIENT_ERR_NOMEM;
            }
            req->metrics_label_len = label_len;
            req->owns_metrics_label = true;
        }
    }

    r_generate_request_id(req->request_id);
    req->start_ms = r_now_ms(client);
    req->dedup_ttl_ms = r_effective_dedup_ttl_ms(client);
    if (req->dedup_ttl_ms == 0u) {
        r_request_free(req);
        return RCLIENT_ERR_CONFIG;
    }
    req->dedup_deadline_ms = req->start_ms + (uint64_t)req->dedup_ttl_ms;
    req->attempt = 0;
    req->total_attempts = client->policy.retry.retry_attempts + 1;
    if (client->policy.retry.total_timeout_ms > 0) {
        req->total_deadline_ms = req->start_ms + client->policy.retry.total_timeout_ms;
    } else {
        req->total_deadline_ms = 0;
    }

    rc = r_request_snapshot_targets(client, req);
    if (rc != RCLIENT_OK) {
        r_request_free(req);
        return rc;
    }

    rc = r_request_start_attempt(client, req);
    if (rc != RCLIENT_OK) {
        r_request_free(req);
        return rc;
    }

    req->next = client->inflight;
    client->inflight = req;
    if (out_req) {
        *out_req = req;
    }
    return RCLIENT_OK;
}

int r_client_check_rate_limit_async(
    r_client_t *client,
    const r_resource_request_t *resources,
    size_t resource_count,
    const r_latency_guard_t *guards,
    size_t guard_count,
    const char *metrics_label,
    size_t metrics_label_len,
    r_rate_limit_cb cb,
    void *user,
    r_client_req_t **out_req
) {
    return r_client_check_rate_limit_async_impl(
        client,
        resources,
        resource_count,
        guards,
        guard_count,
        metrics_label,
        metrics_label_len,
        cb,
        user,
        out_req,
        false
    );
}

int r_client_check_rate_limit_async_borrowed(
    r_client_t *client,
    const r_resource_request_t *resources,
    size_t resource_count,
    const r_latency_guard_t *guards,
    size_t guard_count,
    const char *metrics_label,
    size_t metrics_label_len,
    r_rate_limit_cb cb,
    void *user,
    r_client_req_t **out_req
) {
    return r_client_check_rate_limit_async_impl(
        client,
        resources,
        resource_count,
        guards,
        guard_count,
        metrics_label,
        metrics_label_len,
        cb,
        user,
        out_req,
        true
    );
}

int r_client_report_latency(
    r_client_t *client,
    const r_service_latency_report_t *reports,
    size_t report_count
) {
    if (!client || !reports || report_count == 0) {
        return RCLIENT_ERR_CONFIG;
    }

    r_service_latency_report_t *filtered_reports = NULL;
    size_t filtered_count = 0;
    bool owns_filtered_reports = false;
    int rc = r_filter_latency_reports(
        client,
        reports,
        report_count,
        &filtered_reports,
        &filtered_count,
        &owns_filtered_reports
    );
    if (rc != RCLIENT_OK) {
        return rc;
    }
    if (filtered_count == 0) {
        if (owns_filtered_reports) {
            free(filtered_reports);
        }
        return RCLIENT_OK;
    }
    if (!client->io.udp_send) {
        if (owns_filtered_reports) {
            free(filtered_reports);
        }
        return RCLIENT_ERR_IO;
    }

    (void)r_dns_maybe_refresh(client, false);
    if (client->server_count == 0) {
        (void)r_dns_maybe_refresh(client, true);
        if (owns_filtered_reports) {
            free(filtered_reports);
        }
        return RCLIENT_ERR_DNS;
    }

    uint8_t body[R_MAX_PACKET_SIZE];
    size_t body_len = 0;
    rc = r_build_latency_report_body(filtered_reports, filtered_count, body, sizeof(body), &body_len);
    if (rc != RCLIENT_OK) {
        if (owns_filtered_reports) {
            free(filtered_reports);
        }
        return rc;
    }

    uint8_t pdu[R_MAX_PACKET_SIZE];
    size_t pdu_len = 0;
    rc = r_build_pdu(R_PDU_LATENCY_REPORT, body, body_len, pdu, sizeof(pdu), &pdu_len);
    if (rc != RCLIENT_OK) {
        if (owns_filtered_reports) {
            free(filtered_reports);
        }
        return rc;
    }

    r_tenant_header_t tenant;
    memset(&tenant, 0, sizeof(tenant));
    tenant.tlv_type = R_TLV_TENANT;
    tenant.tlv_size = R_TENANT_TLV_LEN;
    tenant.key_id = client->config.tenant.key_id;
    r_generate_request_id(tenant.unique_id);
    tenant.time_stamp = r_now_ms(client);
    tenant.steering_feedback = 0;
    tenant.tenant_mgmt_flag = 0;

    uint8_t packet[R_MAX_PACKET_SIZE];
    size_t pos = 0;
    r_tenant_header_write(&tenant, packet, R_TENANT_TLV_LEN);
    pos += R_TENANT_TLV_LEN;

    if (client->config.tenant.auth.type == R_AUTH_COOKIE) {
        if (!client->has_cookie) {
            if (owns_filtered_reports) {
                free(filtered_reports);
            }
            return RCLIENT_ERR_AUTH;
        }
        r_write_le16(packet + pos, R_TLV_AUTH_COOKIE);
        r_write_le16(packet + pos + 2, 36);
        pos += 4;
        memcpy(packet + pos, client->cookie, 32);
        pos += 32;
        memcpy(packet + pos, pdu, pdu_len);
        pos += pdu_len;
    } else if (client->config.tenant.auth.type == R_AUTH_AES_GCM) {
        if (!client->has_aes_key) {
            if (owns_filtered_reports) {
                free(filtered_reports);
            }
            return RCLIENT_ERR_AUTH;
        }
        uint8_t cipher[R_MAX_PACKET_SIZE];
        size_t cipher_len = 0;
        uint8_t nonce[12];
        uint8_t tag[16];
        if (r_encrypt_pdu_aes_gcm(pdu, pdu_len, client->aes_key, cipher, sizeof(cipher), &cipher_len, nonce, tag) != 0) {
            if (owns_filtered_reports) {
                free(filtered_reports);
            }
            return RCLIENT_ERR_AUTH;
        }
        r_write_le16(packet + pos, R_TLV_AUTH_AES);
        r_write_le16(packet + pos + 2, 32);
        pos += 4;
        memcpy(packet + pos, nonce, 12);
        pos += 12;
        memcpy(packet + pos, tag, 16);
        pos += 16;
        memcpy(packet + pos, cipher, cipher_len);
        pos += cipher_len;
    } else {
        if (owns_filtered_reports) {
            free(filtered_reports);
        }
        return RCLIENT_ERR_CONFIG;
    }

    if (pos > R_MAX_PACKET_SIZE) {
        if (owns_filtered_reports) {
            free(filtered_reports);
        }
        return RCLIENT_ERR_PROTOCOL;
    }

    for (size_t i = 0; i < client->server_count; i++) {
        int send_rc = client->io.udp_send(client->io.ctx, &client->servers[i].addr, packet, pos);
        if (send_rc != 0) {
            if (owns_filtered_reports) {
                free(filtered_reports);
            }
            return RCLIENT_ERR_IO;
        }
    }
    if (owns_filtered_reports) {
        free(filtered_reports);
    }
    return RCLIENT_OK;
}

int r_client_on_datagram(
    r_client_t *client,
    const uint8_t *buf,
    size_t len,
    const r_addr_t *from
) {
    if (!client || !buf || len == 0 || !from) {
        return RCLIENT_ERR_PROTOCOL;
    }
    r_tenant_header_t tenant;
    size_t pos = 0;
    int rc = r_parse_tenant_header(buf, len, &tenant, &pos);
    if (rc != RCLIENT_OK) {
        return rc;
    }

    r_client_req_t *req = client->inflight;
    while (req) {
        if (memcmp(req->request_id, tenant.unique_id, 16) == 0) {
            break;
        }
        req = req->next;
    }
    if (!req) {
        return RCLIENT_OK;
    }
    uint64_t server_id = tenant.key_id;
    if (!r_allowed_server_id(req, server_id)) {
        uint64_t now_ms = r_now_ms(client);
        r_server_stat_t *stat = r_stats_get_or_add(client, server_id, now_ms);
        if (stat) {
            stat->last_seen_ms = now_ms;
            stat->id_mismatch += 1;
        }
        return RCLIENT_OK;
    }

    uint8_t pdu_buf[R_MAX_PACKET_SIZE];
    const uint8_t *pdu = NULL;
    size_t pdu_len = 0;
    rc = r_extract_pdu_data(client, buf, len, pos, pdu_buf, sizeof(pdu_buf), &pdu, &pdu_len);
    if (rc != RCLIENT_OK) {
        uint64_t now_ms = r_now_ms(client);
        r_server_stat_t *stat = r_stats_get_or_add(client, server_id, now_ms);
        if (stat) {
            stat->last_seen_ms = now_ms;
            if (rc == RCLIENT_ERR_AUTH) {
                stat->auth_fail += 1;
            } else {
                stat->decrypt_fail += 1;
            }
        }
        return rc;
    }

    r_guard_result_t *guard_results = NULL;
    size_t guard_count = 0;
    r_resource_result_t *resource_results = NULL;
    size_t resource_count = 0;
    bool success = false;
    rc = r_parse_rate_response_alloc(
        pdu,
        pdu_len,
        &guard_results,
        &guard_count,
        &resource_results,
        &resource_count,
        &success
    );
    if (rc != RCLIENT_OK) {
        return rc;
    }

    if (!tenant.steering_feedback) {
        req->steering_rebind = true;
    }

    uint64_t now_ms = r_now_ms(client);
    r_server_stat_t *stat = r_stats_get_or_add(client, server_id, now_ms);
    if (stat) {
        stat->last_seen_ms = now_ms;
        stat->valid_responses += 1;
    }

    (void)r_seen_addr_add(req, from);
    (void)r_seen_server_add(req, server_id);

    if (success) {
        req->any_success = true;
    } else {
        req->any_failure = true;
    }

    r_rate_limit_result_t result;
    memset(&result, 0, sizeof(result));
    result.success = success;
    result.server_id = server_id;
    result.steering_feedback = tenant.steering_feedback != 0;

    bool kept = false;
    if (client->policy.wait == R_WAIT_RETURN_ON_FIRST_VALID
        || client->policy.wait == R_WAIT_RETURN_ON_FIRST_STABLE) {
        if (!req->best.has) {
            r_candidate_set(&req->best, result.server_id, result.steering_feedback, result.success,
                guard_results, guard_count, resource_results, resource_count);
            kept = true;
        }
    } else {
        switch (client->policy.select) {
        case R_SELECT_FIRST_VALID:
            if (!req->best.has) {
                r_candidate_set(&req->best, result.server_id, result.steering_feedback, result.success,
                    guard_results, guard_count, resource_results, resource_count);
                kept = true;
            }
            break;
        case R_SELECT_BEST_BY_RELIABILITY:
            kept = r_candidate_try_update_best(client, &req->best, &result, guard_results, guard_count, resource_results, resource_count);
            break;
        case R_SELECT_CONSERVATIVE_DENY:
            if (success) {
                kept = r_candidate_try_update_best(client, &req->best_allow, &result, guard_results, guard_count, resource_results, resource_count);
            } else {
                kept = r_candidate_try_update_best(client, &req->best_deny, &result, guard_results, guard_count, resource_results, resource_count);
            }
            break;
        default:
            break;
        }
    }
    if (!kept) {
        free(guard_results);
        free(resource_results);
    }

    size_t quorum_required = r_quorum_required(client->policy.quorum, req->replica_count);
    bool quorum_met = req->seen_server_id_count >= quorum_required;
    bool all_targets_responded = req->target_count > 0 && req->seen_addr_count >= req->target_count;

    if (client->policy.wait == R_WAIT_RETURN_ON_FIRST_VALID) {
        if (req->best.has) {
            r_request_complete(client, req, RCLIENT_OK, &req->best);
        }
    } else if (client->policy.wait == R_WAIT_RETURN_ON_FIRST_STABLE) {
        if (r_server_stable(client, server_id, now_ms)) {
            r_candidate_t *selected = NULL;
            if (client->policy.select == R_SELECT_CONSERVATIVE_DENY) {
                selected = req->best_deny.has ? &req->best_deny : &req->best_allow;
            } else {
                selected = &req->best;
            }
            if (selected && selected->has) {
                r_request_complete(client, req, RCLIENT_OK, selected);
            }
        }
    } else if (client->policy.wait == R_WAIT_FOR_DEADLINE) {
        // Match Rust client behavior: if every target already responded, return
        // immediately instead of idling until attempt_deadline_ms.
        if (all_targets_responded) {
            r_candidate_t *selected = NULL;
            if (client->policy.select == R_SELECT_CONSERVATIVE_DENY) {
                selected = req->best_deny.has ? &req->best_deny : &req->best_allow;
            } else {
                selected = &req->best;
            }
            if (selected && selected->has) {
                r_request_complete(client, req, RCLIENT_OK, selected);
            }
        }
    } else if (client->policy.wait == R_WAIT_FOR_QUORUM) {
        if (quorum_met) {
            r_candidate_t *selected = NULL;
            if (client->policy.select == R_SELECT_CONSERVATIVE_DENY) {
                selected = req->best_deny.has ? &req->best_deny : &req->best_allow;
            } else {
                selected = &req->best;
            }
            if (selected && selected->has) {
                r_request_complete(client, req, RCLIENT_OK, selected);
            }
        }
    }
    return RCLIENT_OK;
}

int r_client_request_deadline_ms(
    const r_client_req_t *req,
    uint64_t *out_deadline_ms
) {
    if (!req || !out_deadline_ms) {
        return RCLIENT_ERR_CONFIG;
    }
    *out_deadline_ms = req->attempt_deadline_ms;
    return RCLIENT_OK;
}

int r_client_on_timeout(
    r_client_t *client,
    r_client_req_t *req,
    uint64_t now_ms
) {
    if (!client || !req) {
        return RCLIENT_ERR_CONFIG;
    }
    if (now_ms < req->attempt_deadline_ms) {
        return RCLIENT_OK;
    }

    size_t quorum_required = r_quorum_required(client->policy.quorum, req->replica_count);
    bool quorum_met = req->seen_server_id_count >= quorum_required;
    bool inconsistent = r_request_inconsistent(req);

    bool quorum_miss = (client->policy.wait == R_WAIT_FOR_QUORUM
        && !quorum_met
        && client->policy.quorum_requirement == R_QUORUM_HARD);

    if (req->best.has || req->best_allow.has || req->best_deny.has) {
        if (!quorum_miss) {
            r_candidate_t *selected = NULL;
            if (client->policy.select == R_SELECT_CONSERVATIVE_DENY) {
                selected = req->best_deny.has ? &req->best_deny : &req->best_allow;
            } else {
                selected = &req->best;
            }
            if (selected && selected->has) {
                if (inconsistent && client->policy.retry.retry_on == R_RETRY_INCONSISTENT
                    && req->attempt + 1 < req->total_attempts) {
                    if (client->policy.retry.refresh_dns_on_retry || client->policy.dns_resync.on == R_DNS_ON_RETRY) {
                        (void)r_dns_maybe_refresh(client, true);
                    }
                    return r_request_retry_now(client, req, now_ms);
                }
                r_request_complete(client, req, RCLIENT_OK, selected);
                return RCLIENT_OK;
            }
        }
    }

    if (!(req->best.has || req->best_allow.has || req->best_deny.has)) {
        r_stats_timeout_all(client, now_ms);
    }

    bool should_retry = false;
    if (client->policy.retry.retry_on == R_RETRY_TIMEOUT_ONLY) {
        should_retry = !(req->best.has || req->best_allow.has || req->best_deny.has);
    } else if (client->policy.retry.retry_on == R_RETRY_QUORUM_NOT_MET && quorum_miss) {
        should_retry = true;
    }
    if (req->attempt + 1 >= req->total_attempts) {
        should_retry = false;
    }

    if (should_retry) {
        if (client->policy.dns_resync.on == R_DNS_ON_TIMEOUT || client->policy.dns_resync.on == R_DNS_ON_ANY_ERROR) {
            (void)r_dns_maybe_refresh(client, true);
        }
        if (quorum_miss && (client->policy.dns_resync.on == R_DNS_ON_QUORUM_MISS || client->policy.dns_resync.on == R_DNS_ON_ANY_ERROR)) {
            (void)r_dns_maybe_refresh(client, true);
        }
        if (client->policy.retry.refresh_dns_on_retry || client->policy.dns_resync.on == R_DNS_ON_RETRY) {
            (void)r_dns_maybe_refresh(client, true);
        }
        return r_request_retry_now(client, req, now_ms);
    }

    r_request_complete(client, req, RCLIENT_ERR_TIMEOUT, NULL);
    return RCLIENT_OK;
}

void r_client_cancel_request(r_client_t *client, r_client_req_t *req) {
    if (!client || !req) {
        return;
    }
    r_request_remove(client, req);
    r_request_free(req);
}

void r_client_hash_id(const char *input, uint8_t out_id[16]) {
    if (!input || !out_id) {
        return;
    }
    (void)r_hash_id_blake2s_128(input, out_id);
}

int r_client_parse_auth_key(const char *encoded, r_auth_key_info_t *out_info) {
    if (!encoded || !out_info) {
        return RCLIENT_ERR_CONFIG;
    }
    memset(out_info, 0, sizeof(*out_info));

    r_auth_key_info_t info;
    memset(&info, 0, sizeof(info));

    r_bech32_quotas_t quotas;
    memset(&quotas, 0, sizeof(quotas));

    r_auth_type_t type = (r_auth_type_t)0;
    uint64_t key_id = 0;
    size_t secret_len = 0;
    if (r_decode_api_key_bech32_with_quotas(
            encoded,
            &type,
            &key_id,
            info.secret,
            sizeof(info.secret),
            &secret_len,
            &quotas) != 0) {
        return RCLIENT_ERR_CONFIG;
    }

    if ((type != R_AUTH_COOKIE && type != R_AUTH_AES_GCM) || secret_len != 32u) {
        return RCLIENT_ERR_CONFIG;
    }

    info.type = type;
    info.key_id = key_id;
    info.secret_len = secret_len;
    info.rate_buckets_max = quotas.rate_buckets_max;
    info.latency_services_max = quotas.latency_services_max;
    info.metrics_labels_max = quotas.metrics_labels_max;
    info.latency_buffer_size_max = quotas.latency_buffer_size_max;
    info.dedup_ttl_ms_max = quotas.dedup_ttl_ms_max;

    *out_info = info;
    return RCLIENT_OK;
}
