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
/* A producer-publication hook for an exact jump-to-self stopper. Returning
 * one means the hook atomically proved and published a forward resume cursor;
 * zero leaves the stopper parked. This is not a generic skip facility. */
typedef int (*rsx_nr_frame_release_stopper_fn)(
    void* user, u32 get, u32 put, u32 command, u32* resume_get);
/* Exact producer record for an inline data island rooted at this command.
 * Return 1 for the recorded source edge or 2 when this command is the exact
 * payload start reached before that preceding edge became visible. Both
 * results supply the only safe cursor after the island; payload must never be
 * executed. */
typedef int (*rsx_nr_frame_island_edge_fn)(
    void* user, u32 get, u32 put, u32 command, u32* resume_get);
/* Exact producer proof for a forward stopper release whose target happens to
 * be a generated-block boundary word.  This is the only exception to the
 * conservative rule that packet-shaped data at such a boundary is not ready.
 * The producer record must have been published after the dependent bytes. */
typedef int (*rsx_nr_frame_released_edge_fn)(
    void* user, u32 get, u32 put, u32 command,
    u32 target, u32 target_word);
/* Fail-closed repair for one exact published JUMP whose target still starts
 * with non-command payload.  The hook may return a replacement cursor only
 * after proving the complete producer chain and atomically replacing the
 * source JUMP. Return 1 for a proven patched-link repair, 0 for a definitive mismatch in
 * this unchanged source/command/target/word generation, or -1 when an exact
 * producer shape is present but its dependent bytes are still publishing.
 * Pending results are reconsidered only after another bounded proof delay. */
typedef int (*rsx_nr_frame_resolve_jump_fn)(
    void* user, u32 get, u32 put, u32 command, u32 target,
    u32 target_word, u32* resume_get);
/* Equivalent fail-closed proof for a primary-ring cursor parked on an inline
 * generated-data gap, with the same 1/0/-1 result contract. The hook may
 * return a cursor only for the first structurally proven command prologue
 * following that exact unchanged word. previous_get/previous_command name
 * the packet the owner itself executed immediately before this cursor; they
 * are UINT32_MAX/0 when no such sequential provenance exists. */
typedef int (*rsx_nr_frame_resolve_hole_fn)(
    void* user, u32 get, u32 put, u32 word,
    u32 previous_get, u32 previous_command, u32* resume_get);

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
    RSX_NR_FRAME_FAILURE_ISLAND_EDGE,
} rsx_nr_frame_failure_kind;

typedef struct rsx_nr_frame_failure {
    u32 kind;
    u32 get;
    u32 put;
    u32 call_return;
    u32 command;
    u32 method;
    u32 argument;
    u32 argument_index;
    unsigned long long frame;
} rsx_nr_frame_failure;

#define RSX_NR_FRAME_BREADCRUMB_COUNT 8u
typedef struct rsx_nr_frame_breadcrumb {
    u32 get;
    u32 put;
    u32 call_return;
    u32 command;
} rsx_nr_frame_breadcrumb;

typedef struct rsx_nr_frame_flow_origin {
    u32 get;
    u32 command;
    u32 target;
    u32 return_before;
    u32 return_after;
    unsigned long long sequence;
} rsx_nr_frame_flow_origin;

typedef struct rsx_nr_frame_owner_stats {
    unsigned long long steps;
    unsigned long long packets;
    unsigned long long methods;
    unsigned long long control_words;
    unsigned long long waits_empty;
    unsigned long long waits_partial;
    unsigned long long waits_stopper;
    unsigned long long released_stoppers;
    unsigned long long skipped_data_islands;
    unsigned long long recovered_late_island_entries;
    unsigned long long admitted_released_boundaries;
    unsigned long long repaired_generated_links;
    unsigned long long repaired_generated_holes;
    unsigned long long generated_link_attempts;
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
    rsx_nr_frame_release_stopper_fn release_stopper;
    void* release_stopper_user;
    rsx_nr_frame_island_edge_fn island_edge;
    void* island_edge_user;
    rsx_nr_frame_released_edge_fn released_edge;
    void* released_edge_user;
    rsx_nr_frame_resolve_jump_fn resolve_jump;
    void* resolve_jump_user;
    rsx_nr_frame_resolve_hole_fn resolve_hole;
    void* resolve_hole_user;

    u32 fatal;
    u32 packet_active;
    u32 method_inflight;
    u32 packet_get;
    u32 packet_put;
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
    u32 breadcrumb_head;
    u32 breadcrumb_count;
    u32 breadcrumb_last_get;
    u32 breadcrumb_last_return;
    u32 breadcrumb_last_command;
    u32 flow_wait_source;
    u32 flow_wait_target;
    u32 flow_wait_word;
    u32 flow_wait_polls;
    u32 flow_wait_put;
    u32 flow_wait_put_polls;
    u32 flow_wait_limit;
    u32 repair_attempt_valid;
    u32 repair_attempt_kind;
    u32 repair_attempt_source;
    u32 repair_attempt_put;
    u32 repair_attempt_command;
    u32 repair_attempt_target;
    u32 repair_attempt_word;
    u32 primary_segment_bytes;
    u32 generated_block_bytes;
    unsigned long long method_errors_before;

    rsx_nr_frame_failure failure;
    rsx_nr_frame_flow_origin flow_origin;
    rsx_nr_frame_breadcrumb breadcrumbs[RSX_NR_FRAME_BREADCRUMB_COUNT];
    rsx_nr_frame_owner_stats stats;
} rsx_nr_frame_owner;

void rsx_nr_frame_owner_init(rsx_nr_frame_owner* owner,
                             rsx_nir_adapter* adapter,
                             rsx_nr_backend* backend,
                             rsx_nr_ring* ring,
                             rsx_nr_frame_read32_fn read32,
                             void* read_user,
                             rsx_nr_frame_release_stopper_fn release_stopper,
                             void* release_stopper_user,
                             rsx_nr_frame_island_edge_fn island_edge,
                             void* island_edge_user,
                             rsx_nr_frame_released_edge_fn released_edge,
                             void* released_edge_user,
                             rsx_nr_frame_resolve_jump_fn resolve_jump,
                             void* resolve_jump_user,
                             rsx_nr_frame_resolve_hole_fn resolve_hole,
                             void* resolve_hole_user);

rsx_nr_frame_step_result rsx_nr_frame_owner_step(
    rsx_nr_frame_owner* owner, u32 get, u32 put, u32 call_return,
    u32* next_get, u32* next_return);

#ifdef __cplusplus
}
#endif
#endif
