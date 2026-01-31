#ifndef R_CLIENT_IO_H
#define R_CLIENT_IO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct r_addr {
    struct sockaddr_storage sa;
    socklen_t len;
} r_addr_t;

typedef enum r_log_level {
    R_LOG_ERROR = 0,
    R_LOG_WARN = 1,
    R_LOG_INFO = 2,
    R_LOG_DEBUG = 3,
} r_log_level_t;

typedef struct r_io_ops {
    void *ctx;

    // Send a UDP datagram. Return 0 on success, negative on error.
    int (*udp_send)(void *ctx, const r_addr_t *to, const uint8_t *buf, size_t len);

    // Milliseconds since UNIX epoch.
    uint64_t (*now_ms)(void *ctx);

    // Optional logging hook (may be NULL).
    void (*log)(void *ctx, r_log_level_t level, const char *msg);

    // Optional steering feedback hook (keep_port == false means change port).
    void (*on_steering_feedback)(void *ctx, bool keep_port);
} r_io_ops_t;

typedef uint64_t r_dns_req_id_t;

typedef struct r_srv_record {
    const char *target;
    uint16_t port;
    uint16_t priority;
    uint16_t weight;
    uint32_t ttl_ms;
} r_srv_record_t;

typedef void (*r_dns_srv_cb)(
    void *user,
    int status,
    const r_srv_record_t *records,
    size_t record_count
);

typedef void (*r_dns_addr_cb)(
    void *user,
    int status,
    const r_addr_t *addrs,
    size_t addr_count
);

typedef struct r_resolver_ops {
    void *ctx;

    // Resolve SRV records for a name. May invoke callback synchronously.
    int (*resolve_srv)(
        void *ctx,
        const char *name,
        r_dns_req_id_t *out_req_id,
        r_dns_srv_cb cb,
        void *user
    );

    // Resolve A/AAAA records for a name. May invoke callback synchronously.
    int (*resolve_addrs)(
        void *ctx,
        const char *name,
        r_dns_req_id_t *out_req_id,
        r_dns_addr_cb cb,
        void *user
    );

    // Best-effort cancel. Late callbacks may be ignored by the client.
    void (*cancel)(void *ctx, r_dns_req_id_t req_id);
} r_resolver_ops_t;

#ifdef __cplusplus
}
#endif

#endif
