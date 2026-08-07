#include "../include/r_client.h"

#include <string.h>

void r_client_default_request_policy(r_request_policy_t *out_policy) {
    if (!out_policy) {
        return;
    }
    memset(out_policy, 0, sizeof(*out_policy));
    out_policy->unit_ms = 20u;
    out_policy->replay_count = 1u;
    out_policy->replay_gap.kind = R_HA_SCHEDULE_FIXED;
    out_policy->replay_gap.initial_units = 1u;
    out_policy->replay_gap.max_units = 1u;
    out_policy->final_receive_units = 1u;
    out_policy->completion_delivery = true;
}
