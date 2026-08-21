/*
 * ps3recomp - native-render ordered submission ring. See rsx_nr_ring.h.
 */

#include "rsx_nr_ring.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <intrin.h>
static inline long long nr_load64(const volatile long long* p)
{
    /* x64: aligned 64-bit loads are atomic; the compiler barrier orders the
     * subsequent payload reads (acquire on x86-TSO). */
    long long v = *p;
    _ReadWriteBarrier();
    return v;
}
static inline void nr_store64(volatile long long* p, long long v)
{
    _ReadWriteBarrier();   /* release: payload writes stay above the store */
    *p = v;
}
static inline void nr_add64(volatile long long* p, long long v)
{
    _InterlockedExchangeAdd64(p, v);
}
static inline void nr_max64(volatile long long* p, long long v)
{
    long long cur = *p;
    while (v > cur) {
        long long seen = _InterlockedCompareExchange64(p, v, cur);
        if (seen == cur)
            break;
        cur = seen;
    }
}
static inline long nr_load32(const volatile long* p)
{
    long v = *p;
    _ReadWriteBarrier();
    return v;
}
static inline int nr_cas32(volatile long* p, long expect, long desire)
{
    return _InterlockedCompareExchange(p, desire, expect) == expect;
}
#else
static inline long long nr_load64(const volatile long long* p)
{
    return __atomic_load_n((const long long*)p, __ATOMIC_ACQUIRE);
}
static inline void nr_store64(volatile long long* p, long long v)
{
    __atomic_store_n((long long*)p, v, __ATOMIC_RELEASE);
}
static inline void nr_add64(volatile long long* p, long long v)
{
    __atomic_fetch_add((long long*)p, v, __ATOMIC_ACQ_REL);
}
static inline void nr_max64(volatile long long* p, long long v)
{
    long long cur = __atomic_load_n((long long*)p, __ATOMIC_RELAXED);
    while (v > cur &&
           !__atomic_compare_exchange_n((long long*)p, &cur, v, 1,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        ;
}
static inline long nr_load32(const volatile long* p)
{
    return __atomic_load_n((const long*)p, __ATOMIC_ACQUIRE);
}
static inline int nr_cas32(volatile long* p, long expect, long desire)
{
    return __atomic_compare_exchange_n((long*)p, &expect, desire, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
#endif

static int is_pow2(u32 x) { return x && !(x & (x - 1)); }

int rsx_nr_ring_init(rsx_nr_ring* r, u32 op_cap, u32 side_cap)
{
    if (!is_pow2(op_cap) || !is_pow2(side_cap))
        return -1;
    memset(r, 0, sizeof(*r));
    r->slots = calloc(op_cap, sizeof(rsx_nr_slot));
    r->side = calloc(side_cap, 4);
    if (!r->slots || !r->side) {
        free(r->slots);
        free(r->side);
        memset(r, 0, sizeof(*r));
        return -1;
    }
    r->op_cap = op_cap;
    r->side_cap = side_cap;
    r->owns_storage = 1;
    return 0;
}

int rsx_nr_ring_init_fixed(rsx_nr_ring* r, rsx_nr_slot* slots, u32 op_cap,
                           u32* side, u32 side_cap)
{
    if (!is_pow2(op_cap) || !is_pow2(side_cap) || !slots || !side)
        return -1;
    memset(r, 0, sizeof(*r));
    r->slots = slots;
    r->side = side;
    r->op_cap = op_cap;
    r->side_cap = side_cap;
    return 0;
}

void rsx_nr_ring_destroy(rsx_nr_ring* r)
{
    if (r->owns_storage) {
        free(r->slots);
        free(r->side);
    }
    memset(r, 0, sizeof(*r));
}

/* ---- producer --------------------------------------------------------- */

int rsx_nr_ring_can_accept(const rsx_nr_ring* r, u32 ops, u32 side_words)
{
    const long long op_used = r->op_head - nr_load64(&r->op_tail);
    const long long side_used = r->side_head - nr_load64(&r->side_tail);
    /* A contiguous side reservation may need to pad to the ring edge; the
     * worst case wastes side_words-1 words, then the payload itself must
     * fit. Being conservative here is what makes push failure unreachable
     * in a pre-checked producer. */
    const long long side_need =
        (long long)side_words + (side_words ? side_words - 1 : 0);
    return op_used + ops <= (long long)r->op_cap &&
           side_used + r->pending_side + side_need <= (long long)r->side_cap;
}

u32 rsx_nr_ring_side_reserve(rsx_nr_ring* r, u32 count, u32** ptr)
{
    if (!count) {
        if (ptr)
            *ptr = NULL;
        return 0;
    }
    const u32 mask = r->side_cap - 1u;
    long long head = r->side_head + r->pending_side;
    u32 idx = (u32)head & mask;
    u32 pad = 0;
    if (idx + count > r->side_cap) {
        pad = r->side_cap - idx;          /* skip to the ring edge          */
        idx = 0;
    }
    const long long used = head + pad + count - nr_load64(&r->side_tail);
    if (used > (long long)r->side_cap) {
        r->reject_sticky = 1;
        nr_add64(&r->rejects, 1);
        if (ptr)
            *ptr = NULL;
        return ~0u;
    }
    r->pending_side += pad + count;
    nr_add64(&r->side_pad_words, pad);
    if (ptr)
        *ptr = r->side + idx;
    return idx;
}

int rsx_nr_ring_push(rsx_nr_ring* r, const rsx_nir_op* op)
{
    const long long head = r->op_head;
    if (head - nr_load64(&r->op_tail) >= (long long)r->op_cap) {
        r->reject_sticky = 1;
        nr_add64(&r->rejects, 1);
        return -1;
    }
    rsx_nr_slot* slot = &r->slots[(u32)head & (r->op_cap - 1u)];
    slot->op = *op;
    slot->side_take = r->pending_side;
    slot->seq = (u32)head;
    r->pending_side = 0;
    /* publish: payload above, then the head (release) */
    nr_store64(&r->side_head, r->side_head + slot->side_take);
    nr_store64(&r->op_head, head + 1);
    nr_add64(&r->pushes, 1);
    nr_max64(&r->op_high_water, head + 1 - nr_load64(&r->op_tail));
    return 0;
}

static int ring_sink_push(void* user, const rsx_nir_op* op)
{
    return rsx_nr_ring_push((rsx_nr_ring*)user, op);
}

static u32 ring_sink_side_push(void* user, const u32* words, u32 count)
{
    rsx_nr_ring* r = (rsx_nr_ring*)user;
    u32* dst = NULL;
    u32 ofs = rsx_nr_ring_side_reserve(r, count, &dst);
    if (ofs == ~0u)
        return ~0u;
    if (count && dst)
        memcpy(dst, words, (size_t)count * 4);
    nr_add64(&r->side_words, count);
    return ofs;
}

rsx_nir_sink rsx_nr_ring_sink(rsx_nr_ring* r)
{
    rsx_nir_sink k;
    k.user = r;
    k.push = ring_sink_push;
    k.side_push = ring_sink_side_push;
    return k;
}

int rsx_nr_ring_reject_sticky(const rsx_nr_ring* r)
{
    return r->reject_sticky != 0;
}

void rsx_nr_ring_clear_reject(rsx_nr_ring* r)
{
    r->reject_sticky = 0;
}

/* ---- consumer --------------------------------------------------------- */

const rsx_nr_slot* rsx_nr_ring_peek(rsx_nr_ring* r)
{
    const long long tail = r->op_tail;
    if (tail >= nr_load64(&r->op_head))
        return NULL;
    return &r->slots[(u32)tail & (r->op_cap - 1u)];
}

void rsx_nr_ring_pop(rsx_nr_ring* r)
{
    const long long tail = r->op_tail;
    if (tail >= nr_load64(&r->op_head))
        return;
    const rsx_nr_slot* slot = &r->slots[(u32)tail & (r->op_cap - 1u)];
    /* release the side words first? No: the consumer is done with the
     * payload by contract, and the producer only reuses the space after it
     * observes the bumped side_tail; order between the two stores is
     * irrelevant to a single consumer. */
    nr_store64(&r->side_tail, r->side_tail + slot->side_take);
    nr_store64(&r->op_tail, tail + 1);
    nr_add64(&r->pops, 1);
}

u32 rsx_nr_ring_depth(const rsx_nr_ring* r)
{
    long long d = nr_load64(&r->op_head) - nr_load64(&r->op_tail);
    return d > 0 ? (u32)d : 0;
}

/* ---- tokens ----------------------------------------------------------- */

void rsx_nr_tokens_init(rsx_nr_tokens* t)
{
    memset(t, 0, sizeof(*t));
}

void rsx_nr_tokens_signal(rsx_nr_tokens* t, u32 token, u32 value)
{
    if (token >= RSX_NR_MAX_TOKENS)
        return;
    volatile long* p = &t->value[token];
    for (;;) {
        long cur = nr_load32(p);
        /* wrapping (serial number) compare: only ever advance */
        if ((s32)(value - (u32)cur) <= 0)
            return;
        if (nr_cas32(p, cur, (long)value))
            return;
    }
}

int rsx_nr_tokens_satisfied(const rsx_nr_tokens* t, u32 token, u32 value)
{
    if (token >= RSX_NR_MAX_TOKENS)
        return 1;
    u32 cur = (u32)nr_load32(&t->value[token]);
    return (s32)(cur - value) >= 0;
}

u32 rsx_nr_tokens_value(const rsx_nr_tokens* t, u32 token)
{
    if (token >= RSX_NR_MAX_TOKENS)
        return 0;
    return (u32)nr_load32(&t->value[token]);
}
