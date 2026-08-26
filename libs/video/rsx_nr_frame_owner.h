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
#include "rsx_nr_graph.h"
#include "rsx_nr_producer_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*rsx_nr_frame_read32_fn)(void* user, u32 io, u32* value);
typedef unsigned long long (*rsx_nr_frame_now_ms_fn)(void* user);
typedef unsigned long long (*rsx_nr_frame_now_ticks_fn)(void* user);
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
    /* Internal boundary consumed by the single-pass wrapper. */
    RSX_NR_FRAME_GRAPH_BOUNDARY,
} rsx_nr_frame_step_result;

typedef enum rsx_nr_frame_graph_mode {
    RSX_NR_FRAME_GRAPH_DISABLED = 0,
    RSX_NR_FRAME_GRAPH_PASSIVE,
    RSX_NR_FRAME_GRAPH_EXECUTE,
} rsx_nr_frame_graph_mode;

typedef enum rsx_nr_frame_graph_fallback_reason {
    RSX_NR_FRAME_GRAPH_FB_CAPACITY = 0,
    RSX_NR_FRAME_GRAPH_FB_UNSUPPORTED_METHOD,
    RSX_NR_FRAME_GRAPH_FB_EXECUTION,
    RSX_NR_FRAME_GRAPH_FB_REASON_COUNT,
} rsx_nr_frame_graph_fallback_reason;

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

typedef struct rsx_nr_frame_graph_stats {
    unsigned long long calls;
    unsigned long long islands;
    unsigned long long methods;
    unsigned long long ops;
    unsigned long long side_words;
    unsigned long long frames;
    unsigned long long passive_islands;
    unsigned long long passive_equivalent;
    unsigned long long passive_mismatches;
    unsigned long long construction_ticks;
    unsigned long long fallback[RSX_NR_FRAME_GRAPH_FB_REASON_COUNT];
    unsigned long long max_methods;
    unsigned long long max_ops;
    unsigned long long max_side_words;
} rsx_nr_frame_graph_stats;

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
    rsx_nr_frame_now_ms_fn publication_now_ms;
    void* publication_clock_user;
    unsigned long long flow_wait_started_ms;
    unsigned long long flow_wait_cached_ms;
    unsigned long long flow_wait_next_proof_ms;
    u32 flow_wait_clock_next_poll;
    u32 publication_proof_delay_ms;
    u32 publication_failure_delay_ms;
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

    /* Optional fixed-memory single-pass dependency-island recorder.  In
     * execute mode the adapter emits directly into graph_stream; GET is not
     * exposed until the retained island has executed or failed closed. */
    rsx_nir_stream* graph_stream;
    u32 graph_mode;
    u32 graph_exec_pos;
    u32 graph_execution_pending;
    u32 graph_method_start_seen;
    u32 graph_boundary_after_method;
    u32 graph_internal_active;
    u32 graph_cursor_get;
    u32 graph_cursor_ret;
    u32 graph_external_get;
    u32 graph_external_ret;
    u32 graph_yield_after_packet;
    u32 graph_passive_source_ops;
    u32 graph_passive_source_side;
    unsigned long long graph_passive_source_hash;
    unsigned long long graph_started_ticks;
    rsx_nr_frame_now_ticks_fn graph_now_ticks;
    void* graph_clock_user;
    unsigned long long graph_tick_frequency;
    rsx_nr_frame_graph_stats graph_stats;

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

/* Give publication waits a host wall clock so their proof and failure bounds
 * do not depend on how quickly one CPU can spin the consumer.  The clock is
 * sampled only once per fixed poll batch.  A null clock preserves the
 * deterministic poll-bounded behavior used by standalone/offline callers. */
void rsx_nr_frame_owner_set_publication_clock(
    rsx_nr_frame_owner* owner, rsx_nr_frame_now_ms_fn now_ms,
    void* user, u32 proof_delay_ms, u32 failure_delay_ms);

/* Bind a caller-owned fixed stream as the consume-once graph arena.  The
 * scanner graph is unrelated and remains disabled.  PASSIVE preserves the
 * established immediate ring path while proving byte/order equivalence;
 * EXECUTE records once and executes complete islands directly from stream. */
void rsx_nr_frame_owner_set_single_pass_graph(
    rsx_nr_frame_owner* owner, u32 mode, rsx_nir_stream* stream,
    rsx_nr_frame_now_ticks_fn now_ticks, void* clock_user,
    unsigned long long tick_frequency);

rsx_nr_frame_step_result rsx_nr_frame_owner_step(
    rsx_nr_frame_owner* owner, u32 get, u32 put, u32 call_return,
    u32* next_get, u32* next_return);

#ifdef __cplusplus
}
#endif
#endif
