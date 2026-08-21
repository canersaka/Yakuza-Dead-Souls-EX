/*
 * ps3recomp - native-render producer interception layer.
 *
 * The producer-level boundary of the native graphics path: recognized PPU
 * (and, later, SPU-publication) graphics producers call the typed rsx_nr_try_*
 * entry points INSTEAD of encoding guest RSX packets. Every entry point
 * follows one contract:
 *
 *   return 1  -> the operation was emitted as typed native commands into the
 *                submission ring; the caller MUST NOT write the guest packet.
 *   return 0  -> the caller MUST run the existing FIFO packet path for this
 *                operation, unchanged. The refusal is counted by family and
 *                reason. Nothing is ever silently discarded: an operation is
 *                either in the ring or in the FIFO, never neither.
 *
 * Mixed-mode ordering. The ring is the master submission order. When an
 * operation falls back, the layer emits a FALLBACK(ENTER) marker into the
 * ring followed by a TOKEN_WAIT on the reserved FIFO-drain token; the caller
 * then publishes the guest packet as usual. The live FIFO consumer signals
 * the drain token (rsx_nr_intercept_fifo_drained) once it has consumed up to
 * the producer's recorded PUT, which releases the native consumer to run
 * commands emitted after the fallback episode. A new native command after
 * fallback emits FALLBACK(EXIT) first. Offline tests drive the same protocol
 * with a simulated consumer.
 *
 * State coherence. The native emitter must always know the full pipeline
 * state, even when only some families are intercepted: state that still
 * travels as FIFO packets is mirrored into the same emitter through the
 * decode-side shadow (rsx_nr_intercept_shadow_method — a tap at
 * yz_rsx_method). Interception without the shadow armed is refused for
 * state-dependent families (draw/clear/transfer).
 *
 * Selection. YZ_NR_INTERCEPT selects families: a comma-separated list of
 * flip,clear,state,draw,program,transfer,semaphore,report,user,sync — or
 * "all" (or "1"). Unset/empty/0 = fully disabled (default): every try_*
 * returns 0 with reason DISABLED and the shadow stays inert, so the live
 * renderer is byte-for-byte unaffected.
 *
 * Counters are aggregate only (one struct, one formatter); there is no
 * per-event logging anywhere in this layer.
 */

#ifndef PS3RECOMP_RSX_NR_INTERCEPT_H
#define PS3RECOMP_RSX_NR_INTERCEPT_H

#include "rsx_nir_adapter.h"
#include "rsx_nr_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Command families (selectable independently) */
enum rsx_nr_family {
    RSX_NR_FAM_FLIP = 0,   /* flip packet family (P3 func_02108948)        */
    RSX_NR_FAM_CLEAR,      /* clear surface                                */
    RSX_NR_FAM_STATE,      /* state setters / surface / viewport blocks    */
    RSX_NR_FAM_DRAW,       /* draw arrays / index / inline-array wrappers  */
    RSX_NR_FAM_PROGRAM,    /* VP upload + transform-constant bursts        */
    RSX_NR_FAM_TRANSFER,   /* NV0039 / NV3062+NV308A / NV3089              */
    RSX_NR_FAM_SEMAPHORE,  /* NV406E + NV4097 semaphore/label operations   */
    RSX_NR_FAM_REPORT,     /* GET_REPORT / CLEAR_REPORT_VALUE              */
    RSX_NR_FAM_USER,       /* 0xEB00 user command                          */
    RSX_NR_FAM_SYNC,       /* SET_REFERENCE, WAIT_FOR_IDLE                 */
    RSX_NR_FAM_COUNT
};

/* Fallback reasons (per-family counters) */
enum rsx_nr_fb_reason {
    RSX_NR_FB_DISABLED = 0, /* family not selected                         */
    RSX_NR_FB_CAPACITY,     /* ring capacity pre-check failed              */
    RSX_NR_FB_UNSUPPORTED,  /* recognized producer, unsupported arguments  */
    RSX_NR_FB_UNKNOWN,      /* unrecognized producer or command shape      */
    RSX_NR_FB_DYNAMIC,      /* dynamic/SPU-patched content (class C)       */
    RSX_NR_FB_NO_SHADOW,    /* state-dependent family without the shadow   */
    RSX_NR_FB_REJECT,       /* ring reject backstop tripped (loud)         */
    RSX_NR_FB_REASON_COUNT
};

/* Reserved token ids (rsx_nr_tokens) */
#define RSX_NR_TOKEN_FIFO_DRAIN 0   /* FIFO consumer progress at fallback   */
#define RSX_NR_TOKEN_FIRST_FREE 8   /* first id free for semantic tokens    */

typedef struct rsx_nr_stats {
    unsigned long long native_ops[RSX_NR_FAM_COUNT];
    unsigned long long fallbacks[RSX_NR_FAM_COUNT][RSX_NR_FB_REASON_COUNT];
    unsigned long long shadow_methods;      /* FIFO methods mirrored        */
    unsigned long long fallback_episodes;   /* ENTER markers emitted        */
    unsigned long long equivalence_failures;/* fed by the offline harness   */
} rsx_nr_stats;

typedef struct rsx_nr_intercept {
    rsx_nr_ring* ring;
    rsx_nr_tokens* tokens;
    rsx_nir_adapter shadow;      /* decode-side mirror + the shared emitter */
    u32 families;                /* enabled-family bit mask                 */
    int shadow_armed;
    int in_fallback;             /* current ownership: 0 native, 1 FIFO     */
    u32 drain_seq;               /* value the drain token must reach        */
    rsx_nr_stats stats;
} rsx_nr_intercept;

/* Parse YZ_NR_INTERCEPT (see header comment). Exposed for tests. */
u32 rsx_nr_parse_families(const char* spec);

/* Initialize against a ring + token table. `families` normally comes from
 * rsx_nr_parse_families(getenv("YZ_NR_INTERCEPT")). `arm_shadow` mirrors
 * the FIFO method stream into the emitter (required for draw/clear/
 * transfer interception). Returns 0. */
int rsx_nr_intercept_init(rsx_nr_intercept* it, rsx_nr_ring* ring,
                          rsx_nr_tokens* tokens, u32 families,
                          int arm_shadow);

static inline int rsx_nr_family_enabled(const rsx_nr_intercept* it, u32 fam)
{
    return (it->families >> fam) & 1u;
}

/* ---- decode-side shadow (tap at yz_rsx_method) ------------------------- */

/* Mirror one FIFO method into the shared emitter WITHOUT emitting native
 * actions for it (the FIFO path owns execution of this command). Cheap
 * no-op when the shadow is unarmed. */
void rsx_nr_intercept_shadow_method(rsx_nr_intercept* it, u32 method, u32 arg);

/* ---- consumer-side ordering signal ------------------------------------- */

/* The FIFO consumer (or the offline simulator) reports it has drained all
 * guest commands belonging to fallback episodes up to and including
 * `episode` (the value carried by the matching TOKEN_WAIT). */
void rsx_nr_intercept_fifo_drained(rsx_nr_intercept* it, u32 episode);

/* ---- producer-side typed entry points ---------------------------------- */

/* Flip family. Mirrors the packet contract: buffer_id < 8, optional
 * ordered label acquire before presentation. */
int rsx_nr_try_flip(rsx_nr_intercept* it, u32 buffer_id,
                    int wait_for_label, u32 label_index, u32 label_value);

/* Clear, using the shadow's current decoded state for the clear values
 * already staged (mask semantics identical to CLEAR_BUFFERS). */
int rsx_nr_try_clear(rsx_nr_intercept* it, u32 mask, u32 color_value,
                     u32 depth_value, u32 stencil_value);

/* Draw: batches = {first,count} pairs. The pipeline state seen by the
 * draw is whatever the shadow + typed setters have staged. */
int rsx_nr_try_draw(rsx_nr_intercept* it, u32 primitive, u32 indexed,
                    const u32* batches, u32 batch_count);

/* Transfers (typed rsx_nir_transfer; words = inline payload or NULL). */
int rsx_nr_try_transfer(rsx_nr_intercept* it, const rsx_nir_transfer* t,
                        const u32* words);

int rsx_nr_try_semaphore_release(rsx_nr_intercept* it, u32 dma_context,
                                 u32 offset, u32 value, u32 texture_read);
int rsx_nr_try_semaphore_acquire(rsx_nr_intercept* it, u32 dma_context,
                                 u32 offset, u32 value);
int rsx_nr_try_report(rsx_nr_intercept* it, u32 kind, u32 arg, u32 dma_report);
int rsx_nr_try_user_command(rsx_nr_intercept* it, u32 cause);
int rsx_nr_try_set_reference(rsx_nr_intercept* it, u32 value);
int rsx_nr_try_barrier(rsx_nr_intercept* it, u32 kind);

/* Native readiness tokens for understood guest dependencies (e.g. the
 * 0xFE0 movie-decode label). wait emits TOKEN_WAIT; the producing engine
 * signals through rsx_nr_tokens_signal on its completion path. Token ids
 * must be >= RSX_NR_TOKEN_FIRST_FREE. */
int rsx_nr_try_token_wait(rsx_nr_intercept* it, u32 token, u32 value);
int rsx_nr_try_token_signal(rsx_nr_intercept* it, u32 token, u32 value);

/* ---- bookkeeping for the FIFO path ------------------------------------- */

/* The caller ran the FIFO path for an operation of `fam` for `reason`
 * (called automatically by every try_* that returns 0; exposed for
 * producers that skip try_* entirely, e.g. unknown wrappers). Emits the
 * FALLBACK(ENTER) ordering marker on the first FIFO command of an episode
 * when any family is active. */
void rsx_nr_note_fallback(rsx_nr_intercept* it, u32 fam, u32 reason);

/* ---- counters ---------------------------------------------------------- */

void rsx_nr_intercept_get_stats(const rsx_nr_intercept* it, rsx_nr_stats* out);
void rsx_nr_intercept_note_equivalence_failure(rsx_nr_intercept* it);

/* One compact aggregate line (no per-event output anywhere):
 * "nr: native f=1 c=2 ... | fb d=... cap=... unk=... | shadow=... eps=..." */
u32 rsx_nr_stats_format(const rsx_nr_stats* st, char* buf, u32 buf_size);

/* Shutdown-only passive-shadow census.  This scans only the adapter's fixed
 * register metadata and formats one compact aggregate: decoded state/exec
 * writes, stored-only writes, unclassified FIFO-engine writes, and the most
 * frequent stored-only methods.  It performs no allocation or I/O. */
u32 rsx_nr_shadow_census_format(const rsx_nr_intercept* it,
                                char* buf, u32 buf_size);

/* Test/diagnostic helper: the exact packet-path FIFO words for a flip
 * (the committed spec this layer intercepts; from the retired HLE body
 * yz_gcm_append_flip_commands, functional match of lifted func_02108948).
 * Returns the word count (4 or 10). */
u32 rsx_nr_flip_packet_spec(u32* out, u32 buffer_id, int wait_for_label,
                            u32 label_index, u32 label_value);

/* Semaphore DMA selector for the label/report window. */
#define RSX_NR_DMA_SEMAPHORE_RW 0x66616661u

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_NR_INTERCEPT_H */
