#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <linux/io_uring.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "r_client_runtime.h"
#include "r_client_workflow.h"

/*
 * Flow (without liburing)
 * -----------------------
 * 1. io_uring_setup() creates a ring; mmap() exposes SQ, CQ, and SQEs.
 * 2. Publish one IORING_OP_POLL_ADD for each runtime-owned UDP socket.
 * 3. io_uring_enter() waits for a CQE or the admission deadline.
 * 4. Drain a ready socket, re-arm its one-shot poll, and retire its CQE.
 * 5. Run admitted work, measure it, and report one latency sample.
 *
 * Ownership: runtime owns sockets and the app owns all ring mappings. Ring
 * head/tail fields are shared with the kernel; acquire/release atomics publish
 * complete SQEs and retire fully observed CQEs.
 */
typedef struct raw_ring {
    int fd;
    void *sq_mapping;
    size_t sq_mapping_size;
    void *cq_mapping;
    size_t cq_mapping_size;
    struct io_uring_sqe *submissions;
    size_t submissions_size;
    unsigned *sq_head;
    unsigned *sq_tail;
    unsigned *sq_mask;
    unsigned *sq_entries;
    unsigned *sq_array;
    unsigned *cq_head;
    unsigned *cq_tail;
    unsigned *cq_mask;
    struct io_uring_cqe *completions;
} raw_ring_t;

typedef struct io_uring_app {
    raw_ring_t ring;
    r_runtime_client_t runtime;
    r_admission_request_t request;
    r_admission_outcome_t outcome;
    char response[96];
    uint32_t observed_latency_ms;
    int status;
    bool done;
} io_uring_app_t;

static void raw_ring_destroy(raw_ring_t *ring) {
    if (ring->submissions) {
        munmap(ring->submissions, ring->submissions_size);
    }
    if (ring->cq_mapping && ring->cq_mapping != ring->sq_mapping) {
        munmap(ring->cq_mapping, ring->cq_mapping_size);
    }
    if (ring->sq_mapping) {
        munmap(ring->sq_mapping, ring->sq_mapping_size);
    }
    if (ring->fd >= 0) {
        close(ring->fd);
    }
    memset(ring, 0, sizeof(*ring));
    ring->fd = -1;
}

static int raw_ring_init(raw_ring_t *ring, unsigned entries) {
    memset(ring, 0, sizeof(*ring));
    ring->fd = -1;
    struct io_uring_params parameters = {0};
    ring->fd = (int)syscall(__NR_io_uring_setup, entries, &parameters);
    if (ring->fd < 0) {
        return -1;
    }

    /* Offset metadata, rather than local struct layouts, defines each mapping. */
    ring->sq_mapping_size = parameters.sq_off.array
        + parameters.sq_entries * sizeof(unsigned);
    ring->cq_mapping_size = parameters.cq_off.cqes
        + parameters.cq_entries * sizeof(struct io_uring_cqe);
    /* Newer kernels may expose SQ and CQ through one shared mapping. */
    if ((parameters.features & IORING_FEAT_SINGLE_MMAP) != 0) {
        if (ring->cq_mapping_size > ring->sq_mapping_size) {
            ring->sq_mapping_size = ring->cq_mapping_size;
        }
        ring->cq_mapping_size = ring->sq_mapping_size;
    }

    ring->sq_mapping = mmap(NULL, ring->sq_mapping_size,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
        ring->fd, IORING_OFF_SQ_RING);
    if (ring->sq_mapping == MAP_FAILED) {
        ring->sq_mapping = NULL;
        raw_ring_destroy(ring);
        return -1;
    }
    if ((parameters.features & IORING_FEAT_SINGLE_MMAP) != 0) {
        ring->cq_mapping = ring->sq_mapping;
    } else {
        ring->cq_mapping = mmap(NULL, ring->cq_mapping_size,
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
            ring->fd, IORING_OFF_CQ_RING);
        if (ring->cq_mapping == MAP_FAILED) {
            ring->cq_mapping = NULL;
            raw_ring_destroy(ring);
            return -1;
        }
    }

    ring->submissions_size = parameters.sq_entries * sizeof(struct io_uring_sqe);
    ring->submissions = mmap(NULL, ring->submissions_size,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
        ring->fd, IORING_OFF_SQES);
    if (ring->submissions == MAP_FAILED) {
        ring->submissions = NULL;
        raw_ring_destroy(ring);
        return -1;
    }

    char *sq = ring->sq_mapping;
    ring->sq_head = (unsigned *)(sq + parameters.sq_off.head);
    ring->sq_tail = (unsigned *)(sq + parameters.sq_off.tail);
    ring->sq_mask = (unsigned *)(sq + parameters.sq_off.ring_mask);
    ring->sq_entries = (unsigned *)(sq + parameters.sq_off.ring_entries);
    ring->sq_array = (unsigned *)(sq + parameters.sq_off.array);

    char *cq = ring->cq_mapping;
    ring->cq_head = (unsigned *)(cq + parameters.cq_off.head);
    ring->cq_tail = (unsigned *)(cq + parameters.cq_off.tail);
    ring->cq_mask = (unsigned *)(cq + parameters.cq_off.ring_mask);
    ring->completions = (struct io_uring_cqe *)(cq + parameters.cq_off.cqes);
    return 0;
}

static int raw_ring_prep_poll(raw_ring_t *ring, int socket_fd, uint64_t user_data) {
    unsigned head = __atomic_load_n(ring->sq_head, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(ring->sq_tail, __ATOMIC_RELAXED);
    if (tail - head >= *ring->sq_entries) {
        return -1;
    }
    /* Monotonic counters wrap into the fixed-size array through ring_mask. */
    unsigned index = tail & *ring->sq_mask;
    struct io_uring_sqe *submission = &ring->submissions[index];
    memset(submission, 0, sizeof(*submission));
    submission->opcode = IORING_OP_POLL_ADD;
    submission->fd = socket_fd;
    submission->poll32_events = POLLIN;
    submission->user_data = user_data;
    ring->sq_array[index] = index;
    /* Publish only after the SQE and its array entry are completely written. */
    __atomic_store_n(ring->sq_tail, tail + 1u, __ATOMIC_RELEASE);
    return 0;
}

static int raw_ring_submit(raw_ring_t *ring) {
    unsigned head = __atomic_load_n(ring->sq_head, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(ring->sq_tail, __ATOMIC_ACQUIRE);
    unsigned count = tail - head;
    if (count == 0) {
        return 0;
    }
    int result = (int)syscall(__NR_io_uring_enter, ring->fd, count, 0, 0, NULL, 0);
    return result < 0 ? -1 : 0;
}

static int raw_ring_wait(raw_ring_t *ring, uint64_t delay_ms) {
    struct __kernel_timespec timeout = {
        .tv_sec = (long long)(delay_ms / 1000u),
        .tv_nsec = (long long)((delay_ms % 1000u) * 1000000u),
    };
    struct io_uring_getevents_arg arguments = {
        .ts = (uint64_t)(uintptr_t)&timeout,
    };
    /* EXT_ARG lets one syscall wait for either a CQE or the client deadline. */
    return (int)syscall(
        __NR_io_uring_enter,
        ring->fd,
        0,
        1,
        IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG,
        &arguments,
        sizeof(arguments)
    );
}

static int raw_ring_pop(raw_ring_t *ring, struct io_uring_cqe *completion) {
    unsigned head = __atomic_load_n(ring->cq_head, __ATOMIC_RELAXED);
    unsigned tail = __atomic_load_n(ring->cq_tail, __ATOMIC_ACQUIRE);
    if (head == tail) {
        return 0;
    }
    *completion = ring->completions[head & *ring->cq_mask];
    /* Release tells the kernel this CQ slot can be reused. */
    __atomic_store_n(ring->cq_head, head + 1u, __ATOMIC_RELEASE);
    return 1;
}

static int prepare_response(void *user) {
    io_uring_app_t *app = user;
    int length = snprintf(
        app->response,
        sizeof(app->response),
        "inventory response prepared by raw io_uring"
    );
    return length >= 0 && (size_t)length < sizeof(app->response)
        ? RCLIENT_OK
        : RCLIENT_ERR_IO;
}

static void on_admission(
    void *user,
    int status,
    const r_admission_outcome_t *outcome
) {
    io_uring_app_t *app = user;
    app->status = status;
    app->outcome = *outcome;
    if (status == RCLIENT_OK && outcome->allowed) {
        app->status = r_runtime_admission_run_and_report(
            &app->runtime,
            &app->request,
            prepare_response,
            app,
            &app->observed_latency_ms
        );
    }
    app->done = true;
}

static int arm_socket(io_uring_app_t *app, size_t index) {
    /* user_data is index + 1 because zero is reserved as an invalid tag. */
    return raw_ring_prep_poll(
        &app->ring,
        (int)r_runtime_socket_at(&app->runtime, index),
        index + 1u
    );
}

static int run_loop(io_uring_app_t *app) {
    size_t socket_count = r_runtime_socket_count(&app->runtime);
    for (size_t i = 0; i < socket_count; i++) {
        if (arm_socket(app, i) != 0) {
            return RCLIENT_ERR_IO;
        }
    }
    if (raw_ring_submit(&app->ring) != 0) {
        return RCLIENT_ERR_IO;
    }

    while (!app->done) {
        uint64_t delay_ms = 0;
        int status = r_runtime_admission_delay_ms(&app->request, &delay_ms);
        if (status != RCLIENT_OK) {
            return status;
        }
        int wait_result = raw_ring_wait(&app->ring, delay_ms);
        if (wait_result < 0 && errno == ETIME) {
            status = r_runtime_admission_on_timeout(&app->runtime, &app->request);
            if (status != RCLIENT_OK) {
                return status;
            }
            continue;
        }
        if (wait_result < 0 && errno == EINTR) {
            continue;
        }
        if (wait_result < 0) {
            return RCLIENT_ERR_IO;
        }

        struct io_uring_cqe completion;
        bool found_completion = false;
        while (raw_ring_pop(&app->ring, &completion)) {
            found_completion = true;
            if (completion.res < 0
                || completion.user_data == 0
                || completion.user_data > socket_count) {
                return RCLIENT_ERR_IO;
            }
            size_t index = (size_t)(completion.user_data - 1u);
            status = r_runtime_client_on_readable(
                &app->runtime,
                r_runtime_socket_at(&app->runtime, index)
            );
            if (status != RCLIENT_OK) {
                return status;
            }
            /* POLL_ADD is one-shot; re-arm after draining this descriptor. */
            if (!app->done && arm_socket(app, index) != 0) {
                return RCLIENT_ERR_IO;
            }
        }
        if (!found_completion || (!app->done && raw_ring_submit(&app->ring) != 0)) {
            return RCLIENT_ERR_IO;
        }
    }
    return RCLIENT_OK;
}

int main(void) {
    r_runtime_options_t options;
    if (r_runtime_options_from_env(&options) != RCLIENT_OK) {
        fprintf(stderr, "set RATELIMITLY_TENANT and RATELIMITLY_AUTH_KEY\n");
        return EXIT_FAILURE;
    }

    io_uring_app_t app = {0};
    app.ring.fd = -1;
    app.status = RCLIENT_ERR_IO;
    if (raw_ring_init(&app.ring, 8) != 0) {
        perror("io_uring_setup");
        return EXIT_FAILURE;
    }
    int status = r_runtime_client_init(&app.runtime, &options);
    if (status != RCLIENT_OK) {
        fprintf(stderr, "client initialization failed: %s (%d)\n",
            r_runtime_status_name(status), status);
    }
    if (status == RCLIENT_OK) {
        r_admission_config_t config;
        r_client_admission_config_defaults(&config);
        config.bucket_name = "io-uring-example";
        config.service_name = "io-uring-protected-service";
        config.metrics_label = "io-uring-example";
        status = r_client_admission_start(
            app.runtime.handle,
            &app.request,
            &config,
            on_admission,
            &app
        );
    }
    if (status == RCLIENT_OK) {
        status = run_loop(&app);
    }
    if (status != RCLIENT_OK && !app.done) {
        app.status = status;
    }

    if (app.request.active) {
        r_runtime_admission_cancel(&app.runtime, &app.request);
    }
    /* Tear down kernel poll requests before closing their target sockets. */
    raw_ring_destroy(&app.ring);
    r_runtime_client_destroy(&app.runtime);

    if (app.status != RCLIENT_OK) {
        fprintf(stderr, "rate-limit check failed: %s (%d)\n",
            r_runtime_status_name(app.status), app.status);
        return EXIT_FAILURE;
    }
    if (app.outcome.allowed) {
        printf("allowed: %s; latency=%" PRIu32 " ms\n",
            app.response, app.observed_latency_ms);
    } else if (app.outcome.rate_limited && app.outcome.latency_limited) {
        puts("denied: resource limit and latency guard");
    } else if (app.outcome.latency_limited) {
        puts("denied: latency guard");
    } else {
        puts("denied: resource rate limit");
    }
    return app.outcome.allowed ? EXIT_SUCCESS : 2;
}
