#ifndef R_PROTOCOL_H
#define R_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "../include/r_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define R_TENANT_TLV_LEN 40u
#define R_PDU_HEADER_LEN 8u
#define R_MAX_PACKET_SIZE 1200u

#define R_TLV_TENANT 0x4C52u
#define R_TLV_AUTH_COOKIE 0x4143u
#define R_TLV_AUTH_AES 0x4541u
#define R_TLV_METRICS_LABEL 0x4C4Du

#define R_PDU_RATE_REQUEST 0x5452u
#define R_PDU_RATE_RESPONSE 0x5252u
#define R_PDU_LATENCY_REPORT 0x524Cu

typedef struct r_tenant_header {
    uint16_t tlv_type;
    uint16_t tlv_size;
    uint64_t key_id;
    uint8_t unique_id[16];
    uint64_t time_stamp;
    uint8_t steering_feedback;
    uint8_t tenant_mgmt_flag;
    uint8_t padding[2];
} r_tenant_header_t;

typedef struct r_guard_block {
    uint8_t service_id[16];
    uint32_t ttl_ms;
    uint32_t max_samples;
    uint32_t buffer_size;
    uint32_t min_sample_threshold;
    uint32_t latency_threshold;
    uint32_t current_latency;
} r_guard_block_t;

typedef struct r_resource_block {
    uint8_t bucket_id[16];
    uint32_t window_size_ms;
    uint32_t rate_limit;
    uint16_t tokens_requested;
    uint16_t padding;
} r_resource_block_t;

typedef struct r_service_latency_block {
    uint8_t service_id[16];
    uint32_t ttl_ms;
    uint32_t max_samples;
    uint32_t buffer_size;
    uint32_t min_sample_threshold;
    uint32_t observed_latency;
    uint32_t padding;
} r_service_latency_block_t;

void r_tenant_header_write(const r_tenant_header_t *header, uint8_t *buf, size_t len);
int r_tenant_header_read(const uint8_t *buf, size_t len, r_tenant_header_t *out);

int r_parse_tenant_header(
    const uint8_t *buf,
    size_t len,
    r_tenant_header_t *out,
    size_t *out_pos
);

int r_parse_auth_tlv_header(
    const uint8_t *buf,
    size_t len,
    size_t pos,
    uint16_t *out_type,
    size_t *out_size,
    const uint8_t **out_body,
    size_t *out_body_len,
    size_t *out_pdu_pos
);

uint64_t r_peek_server_id(const uint8_t *buf, size_t len, bool *ok);
int r_peek_request_id(const uint8_t *buf, size_t len, uint8_t out_id[16]);

int r_append_metrics_label_tlv(
    uint8_t *buf,
    size_t buf_cap,
    size_t *inout_len,
    const char *label,
    size_t label_len
);

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
);

int r_build_latency_report_body(
    const r_service_latency_report_t *reports,
    size_t report_count,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len
);

int r_build_rate_request_pdu(
    uint32_t dedup_ttl_ms,
    const uint8_t *body,
    size_t body_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len
);

int r_build_pdu(
    uint16_t pdu_type,
    const uint8_t *body,
    size_t body_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len
);

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
);

#ifdef __cplusplus
}
#endif

#endif
