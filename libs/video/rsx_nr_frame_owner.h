/*
 * Strict native RSX frame owner.
 *
 * This is the consume-once FIFO front end used by the full-native runtime.
 * It never invokes or falls back to the legacy renderer.  Each published
 * method is validated against the current typed state, translated exactly
 * once, and drained through rsx_nr_backend before GET may advance.  A blocked
 * acquire retains the in-flight typed op; retries only re-step that op.
 *
 * Unsupported or malformed input is a bounded development failure.  The
 * first exact command/method/value is retained in memory and the owner stays
 * fatal.  It is deliberately not an admission scanner.
 */

#ifndef PS3RECOMP_RSX_NR_FRAME_OWNER_H
#define PS3RECOMP_RSX_NR_FRAME_OWNER_H

#include "rsx_nir_adapter.h"
#include "rsx_nr_backend.h"
#include "rsx_nr_producer_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*rsx_nr_frame_read32_fn)(void* user, u32 io, u32* value);

typedef enum rsx_nr_frame_step_result {
    RSX_NR_FRAME_ADVANCED = 0,
    RSX_NR_FRAME_WAIT_EMPTY,
    RSX_NR_FRAME_WAIT_PARTIAL,
    RSX_NR_FRAME_WAIT_STOPPER,
    RSX_NR_FRAME_WAIT_SEMAPHORE,
    RSX_NR_FRAME_FATAL,
} rsx_nr_frame_step_result;

typedef enum rsx_nr_frame_failure_kind {
    RSX_NR_FRAME_FAILURE_NONE = 0,
    RSX_NR_FRAME_FAILURE_UNMAPPED,
    RSX_NR_FRAME_FAILURE_BAD_FLOW,
    RSX_NR_FRAME_FAILURE_UNSUPPORTED_METHOD,
    RSX_NR_FRAME_FAILURE_RING_CAPACITY,
    RSX_NR_FRAME_FAILURE_EXECUTION,
    RSX_NR_FRAME_FAILURE_CURSOR_CHANGED,
} rsx_nr_frame_failure_kind;

typedef struct rsx_nr_frame_failure {
    u32 kind;
    u32 get;
    u32 call_return;
    u32 command;
    u32 method;
    u32 argument;
    u32 argument_index;
    unsigned long long frame;
} rsx_nr_frame_failure;

typedef struct rsx_nr_frame_owner_stats {
    unsigned long long steps;
    unsigned long long packets;
    unsigned long long methods;
    unsigned long long control_words;
    unsigned long long waits_empty;
    unsigned long long waits_partial;
    unsigned long long waits_stopper;
    unsigned long long waits_semaphore;
    unsigned long long frames;
    unsigned long long backend_ops;
} rsx_nr_frame_owner_stats;

typedef struct rsx_nr_frame_owner {
    rsx_nir_adapter* adapter;
    rsx_nr_backend* backend;
    rsx_nr_ring* ring;
    rsx_nr_frame_read32_fn read32;
    void* read_user;

    u32 fatal;
    u32 packet_active;
    u32 method_inflight;
    u32 packet_get;
    u32 packet_ret;
    u32 packet_command;
    u32 packet_count;
    u32 packet_index;
    u32 packet_method;
    u32 packet_argument;
    u32 packet_non_incrementing;
    u32 packet_next_get;
    u32 packet_next_ret;
    u32 control_streak;
    unsigned long long method_errors_before;

    rsx_nr_frame_failure failure;
    rsx_nr_frame_owner_stats stats;
} rsx_nr_frame_owner;

void rsx_nr_frame_owner_init(rsx_nr_frame_owner* owner,
                             rsx_nir_adapter* adapter,
                             rsx_nr_backend* backend,
                             rsx_nr_ring* ring,
                             rsx_nr_frame_read32_fn read32,
                             void* read_user);

rsx_nr_frame_step_result rsx_nr_frame_owner_step(
    rsx_nr_frame_owner* owner, u32 get, u32 put, u32 call_return,
    u32* next_get, u32* next_return);

#ifdef __cplusplus
}
#endif
#endif
