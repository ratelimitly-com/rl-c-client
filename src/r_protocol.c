#include "r_protocol.h"

#include <string.h>

static uint16_t r_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t r_le32(const uint8_t *p) {
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static uint64_t r_le64(const uint8_t *p) {
    return (uint64_t)p[0]
        | ((uint64_t)p[1] << 8)
        | ((uint64_t)p[2] << 16)
        | ((uint64_t)p[3] << 24)
        | ((uint64_t)p[4] << 32)
        | ((uint64_t)p[5] << 40)
        | ((uint64_t)p[6] << 48)
        | ((uint64_t)p[7] << 56);
}

static void r_write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void r_write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void r_write_le64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
    p[4] = (uint8_t)((v >> 32) & 0xff);
    p[5] = (uint8_t)((v >> 40) & 0xff);
    p[6] = (uint8_t)((v >> 48) & 0xff);
    p[7] = (uint8_t)((v >> 56) & 0xff);
}

void r_tenant_header_write(const r_tenant_header_t *header, uint8_t *buf, size_t len) {
    if (!header || !buf || len < R_TENANT_TLV_LEN) {
        return;
    }
    r_write_le16(buf, header->tlv_type);
    r_write_le16(buf + 2, header->tlv_size);
    r_write_le64(buf + 4, header->key_id);
    memcpy(buf + 12, header->unique_id, 16);
    r_write_le64(buf + 28, header->time_stamp);
    buf[36] = header->steering_feedback;
    buf[37] = header->tenant_mgmt_flag;
    buf[38] = header->padding[0];
    buf[39] = header->padding[1];
}

int r_tenant_header_read(const uint8_t *buf, size_t len, r_tenant_header_t *out) {
    if (!buf || !out || len < R_TENANT_TLV_LEN) {
        return RCLIENT_ERR_PROTOCOL;
    }
    out->tlv_type = r_le16(buf);
    out->tlv_size = r_le16(buf + 2);
    out->key_id = r_le64(buf + 4);
    memcpy(out->unique_id, buf + 12, 16);
    out->time_stamp = r_le64(buf + 28);
    out->steering_feedback = buf[36];
    out->tenant_mgmt_flag = buf[37];
    out->padding[0] = buf[38];
    out->padding[1] = buf[39];
    return RCLIENT_OK;
}

int r_parse_tenant_header(
    const uint8_t *buf,
    size_t len,
    r_tenant_header_t *out,
    size_t *out_pos
) {
    if (!buf || len < 4) {
        return RCLIENT_ERR_PROTOCOL;
    }
    uint16_t tlv_type = r_le16(buf);
    if (tlv_type != R_TLV_TENANT) {
        return RCLIENT_ERR_PROTOCOL;
    }
    uint16_t tlv_size = r_le16(buf + 2);
    if (tlv_size < R_TENANT_TLV_LEN || len < tlv_size) {
        return RCLIENT_ERR_PROTOCOL;
    }
    if (out) {
        r_tenant_header_read(buf, len, out);
    }
    if (out_pos) {
        *out_pos = tlv_size;
    }
    return RCLIENT_OK;
}

int r_parse_auth_tlv_header(
    const uint8_t *buf,
    size_t len,
    size_t pos,
    uint16_t *out_type,
    size_t *out_size,
    const uint8_t **out_body,
    size_t *out_body_len,
    size_t *out_pdu_pos
) {
    if (!buf || len < pos + 4) {
        return RCLIENT_ERR_PROTOCOL;
    }
    uint16_t tlv_type = r_le16(buf + pos);
    uint16_t tlv_size = r_le16(buf + pos + 2);
    if (tlv_size < 4 || len < pos + tlv_size) {
        return RCLIENT_ERR_PROTOCOL;
    }

    if (out_type) {
        *out_type = tlv_type;
    }
    if (out_size) {
        *out_size = tlv_size;
    }
    if (out_body) {
        *out_body = buf + pos + 4;
    }
    if (out_body_len) {
        *out_body_len = (size_t)tlv_size - 4;
    }
    if (out_pdu_pos) {
        *out_pdu_pos = pos + tlv_size;
    }
    return RCLIENT_OK;
}

uint64_t r_peek_server_id(const uint8_t *buf, size_t len, bool *ok) {
    if (ok) {
        *ok = false;
    }
    if (!buf || len < 12) {
        return 0;
    }
    uint16_t tlv_type = r_le16(buf);
    uint16_t tlv_size = r_le16(buf + 2);
    if (tlv_type != R_TLV_TENANT || tlv_size < R_TENANT_TLV_LEN || len < tlv_size) {
        return 0;
    }
    if (ok) {
        *ok = true;
    }
    return r_le64(buf + 4);
}

int r_peek_request_id(const uint8_t *buf, size_t len, uint8_t out_id[16]) {
    if (!buf || len < 28 || !out_id) {
        return RCLIENT_ERR_PROTOCOL;
    }
    uint16_t tlv_type = r_le16(buf);
    uint16_t tlv_size = r_le16(buf + 2);
    if (tlv_type != R_TLV_TENANT || tlv_size < R_TENANT_TLV_LEN || len < tlv_size) {
        return RCLIENT_ERR_PROTOCOL;
    }
    memcpy(out_id, buf + 12, 16);
    return RCLIENT_OK;
}

int r_append_metrics_label_tlv(
    uint8_t *buf,
    size_t buf_cap,
    size_t *inout_len,
    const char *label,
    size_t label_len
) {
    if (!buf || !inout_len || !label) {
        return RCLIENT_ERR_PROTOCOL;
    }
    size_t len = *inout_len;
    if (label_len == 0) {
        label_len = strlen(label);
    }
    if (label_len > 0xffffu) {
        return RCLIENT_ERR_PROTOCOL;
    }
    size_t body_len = 2 + label_len;
    size_t padding = (4 - (body_len % 4)) % 4;
    size_t tlv_size = 4 + body_len + padding;
    if (tlv_size > 0xffffu) {
        return RCLIENT_ERR_PROTOCOL;
    }
    if (len + tlv_size > buf_cap) {
        return RCLIENT_ERR_PROTOCOL;
    }

    r_write_le16(buf + len, R_TLV_METRICS_LABEL);
    r_write_le16(buf + len + 2, (uint16_t)tlv_size);
    r_write_le16(buf + len + 4, (uint16_t)label_len);
    memcpy(buf + len + 6, label, label_len);
    if (padding > 0) {
        memset(buf + len + 6 + label_len, 0, padding);
    }
    *inout_len = len + tlv_size;
    return RCLIENT_OK;
}

int r_build_rate_request_body(
    const r_resource_request_t *resources,
    size_t resource_count,
    const r_latency_guard_t *guards,
    size_t guard_count,
    const char *metrics_label,
    size_t metrics_label_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len
) {
    if (!out || !out_len) {
        return RCLIENT_ERR_PROTOCOL;
    }
    if (resource_count > 0xffffu || guard_count > 0xffffu) {
        return RCLIENT_ERR_PROTOCOL;
    }
    size_t needed = 4 + guard_count * sizeof(r_guard_block_t) + resource_count * sizeof(r_resource_block_t);
    if (needed > out_cap) {
        return RCLIENT_ERR_PROTOCOL;
    }
    size_t pos = 0;
    r_write_le16(out + pos, (uint16_t)guard_count);
    pos += 2;
    r_write_le16(out + pos, (uint16_t)resource_count);
    pos += 2;

    for (size_t i = 0; i < guard_count; i++) {
        r_guard_block_t block;
        memcpy(block.service_id, guards[i].service_id, 16);
        block.ttl_ms = guards[i].ttl_ms;
        block.max_samples = guards[i].max_samples;
        block.buffer_size = guards[i].buffer_size;
        block.min_sample_threshold = guards[i].min_sample_threshold;
        block.latency_threshold = guards[i].threshold_ms;
        block.current_latency = 0;

        memcpy(out + pos, block.service_id, 16);
        r_write_le32(out + pos + 16, block.ttl_ms);
        r_write_le32(out + pos + 20, block.max_samples);
        r_write_le32(out + pos + 24, block.buffer_size);
        r_write_le32(out + pos + 28, block.min_sample_threshold);
        r_write_le32(out + pos + 32, block.latency_threshold);
        r_write_le32(out + pos + 36, block.current_latency);
        pos += sizeof(r_guard_block_t);
    }

    for (size_t i = 0; i < resource_count; i++) {
        r_resource_block_t block;
        memcpy(block.bucket_id, resources[i].bucket_id, 16);
        block.window_size_ms = resources[i].window_size_ms;
        block.rate_limit = resources[i].rate_limit;
        block.tokens_requested = resources[i].tokens_requested;
        block.padding = 0;

        memcpy(out + pos, block.bucket_id, 16);
        r_write_le32(out + pos + 16, block.window_size_ms);
        r_write_le32(out + pos + 20, block.rate_limit);
        r_write_le16(out + pos + 24, block.tokens_requested);
        r_write_le16(out + pos + 26, block.padding);
        pos += sizeof(r_resource_block_t);
    }

    if (metrics_label && metrics_label[0] != '\0') {
        int rc = r_append_metrics_label_tlv(out, out_cap, &pos, metrics_label, metrics_label_len);
        if (rc != RCLIENT_OK) {
            return rc;
        }
    }

    *out_len = pos;
    return RCLIENT_OK;
}

int r_build_latency_report_body(
    const r_service_latency_report_t *reports,
    size_t report_count,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len
) {
    if (!out || !out_len) {
        return RCLIENT_ERR_PROTOCOL;
    }
    if (report_count > 0xffffu) {
        return RCLIENT_ERR_PROTOCOL;
    }
    size_t needed = 4 + report_count * sizeof(r_service_latency_block_t);
    if (needed > out_cap) {
        return RCLIENT_ERR_PROTOCOL;
    }
    size_t pos = 0;
    r_write_le16(out + pos, (uint16_t)report_count);
    pos += 2;
    r_write_le16(out + pos, 0);
    pos += 2;

    for (size_t i = 0; i < report_count; i++) {
        r_service_latency_block_t block;
        memcpy(block.service_id, reports[i].service_id, 16);
        block.ttl_ms = reports[i].ttl_ms;
        block.max_samples = reports[i].max_samples;
        block.buffer_size = reports[i].buffer_size;
        block.min_sample_threshold = reports[i].min_sample_threshold;
        block.observed_latency = reports[i].observed_latency;
        block.padding = 0;

        memcpy(out + pos, block.service_id, 16);
        r_write_le32(out + pos + 16, block.ttl_ms);
        r_write_le32(out + pos + 20, block.max_samples);
        r_write_le32(out + pos + 24, block.buffer_size);
        r_write_le32(out + pos + 28, block.min_sample_threshold);
        r_write_le32(out + pos + 32, block.observed_latency);
        r_write_le32(out + pos + 36, block.padding);
        pos += sizeof(r_service_latency_block_t);
    }

    *out_len = pos;
    return RCLIENT_OK;
}

int r_build_pdu(
    uint16_t pdu_type,
    const uint8_t *body,
    size_t body_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len
) {
    if (!out || !out_len || !body) {
        return RCLIENT_ERR_PROTOCOL;
    }
    size_t pdu_size = R_PDU_HEADER_LEN + body_len;
    if (pdu_size > 0xffffu || pdu_size > out_cap) {
        return RCLIENT_ERR_PROTOCOL;
    }
    r_write_le16(out, pdu_type);
    r_write_le16(out + 2, (uint16_t)pdu_size);
    r_write_le16(out + 4, 0);
    r_write_le16(out + 6, 0);
    memcpy(out + R_PDU_HEADER_LEN, body, body_len);
    *out_len = pdu_size;
    return RCLIENT_OK;
}

int r_parse_rate_response_pdu(
    const uint8_t *pdu,
    size_t pdu_len,
    r_guard_result_t *guards,
    size_t guard_cap,
    size_t *out_guard_count,
    r_resource_result_t *resources,
    size_t res_cap,
    size_t *out_res_count,
    bool *out_success
) {
    if (!pdu || pdu_len < R_PDU_HEADER_LEN) {
        return RCLIENT_ERR_PROTOCOL;
    }
    uint16_t pdu_type = r_le16(pdu);
    if (pdu_type != R_PDU_RATE_RESPONSE) {
        return RCLIENT_ERR_PROTOCOL;
    }
    size_t pdu_size = r_le16(pdu + 2);
    if (pdu_size < R_PDU_HEADER_LEN || pdu_len < pdu_size) {
        return RCLIENT_ERR_PROTOCOL;
    }
    const uint8_t *body = pdu + R_PDU_HEADER_LEN;
    size_t body_len = pdu_size - R_PDU_HEADER_LEN;
    if (body_len < 4) {
        return RCLIENT_ERR_PROTOCOL;
    }

    size_t pos = 0;
    uint16_t guard_count = r_le16(body + pos);
    pos += 2;
    uint16_t resource_count = r_le16(body + pos);
    pos += 2;

    if (guard_count > guard_cap || resource_count > res_cap) {
        return RCLIENT_ERR_PROTOCOL;
    }

    bool success = true;

    for (uint16_t i = 0; i < guard_count; i++) {
        if (body_len < pos + sizeof(r_guard_block_t)) {
            return RCLIENT_ERR_PROTOCOL;
        }
        const uint8_t *p = body + pos;
        memcpy(guards[i].service_id, p, 16);
        uint32_t latency_threshold = r_le32(p + 32);
        uint32_t current_latency = r_le32(p + 36);
        guards[i].threshold_ms = latency_threshold;
        guards[i].current_latency_ms = current_latency;
        guards[i].passed = current_latency < latency_threshold;
        if (!guards[i].passed) {
            success = false;
        }
        pos += sizeof(r_guard_block_t);
    }

    for (uint16_t i = 0; i < resource_count; i++) {
        if (body_len < pos + sizeof(r_resource_block_t)) {
            return RCLIENT_ERR_PROTOCOL;
        }
        const uint8_t *p = body + pos;
        memcpy(resources[i].bucket_id, p, 16);
        resources[i].actual_rate = r_le32(p + 20);
        resources[i].tokens_deficit = r_le16(p + 24);
        if (resources[i].tokens_deficit != 0) {
            success = false;
        }
        pos += sizeof(r_resource_block_t);
    }

    if (out_guard_count) {
        *out_guard_count = guard_count;
    }
    if (out_res_count) {
        *out_res_count = resource_count;
    }
    if (out_success) {
        *out_success = success;
    }
    return RCLIENT_OK;
}

