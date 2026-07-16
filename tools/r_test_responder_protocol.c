#include "r_test_responder.h"

#include <openssl/crypto.h>
#include <string.h>

#include "../src/r_crypto.h"
#include "../src/r_protocol.h"

const char R_TEST_RESPONDER_AES_KEY[] =
    "rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqhmzd8l";
const char R_TEST_RESPONDER_COOKIE_KEY[] =
    "rl-cookie1qgqqqqqqqqqqqqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqfn54mv";

typedef struct r_test_parsed_request {
    r_tenant_header_t tenant;
    uint16_t pdu_type;
    const uint8_t *body;
    size_t body_len;
    uint16_t guard_count;
    uint16_t resource_count;
    uint16_t report_count;
    const uint8_t *guards;
    const uint8_t *resources;
    const uint8_t *reports;
    char label[R_TEST_RESPONDER_LABEL_CAP + 1u];
} r_test_parsed_request_t;

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static void write_le16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void write_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)((value >> 8) & 0xffu);
    p[2] = (uint8_t)((value >> 16) & 0xffu);
    p[3] = (uint8_t)((value >> 24) & 0xffu);
}

const char *r_test_scenario_name(r_test_scenario_t scenario) {
    switch (scenario) {
        case R_TEST_SCENARIO_ALLOW: return "allow";
        case R_TEST_SCENARIO_DENY: return "deny";
        case R_TEST_SCENARIO_GUARD_PASS: return "guard-pass";
        case R_TEST_SCENARIO_GUARD_DENY: return "guard-deny";
        case R_TEST_SCENARIO_QUOTA: return "quota";
        case R_TEST_SCENARIO_DROP: return "drop";
        case R_TEST_SCENARIO_MALFORMED_AUTH: return "malformed-auth";
        case R_TEST_SCENARIO_MALFORMED_TRUNCATED: return "malformed-truncated";
        case R_TEST_SCENARIO_MALFORMED_REQUEST_ID: return "malformed-request-id";
        case R_TEST_SCENARIO_COUNT_EMPTY: return "count-empty";
        case R_TEST_SCENARIO_COUNT_SHORT: return "count-short";
        case R_TEST_SCENARIO_COUNT_EXTRA: return "count-extra";
    }
    return "unknown";
}

int r_test_scenario_parse(const char *name, r_test_scenario_t *out) {
    if (!name || !out) {
        return RCLIENT_ERR_CONFIG;
    }
    for (int value = R_TEST_SCENARIO_ALLOW; value <= R_TEST_SCENARIO_COUNT_EXTRA; value++) {
        r_test_scenario_t scenario = (r_test_scenario_t)value;
        if (strcmp(name, r_test_scenario_name(scenario)) == 0) {
            *out = scenario;
            return RCLIENT_OK;
        }
    }
    return RCLIENT_ERR_CONFIG;
}

int r_test_responder_init(
    r_test_responder_state_t *state,
    r_test_scenario_t scenario,
    const char *auth_key,
    uint64_t server_id,
    bool steering_keep_port,
    uint64_t allow_count
) {
    if (!state || !auth_key || server_id == 0u
        || scenario < R_TEST_SCENARIO_ALLOW || scenario > R_TEST_SCENARIO_COUNT_EXTRA) {
        return RCLIENT_ERR_CONFIG;
    }
    r_auth_key_info_t info;
    memset(&info, 0, sizeof(info));
    int rc = r_client_parse_auth_key(auth_key, &info);
    if (rc != RCLIENT_OK || info.secret_len != sizeof(state->secret)) {
        OPENSSL_cleanse(&info, sizeof(info));
        return RCLIENT_ERR_CONFIG;
    }

    memset(state, 0, sizeof(*state));
    state->scenario = scenario;
    state->auth_type = info.type;
    state->tenant_key_id = info.key_id;
    memcpy(state->secret, info.secret, sizeof(state->secret));
    state->server_id = server_id;
    state->steering_keep_port = steering_keep_port;
    state->allow_count = allow_count;
    OPENSSL_cleanse(&info, sizeof(info));
    return RCLIENT_OK;
}

static int extract_authenticated_pdu(
    const r_test_responder_state_t *state,
    const uint8_t *input,
    size_t input_len,
    r_tenant_header_t *tenant,
    uint8_t decrypted[R_MAX_PACKET_SIZE],
    const uint8_t **pdu,
    size_t *pdu_len
) {
    size_t pos = 0u;
    int rc = r_parse_tenant_header(input, input_len, tenant, &pos);
    if (rc != RCLIENT_OK || tenant->key_id != state->tenant_key_id) {
        return RCLIENT_ERR_AUTH;
    }

    uint16_t auth_type = 0u;
    size_t auth_size = 0u;
    const uint8_t *auth_body = NULL;
    size_t auth_body_len = 0u;
    size_t pdu_pos = 0u;
    rc = r_parse_auth_tlv_header(
        input,
        input_len,
        pos,
        &auth_type,
        &auth_size,
        &auth_body,
        &auth_body_len,
        &pdu_pos
    );
    if (rc != RCLIENT_OK) {
        return rc;
    }

    if (state->auth_type == R_AUTH_COOKIE) {
        if (auth_type != R_TLV_AUTH_COOKIE || auth_size != 36u || auth_body_len != 32u) {
            return RCLIENT_ERR_AUTH;
        }
        if (CRYPTO_memcmp(auth_body, state->secret, 32u) != 0) {
            return RCLIENT_ERR_AUTH;
        }
        *pdu = input + pdu_pos;
        *pdu_len = input_len - pdu_pos;
        return RCLIENT_OK;
    }

    if (state->auth_type != R_AUTH_AES_GCM || auth_type != R_TLV_AUTH_AES
        || auth_size != 32u || auth_body_len != 28u || input_len <= pdu_pos) {
        return RCLIENT_ERR_AUTH;
    }

    size_t decrypted_len = 0u;
    if (r_decrypt_pdu_aes_gcm(
            input + pdu_pos,
            input_len - pdu_pos,
            state->secret,
            auth_body,
            auth_body + 12u,
            input,
            pos + 4u + 12u,
            decrypted,
            R_MAX_PACKET_SIZE,
            &decrypted_len
        ) != 0) {
        return RCLIENT_ERR_AUTH;
    }
    *pdu = decrypted;
    *pdu_len = decrypted_len;
    return RCLIENT_OK;
}

static int parse_metrics_label(
    const uint8_t *body,
    size_t body_len,
    size_t pos,
    char out[R_TEST_RESPONDER_LABEL_CAP + 1u]
) {
    out[0] = '\0';
    if (pos == body_len) {
        return RCLIENT_OK;
    }
    if (body_len - pos < 6u || read_le16(body + pos) != R_TLV_METRICS_LABEL) {
        return RCLIENT_ERR_PROTOCOL;
    }
    size_t tlv_size = read_le16(body + pos + 2u);
    size_t label_len = read_le16(body + pos + 4u);
    if (tlv_size < 6u || tlv_size > body_len - pos || pos + tlv_size != body_len
        || label_len > tlv_size - 6u || label_len > R_TEST_RESPONDER_LABEL_CAP) {
        return RCLIENT_ERR_PROTOCOL;
    }
    memcpy(out, body + pos + 6u, label_len);
    out[label_len] = '\0';
    return RCLIENT_OK;
}

static int parse_request_pdu(
    const uint8_t *pdu,
    size_t pdu_len,
    r_test_parsed_request_t *request
) {
    if (!pdu || !request || pdu_len < R_PDU_HEADER_LEN) {
        return RCLIENT_ERR_PROTOCOL;
    }
    size_t declared_len = read_le16(pdu + 2u);
    if (declared_len != pdu_len || declared_len < R_PDU_HEADER_LEN) {
        return RCLIENT_ERR_PROTOCOL;
    }
    request->pdu_type = read_le16(pdu);
    request->body = pdu + R_PDU_HEADER_LEN;
    request->body_len = pdu_len - R_PDU_HEADER_LEN;

    if (request->body_len < 4u) {
        return RCLIENT_ERR_PROTOCOL;
    }
    if (request->pdu_type == R_PDU_RATE_REQUEST) {
        request->guard_count = read_le16(request->body);
        request->resource_count = read_le16(request->body + 2u);
        size_t guard_bytes = (size_t)request->guard_count * R_GUARD_BLOCK_WIRE_LEN;
        size_t resource_bytes = (size_t)request->resource_count * R_RESOURCE_BLOCK_WIRE_LEN;
        if (guard_bytes > request->body_len - 4u
            || resource_bytes > request->body_len - 4u - guard_bytes) {
            return RCLIENT_ERR_PROTOCOL;
        }
        request->guards = request->body + 4u;
        request->resources = request->guards + guard_bytes;
        return parse_metrics_label(
            request->body,
            request->body_len,
            4u + guard_bytes + resource_bytes,
            request->label
        );
    }
    if (request->pdu_type == R_PDU_LATENCY_REPORT) {
        request->report_count = read_le16(request->body);
        size_t report_bytes = (size_t)request->report_count * R_SERVICE_LATENCY_BLOCK_WIRE_LEN;
        if (report_bytes != request->body_len - 4u) {
            return RCLIENT_ERR_PROTOCOL;
        }
        request->reports = request->body + 4u;
        return RCLIENT_OK;
    }
    return RCLIENT_ERR_PROTOCOL;
}

static int quota_allows(r_test_responder_state_t *state, const uint8_t bucket_id[16]) {
    for (size_t i = 0u; i < state->quota_count; i++) {
        if (memcmp(state->quota[i].bucket_id, bucket_id, 16u) == 0) {
            bool allowed = state->quota[i].requests < state->allow_count;
            state->quota[i].requests++;
            return allowed ? 1 : 0;
        }
    }
    if (state->quota_count >= R_TEST_RESPONDER_QUOTA_CAP) {
        return -1;
    }
    r_test_quota_entry_t *entry = &state->quota[state->quota_count++];
    memcpy(entry->bucket_id, bucket_id, 16u);
    entry->requests = 1u;
    return state->allow_count > 0u ? 1 : 0;
}

static int build_response_pdu(
    r_test_responder_state_t *state,
    const r_test_parsed_request_t *request,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len
) {
    uint16_t guard_count = request->guard_count;
    uint16_t resource_count = request->resource_count;
    if (state->scenario == R_TEST_SCENARIO_COUNT_EMPTY) {
        guard_count = 0u;
        resource_count = 0u;
    } else if (state->scenario == R_TEST_SCENARIO_COUNT_SHORT) {
        if (resource_count > 0u) {
            resource_count--;
        } else if (guard_count > 0u) {
            guard_count--;
        }
    } else if (state->scenario == R_TEST_SCENARIO_COUNT_EXTRA) {
        if (resource_count < UINT16_MAX) {
            resource_count++;
        } else if (guard_count < UINT16_MAX) {
            guard_count++;
        } else {
            return RCLIENT_ERR_PROTOCOL;
        }
    }

    size_t body_len = 4u
        + (size_t)guard_count * R_GUARD_BLOCK_WIRE_LEN
        + (size_t)resource_count * R_RESOURCE_BLOCK_WIRE_LEN;
    if (body_len > R_MAX_PACKET_SIZE || body_len + R_PDU_HEADER_LEN > out_cap) {
        return RCLIENT_ERR_PROTOCOL;
    }
    uint8_t body[R_MAX_PACKET_SIZE];
    write_le16(body, guard_count);
    write_le16(body + 2u, resource_count);
    size_t pos = 4u;

    for (uint16_t i = 0u; i < guard_count; i++) {
        if (i < request->guard_count) {
            memcpy(body + pos, request->guards + (size_t)i * R_GUARD_BLOCK_WIRE_LEN,
                R_GUARD_BLOCK_WIRE_LEN);
        } else {
            memset(body + pos, 0, R_GUARD_BLOCK_WIRE_LEN);
            write_le32(body + pos + 32u, 1u);
        }
        uint32_t threshold = read_le32(body + pos + 32u);
        uint32_t current = threshold > 0u ? threshold - 1u : 0u;
        if (state->scenario == R_TEST_SCENARIO_GUARD_DENY) {
            current = threshold;
        }
        write_le32(body + pos + 36u, current);
        pos += R_GUARD_BLOCK_WIRE_LEN;
    }

    for (uint16_t i = 0u; i < resource_count; i++) {
        uint16_t deficit = 0u;
        if (i < request->resource_count) {
            const uint8_t *source = request->resources + (size_t)i * R_RESOURCE_BLOCK_WIRE_LEN;
            memcpy(body + pos, source, R_RESOURCE_BLOCK_WIRE_LEN);
            if (state->scenario == R_TEST_SCENARIO_DENY) {
                deficit = read_le16(source + 24u);
                if (deficit == 0u) {
                    deficit = 1u;
                }
            } else if (state->scenario == R_TEST_SCENARIO_QUOTA) {
                int allowed = quota_allows(state, source);
                if (allowed < 0) {
                    return RCLIENT_ERR_NOMEM;
                }
                if (!allowed) {
                    deficit = read_le16(source + 24u);
                    if (deficit == 0u) {
                        deficit = 1u;
                    }
                }
            }
        } else {
            memset(body + pos, 0, R_RESOURCE_BLOCK_WIRE_LEN);
            deficit = 1u;
        }
        write_le16(body + pos + 24u, deficit);
        write_le16(body + pos + 26u, 0u);
        pos += R_RESOURCE_BLOCK_WIRE_LEN;
    }

    return r_build_pdu(R_PDU_RATE_RESPONSE, body, body_len, out, out_cap, out_len);
}

static int build_authenticated_response(
    const r_test_responder_state_t *state,
    const r_tenant_header_t *request_tenant,
    const uint8_t *pdu,
    size_t pdu_len,
    bool wrong_request_id,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len
) {
    if (out_cap < R_TENANT_TLV_LEN) {
        return RCLIENT_ERR_PROTOCOL;
    }
    r_tenant_header_t tenant;
    memset(&tenant, 0, sizeof(tenant));
    tenant.tlv_type = R_TLV_TENANT;
    tenant.tlv_size = R_TENANT_TLV_LEN;
    tenant.key_id = state->server_id;
    memcpy(tenant.unique_id, request_tenant->unique_id, sizeof(tenant.unique_id));
    if (wrong_request_id) {
        tenant.unique_id[0] ^= 0x80u;
    }
    tenant.time_stamp = request_tenant->time_stamp;
    tenant.steering_feedback = state->steering_keep_port ? 1u : 0u;
    r_tenant_header_write(&tenant, out, out_cap);
    size_t pos = R_TENANT_TLV_LEN;

    if (state->auth_type == R_AUTH_COOKIE) {
        if (pos + 36u + pdu_len > out_cap) {
            return RCLIENT_ERR_PROTOCOL;
        }
        write_le16(out + pos, R_TLV_AUTH_COOKIE);
        write_le16(out + pos + 2u, 36u);
        pos += 4u;
        memcpy(out + pos, state->secret, 32u);
        pos += 32u;
        memcpy(out + pos, pdu, pdu_len);
        pos += pdu_len;
        *out_len = pos;
        return RCLIENT_OK;
    }

    if (state->auth_type != R_AUTH_AES_GCM || pos + 32u + pdu_len > out_cap) {
        return RCLIENT_ERR_PROTOCOL;
    }
    uint8_t cipher[R_MAX_PACKET_SIZE];
    size_t cipher_len = 0u;
    uint8_t nonce[12];
    uint8_t tag[16];
    write_le16(out + pos, R_TLV_AUTH_AES);
    write_le16(out + pos + 2u, 32u);
    if (r_encrypt_pdu_aes_gcm(
            pdu,
            pdu_len,
            state->secret,
            out,
            pos + 4u,
            cipher,
            sizeof(cipher),
            &cipher_len,
            nonce,
            tag
        ) != 0) {
        return RCLIENT_ERR_AUTH;
    }
    pos += 4u;
    memcpy(out + pos, nonce, sizeof(nonce));
    pos += sizeof(nonce);
    memcpy(out + pos, tag, sizeof(tag));
    pos += sizeof(tag);
    memcpy(out + pos, cipher, cipher_len);
    pos += cipher_len;
    *out_len = pos;
    return RCLIENT_OK;
}

int r_test_responder_process(
    r_test_responder_state_t *state,
    const uint8_t *input,
    size_t input_len,
    uint8_t *output,
    size_t output_cap,
    size_t *output_len,
    bool *send_response,
    r_test_event_t *event
) {
    if (!state || !input || input_len == 0u || !output || !output_len
        || !send_response || !event) {
        return RCLIENT_ERR_CONFIG;
    }
    memset(event, 0, sizeof(*event));
    event->kind = R_TEST_EVENT_INPUT_REJECTED;
    event->sequence = ++state->sequence;
    event->disposition = "rejected";
    *output_len = 0u;
    *send_response = false;

    uint8_t decrypted[R_MAX_PACKET_SIZE];
    const uint8_t *pdu = NULL;
    size_t pdu_len = 0u;
    r_test_parsed_request_t request;
    memset(&request, 0, sizeof(request));
    int rc = extract_authenticated_pdu(
        state,
        input,
        input_len,
        &request.tenant,
        decrypted,
        &pdu,
        &pdu_len
    );
    if (rc != RCLIENT_OK) {
        return rc;
    }
    rc = parse_request_pdu(pdu, pdu_len, &request);
    if (rc != RCLIENT_OK) {
        return rc;
    }

    if (request.pdu_type == R_PDU_LATENCY_REPORT) {
        event->kind = R_TEST_EVENT_LATENCY_REPORT;
        event->report_count = request.report_count;
        event->disposition = "observed";
        return RCLIENT_OK;
    }

    event->kind = R_TEST_EVENT_RATE_REQUEST;
    event->guard_count = request.guard_count;
    event->resource_count = request.resource_count;
    memcpy(event->label, request.label, sizeof(event->label));
    if (state->scenario == R_TEST_SCENARIO_DROP) {
        event->disposition = "dropped";
        return RCLIENT_OK;
    }

    uint8_t response_pdu[R_MAX_PACKET_SIZE];
    size_t response_pdu_len = 0u;
    rc = build_response_pdu(
        state,
        &request,
        response_pdu,
        sizeof(response_pdu),
        &response_pdu_len
    );
    if (rc != RCLIENT_OK) {
        return rc;
    }
    rc = build_authenticated_response(
        state,
        &request.tenant,
        response_pdu,
        response_pdu_len,
        state->scenario == R_TEST_SCENARIO_MALFORMED_REQUEST_ID,
        output,
        output_cap,
        output_len
    );
    if (rc != RCLIENT_OK) {
        return rc;
    }

    if (state->scenario == R_TEST_SCENARIO_MALFORMED_AUTH) {
        size_t auth_offset = R_TENANT_TLV_LEN + 4u;
        size_t corrupt_offset = state->auth_type == R_AUTH_AES_GCM
            ? auth_offset + 12u
            : auth_offset;
        if (corrupt_offset >= *output_len) {
            return RCLIENT_ERR_PROTOCOL;
        }
        output[corrupt_offset] ^= 0x01u;
        event->disposition = "malformed-auth";
    } else if (state->scenario == R_TEST_SCENARIO_MALFORMED_TRUNCATED) {
        if (*output_len == 0u) {
            return RCLIENT_ERR_PROTOCOL;
        }
        (*output_len)--;
        event->disposition = "malformed-truncated";
    } else if (state->scenario == R_TEST_SCENARIO_MALFORMED_REQUEST_ID) {
        event->disposition = "malformed-request-id";
    } else {
        event->disposition = r_test_scenario_name(state->scenario);
    }
    *send_response = true;
    return RCLIENT_OK;
}
