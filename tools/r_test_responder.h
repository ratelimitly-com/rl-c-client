#ifndef R_TEST_RESPONDER_H
#define R_TEST_RESPONDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../include/r_client.h"

#define R_TEST_RESPONDER_LABEL_CAP 256u
#define R_TEST_RESPONDER_QUOTA_CAP 256u

extern const char R_TEST_RESPONDER_AES_KEY[];
extern const char R_TEST_RESPONDER_COOKIE_KEY[];

typedef enum r_test_scenario {
    R_TEST_SCENARIO_ALLOW = 0,
    R_TEST_SCENARIO_DENY,
    R_TEST_SCENARIO_GUARD_PASS,
    R_TEST_SCENARIO_GUARD_DENY,
    R_TEST_SCENARIO_QUOTA,
    R_TEST_SCENARIO_DROP,
    R_TEST_SCENARIO_MALFORMED_AUTH,
    R_TEST_SCENARIO_MALFORMED_TRUNCATED,
    R_TEST_SCENARIO_MALFORMED_REQUEST_ID,
    R_TEST_SCENARIO_COUNT_EMPTY,
    R_TEST_SCENARIO_COUNT_SHORT,
    R_TEST_SCENARIO_COUNT_EXTRA,
} r_test_scenario_t;

typedef enum r_test_event_kind {
    R_TEST_EVENT_INPUT_REJECTED = 0,
    R_TEST_EVENT_RATE_REQUEST,
    R_TEST_EVENT_LATENCY_REPORT,
} r_test_event_kind_t;

typedef struct r_test_event {
    r_test_event_kind_t kind;
    uint64_t sequence;
    size_t guard_count;
    size_t resource_count;
    size_t report_count;
    char label[R_TEST_RESPONDER_LABEL_CAP + 1u];
    const char *disposition;
} r_test_event_t;

typedef struct r_test_quota_entry {
    uint8_t bucket_id[16];
    uint64_t requests;
} r_test_quota_entry_t;

typedef struct r_test_responder_state {
    r_test_scenario_t scenario;
    r_auth_type_t auth_type;
    uint64_t tenant_key_id;
    uint8_t secret[32];
    uint64_t server_id;
    bool steering_keep_port;
    uint64_t allow_count;
    uint64_t sequence;
    r_test_quota_entry_t quota[R_TEST_RESPONDER_QUOTA_CAP];
    size_t quota_count;
} r_test_responder_state_t;

const char *r_test_scenario_name(r_test_scenario_t scenario);
int r_test_scenario_parse(const char *name, r_test_scenario_t *out);

int r_test_responder_init(
    r_test_responder_state_t *state,
    r_test_scenario_t scenario,
    const char *auth_key,
    uint64_t server_id,
    bool steering_keep_port,
    uint64_t allow_count
);

int r_test_responder_process(
    r_test_responder_state_t *state,
    const uint8_t *input,
    size_t input_len,
    uint8_t *output,
    size_t output_cap,
    size_t *output_len,
    bool *send_response,
    r_test_event_t *event
);

#endif
