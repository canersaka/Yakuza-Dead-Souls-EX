/*
 * ps3recomp - native-render ordered submission ring.
 *
 * The live transport between the producer-interception layer and the
 * native renderer backend: a fixed-capacity single-producer/single-consumer
 * ring of rsx_nir_op records plus a word ring for bulk payloads (vertex
 * program words, constant runs, draw batch lists, inline transfer data).
 * Allocation happens exactly once at init (or never, with caller storage);
 * production is allocation-free.
 *
 * Ordering: ops execute strictly in push order — the ring IS the submission
 * order, mirroring the guest FIFO's ordering guarantee. Cross-engine
 * dependencies (e.g. "SPU data ready") are expressed as TOKEN_WAIT ops
 * against the token table below, signaled from the producing engine's
 * completion path; the consumer must not execute past an unsatisfied
 * TOKEN_WAIT.
 *
 * Producer contract (the interception layer):
 *   1. Serialize producers externally (same discipline as FIFO PUT writes;
 *      one pusher at a time).
 *   2. Before emitting a command, check rsx_nr_ring_can_accept() with a
 *      worst-case op/side bound for the whole command. If it fails, DO NOT
 *      emit — fall back to the FIFO path for that command (counted, never
 *      silent).
 *   3. If a push is ever rejected anyway (backstop; the pre-check makes
 *      this unreachable), the ring records a sticky reject. The producer
 *      must then re-prime its emitter (state resync) and permanently fall
 *      back for the family; partially-flushed state groups are harmless
 *      because no action op follows them.
 *
 * Consumer contract (the native backend):
 *   peek -> execute -> pop. pop releases the op's side words. The side
 *   payload pointer stays valid until pop.
 */

#ifndef PS3RECOMP_RSX_NR_RING_H
#define PS3RECOMP_RSX_NR_RING_H

#include "rsx_nir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_nr_slot {
    rsx_nir_op op;
    u32 side_take;               /* side words released when this op pops  */
    u32 seq;                     /* low 32 bits of the submit sequence     */
} rsx_nr_slot;

typedef struct rsx_nr_ring {
    rsx_nr_slot* slots;          /* capacity = op_cap (power of two)       */
    u32* side;                   /* capacity = side_cap words (pow. of 2)  */
    u32 op_cap, side_cap;        /* masks are cap-1                        */
    u32 owns_storage;

    /* Absolute (non-wrapping) 64-bit cursors; masked on access. Producer
     * advances heads with release stores; consumer advances tails. Atomic
     * access is confined to the implementation (Interlocked/__atomic), so
     * this header stays includable from C and C++ alike. */
    volatile long long op_head;
    volatile long long op_tail;
    volatile long long side_head;
    volatile long long side_tail;

    /* producer-side scratch for the in-flight (not yet pushed) command */
    u32 pending_side;            /* side words reserved since last push    */

    /* counters (aggregate, no per-event logging) */
    volatile long long pushes;
    volatile long long pops;
    volatile long long side_words;
    volatile long long side_pad_words;
    volatile long long rejects;        /* backstop refusals (sticky flag)  */
    volatile long long op_high_water;
    u32 reject_sticky;
} rsx_nr_ring;

/* op_cap/side_cap must be powers of two. init allocates once (calloc);
 * init_fixed uses caller storage (slots[op_cap], side[side_cap]).
 * Returns 0 on success. */
int  rsx_nr_ring_init(rsx_nr_ring* r, u32 op_cap, u32 side_cap);
int  rsx_nr_ring_init_fixed(rsx_nr_ring* r, rsx_nr_slot* slots, u32 op_cap,
                            u32* side, u32 side_cap);
void rsx_nr_ring_destroy(rsx_nr_ring* r);

/* ---- producer side ---------------------------------------------------- */

/* 1 when at least `ops` op slots and `side_words` side words (plus the
 * worst-case wrap padding) are free. The capacity gate for one whole
 * command; call before emitting anything. */
int rsx_nr_ring_can_accept(const rsx_nr_ring* r, u32 ops, u32 side_words);

/* Reserve `count` contiguous side words. Returns the masked word offset to
 * store in the op payload and sets *ptr for the producer to fill, or ~0u
 * (sticky reject) when space is insufficient. The reservation is charged
 * to the next pushed op. */
u32 rsx_nr_ring_side_reserve(rsx_nr_ring* r, u32 count, u32** ptr);

/* Push one op, consuming every side word reserved since the previous push.
 * Returns 0, or -1 (sticky reject) when the ring is full. */
int rsx_nr_ring_push(rsx_nr_ring* r, const rsx_nir_op* op);

/* A sink emitting into the ring: side_push = reserve + copy, push = push.
 * Rejections surface through rsx_nr_ring_reject_sticky(). */
rsx_nir_sink rsx_nr_ring_sink(rsx_nr_ring* r);

/* Sticky backstop flag: any reserve/push refusal since the last clear.
 * The producer polls it at command boundaries. */
int  rsx_nr_ring_reject_sticky(const rsx_nr_ring* r);
void rsx_nr_ring_clear_reject(rsx_nr_ring* r);

/* ---- consumer side ---------------------------------------------------- */

/* Oldest unexecuted op, or NULL when the ring is empty. Valid until pop. */
const rsx_nr_slot* rsx_nr_ring_peek(rsx_nr_ring* r);

/* Retire the peeked op and release its side words. */
void rsx_nr_ring_pop(rsx_nr_ring* r);

/* Resolve a side offset stored in an op payload (valid until that op pops). */
static inline const u32* rsx_nr_ring_side_ptr(const rsx_nr_ring* r, u32 ofs)
{
    return r->side + (ofs & (r->side_cap - 1u));
}

/* Ops currently queued (consumer lag). */
u32 rsx_nr_ring_depth(const rsx_nr_ring* r);

/* ---------------------------------------------------------------------------
 * Native readiness tokens.
 *
 * A token is a monotonically increasing u32 value. TOKEN_SIGNAL publishes
 * value v (monotonic max, release); TOKEN_WAIT(v) is satisfied when the
 * table value is >= v (acquire), using serial-number (wrapping) compare.
 * Signals may arrive from any thread (SPU completion, PPU publication
 * paths); the table is multi-writer safe.
 * -----------------------------------------------------------------------*/

#define RSX_NR_MAX_TOKENS 64

typedef struct rsx_nr_tokens {
    volatile long value[RSX_NR_MAX_TOKENS];   /* u32 payloads              */
} rsx_nr_tokens;

void rsx_nr_tokens_init(rsx_nr_tokens* t);
void rsx_nr_tokens_signal(rsx_nr_tokens* t, u32 token, u32 value);
/* 1 when the token has reached `value` (wrapping >= compare). */
int  rsx_nr_tokens_satisfied(const rsx_nr_tokens* t, u32 token, u32 value);
u32  rsx_nr_tokens_value(const rsx_nr_tokens* t, u32 token);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_NR_RING_H */
