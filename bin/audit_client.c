#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <poll.h>
#include <resolv.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../include/r_client.h"
#include "../src/r_crypto.h"
#include "../src/r_protocol.h"

#define AUDIT_PACKET_CAP 1400u
#define AUDIT_DNS_ANSWER_CAP 4096u
#define AUDIT_NAME_CAP 128u
#define AUDIT_RUN_ID_CAP 48u
#define AUDIT_TRACKER_TTL_MS 10000u
#define AUDIT_TRACKER_MAX_SAMPLES 10u
#define AUDIT_TRACKER_BUFFER_SIZE 10u
#define AUDIT_TRACKER_MIN_SAMPLES 5u
#define AUDIT_GUARD_THRESHOLD_MS 1000u
#define AUDIT_REPORTED_LATENCY_MS 100u
#define AUDIT_RATE_WINDOW_MS 10000u
#define AUDIT_RATE_LIMIT 10u
#define AUDIT_RATE_REQUEST_COUNT 20u
#define AUDIT_METRICS_FAMILY_SUMMARY 0x01u
#define AUDIT_PDU_METRICS_QUERY 0x514du
#define AUDIT_PDU_METRICS_RESPONSE 0x524du

typedef struct audit_addr_entry {
    char *name;
    r_addr_t *items;
    size_t count;
} audit_addr_entry_t;

typedef struct audit_target {
    const char *name;
    uint16_t port;
    const r_addr_t *addresses;
    size_t address_count;
} audit_target_t;

typedef struct audit_dns_cache {
    char *tenant_name;
    char *srv_name;
    r_srv_record_t *srv_records;
    size_t srv_count;
    audit_addr_entry_t *addr_entries;
    size_t addr_entry_count;
    audit_target_t *targets;
    size_t target_count;
} audit_dns_cache_t;

typedef struct audit_resolver {
    struct __res_state state;
    bool initialized;
} audit_resolver_t;

typedef struct audit_addr_list {
    r_addr_t *items;
    size_t count;
    size_t capacity;
} audit_addr_list_t;

typedef struct audit_io {
    int socket_fd;
} audit_io_t;

typedef struct audit_request_result {
    bool done;
    int status;
    bool accepted;
    uint64_t server_id;
    uint32_t current_latency_ms;
    uint32_t actual_rate;
} audit_request_result_t;

typedef struct audit_phase_stats {
    uint32_t accepted;
    uint32_t rejected;
    uint32_t errors;
    uint64_t elapsed_ms;
} audit_phase_stats_t;

typedef struct audit_summary_metrics {
    uint64_t requests_success;
    uint64_t requests_rate_limited;
    uint64_t requests_guard_failed;
    uint64_t requests_auth_failed;
    uint64_t service_latency_reports_total;
} audit_summary_metrics_t;

typedef struct audit_metrics_snapshot {
    bool valid;
    uint64_t server_id;
    audit_summary_metrics_t summary;
} audit_metrics_snapshot_t;

typedef struct audit_metrics_response {
    uint64_t server_id;
    uint8_t family;
    uint8_t page_kind;
    uint8_t body[AUDIT_PACKET_CAP];
    size_t body_len;
} audit_metrics_response_t;

typedef struct audit_config {
    const char *auth_key;
    const char *management_key;
    const char *run_id;
    uint32_t metrics_timeout_ms;
    uint32_t metrics_attempts;
    r_request_policy_t policy;
} audit_config_t;

static uint16_t audit_read_le16(const uint8_t *input) {
    return (uint16_t)input[0] | ((uint16_t)input[1] << 8);
}

static uint64_t audit_read_le64(const uint8_t *input) {
    uint64_t value = 0u;
    for (unsigned int index = 0u; index < 8u; index++) {
        value |= (uint64_t)input[index] << (index * 8u);
    }
    return value;
}

static void audit_write_le16(uint8_t *output, uint16_t value) {
    output[0] = (uint8_t)(value & 0xffu);
    output[1] = (uint8_t)((value >> 8) & 0xffu);
}

static char *audit_strdup(const char *input) {
    if (!input) {
        return NULL;
    }
    size_t length = strlen(input);
    char *output = (char *)malloc(length + 1u);
    if (output) {
        memcpy(output, input, length + 1u);
    }
    return output;
}

static uint64_t audit_wall_time_ms(void *context) {
    (void)context;
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return 0u;
    }
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static uint64_t audit_monotonic_time_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0u;
    }
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static uint64_t audit_monotonic_time_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0u;
    }
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static void audit_print_timestamp(FILE *stream) {
    struct timespec now;
    struct tm local;
    char date[32];
    if (clock_gettime(CLOCK_REALTIME, &now) != 0
        || !localtime_r(&now.tv_sec, &local)
        || strftime(date, sizeof(date), "%Y/%m/%d %H:%M:%S", &local) == 0u) {
        fputs("0000/00/00 00:00:00.000000", stream);
        return;
    }
    fprintf(stream, "%s.%06ld", date, now.tv_nsec / 1000L);
}

static void audit_format_short_id(const uint8_t id[16], char output[17]) {
    for (size_t index = 0u; index < 8u; index++) {
        (void)snprintf(output + index * 2u, 3u, "%02x", id[index]);
    }
}

static void audit_sleep_ms(uint64_t milliseconds) {
    struct timespec remaining = {
        .tv_sec = (time_t)(milliseconds / 1000u),
        .tv_nsec = (long)((milliseconds % 1000u) * 1000000u),
    };
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
    }
}

static const char *audit_status_name(int status) {
    switch (status) {
        case RCLIENT_OK: return "ok";
        case RCLIENT_ERR_IO: return "io error";
        case RCLIENT_ERR_TIMEOUT: return "timeout";
        case RCLIENT_ERR_PROTOCOL: return "protocol error";
        case RCLIENT_ERR_AUTH: return "auth error";
        case RCLIENT_ERR_DNS: return "dns error";
        case RCLIENT_ERR_CONFIG: return "configuration error";
        case RCLIENT_ERR_NOMEM: return "out of memory";
        default: return "unknown error";
    }
}

static bool audit_parse_u64(const char *text, uint64_t minimum, uint64_t maximum, uint64_t *output) {
    if (!text || text[0] == '\0' || text[0] == '-' || !output) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0'
        || parsed < minimum || parsed > maximum) {
        return false;
    }
    *output = (uint64_t)parsed;
    return true;
}

static bool audit_parse_bool(const char *text, bool *output) {
    if (!text || !output) {
        return false;
    }
    if (strcmp(text, "true") == 0 || strcmp(text, "1") == 0) {
        *output = true;
        return true;
    }
    if (strcmp(text, "false") == 0 || strcmp(text, "0") == 0) {
        *output = false;
        return true;
    }
    return false;
}

static const char *audit_option_value(
    int argc,
    char **argv,
    int *index,
    const char *name,
    bool *matched
) {
    size_t length = strlen(name);
    const char *argument = argv[*index];
    *matched = false;
    if (strncmp(argument, name, length) != 0) {
        return NULL;
    }
    if (argument[length] == '=') {
        *matched = true;
        return argument + length + 1u;
    }
    if (argument[length] != '\0') {
        return NULL;
    }
    *matched = true;
    if (*index + 1 >= argc) {
        return NULL;
    }
    (*index)++;
    return argv[*index];
}

static const char *audit_schedule_name(r_ha_schedule_kind_t kind) {
    switch (kind) {
        case R_HA_SCHEDULE_FIXED: return "fixed";
        case R_HA_SCHEDULE_LINEAR: return "linear";
        case R_HA_SCHEDULE_EXPONENTIAL: return "exponential";
    }
    return "unknown";
}

static uint32_t audit_schedule_growth(const r_request_policy_t *policy) {
    if (policy->replay_gap.kind == R_HA_SCHEDULE_LINEAR) {
        return policy->replay_gap.growth.linear_step_units;
    }
    if (policy->replay_gap.kind == R_HA_SCHEDULE_EXPONENTIAL) {
        return policy->replay_gap.growth.exponential_factor;
    }
    return 0u;
}

static bool audit_schedule_units(
    const r_ha_schedule_t *schedule,
    uint32_t round,
    uint32_t *output
) {
    if (!schedule || !output || schedule->initial_units == 0u
        || schedule->initial_units > schedule->max_units) {
        return false;
    }
    uint32_t value = schedule->initial_units;
    if (schedule->kind == R_HA_SCHEDULE_FIXED) {
        *output = value;
        return true;
    }
    if (schedule->kind == R_HA_SCHEDULE_LINEAR) {
        uint32_t step = schedule->growth.linear_step_units;
        if (step == 0u) {
            return false;
        }
        uint64_t candidate = (uint64_t)value + (uint64_t)step * round;
        *output = candidate > schedule->max_units
            ? schedule->max_units : (uint32_t)candidate;
        return true;
    }
    if (schedule->kind == R_HA_SCHEDULE_EXPONENTIAL) {
        uint32_t factor = schedule->growth.exponential_factor;
        if (factor < 2u) {
            return false;
        }
        while (round > 0u && value < schedule->max_units) {
            if (value > schedule->max_units / factor) {
                value = schedule->max_units;
            } else {
                value *= factor;
            }
            round--;
        }
        *output = value;
        return true;
    }
    return false;
}

static bool audit_policy_horizon_ms(const r_request_policy_t *policy, uint64_t *output) {
    if (!policy || !output || policy->unit_ms == 0u
        || policy->replay_count > R_CLIENT_HA_MAX_REPLAY_COUNT) {
        return false;
    }
    uint64_t units = policy->final_receive_units;
    for (uint32_t round = 0u; round <= policy->replay_count; round++) {
        uint32_t round_units = 0u;
        if (!audit_schedule_units(&policy->replay_gap, round, &round_units)
            || UINT64_MAX - units < round_units) {
            return false;
        }
        units += round_units;
    }
    if (units > UINT64_MAX / policy->unit_ms) {
        return false;
    }
    *output = units * policy->unit_ms;
    return true;
}

static void audit_print_help(void) {
    printf("RateLimitly server audit client\n\n");
    printf("Usage: audit_client --auth=<bech32> [OPTIONS]\n\n");
    printf("Credentials and discovery:\n");
    printf("  --auth=<bech32>                 Tenant key (required; rl-cookie... or rl-aes...)\n");
    printf("  --management-key=<bech32>       Server rl-secret; enables metrics snapshots\n");
    printf("  --run-id=<text>                 Stable suffix for bucket/latency names\n\n");
    printf("HA policy:\n");
    printf("  --unit-ms=<n>                   Base scheduling unit U (default: 20)\n");
    printf("  --replay-count=<n>              Replays after the initial send (default: 1)\n");
    printf("  --replay-schedule=<kind>        fixed, linear, or exponential (default: fixed)\n");
    printf("  --replay-initial-units=<n>      First round duration in U (default: 1)\n");
    printf("  --replay-max-units=<n>          Round-duration ceiling in U (default: 1)\n");
    printf("  --replay-growth=<n>             Linear step or exponential factor\n");
    printf("  --final-receive-units=<n>       Receive-only interval in U (default: 1)\n");
    printf("  --completion-delivery=<bool>    Send to missing servers before return (default: true)\n\n");
    printf("Metrics:\n");
    printf("  --metrics-timeout-ms=<n>        Per-query response timeout (default: 200)\n");
    printf("  --metrics-attempts=<n>          Query attempts per endpoint (default: 3)\n");
    printf("  --help                          Show this help\n\n");
    printf("RCLIENT_DNS_SERVER=IPv4[:port] selects a non-system DNS server.\n");
}

static int audit_parse_config(int argc, char **argv, audit_config_t *config) {
    memset(config, 0, sizeof(*config));
    r_client_default_request_policy(&config->policy);
    config->metrics_timeout_ms = 200u;
    config->metrics_attempts = 3u;
    bool initial_set = false;
    bool max_set = false;
    bool growth_set = false;

    for (int index = 1; index < argc; index++) {
        const char *argument = argv[index];
        if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
            audit_print_help();
            exit(0);
        }

        bool matched = false;
        const char *value = audit_option_value(argc, argv, &index, "--auth", &matched);
        if (matched) {
            if (!value || value[0] == '\0') goto missing_value;
            config->auth_key = value;
            continue;
        }
        value = audit_option_value(argc, argv, &index, "--management-key", &matched);
        if (matched) {
            if (!value || value[0] == '\0') goto missing_value;
            config->management_key = value;
            continue;
        }
        value = audit_option_value(argc, argv, &index, "--run-id", &matched);
        if (matched) {
            if (!value || value[0] == '\0' || strlen(value) > AUDIT_RUN_ID_CAP) goto invalid_value;
            config->run_id = value;
            continue;
        }
        value = audit_option_value(argc, argv, &index, "--unit-ms", &matched);
        if (matched) {
            uint64_t parsed = 0u;
            if (!value || !audit_parse_u64(value, 1u, UINT64_MAX, &parsed)) goto invalid_value;
            config->policy.unit_ms = parsed;
            continue;
        }
        value = audit_option_value(argc, argv, &index, "--replay-count", &matched);
        if (matched) {
            uint64_t parsed = 0u;
            if (!value || !audit_parse_u64(value, 0u, R_CLIENT_HA_MAX_REPLAY_COUNT, &parsed)) goto invalid_value;
            config->policy.replay_count = (uint32_t)parsed;
            continue;
        }
        value = audit_option_value(argc, argv, &index, "--replay-schedule", &matched);
        if (matched) {
            if (!value) goto missing_value;
            if (strcmp(value, "fixed") == 0) {
                config->policy.replay_gap.kind = R_HA_SCHEDULE_FIXED;
            } else if (strcmp(value, "linear") == 0) {
                config->policy.replay_gap.kind = R_HA_SCHEDULE_LINEAR;
            } else if (strcmp(value, "exponential") == 0) {
                config->policy.replay_gap.kind = R_HA_SCHEDULE_EXPONENTIAL;
            } else {
                goto invalid_value;
            }
            continue;
        }
        value = audit_option_value(argc, argv, &index, "--replay-initial-units", &matched);
        if (matched) {
            uint64_t parsed = 0u;
            if (!value || !audit_parse_u64(value, 1u, UINT32_MAX, &parsed)) goto invalid_value;
            config->policy.replay_gap.initial_units = (uint32_t)parsed;
            initial_set = true;
            continue;
        }
        value = audit_option_value(argc, argv, &index, "--replay-max-units", &matched);
        if (matched) {
            uint64_t parsed = 0u;
            if (!value || !audit_parse_u64(value, 1u, UINT32_MAX, &parsed)) goto invalid_value;
            config->policy.replay_gap.max_units = (uint32_t)parsed;
            max_set = true;
            continue;
        }
        value = audit_option_value(argc, argv, &index, "--replay-growth", &matched);
        if (matched) {
            uint64_t parsed = 0u;
            if (!value || !audit_parse_u64(value, 0u, UINT32_MAX, &parsed)) goto invalid_value;
            config->policy.replay_gap.growth.linear_step_units = (uint32_t)parsed;
            growth_set = true;
            continue;
        }
        value = audit_option_value(argc, argv, &index, "--final-receive-units", &matched);
        if (matched) {
            uint64_t parsed = 0u;
            if (!value || !audit_parse_u64(value, 0u, UINT32_MAX, &parsed)) goto invalid_value;
            config->policy.final_receive_units = (uint32_t)parsed;
            continue;
        }
        value = audit_option_value(argc, argv, &index, "--completion-delivery", &matched);
        if (matched) {
            if (!value || !audit_parse_bool(value, &config->policy.completion_delivery)) goto invalid_value;
            continue;
        }
        value = audit_option_value(argc, argv, &index, "--metrics-timeout-ms", &matched);
        if (matched) {
            uint64_t parsed = 0u;
            if (!value || !audit_parse_u64(value, 1u, UINT32_MAX, &parsed)) goto invalid_value;
            config->metrics_timeout_ms = (uint32_t)parsed;
            continue;
        }
        value = audit_option_value(argc, argv, &index, "--metrics-attempts", &matched);
        if (matched) {
            uint64_t parsed = 0u;
            if (!value || !audit_parse_u64(value, 1u, UINT32_MAX, &parsed)) goto invalid_value;
            config->metrics_attempts = (uint32_t)parsed;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argument);
        return 2;

missing_value:
        fprintf(stderr, "Missing value for %s\n", argument);
        return 2;
invalid_value:
        fprintf(stderr, "Invalid value for %s\n", argument);
        return 2;
    }

    if (!config->auth_key) {
        fprintf(stderr, "Missing required --auth value (expected rl-cookie... or rl-aes...)\n");
        return 2;
    }
    if (initial_set && !max_set) {
        config->policy.replay_gap.max_units = config->policy.replay_gap.initial_units;
    }
    if (!growth_set) {
        if (config->policy.replay_gap.kind == R_HA_SCHEDULE_LINEAR) {
            config->policy.replay_gap.growth.linear_step_units = 1u;
        } else if (config->policy.replay_gap.kind == R_HA_SCHEDULE_EXPONENTIAL) {
            config->policy.replay_gap.growth.exponential_factor = 2u;
        }
    }
    uint64_t horizon_ms = 0u;
    if (!audit_policy_horizon_ms(&config->policy, &horizon_ms)) {
        fprintf(stderr, "Invalid HA policy\n");
        return 2;
    }
    return 0;
}

static bool audit_parse_dns_server(const char *text, struct sockaddr_in *output) {
    if (!text || !output) {
        return false;
    }
    const char *colon = strrchr(text, ':');
    size_t host_length = colon ? (size_t)(colon - text) : strlen(text);
    if (host_length == 0u || host_length >= INET_ADDRSTRLEN) {
        return false;
    }
    char host[INET_ADDRSTRLEN];
    memcpy(host, text, host_length);
    host[host_length] = '\0';
    uint16_t port = 53u;
    if (colon) {
        uint64_t parsed = 0u;
        if (!audit_parse_u64(colon + 1u, 1u, 65535u, &parsed)) {
            return false;
        }
        port = (uint16_t)parsed;
    }
    memset(output, 0, sizeof(*output));
    output->sin_family = AF_INET;
    output->sin_port = htons(port);
    return inet_pton(AF_INET, host, &output->sin_addr) == 1;
}

static int audit_resolver_init(audit_resolver_t *resolver) {
    memset(resolver, 0, sizeof(*resolver));
    if (res_ninit(&resolver->state) != 0) {
        return -1;
    }
    const char *server = getenv("RCLIENT_DNS_SERVER");
    if (server && server[0] != '\0') {
        struct sockaddr_in address;
        if (!audit_parse_dns_server(server, &address)) {
            fprintf(stderr, "Invalid RCLIENT_DNS_SERVER value\n");
            res_nclose(&resolver->state);
            return -1;
        }
        resolver->state.nscount = 1;
        resolver->state.nsaddr_list[0] = address;
    }
    resolver->initialized = true;
    return 0;
}

static void audit_resolver_close(audit_resolver_t *resolver) {
    if (resolver && resolver->initialized) {
        res_nclose(&resolver->state);
        resolver->initialized = false;
    }
}

static int audit_addr_list_push(
    audit_addr_list_t *list,
    const struct sockaddr *address,
    socklen_t length
) {
    if (!list || !address || length == 0u || length > sizeof(struct sockaddr_storage)) {
        return -1;
    }
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0u ? 4u : list->capacity * 2u;
        r_addr_t *items = (r_addr_t *)realloc(list->items, capacity * sizeof(*items));
        if (!items) {
            return -1;
        }
        list->items = items;
        list->capacity = capacity;
    }
    r_addr_t *item = &list->items[list->count++];
    memset(item, 0, sizeof(*item));
    memcpy(&item->sa, address, length);
    item->len = length;
    return 0;
}

static void audit_addr_list_reset(audit_addr_list_t *list) {
    if (list) {
        free(list->items);
        memset(list, 0, sizeof(*list));
    }
}

static int audit_parse_dns_addresses(
    const uint8_t *answer,
    int answer_length,
    int query_type,
    audit_addr_list_t *list
) {
    ns_msg message;
    if (ns_initparse(answer, answer_length, &message) < 0) {
        return -1;
    }
    int count = ns_msg_count(message, ns_s_an);
    for (int index = 0; index < count; index++) {
        ns_rr record;
        if (ns_parserr(&message, ns_s_an, index, &record) != 0
            || ns_rr_class(record) != ns_c_in || (int)ns_rr_type(record) != query_type) {
            continue;
        }
        const uint8_t *data = ns_rr_rdata(record);
        if (query_type == ns_t_a && ns_rr_rdlen(record) == 4u) {
            struct sockaddr_in address;
            memset(&address, 0, sizeof(address));
            address.sin_family = AF_INET;
            memcpy(&address.sin_addr, data, 4u);
            if (audit_addr_list_push(list, (const struct sockaddr *)&address, sizeof(address)) != 0) {
                return -1;
            }
        } else if (query_type == ns_t_aaaa && ns_rr_rdlen(record) == 16u) {
            struct sockaddr_in6 address;
            memset(&address, 0, sizeof(address));
            address.sin6_family = AF_INET6;
            memcpy(&address.sin6_addr, data, 16u);
            if (audit_addr_list_push(list, (const struct sockaddr *)&address, sizeof(address)) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int audit_resolve_addresses(
    audit_resolver_t *resolver,
    const char *name,
    audit_addr_list_t *list
) {
    memset(list, 0, sizeof(*list));
    uint8_t answer[AUDIT_DNS_ANSWER_CAP];
    int length = res_nquery(&resolver->state, name, ns_c_in, ns_t_a, answer, sizeof(answer));
    if (length > 0 && audit_parse_dns_addresses(answer, length, ns_t_a, list) != 0) {
        audit_addr_list_reset(list);
        return -1;
    }
    length = res_nquery(&resolver->state, name, ns_c_in, ns_t_aaaa, answer, sizeof(answer));
    if (length > 0 && audit_parse_dns_addresses(answer, length, ns_t_aaaa, list) != 0) {
        audit_addr_list_reset(list);
        return -1;
    }
    return list->count > 0u ? 0 : -1;
}

static int audit_query_srv(
    audit_resolver_t *resolver,
    const char *name,
    r_srv_record_t **output,
    size_t *output_count
) {
    *output = NULL;
    *output_count = 0u;
    uint8_t answer[AUDIT_DNS_ANSWER_CAP];
    int length = res_nquery(&resolver->state, name, ns_c_in, ns_t_srv, answer, sizeof(answer));
    if (length <= 0) {
        return -1;
    }
    ns_msg message;
    if (ns_initparse(answer, length, &message) < 0) {
        return -1;
    }
    int answer_count = ns_msg_count(message, ns_s_an);
    if (answer_count <= 0) {
        return -1;
    }
    r_srv_record_t *records = (r_srv_record_t *)calloc((size_t)answer_count, sizeof(*records));
    if (!records) {
        return -1;
    }
    size_t count = 0u;
    for (int index = 0; index < answer_count; index++) {
        ns_rr record;
        if (ns_parserr(&message, ns_s_an, index, &record) != 0
            || ns_rr_type(record) != ns_t_srv || ns_rr_class(record) != ns_c_in
            || ns_rr_rdlen(record) < 7u) {
            continue;
        }
        const uint8_t *data = ns_rr_rdata(record);
        char target[NS_MAXDNAME];
        if (dn_expand(ns_msg_base(message), ns_msg_end(message), data + 6u,
                target, sizeof(target)) < 0) {
            continue;
        }
        records[count].priority = ns_get16(data);
        records[count].weight = ns_get16(data + 2u);
        records[count].port = ns_get16(data + 4u);
        records[count].ttl_ms = (uint32_t)ns_rr_ttl(record) * 1000u;
        records[count].target = audit_strdup(target);
        if (records[count].target) {
            count++;
        }
    }
    if (count == 0u) {
        free(records);
        return -1;
    }
    *output = records;
    *output_count = count;
    return 0;
}

static void audit_dns_cache_reset(audit_dns_cache_t *cache) {
    if (!cache) {
        return;
    }
    for (size_t index = 0u; index < cache->srv_count; index++) {
        free((char *)cache->srv_records[index].target);
    }
    for (size_t index = 0u; index < cache->addr_entry_count; index++) {
        free(cache->addr_entries[index].name);
        free(cache->addr_entries[index].items);
    }
    free(cache->targets);
    free(cache->addr_entries);
    free(cache->srv_records);
    free(cache->srv_name);
    free(cache->tenant_name);
    memset(cache, 0, sizeof(*cache));
}

static int audit_dns_cache_add_addresses(
    audit_dns_cache_t *cache,
    const char *name,
    audit_addr_list_t *list
) {
    audit_addr_entry_t *entries = (audit_addr_entry_t *)realloc(
        cache->addr_entries,
        (cache->addr_entry_count + 1u) * sizeof(*entries)
    );
    if (!entries) {
        return -1;
    }
    cache->addr_entries = entries;
    audit_addr_entry_t *entry = &entries[cache->addr_entry_count];
    memset(entry, 0, sizeof(*entry));
    entry->name = audit_strdup(name);
    if (!entry->name) {
        return -1;
    }
    entry->items = list->items;
    entry->count = list->count;
    list->items = NULL;
    list->count = 0u;
    list->capacity = 0u;
    cache->addr_entry_count++;
    return 0;
}

static const audit_addr_entry_t *audit_dns_cache_find(
    const audit_dns_cache_t *cache,
    const char *name
) {
    for (size_t index = 0u; index < cache->addr_entry_count; index++) {
        if (strcmp(cache->addr_entries[index].name, name) == 0) {
            return &cache->addr_entries[index];
        }
    }
    return NULL;
}

static int audit_dns_cache_build_targets(audit_dns_cache_t *cache) {
    cache->targets = (audit_target_t *)calloc(cache->srv_count, sizeof(*cache->targets));
    if (!cache->targets) {
        return -1;
    }
    for (size_t index = 0u; index < cache->srv_count; index++) {
        const audit_addr_entry_t *entry = audit_dns_cache_find(cache, cache->srv_records[index].target);
        if (!entry || entry->count == 0u) {
            continue;
        }
        audit_target_t *target = &cache->targets[cache->target_count++];
        target->name = cache->srv_records[index].target;
        target->port = cache->srv_records[index].port;
        target->addresses = entry->items;
        target->address_count = entry->count;
    }
    return cache->target_count > 0u ? 0 : -1;
}

static int audit_dns_cache_init(
    audit_dns_cache_t *cache,
    const char *tenant_name
) {
    memset(cache, 0, sizeof(*cache));
    cache->tenant_name = audit_strdup(tenant_name);
    if (!cache->tenant_name) {
        return -1;
    }

    char srv_name[512];
    int written = snprintf(srv_name, sizeof(srv_name), "_ratelimitly._udp.%s", tenant_name);
    if (written < 0 || (size_t)written >= sizeof(srv_name)) {
        audit_dns_cache_reset(cache);
        return -1;
    }
    cache->srv_name = audit_strdup(srv_name);
    audit_resolver_t resolver;
    if (!cache->srv_name || audit_resolver_init(&resolver) != 0) {
        audit_dns_cache_reset(cache);
        return -1;
    }
    if (audit_query_srv(&resolver, cache->srv_name, &cache->srv_records, &cache->srv_count) != 0) {
        audit_resolver_close(&resolver);
        audit_dns_cache_reset(cache);
        return -1;
    }
    for (size_t index = 0u; index < cache->srv_count; index++) {
        audit_addr_list_t addresses;
        if (audit_resolve_addresses(&resolver, cache->srv_records[index].target, &addresses) != 0
            || audit_dns_cache_add_addresses(
                cache,
                cache->srv_records[index].target,
                &addresses
            ) != 0) {
            audit_addr_list_reset(&addresses);
            audit_resolver_close(&resolver);
            audit_dns_cache_reset(cache);
            return -1;
        }
    }
    audit_resolver_close(&resolver);
    if (audit_dns_cache_build_targets(cache) != 0) {
        audit_dns_cache_reset(cache);
        return -1;
    }
    return 0;
}

static int audit_resolve_srv(
    void *context,
    const char *name,
    r_dns_req_id_t *request_id,
    r_dns_srv_cb callback,
    void *user
) {
    (void)request_id;
    const audit_dns_cache_t *cache = (const audit_dns_cache_t *)context;
    if (!cache || !name || !callback) {
        return -1;
    }
    if (strcmp(name, cache->srv_name) != 0) {
        callback(user, -1, NULL, 0u);
    } else {
        callback(user, 0, cache->srv_records, cache->srv_count);
    }
    return 0;
}

static int audit_resolve_addrs(
    void *context,
    const char *name,
    r_dns_req_id_t *request_id,
    r_dns_addr_cb callback,
    void *user
) {
    (void)request_id;
    const audit_dns_cache_t *cache = (const audit_dns_cache_t *)context;
    if (!cache || !name || !callback) {
        return -1;
    }
    const audit_addr_entry_t *entry = audit_dns_cache_find(cache, name);
    if (!entry) {
        callback(user, -1, NULL, 0u);
    } else {
        callback(user, 0, entry->items, entry->count);
    }
    return 0;
}

static void audit_resolver_cancel(void *context, r_dns_req_id_t request_id) {
    (void)context;
    (void)request_id;
}

static int audit_open_socket(void) {
    int socket_fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        return -1;
    }
    int disabled = 0;
    (void)setsockopt(socket_fd, IPPROTO_IPV6, IPV6_V6ONLY, &disabled, sizeof(disabled));
    struct sockaddr_in6 local;
    memset(&local, 0, sizeof(local));
    local.sin6_family = AF_INET6;
    local.sin6_addr = in6addr_any;
    if (bind(socket_fd, (const struct sockaddr *)&local, sizeof(local)) != 0) {
        close(socket_fd);
        return -1;
    }
    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

static int audit_send_to_address(
    int socket_fd,
    const r_addr_t *address,
    uint16_t port,
    const uint8_t *data,
    size_t length
) {
    const struct sockaddr *socket_address = (const struct sockaddr *)&address->sa;
    socklen_t socket_length = address->len;
    struct sockaddr_in ipv4;
    struct sockaddr_in6 ipv6;
    if (socket_address->sa_family == AF_INET) {
        memcpy(&ipv4, socket_address, sizeof(ipv4));
        ipv4.sin_port = htons(port);
        memset(&ipv6, 0, sizeof(ipv6));
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = ipv4.sin_port;
        ipv6.sin6_addr.s6_addr[10] = 0xffu;
        ipv6.sin6_addr.s6_addr[11] = 0xffu;
        memcpy(&ipv6.sin6_addr.s6_addr[12], &ipv4.sin_addr, 4u);
        socket_address = (const struct sockaddr *)&ipv6;
        socket_length = sizeof(ipv6);
    } else if (socket_address->sa_family == AF_INET6) {
        memcpy(&ipv6, socket_address, sizeof(ipv6));
        ipv6.sin6_port = htons(port);
        socket_address = (const struct sockaddr *)&ipv6;
        socket_length = sizeof(ipv6);
    } else {
        return -1;
    }
    ssize_t sent = sendto(socket_fd, data, length, 0, socket_address, socket_length);
    return sent >= 0 && (size_t)sent == length ? 0 : -1;
}

static int audit_udp_send(void *context, const r_addr_t *to, const uint8_t *data, size_t length) {
    audit_io_t *io = (audit_io_t *)context;
    if (!io || io->socket_fd < 0) {
        return -1;
    }
    const struct sockaddr *socket_address = (const struct sockaddr *)&to->sa;
    uint16_t port = 0u;
    if (socket_address->sa_family == AF_INET) {
        port = ntohs(((const struct sockaddr_in *)socket_address)->sin_port);
    } else if (socket_address->sa_family == AF_INET6) {
        port = ntohs(((const struct sockaddr_in6 *)socket_address)->sin6_port);
    }
    return audit_send_to_address(io->socket_fd, to, port, data, length);
}

static void audit_log(void *context, r_log_level_t level, const char *message) {
    (void)context;
    if (level == R_LOG_ERROR || level == R_LOG_WARN) {
        audit_print_timestamp(stderr);
        fprintf(stderr, " Client{%s} %s\n",
            level == R_LOG_ERROR ? "error" : "warning",
            message ? message : "(no message)");
    }
}

static void audit_request_callback(
    void *user,
    r_client_req_t *request,
    int status,
    const r_rate_limit_result_t *result
) {
    (void)request;
    audit_request_result_t *output = (audit_request_result_t *)user;
    output->status = status;
    if (status == RCLIENT_OK && result) {
        output->accepted = result->success;
        output->server_id = result->server_id;
        if (result->guard_count > 0u) {
            output->current_latency_ms = result->guards[0].current_latency_ms;
        }
        if (result->resource_count > 0u) {
            output->actual_rate = result->resources[0].actual_rate;
        }
    }
    output->done = true;
}

static int audit_wait_for_request(
    audit_io_t *io,
    r_client_t *client,
    r_client_req_t *request,
    audit_request_result_t *result
) {
    uint8_t packet[AUDIT_PACKET_CAP];
    while (!result->done) {
        uint64_t deadline_ms = 0u;
        int status = r_client_request_deadline_ms(request, &deadline_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        uint64_t now_ms = audit_wall_time_ms(NULL);
        uint64_t difference = deadline_ms > now_ms ? deadline_ms - now_ms : 0u;
        int timeout_ms = difference > INT_MAX ? INT_MAX : (int)difference;
        struct pollfd poll_fd = {.fd = io->socket_fd, .events = POLLIN};
        int poll_status = poll(&poll_fd, 1u, timeout_ms);
        if (poll_status < 0 && errno != EINTR) {
            return RCLIENT_ERR_IO;
        }
        if (poll_status > 0 && (poll_fd.revents & POLLIN) != 0) {
            for (;;) {
                struct sockaddr_storage source;
                socklen_t source_length = sizeof(source);
                ssize_t length = recvfrom(
                    io->socket_fd,
                    packet,
                    sizeof(packet),
                    0,
                    (struct sockaddr *)&source,
                    &source_length
                );
                if (length < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    return RCLIENT_ERR_IO;
                }
                r_addr_t from;
                memset(&from, 0, sizeof(from));
                memcpy(&from.sa, &source, source_length);
                from.len = source_length;
                (void)r_client_on_datagram(client, packet, (size_t)length, &from);
                if (result->done) {
                    break;
                }
            }
        }
        now_ms = audit_wall_time_ms(NULL);
        if (!result->done && deadline_ms <= now_ms) {
            (void)r_client_on_timeout(client, request, now_ms);
        }
    }
    return result->status;
}

static int audit_one_request(
    audit_io_t *io,
    r_client_t *client,
    const r_resource_request_t *resource,
    const r_latency_guard_t *guard,
    audit_request_result_t *result
) {
    memset(result, 0, sizeof(*result));
    r_client_req_t *request = NULL;
    int status = r_client_check_rate_limit_async_borrowed(
        client,
        resource,
        resource ? 1u : 0u,
        guard,
        guard ? 1u : 0u,
        NULL,
        0u,
        audit_request_callback,
        result,
        &request
    );
    if (status != RCLIENT_OK) {
        result->status = status;
        result->done = true;
        return status;
    }
    if (!request) {
        return result->status;
    }
    status = audit_wait_for_request(io, client, request, result);
    if (status != RCLIENT_OK && !result->done) {
        r_client_cancel_request(client, request);
        result->status = status;
        result->done = true;
    }
    return result->status;
}

static audit_phase_stats_t audit_run_requests(
    audit_io_t *io,
    r_client_t *client,
    const r_resource_request_t *resource,
    const r_latency_guard_t *guard,
    uint32_t count
) {
    audit_phase_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    char resource_id[17] = "";
    char guard_id[17] = "";
    if (resource) {
        audit_format_short_id(resource->bucket_id, resource_id);
    }
    if (guard) {
        audit_format_short_id(guard->latency_tracker_id, guard_id);
    }
    uint64_t started_ms = audit_monotonic_time_ms();
    for (uint32_t index = 0u; index < count; index++) {
        audit_request_result_t result;
        uint64_t request_started_ns = audit_monotonic_time_ns();
        int status = audit_one_request(io, client, resource, guard, &result);
        uint64_t request_ended_ns = audit_monotonic_time_ns();
        uint64_t duration_ns = request_ended_ns >= request_started_ns
            ? request_ended_ns - request_started_ns : 0u;
        double duration_ms = (double)duration_ns / 1000000.0;
        audit_print_timestamp(stdout);
        if (guard) {
            printf(" Request{Guard:{%s:<%ums},Tokens:{}}",
                guard_id,
                guard->threshold_ms);
        } else {
            printf(" Request{Guard:{},Tokens:{%s:%u}}",
                resource_id,
                resource->tokens_requested);
        }
        if (status != RCLIENT_OK) {
            stats.errors++;
            printf(" Error{%s:%d} - %.3f ms\n",
                audit_status_name(status),
                status,
                duration_ms);
        } else if (result.accepted) {
            stats.accepted++;
            printf(" Granted - %.3f ms\n", duration_ms);
        } else {
            stats.rejected++;
            printf(" Rejected - %.3f ms\n", duration_ms);
        }
        fflush(stdout);
    }
    uint64_t ended_ms = audit_monotonic_time_ms();
    stats.elapsed_ms = ended_ms >= started_ms ? ended_ms - started_ms : 0u;
    return stats;
}

static int audit_build_metrics_packet(
    uint64_t tenant_id,
    const uint8_t management_key[32],
    uint8_t packet[AUDIT_PACKET_CAP],
    size_t *packet_length,
    uint8_t request_id[16]
) {
    uint8_t pdu[AUDIT_PACKET_CAP];
    const size_t pdu_length = 8u;
    audit_write_le16(pdu, AUDIT_PDU_METRICS_QUERY);
    pdu[4] = AUDIT_METRICS_FAMILY_SUMMARY;
    pdu[5] = 0u;
    pdu[6] = 0u;
    pdu[7] = 0u;
    audit_write_le16(pdu + 2u, (uint16_t)pdu_length);

    if (RAND_bytes(request_id, 16) != 1) {
        return -1;
    }
    r_tenant_header_t tenant;
    memset(&tenant, 0, sizeof(tenant));
    tenant.tlv_type = R_TLV_TENANT;
    tenant.tlv_size = R_TENANT_TLV_LEN;
    tenant.key_id = tenant_id;
    memcpy(tenant.unique_id, request_id, 16u);
    tenant.time_stamp = audit_wall_time_ms(NULL);
    tenant.steering_feedback = 1u;
    tenant.tenant_mgmt_flag = 1u;
    r_tenant_header_write(&tenant, packet, AUDIT_PACKET_CAP);
    size_t position = R_TENANT_TLV_LEN;
    audit_write_le16(packet + position, R_TLV_AUTH_AES);
    audit_write_le16(packet + position + 2u, 32u);

    uint8_t ciphertext[AUDIT_PACKET_CAP];
    size_t ciphertext_length = 0u;
    uint8_t nonce[12];
    uint8_t tag[16];
    if (r_encrypt_pdu_aes_gcm(
            pdu,
            pdu_length,
            management_key,
            packet,
            position + 4u,
            ciphertext,
            sizeof(ciphertext),
            &ciphertext_length,
            nonce,
            tag
        ) != 0) {
        return -1;
    }
    position += 4u;
    memcpy(packet + position, nonce, sizeof(nonce));
    position += sizeof(nonce);
    memcpy(packet + position, tag, sizeof(tag));
    position += sizeof(tag);
    if (position + ciphertext_length > AUDIT_PACKET_CAP) {
        return -1;
    }
    memcpy(packet + position, ciphertext, ciphertext_length);
    position += ciphertext_length;
    *packet_length = position;
    return 0;
}

static int audit_parse_metrics_response(
    const uint8_t *packet,
    size_t packet_length,
    const uint8_t management_key[32],
    const uint8_t expected_request_id[16],
    audit_metrics_response_t *response
) {
    r_tenant_header_t tenant;
    size_t tenant_end = 0u;
    if (r_parse_tenant_header(packet, packet_length, &tenant, &tenant_end) != RCLIENT_OK
        || tenant.tenant_mgmt_flag != 1u
        || CRYPTO_memcmp(tenant.unique_id, expected_request_id, 16u) != 0) {
        return 1;
    }
    uint16_t auth_type = 0u;
    size_t auth_size = 0u;
    const uint8_t *auth_body = NULL;
    size_t auth_body_length = 0u;
    size_t pdu_position = 0u;
    if (r_parse_auth_tlv_header(
            packet,
            packet_length,
            tenant_end,
            &auth_type,
            &auth_size,
            &auth_body,
            &auth_body_length,
            &pdu_position
        ) != RCLIENT_OK
        || auth_type != R_TLV_AUTH_AES || auth_size != 32u || auth_body_length != 28u) {
        return -1;
    }
    uint8_t pdu[AUDIT_PACKET_CAP];
    size_t pdu_length = 0u;
    if (r_decrypt_pdu_aes_gcm(
            packet + pdu_position,
            packet_length - pdu_position,
            management_key,
            auth_body,
            auth_body + 12u,
            packet,
            tenant_end + 4u + 12u,
            pdu,
            sizeof(pdu),
            &pdu_length
        ) != 0
        || pdu_length < 8u
        || audit_read_le16(pdu) != AUDIT_PDU_METRICS_RESPONSE
        || audit_read_le16(pdu + 2u) != pdu_length
        || pdu[6] != 0u || pdu[7] != 0u) {
        return -1;
    }
    memset(response, 0, sizeof(*response));
    response->server_id = tenant.key_id;
    response->family = pdu[4];
    response->page_kind = pdu[5];
    response->body_len = pdu_length - 8u;
    memcpy(response->body, pdu + 8u, response->body_len);
    return 0;
}

static int audit_query_summary_response(
    audit_io_t *io,
    const audit_target_t *target,
    uint64_t tenant_id,
    const uint8_t management_key[32],
    uint32_t timeout_ms,
    uint32_t attempts,
    audit_metrics_response_t *response
) {
    uint8_t incoming[AUDIT_PACKET_CAP];
    for (uint32_t attempt = 0u; attempt < attempts; attempt++) {
        for (size_t address_index = 0u; address_index < target->address_count; address_index++) {
            uint8_t packet[AUDIT_PACKET_CAP];
            size_t packet_length = 0u;
            uint8_t request_id[16];
            if (audit_build_metrics_packet(
                    tenant_id,
                    management_key,
                    packet,
                    &packet_length,
                    request_id
                ) != 0
                || audit_send_to_address(
                    io->socket_fd,
                    &target->addresses[address_index],
                    target->port,
                    packet,
                    packet_length
                ) != 0) {
                continue;
            }

            uint64_t started_ms = audit_monotonic_time_ms();
            uint64_t deadline_ms = started_ms + timeout_ms;
            for (;;) {
                uint64_t now_ms = audit_monotonic_time_ms();
                if (now_ms >= deadline_ms) {
                    break;
                }
                uint64_t remaining = deadline_ms - now_ms;
                struct pollfd poll_fd = {.fd = io->socket_fd, .events = POLLIN};
                int poll_status = poll(&poll_fd, 1u, remaining > INT_MAX ? INT_MAX : (int)remaining);
                if (poll_status < 0) {
                    if (errno == EINTR) continue;
                    return -1;
                }
                if (poll_status == 0) {
                    break;
                }
                for (;;) {
                    ssize_t length = recvfrom(io->socket_fd, incoming, sizeof(incoming), 0, NULL, NULL);
                    if (length < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        return -1;
                    }
                    int parsed = audit_parse_metrics_response(
                        incoming,
                        (size_t)length,
                        management_key,
                        request_id,
                        response
                    );
                    if (parsed == 0) {
                        return response->family == AUDIT_METRICS_FAMILY_SUMMARY ? 0 : -1;
                    }
                    if (parsed < 0) {
                        return -1;
                    }
                }
            }
        }
    }
    return -1;
}

static int audit_query_summary(
    audit_io_t *io,
    const audit_target_t *target,
    uint64_t tenant_id,
    const uint8_t management_key[32],
    const audit_config_t *config,
    uint64_t *server_id,
    audit_summary_metrics_t *summary
) {
    audit_metrics_response_t response;
    if (audit_query_summary_response(
            io,
            target,
            tenant_id,
            management_key,
            config->metrics_timeout_ms,
            config->metrics_attempts,
            &response
        ) != 0
        || response.page_kind != 0u || response.body_len != 40u) {
        return -1;
    }
    *server_id = response.server_id;
    summary->requests_success = audit_read_le64(response.body);
    summary->requests_rate_limited = audit_read_le64(response.body + 8u);
    summary->requests_guard_failed = audit_read_le64(response.body + 16u);
    summary->requests_auth_failed = audit_read_le64(response.body + 24u);
    summary->service_latency_reports_total = audit_read_le64(response.body + 32u);
    return 0;
}

static int audit_collect_metrics(
    audit_io_t *io,
    const audit_dns_cache_t *dns,
    uint64_t tenant_id,
    const uint8_t management_key[32],
    const audit_config_t *config,
    audit_metrics_snapshot_t *snapshots
) {
    int failures = 0;
    for (size_t index = 0u; index < dns->target_count; index++) {
        audit_metrics_snapshot_t snapshot;
        memset(&snapshot, 0, sizeof(snapshot));
        if (audit_query_summary(
                io,
                &dns->targets[index],
                tenant_id,
                management_key,
                config,
                &snapshot.server_id,
                &snapshot.summary
            ) != 0) {
            audit_print_timestamp(stderr);
            fprintf(stderr, " Tenant metrics retrieval failed for %s:%u\n",
                dns->targets[index].name, dns->targets[index].port);
            failures++;
        } else {
            snapshot.valid = true;
        }
        snapshots[index] = snapshot;
    }
    return failures == 0 ? 0 : -1;
}

static uint64_t audit_counter_delta(uint64_t current, uint64_t previous) {
    return current >= previous ? current - previous : 0u;
}

static void audit_print_metrics(
    const char *phase,
    const audit_dns_cache_t *dns,
    const audit_metrics_snapshot_t *previous,
    const audit_metrics_snapshot_t *current
) {
    audit_print_timestamp(stdout);
    printf(" Tenant metrics snapshot after %s\n", phase);
    for (size_t index = 0u; index < dns->target_count; index++) {
        const audit_metrics_snapshot_t *now = &current[index];
        const audit_metrics_snapshot_t *before = &previous[index];
        if (!now->valid) {
            audit_print_timestamp(stdout);
            printf(" Metrics{Endpoint:%s:%u} Unavailable\n",
                dns->targets[index].name, dns->targets[index].port);
            continue;
        }
        bool comparable = before->valid && before->server_id == now->server_id;
        audit_print_timestamp(stdout);
        printf(" Metrics{Endpoint:%s:%u,Server:%llu} Retrieved\n",
            dns->targets[index].name,
            dns->targets[index].port,
            (unsigned long long)now->server_id);
        audit_print_timestamp(stdout);
        printf(" TenantMetrics{Success:%llu,RateLimited:%llu,GuardFailed:%llu,AuthFailed:%llu,LatencyReports:%llu} Absolute\n",
            (unsigned long long)now->summary.requests_success,
            (unsigned long long)now->summary.requests_rate_limited,
            (unsigned long long)now->summary.requests_guard_failed,
            (unsigned long long)now->summary.requests_auth_failed,
            (unsigned long long)now->summary.service_latency_reports_total);
        if (comparable) {
            audit_print_timestamp(stdout);
            printf(" TenantMetrics{Success:%llu,RateLimited:%llu,GuardFailed:%llu,AuthFailed:%llu,LatencyReports:%llu} Delta\n",
                (unsigned long long)audit_counter_delta(now->summary.requests_success, before->summary.requests_success),
                (unsigned long long)audit_counter_delta(now->summary.requests_rate_limited, before->summary.requests_rate_limited),
                (unsigned long long)audit_counter_delta(now->summary.requests_guard_failed, before->summary.requests_guard_failed),
                (unsigned long long)audit_counter_delta(now->summary.requests_auth_failed, before->summary.requests_auth_failed),
                (unsigned long long)audit_counter_delta(now->summary.service_latency_reports_total, before->summary.service_latency_reports_total));
        }
    }
}

static int audit_snapshot_after_phase(
    const char *phase,
    audit_io_t *io,
    const audit_dns_cache_t *dns,
    uint64_t tenant_id,
    const uint8_t management_key[32],
    const audit_config_t *config,
    audit_metrics_snapshot_t *previous,
    audit_metrics_snapshot_t *current
) {
    if (!config->management_key) {
        audit_print_timestamp(stdout);
        printf(" Tenant metrics snapshot after %s skipped: no --management-key\n", phase);
        return 0;
    }
    int status = audit_collect_metrics(
        io,
        dns,
        tenant_id,
        management_key,
        config,
        current
    );
    audit_print_metrics(phase, dns, previous, current);
    memcpy(previous, current, dns->target_count * sizeof(*previous));
    return status;
}

static bool audit_make_run_id(char output[AUDIT_RUN_ID_CAP + 1u]) {
    uint8_t random[8];
    if (RAND_bytes(random, sizeof(random)) != 1) {
        return false;
    }
    for (size_t index = 0u; index < sizeof(random); index++) {
        (void)snprintf(output + index * 2u, 3u, "%02x", random[index]);
    }
    return true;
}

int main(int argc, char **argv) {
    (void)setvbuf(stdout, NULL, _IOLBF, 0);

    audit_config_t config;
    int parse_status = audit_parse_config(argc, argv, &config);
    if (parse_status != 0) {
        return parse_status;
    }

    r_auth_key_info_t auth_info;
    memset(&auth_info, 0, sizeof(auth_info));
    if (r_client_parse_auth_key(config.auth_key, &auth_info) != RCLIENT_OK) {
        fprintf(stderr, "Invalid --auth value (expected rl-cookie... or rl-aes...)\n");
        return 2;
    }
    uint8_t management_key[32];
    memset(management_key, 0, sizeof(management_key));
    if (config.management_key
        && r_decode_management_key_bech32(config.management_key, management_key) != 0) {
        fprintf(stderr, "Invalid --management-key value (expected a 32-byte rl-secret...)\n");
        OPENSSL_cleanse(&auth_info, sizeof(auth_info));
        return 2;
    }

    uint64_t policy_horizon_ms = 0u;
    if (!audit_policy_horizon_ms(&config.policy, &policy_horizon_ms)
        || policy_horizon_ms > auth_info.dedup_ttl_ms_max) {
        fprintf(stderr,
            "Invalid HA policy: derived deduplication TTL %llu ms exceeds credential limit %u ms\n",
            (unsigned long long)policy_horizon_ms,
            auth_info.dedup_ttl_ms_max);
        OPENSSL_cleanse(management_key, sizeof(management_key));
        OPENSSL_cleanse(&auth_info, sizeof(auth_info));
        return 2;
    }
    if (auth_info.rate_buckets_max < 1u || auth_info.latency_services_max < 1u
        || auth_info.latency_buffer_size_max < AUDIT_TRACKER_BUFFER_SIZE) {
        fprintf(stderr,
            "Credential quotas cannot run the audit (need one bucket, one latency tracker, and buffer size 10)\n");
        OPENSSL_cleanse(management_key, sizeof(management_key));
        OPENSSL_cleanse(&auth_info, sizeof(auth_info));
        return 2;
    }

    char default_domain[R_CLIENT_DEFAULT_TENANT_DNS_CAPACITY];
    if (r_client_format_default_tenant_dns(
            auth_info.key_id,
            default_domain,
            sizeof(default_domain)
        ) != RCLIENT_OK) {
        fprintf(stderr, "Failed to derive tenant DNS domain from --auth\n");
        goto fatal_before_dns;
    }
    const char *tenant_domain = default_domain;

    audit_dns_cache_t dns;
    if (audit_dns_cache_init(&dns, tenant_domain) != 0) {
        fprintf(stderr, "Failed to resolve RateLimitly endpoints for %s\n", tenant_domain);
        fprintf(stderr, "Set RCLIENT_DNS_SERVER=IPv4[:port] when using a local DNS fixture.\n");
        goto fatal_before_dns;
    }

    audit_io_t io = {.socket_fd = audit_open_socket()};
    if (io.socket_fd < 0) {
        fprintf(stderr, "Failed to open the UDP audit socket\n");
        audit_dns_cache_reset(&dns);
        goto fatal_before_dns;
    }

    r_io_ops_t io_ops;
    memset(&io_ops, 0, sizeof(io_ops));
    io_ops.ctx = &io;
    io_ops.udp_send = audit_udp_send;
    io_ops.now_ms = audit_wall_time_ms;
    io_ops.log = audit_log;

    r_resolver_ops_t resolver_ops;
    memset(&resolver_ops, 0, sizeof(resolver_ops));
    resolver_ops.ctx = &dns;
    resolver_ops.resolve_srv = audit_resolve_srv;
    resolver_ops.resolve_addrs = audit_resolve_addrs;
    resolver_ops.cancel = audit_resolver_cancel;

    r_client_config_t client_config;
    memset(&client_config, 0, sizeof(client_config));
    client_config.tenant.dns_name = tenant_domain;
    client_config.tenant.key_id = auth_info.key_id;
    client_config.tenant.auth.type = auth_info.type;
    client_config.tenant.auth.secret = config.auth_key;
    client_config.request_policy = &config.policy;
    client_config.dns_refresh.refresh_interval_ms = 3600000u;

    r_client_t *client = NULL;
    int status = r_client_create(&client_config, &io_ops, &resolver_ops, &client);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "Failed to initialize the RateLimitly client: %s (%d)\n",
            audit_status_name(status), status);
        close(io.socket_fd);
        audit_dns_cache_reset(&dns);
        goto fatal_before_dns;
    }

    char generated_run_id[AUDIT_RUN_ID_CAP + 1u];
    memset(generated_run_id, 0, sizeof(generated_run_id));
    if (!config.run_id && !audit_make_run_id(generated_run_id)) {
        fprintf(stderr, "Failed to generate an isolated audit run ID\n");
        r_client_destroy(client);
        close(io.socket_fd);
        audit_dns_cache_reset(&dns);
        goto fatal_before_dns;
    }
    const char *run_id = config.run_id ? config.run_id : generated_run_id;
    char bucket_name[AUDIT_NAME_CAP];
    char tracker_name[AUDIT_NAME_CAP];
    int bucket_written = snprintf(bucket_name, sizeof(bucket_name), "bucket/%s", run_id);
    int tracker_written = snprintf(tracker_name, sizeof(tracker_name), "latency/%s", run_id);
    if (bucket_written < 0 || (size_t)bucket_written >= sizeof(bucket_name)
        || tracker_written < 0 || (size_t)tracker_written >= sizeof(tracker_name)) {
        fprintf(stderr, "Audit run ID is too long\n");
        r_client_destroy(client);
        close(io.socket_fd);
        audit_dns_cache_reset(&dns);
        goto fatal_before_dns;
    }

    r_resource_request_t resource;
    memset(&resource, 0, sizeof(resource));
    resource.window_size_ms = AUDIT_RATE_WINDOW_MS;
    resource.rate_limit = AUDIT_RATE_LIMIT;
    resource.tokens_requested = 1u;
    r_latency_guard_t guard;
    memset(&guard, 0, sizeof(guard));
    guard.threshold_ms = AUDIT_GUARD_THRESHOLD_MS;
    guard.ttl_ms = AUDIT_TRACKER_TTL_MS;
    guard.max_samples = AUDIT_TRACKER_MAX_SAMPLES;
    guard.buffer_size = AUDIT_TRACKER_BUFFER_SIZE;
    guard.min_sample_threshold = AUDIT_TRACKER_MIN_SAMPLES;
    if (r_client_derive_bucket_id(
            bucket_name,
            strlen(bucket_name),
            resource.window_size_ms,
            resource.rate_limit,
            resource.bucket_id
        ) != RCLIENT_OK
        || r_client_derive_latency_tracker_id(
            tracker_name,
            strlen(tracker_name),
            guard.ttl_ms,
            guard.max_samples,
            guard.buffer_size,
            guard.min_sample_threshold,
            guard.latency_tracker_id
        ) != RCLIENT_OK) {
        fprintf(stderr, "Failed to derive audit state identifiers\n");
        r_client_destroy(client);
        close(io.socket_fd);
        audit_dns_cache_reset(&dns);
        goto fatal_before_dns;
    }

    printf("RateLimitly server audit\n");
    printf("========================\n");
    printf("tenant_id=%llu domain=%s servers=%zu run_id=%s\n",
        (unsigned long long)auth_info.key_id,
        tenant_domain,
        dns.target_count,
        run_id);
    printf("logical names: bucket=%s latency=%s\n", bucket_name, tracker_name);
    printf("HA policy: unit_ms=%llu replay_count=%u schedule=%s initial=%u max=%u growth=%u final_receive_units=%u completion_delivery=%s dedup_ttl_ms=%llu\n",
        (unsigned long long)config.policy.unit_ms,
        config.policy.replay_count,
        audit_schedule_name(config.policy.replay_gap.kind),
        config.policy.replay_gap.initial_units,
        config.policy.replay_gap.max_units,
        audit_schedule_growth(&config.policy),
        config.policy.final_receive_units,
        config.policy.completion_delivery ? "true" : "false",
        (unsigned long long)policy_horizon_ms);
    printf("tenant_metrics=%s\n\n", config.management_key ? "enabled" : "disabled");

    audit_metrics_snapshot_t *previous = (audit_metrics_snapshot_t *)calloc(
        dns.target_count,
        sizeof(*previous)
    );
    audit_metrics_snapshot_t *current = (audit_metrics_snapshot_t *)calloc(
        dns.target_count,
        sizeof(*current)
    );
    if (!previous || !current) {
        fprintf(stderr, "Out of memory allocating metrics snapshots\n");
        free(previous);
        free(current);
        r_client_destroy(client);
        close(io.socket_fd);
        audit_dns_cache_reset(&dns);
        goto fatal_before_dns;
    }

    unsigned int operational_errors = 0u;
    if (config.management_key && audit_collect_metrics(
            &io,
            &dns,
            auth_info.key_id,
            management_key,
            &config,
            previous
        ) != 0) {
        audit_print_timestamp(stderr);
        fprintf(stderr, " Initial metrics snapshot failed\n");
        operational_errors++;
    }

    char bucket_short_id[17];
    char tracker_short_id[17];
    audit_format_short_id(resource.bucket_id, bucket_short_id);
    audit_format_short_id(guard.latency_tracker_id, tracker_short_id);
    audit_phase_stats_t phases[4];
    memset(phases, 0, sizeof(phases));

    printf("Test 1 - Rate bucket saturation\n");
    printf("Send 20 sequential one-token requests to bucket %s, configured for 10 tokens per 10 seconds.\n", bucket_short_id);
    printf("The log records the selected result and complete logical-request duration.\n");
    phases[0] = audit_run_requests(
        &io,
        client,
        &resource,
        NULL,
        AUDIT_RATE_REQUEST_COUNT
    );
    operational_errors += phases[0].errors;
    if (audit_snapshot_after_phase(
            "bucket saturation",
            &io,
            &dns,
            auth_info.key_id,
            management_key,
            &config,
            previous,
            current
        ) != 0) operational_errors++;
    putchar('\n');

    printf("Test 2 - Latency tracker saturation\n");
    printf("Send 10 sequential guard-only requests to tracker %s with threshold 1000 ms, TTL 10 seconds, max_samples 10, buffer_size 10, and min_sample_threshold 5.\n", tracker_short_id);
    printf("Granted guards contribute the server's speculative latency sample.\n");
    phases[1] = audit_run_requests(&io, client, NULL, &guard, 10u);
    operational_errors += phases[1].errors;
    if (audit_snapshot_after_phase(
            "latency tracker saturation",
            &io,
            &dns,
            auth_info.key_id,
            management_key,
            &config,
            previous,
            current
        ) != 0) operational_errors++;
    putchar('\n');

    r_service_latency_report_t report;
    memset(&report, 0, sizeof(report));
    memcpy(report.latency_tracker_id, guard.latency_tracker_id, 16u);
    report.observed_latency = AUDIT_REPORTED_LATENCY_MS;
    report.ttl_ms = guard.ttl_ms;
    report.max_samples = guard.max_samples;
    report.buffer_size = guard.buffer_size;
    report.min_sample_threshold = guard.min_sample_threshold;
    printf("Test 3 - Latency report followed by guard requests\n");
    printf("Report a 100 ms latency sample to tracker %s, then send 20 sequential guard-only requests.\n", tracker_short_id);
    uint64_t report_started_ns = audit_monotonic_time_ns();
    status = r_client_report_latency(client, &report, 1u);
    uint64_t report_ended_ns = audit_monotonic_time_ns();
    double report_duration_ms = (double)(report_ended_ns - report_started_ns) / 1000000.0;
    audit_print_timestamp(status == RCLIENT_OK ? stdout : stderr);
    if (status != RCLIENT_OK) {
        fprintf(stderr, " LatencyReport{Tracker:%s,Observed:100ms} Error{%s:%d} - %.3f ms\n",
            tracker_short_id, audit_status_name(status), status, report_duration_ms);
        operational_errors++;
    } else {
        printf(" LatencyReport{Tracker:%s,Observed:100ms} Sent - %.3f ms\n",
            tracker_short_id, report_duration_ms);
    }
    audit_sleep_ms(config.policy.unit_ms);
    phases[2] = audit_run_requests(&io, client, NULL, &guard, 20u);
    operational_errors += phases[2].errors;
    if (audit_snapshot_after_phase(
            "report and twenty guards",
            &io,
            &dns,
            auth_info.key_id,
            management_key,
            &config,
            previous,
            current
        ) != 0) operational_errors++;
    putchar('\n');

    printf("Test 4 - Latency tracker expiry\n");
    printf("Report another 100 ms latency sample, wait for the 10-second tracker TTL, then send 10 sequential guard-only requests.\n");
    report_started_ns = audit_monotonic_time_ns();
    status = r_client_report_latency(client, &report, 1u);
    report_ended_ns = audit_monotonic_time_ns();
    report_duration_ms = (double)(report_ended_ns - report_started_ns) / 1000000.0;
    audit_print_timestamp(status == RCLIENT_OK ? stdout : stderr);
    if (status != RCLIENT_OK) {
        fprintf(stderr, " LatencyReport{Tracker:%s,Observed:100ms} Error{%s:%d} - %.3f ms\n",
            tracker_short_id, audit_status_name(status), status, report_duration_ms);
        operational_errors++;
    } else {
        printf(" LatencyReport{Tracker:%s,Observed:100ms} Sent - %.3f ms\n",
            tracker_short_id, report_duration_ms);
    }
    audit_print_timestamp(stdout);
    printf(" Waiting %u ms for tracker %s to expire\n",
        AUDIT_TRACKER_TTL_MS, tracker_short_id);
    fflush(stdout);
    audit_sleep_ms((uint64_t)AUDIT_TRACKER_TTL_MS + 1u);
    audit_print_timestamp(stdout);
    printf(" Tracker wait completed; sending guard requests\n");
    phases[3] = audit_run_requests(&io, client, NULL, &guard, 10u);
    operational_errors += phases[3].errors;
    if (audit_snapshot_after_phase(
            "expired report and ten guards",
            &io,
            &dns,
            auth_info.key_id,
            management_key,
            &config,
            previous,
            current
        ) != 0) operational_errors++;

    printf("\nConclusion\n");
    printf("1. Rate bucket saturation: granted=%u rejected=%u errors=%u duration=%llu ms\n",
        phases[0].accepted, phases[0].rejected, phases[0].errors,
        (unsigned long long)phases[0].elapsed_ms);
    printf("2. Latency tracker saturation: granted=%u rejected=%u errors=%u duration=%llu ms\n",
        phases[1].accepted, phases[1].rejected, phases[1].errors,
        (unsigned long long)phases[1].elapsed_ms);
    printf("3. Report followed by guard requests: granted=%u rejected=%u errors=%u duration=%llu ms\n",
        phases[2].accepted, phases[2].rejected, phases[2].errors,
        (unsigned long long)phases[2].elapsed_ms);
    printf("4. Tracker expiry: granted=%u rejected=%u errors=%u request_duration=%llu ms wait=%u ms\n",
        phases[3].accepted, phases[3].rejected, phases[3].errors,
        (unsigned long long)phases[3].elapsed_ms,
        AUDIT_TRACKER_TTL_MS);
    printf("Operational errors: %u\n", operational_errors);

    free(previous);
    free(current);
    r_client_destroy(client);
    close(io.socket_fd);
    audit_dns_cache_reset(&dns);
    OPENSSL_cleanse(management_key, sizeof(management_key));
    OPENSSL_cleanse(&auth_info, sizeof(auth_info));
    return operational_errors == 0u ? 0 : 1;

fatal_before_dns:
    OPENSSL_cleanse(management_key, sizeof(management_key));
    OPENSSL_cleanse(&auth_info, sizeof(auth_info));
    return 1;
}
