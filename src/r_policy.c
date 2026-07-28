#include "../include/r_client.h"

#include <string.h>

void r_client_default_oldest_first_ha_policy(
    r_oldest_first_ha_policy_t *out_policy
) {
    if (!out_policy) {
        return;
    }
    memset(out_policy, 0, sizeof(*out_policy));
    out_policy->unit_ms = 20u;
    out_policy->replay_count = 1u;
    out_policy->replay_gap.kind = R_HA_SCHEDULE_FIXED;
    out_policy->replay_gap.initial_units = 1u;
    out_policy->replay_gap.max_units = 1u;
    out_policy->preference.kind = R_HA_SCHEDULE_FIXED;
    out_policy->preference.initial_units = 1u;
    out_policy->preference.max_units = 1u;
    out_policy->final_receive_units = 1u;
    out_policy->final_preference_units = 0u;
    out_policy->completion_delivery = true;
}

void r_client_default_request_policy(r_request_policy_t *out_policy) {
    if (!out_policy) {
        return;
    }
    memset(out_policy, 0, sizeof(*out_policy));
    out_policy->kind = R_REQUEST_POLICY_OLDEST_FIRST_HA;
    r_client_default_oldest_first_ha_policy(&out_policy->oldest_first_ha);

    out_policy->attempt_timeout_ms = 20;
    out_policy->dedup_ttl_ms = 60;
    out_policy->wait = R_WAIT_TWO_ROUND_OLDEST_THEN_FIRST;
    out_policy->quorum.kind = R_QUORUM_ALL;
    out_policy->quorum.count = 0;
    out_policy->quorum_requirement = R_QUORUM_SOFT;
    out_policy->select = R_SELECT_BEST_BY_RELIABILITY;

    // The default HA mode performs exactly one resend after a silent first
    // interval, then enters its receive-only grace interval after a silent
    // second interval.
    out_policy->retry.retry_attempts = 1;
    out_policy->retry.retry_on = R_RETRY_TIMEOUT_ONLY;
    out_policy->retry.backoff.kind = R_BACKOFF_NONE;
    out_policy->retry.backoff.delay_ms = 0;
    out_policy->retry.backoff.base_delay_ms = 0;
    out_policy->retry.backoff.max_delay_ms = 0;
    out_policy->retry.backoff.jitter_ms = 0;
    out_policy->retry.resend = R_RESEND_ALL;
    out_policy->retry.refresh_dns_on_retry = false;
    out_policy->retry.total_timeout_ms = 0;

    out_policy->dns_resync.on = R_DNS_INTERVAL_ONLY;
    out_policy->dns_resync.refresh_interval_ms = 300000;
    out_policy->dns_resync.min_interval_ms = 1000;
    out_policy->dns_resync.jitter_ms = 0;
}
