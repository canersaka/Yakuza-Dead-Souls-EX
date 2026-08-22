#include "rsx_nr_span_router.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum {
    SPAN_EMPTY = 0,
    SPAN_WRITING,
    SPAN_READY,
    SPAN_CLAIMED,
    SPAN_TOMBSTONE,
};

enum {
    SPAN_C_PUBLISHED = 0,
    SPAN_C_CLAIMED,
    SPAN_C_FAST_MISS,
    SPAN_C_EXACT_MISS,
    SPAN_C_NOT_READY,
    SPAN_C_DUPLICATE,
    SPAN_C_BUSY,
    SPAN_C_FULL,
    SPAN_C_CORRUPT,
};

#define SPAN_GUEST_PAGE_COUNT (1u << 20)

typedef struct span_slot {
    _Atomic u32 state;
    rsx_nr_span span;
} span_slot;

typedef _Atomic u32 span_page_count;
typedef _Atomic long long span_counter;

static u32 span_hash(u32 ea, u32 generation)
{
    u32 x = (ea >> 2) ^ (generation * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    return x ^ (x >> 16);
}

static void span_count(rsx_nr_span_router* r, u32 index)
{
    if (!r->count_misses &&
        (index == SPAN_C_FAST_MISS || index == SPAN_C_EXACT_MISS))
        return;
    atomic_fetch_add_explicit((span_counter*)&r->counters[index], 1,
                              memory_order_relaxed);
}

static u32 fnv_bytes(u32 hash, const void* data, size_t size)
{
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

u32 rsx_nr_span_fingerprint(const rsx_nr_span* span)
{
    if (!span || span->payload.op_count > RSX_NR_SPAN_MAX_OPS ||
        span->payload.side_count > RSX_NR_SPAN_MAX_SIDE)
        return 0;
    u32 hash = 2166136261u;
    hash = fnv_bytes(hash, &span->ea, sizeof(span->ea));
    hash = fnv_bytes(hash, &span->word_count, sizeof(span->word_count));
    hash = fnv_bytes(hash, &span->generation, sizeof(span->generation));
    hash = fnv_bytes(hash, &span->payload.op_count,
                     sizeof(span->payload.op_count));
    hash = fnv_bytes(hash, &span->payload.side_count,
                     sizeof(span->payload.side_count));
    hash = fnv_bytes(hash, span->payload.ops,
                     span->payload.op_count * sizeof(span->payload.ops[0]));
    hash = fnv_bytes(hash, span->payload.side,
                     span->payload.side_count * sizeof(span->payload.side[0]));
    return hash ? hash : 1u;
}

int rsx_nr_span_router_init(rsx_nr_span_router* r, u32 capacity)
{
    if (!r || capacity < 8u || (capacity & (capacity - 1u)))
        return -1;
    memset(r, 0, sizeof(*r));
    r->slots = calloc(capacity, sizeof(span_slot));
    r->page_counts = calloc(SPAN_GUEST_PAGE_COUNT,
                            sizeof(span_page_count));
    if (!r->slots || !r->page_counts) {
        free(r->slots);
        free(r->page_counts);
        memset(r, 0, sizeof(*r));
        return -1;
    }
    r->capacity = capacity;
    r->count_misses = 1u;
    atomic_store_explicit((_Atomic u32*)&r->generation, 1u,
                          memory_order_release);
    atomic_store_explicit((_Atomic u32*)&r->publication_epoch, 1u,
                          memory_order_release);
    return 0;
}

void rsx_nr_span_router_destroy(rsx_nr_span_router* r)
{
    if (!r)
        return;
    free(r->slots);
    free(r->page_counts);
    memset(r, 0, sizeof(*r));
}

u32 rsx_nr_span_router_reset(rsx_nr_span_router* r)
{
    if (!r || !r->slots || !r->page_counts)
        return 0;
    span_slot* slots = (span_slot*)r->slots;
    span_page_count* pages = (span_page_count*)r->page_counts;
    for (u32 i = 0; i < r->capacity; ++i)
        atomic_store_explicit(&slots[i].state, SPAN_EMPTY,
                              memory_order_relaxed);
    for (u32 i = 0; i < SPAN_GUEST_PAGE_COUNT; ++i)
        atomic_store_explicit(&pages[i], 0, memory_order_relaxed);
    u32 generation = atomic_load_explicit(
        (_Atomic u32*)&r->generation, memory_order_relaxed) + 1u;
    if (!generation)
        generation = 1u;
    atomic_store_explicit((_Atomic u32*)&r->generation, generation,
                          memory_order_release);
    atomic_fetch_add_explicit((_Atomic u32*)&r->publication_epoch, 1u,
                              memory_order_release);
    return generation;
}

u32 rsx_nr_span_router_generation(const rsx_nr_span_router* r)
{
    return r ? atomic_load_explicit((const _Atomic u32*)&r->generation,
                                    memory_order_acquire) : 0;
}

u32 rsx_nr_span_router_publication_epoch(const rsx_nr_span_router* r)
{
    return r ? atomic_load_explicit(
                   (const _Atomic u32*)&r->publication_epoch,
                   memory_order_acquire) : 0;
}

void rsx_nr_span_router_set_miss_counting(rsx_nr_span_router* r, int enabled)
{
    if (r)
        r->count_misses = enabled ? 1u : 0u;
}

static int span_valid(const rsx_nr_span_router* r, const rsx_nr_span* span)
{
    return r && r->slots && r->page_counts && span &&
           !(span->ea & 3u) && span->word_count &&
           span->payload.op_count &&
           span->payload.op_count <= RSX_NR_SPAN_MAX_OPS &&
           span->payload.side_count <= RSX_NR_SPAN_MAX_SIDE &&
           span->generation == rsx_nr_span_router_generation(r);
}

rsx_nr_span_publish_result
rsx_nr_span_router_publish(rsx_nr_span_router* r, const rsx_nr_span* span)
{
    if (!span_valid(r, span))
        return RSX_NR_SPAN_PUBLISH_INVALID;

    long unlocked = 0;
    if (!atomic_compare_exchange_strong_explicit(
            (_Atomic long*)&r->producer_lock, &unlocked, 1,
            memory_order_acquire, memory_order_relaxed)) {
        span_count(r, SPAN_C_BUSY);
        return RSX_NR_SPAN_PUBLISH_BUSY;
    }

    span_slot* slots = (span_slot*)r->slots;
    const u32 mask = r->capacity - 1u;
    const u32 first = span_hash(span->ea, span->generation) & mask;
    u32 candidate = ~0u;
    u32 candidate_state = SPAN_EMPTY;
    for (u32 probe = 0; probe < r->capacity; ++probe) {
        const u32 index = (first + probe) & mask;
        const u32 state = atomic_load_explicit(&slots[index].state,
                                               memory_order_acquire);
        if (state == SPAN_READY &&
            slots[index].span.ea == span->ea &&
            slots[index].span.generation == span->generation) {
            atomic_store_explicit((_Atomic long*)&r->producer_lock, 0,
                                  memory_order_release);
            span_count(r, SPAN_C_DUPLICATE);
            return RSX_NR_SPAN_PUBLISH_DUPLICATE;
        }
        if (state == SPAN_WRITING) {
            atomic_store_explicit((_Atomic long*)&r->producer_lock, 0,
                                  memory_order_release);
            span_count(r, SPAN_C_BUSY);
            return RSX_NR_SPAN_PUBLISH_BUSY;
        }
        if (state == SPAN_TOMBSTONE && candidate == ~0u) {
            candidate = index;
            candidate_state = SPAN_TOMBSTONE;
        }
        if (state == SPAN_EMPTY) {
            if (candidate == ~0u) {
                candidate = index;
                candidate_state = SPAN_EMPTY;
            }
            break;
        }
    }

    if (candidate == ~0u) {
        atomic_store_explicit((_Atomic long*)&r->producer_lock, 0,
                              memory_order_release);
        span_count(r, SPAN_C_FULL);
        return RSX_NR_SPAN_PUBLISH_FULL;
    }

    u32 expected = candidate_state;
    if (!atomic_compare_exchange_strong_explicit(
            &slots[candidate].state, &expected, SPAN_WRITING,
            memory_order_acq_rel, memory_order_acquire)) {
        atomic_store_explicit((_Atomic long*)&r->producer_lock, 0,
                              memory_order_release);
        span_count(r, SPAN_C_BUSY);
        return RSX_NR_SPAN_PUBLISH_BUSY;
    }

    slots[candidate].span = *span;
    slots[candidate].span.fingerprint =
        rsx_nr_span_fingerprint(&slots[candidate].span);
    atomic_store_explicit(&slots[candidate].state, SPAN_READY,
                          memory_order_release);

    const u32 page = span->ea >> 12;
    span_page_count* pages = (span_page_count*)r->page_counts;
    atomic_fetch_add_explicit(&pages[page], 1u, memory_order_release);
    atomic_fetch_add_explicit((_Atomic u32*)&r->publication_epoch, 1u,
                              memory_order_release);
    atomic_store_explicit((_Atomic long*)&r->producer_lock, 0,
                          memory_order_release);
    span_count(r, SPAN_C_PUBLISHED);
    return RSX_NR_SPAN_PUBLISHED;
}

rsx_nr_span_take_result
rsx_nr_span_router_take(rsx_nr_span_router* r, u32 ea, rsx_nr_span* out)
{
    if (!r || !r->slots || !r->page_counts || !out || (ea & 3u))
        return RSX_NR_SPAN_TAKE_MISS;

    span_page_count* pages = (span_page_count*)r->page_counts;
    const u32 page = ea >> 12;
    if (atomic_load_explicit(&pages[page], memory_order_acquire) == 0) {
        span_count(r, SPAN_C_FAST_MISS);
        return RSX_NR_SPAN_TAKE_FAST_MISS;
    }

    const u32 generation = rsx_nr_span_router_generation(r);
    span_slot* slots = (span_slot*)r->slots;
    const u32 mask = r->capacity - 1u;
    const u32 first = span_hash(ea, generation) & mask;
    for (u32 probe = 0; probe < r->capacity; ++probe) {
        span_slot* const slot = &slots[(first + probe) & mask];
        u32 state = atomic_load_explicit(&slot->state, memory_order_acquire);
        if (state == SPAN_EMPTY)
            break;
        if (state == SPAN_WRITING) {
            span_count(r, SPAN_C_NOT_READY);
            return RSX_NR_SPAN_TAKE_NOT_READY;
        }
        if (state != SPAN_READY || slot->span.ea != ea ||
            slot->span.generation != generation)
            continue;
        u32 ready = SPAN_READY;
        if (!atomic_compare_exchange_strong_explicit(
                &slot->state, &ready, SPAN_CLAIMED,
                memory_order_acq_rel, memory_order_acquire)) {
            span_count(r, SPAN_C_NOT_READY);
            return RSX_NR_SPAN_TAKE_NOT_READY;
        }
        *out = slot->span;
        const u32 fingerprint = rsx_nr_span_fingerprint(out);
        atomic_store_explicit(&slot->state, SPAN_TOMBSTONE,
                              memory_order_release);
        atomic_fetch_sub_explicit(&pages[page], 1u, memory_order_release);
        if (!fingerprint || fingerprint != out->fingerprint) {
            span_count(r, SPAN_C_CORRUPT);
            return RSX_NR_SPAN_TAKE_CORRUPT;
        }
        span_count(r, SPAN_C_CLAIMED);
        return RSX_NR_SPAN_TAKE_CLAIMED;
    }

    span_count(r, SPAN_C_EXACT_MISS);
    return RSX_NR_SPAN_TAKE_MISS;
}

void rsx_nr_span_router_get_stats(const rsx_nr_span_router* r,
                                  rsx_nr_span_router_stats* out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!r)
        return;
    const unsigned long long count[9] = {
        (unsigned long long)atomic_load_explicit(
            (const span_counter*)&r->counters[0], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(
            (const span_counter*)&r->counters[1], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(
            (const span_counter*)&r->counters[2], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(
            (const span_counter*)&r->counters[3], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(
            (const span_counter*)&r->counters[4], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(
            (const span_counter*)&r->counters[5], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(
            (const span_counter*)&r->counters[6], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(
            (const span_counter*)&r->counters[7], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(
            (const span_counter*)&r->counters[8], memory_order_relaxed),
    };
    out->published = count[SPAN_C_PUBLISHED];
    out->claimed = count[SPAN_C_CLAIMED];
    out->fast_misses = count[SPAN_C_FAST_MISS];
    out->exact_misses = count[SPAN_C_EXACT_MISS];
    out->not_ready = count[SPAN_C_NOT_READY];
    out->duplicates = count[SPAN_C_DUPLICATE];
    out->busy = count[SPAN_C_BUSY];
    out->full = count[SPAN_C_FULL];
    out->corrupt = count[SPAN_C_CORRUPT];
}
