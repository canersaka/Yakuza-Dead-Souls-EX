/*
 * ps3recomp - strict-native producer-island / pass compiler (offline
 * foundation; see docs/HANA_ISLAND_COMPILER.md).
 *
 * Wraps the strict rsx_nr_frame_owner. Linear method-packet runs between
 * flow control and guest-visible dependencies (the established dependency
 * islands, one terminal action each) are fingerprinted over their invariant
 * structure, compiled once into a persistent template, and re-executed on
 * later occurrences with only: content validation, a register-truth resync,
 * derivation of the touched state groups from the (resynced) register file,
 * patching of dynamic values, and typed execution through the unchanged
 * rsx_nr_backend/stream interpreter. No adaptation (rsx_nir_adapter_method /
 * stage_state / emitter diffing) runs for a compiled island.
 *
 * Everything the compiler does not own — JUMP/CALL/RET, stoppers, data
 * islands, generated-block admission and repair, unsupported methods,
 * oversized islands, capacity exhaustion — is delegated to the wrapped
 * frame owner from the exact island start before any state is touched, so
 * refusal is atomic and the delegated path is byte-identical to the current
 * strict-native behavior. The compiler never falls back mid-island.
 *
 * Fixed memory only: the template arena, index, and scratch stream are
 * caller-provided at init and never grow. A full table or arena delegates
 * further new islands (counted) instead of allocating or evicting.
 */

#ifndef PS3RECOMP_RSX_NR_ISLAND_H
#define PS3RECOMP_RSX_NR_ISLAND_H

#include "rsx_nr_frame_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed per-island bounds. Islands beyond any bound delegate to the
 * wrapped owner before anything is touched. */
#define RSX_NR_ISLAND_MAX_WORDS       32768u
#define RSX_NR_ISLAND_MAX_BATCHES     4096u
#define RSX_NR_ISLAND_MAX_CONST_SLOTS 512u

typedef enum rsx_nr_island_delegate_reason {
    RSX_NR_ISLAND_DELEGATE_FLOW = 0,      /* control word / stopper at GET  */
    RSX_NR_ISLAND_DELEGATE_UNSUPPORTED,   /* method refused by the adapter  */
    RSX_NR_ISLAND_DELEGATE_GRAMMAR,       /* malformed/segment-tail/window  */
    RSX_NR_ISLAND_DELEGATE_CAPACITY,      /* extent/table/arena bounds      */
    RSX_NR_ISLAND_DELEGATE_STATE_ONLY,    /* boundary closed an empty run   */
    RSX_NR_ISLAND_DELEGATE_VALIDATION,    /* identity collision/mismatch    */
    RSX_NR_ISLAND_DELEGATE_REASON_COUNT
} rsx_nr_island_delegate_reason;

typedef struct rsx_nr_island_stats {
    unsigned long long steps;
    unsigned long long islands_hit;
    unsigned long long islands_compiled;      /* miss -> new template       */
    unsigned long long islands_recompiled;    /* fingerprint slot replaced  */
    unsigned long long islands_delegated[RSX_NR_ISLAND_DELEGATE_REASON_COUNT];
    unsigned long long delegated_steps;       /* wrapped-owner step calls   */
    unsigned long long methods_owned;         /* method words the compiler
                                                 consumed (hit + miss)      */
    unsigned long long methods_hit;           /* subset consumed via a hit  */
    unsigned long long adaptations_avoided;   /* = methods_hit: adapter
                                                 method calls not run       */
    unsigned long long actions_executed;
    unsigned long long groups_derived;
    unsigned long long constants_slots_patched;
    unsigned long long validation_mismatches; /* fingerprint collision or
                                                 racing content change      */
    unsigned long long generation_fast_hits;  /* validation skipped by an
                                                 unchanged content
                                                 generation                 */
    unsigned long long templates_live;
    unsigned long long template_arena_used;   /* bytes                      */
    unsigned long long invalidations;         /* invalidate_all calls       */
    /* tick buckets (caller clock; zero when no clock installed) */
    unsigned long long ticks_scan;            /* fingerprint + supported    */
    unsigned long long ticks_validate;        /* skeleton compare           */
    unsigned long long ticks_resync;          /* null-sink register truth   */
    unsigned long long ticks_derive_patch;    /* group derive + patching    */
    unsigned long long ticks_execute;         /* backend stream execution   */
    unsigned long long ticks_compile;         /* template construction      */
} rsx_nr_island_stats;

typedef struct rsx_nr_island_template rsx_nr_island_template;

typedef struct rsx_nr_island_compiler {
    rsx_nr_frame_owner* owner;      /* wrapped strict owner (flow + refusal) */

    /* fixed template storage */
    unsigned char* arena;
    u32 arena_cap;                  /* bytes                                 */
    u32 arena_used;                 /* bump offset                           */
    u32* index_fp_lo;               /* open-addressed fingerprint index      */
    u32* index_fp_hi;
    u32* index_ofs;                 /* arena offset + 1; 0 = empty slot      */
    u32 index_cap;                  /* power of two                          */
    u32 index_live;

    /* fixed execution scratch (one island) */
    rsx_nir_stream scratch;         /* fixed-mode stream built per island    */
    rsx_nir_op* scratch_ops;
    u32* scratch_side;
    u32 scratch_op_cap;
    u32 scratch_side_cap;

    /* in-flight island execution (blocked-acquire resume). The scratch
     * stream and these fields alone carry the retained island, so
     * invalidate_all may drop every template without abandoning or
     * re-executing work already sent to the backend. */
    u32 exec_active;
    u32 exec_pos;
    u32 exec_get;                   /* island start GET                      */
    u32 exec_ret;
    u32 exec_next_get;              /* GET after the island                  */
    u32 exec_action_kind;           /* finish-action bookkeeping             */
    u32 exec_action_a;

    /* delegated-run bookkeeping: while nonzero, every step forwards to the
     * wrapped owner until its decode state is idle again, keeping refusal
     * atomic at island granularity. */
    u32 delegate_active;

    /* optional content-generation fast path: the embedder bumps this when
     * any byte under the primary ring / generated windows may have changed.
     * A hit whose template was validated under the current generation skips
     * the invariant compare (offline tests exercise both paths). */
    u32 content_generation;

    rsx_nr_frame_now_ticks_fn now_ticks;
    void* clock_user;

    /* Scan-progress cache across WAIT verdicts: published FIFO words in
     * [get, put) are immutable under the ring contract, so a half-scanned
     * island resumes instead of rescanning when more words publish. The
     * payload is the module-private island_scan record. */
    u32 scan_resume_valid;
    u32 scan_resume_get;
    unsigned char scan_resume_raw[192];

    /* Per-method property table built once at init from the module's
     * classification authorities (dynamic table, group map, graph boundary,
     * adapter support class); one load replaces four calls in the scan. */
    u32 method_props[0x4000];

    /* fixed scan/patch scratch (pure pass-1 output; no allocation ever) */
    u32 word_buf[RSX_NR_ISLAND_MAX_WORDS];
    u32 dyn_mask[RSX_NR_ISLAND_MAX_WORDS / 32u];
    u32 resync_list[RSX_NR_ISLAND_MAX_WORDS * 2u];
    u32 const_slots[RSX_NR_ISLAND_MAX_CONST_SLOTS];
    u32 resolved_const_count;
    u32 batch_words[RSX_NR_ISLAND_MAX_BATCHES];
    u32 batch_pairs[RSX_NR_ISLAND_MAX_BATCHES * 2u];
    u32 batch_indexed;

    rsx_nr_island_stats stats;
} rsx_nr_island_compiler;

/* All storage is caller-owned and fixed for the compiler's lifetime.
 * index_cap must be a power of two. Returns 0 on success. */
int rsx_nr_island_compiler_init(
    rsx_nr_island_compiler* ic, rsx_nr_frame_owner* owner,
    unsigned char* arena, u32 arena_bytes,
    u32* index_fp_lo, u32* index_fp_hi, u32* index_ofs, u32 index_cap,
    rsx_nir_op* scratch_ops, u32 scratch_op_cap,
    u32* scratch_side, u32 scratch_side_cap);

void rsx_nr_island_compiler_set_clock(
    rsx_nr_island_compiler* ic, rsx_nr_frame_now_ticks_fn now_ticks,
    void* user);

/* Drop every template and any in-flight island state. Safe (and required)
 * at reset, movie-ownership handoff, and shutdown. The wrapped owner and
 * backend are not touched. */
void rsx_nr_island_compiler_invalidate_all(rsx_nr_island_compiler* ic);

/* Publish that island content bytes may have changed (any guest write into
 * the command windows). Templates stay resident; the next hit revalidates
 * content instead of using the generation fast path. */
static inline void rsx_nr_island_compiler_note_content_write(
    rsx_nr_island_compiler* ic)
{
    ic->content_generation++;
}

/* Step the compiler exactly like rsx_nr_frame_owner_step. The wrapped
 * owner's failure/stats surfaces remain authoritative for delegated work. */
rsx_nr_frame_step_result rsx_nr_island_compiler_step(
    rsx_nr_island_compiler* ic, u32 get, u32 put, u32 call_return,
    u32* next_get, u32* next_return);

/* True when the word classifies as a dynamic value for method `method`
 * (exposed for tests; the table is the single classification authority). */
int rsx_nr_island_method_arg_is_dynamic(u32 method);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_NR_ISLAND_H */
