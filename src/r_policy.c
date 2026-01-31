#include "../include/r_client.h"

void r_client_default_request_policy(r_request_policy_t *out_policy) {
    if (!out_policy) {
        return;
    }
    out_policy->attempt_timeout_ms = 1000;
    out_policy->wait = R_WAIT_FOR_DEADLINE;
    out_policy->quorum.kind = R_QUORUM_ALL;
    out_policy->quorum.count = 0;
    out_policy->quorum_requirement = R_QUORUM_SOFT;
    out_policy->select = R_SELECT_BEST_BY_RELIABILITY;

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

