/*
 * ps3recomp - native-render resource model implementation. See
 * rsx_nr_resources.h.
 */

#include "rsx_nr_resources.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <intrin.h>
static inline long long nrr_load64(const volatile long long* p)
{
    long long v = *p;
    _ReadWriteBarrier();
    return v;
}
static inline void nrr_max64(volatile long long* p, long long v)
{
    long long cur = *p;
    while ((unsigned long long)v > (unsigned long long)cur) {
        long long seen = _InterlockedCompareExchange64(p, v, cur);
        if (seen == cur)
            break;
        cur = seen;
    }
}
#else
static inline long long nrr_load64(const volatile long long* p)
{
    return __atomic_load_n((const long long*)p, __ATOMIC_ACQUIRE);
}
static inline void nrr_max64(volatile long long* p, long long v)
{
    long long cur = __atomic_load_n((long long*)p, __ATOMIC_RELAXED);
    while ((unsigned long long)v > (unsigned long long)cur &&
           !__atomic_compare_exchange_n((long long*)p, &cur, v, 1,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        ;
}
#endif

/* ---- mapping epochs ---------------------------------------------------- */

void rsx_nr_maps_init(rsx_nr_maps* m)
{
    memset(m, 0, sizeof(*m));
}

static int maps_overlap(u32 a0, u32 alen, u32 b0, u32 blen)
{
    return a0 < b0 + blen && b0 < a0 + alen;
}

int rsx_nr_maps_map(rsx_nr_maps* m, u32 io_start, u32 ea_start, u32 size)
{
    if (!size)
        return -1;
    int free_slot = -1;
    for (u32 i = 0; i < RSX_NR_MAX_IOMAPS; i++) {
        if (!m->win[i].live) {
            if (free_slot < 0)
                free_slot = (int)i;
            continue;
        }
        if (m->win[i].io_start == io_start) {
            /* exact-start remap: reuse the slot */
            if (m->win[i].size != size || m->win[i].ea_start != ea_start) {
                m->win[i].size = size;
                m->win[i].ea_start = ea_start;
                m->win[i].gen++;
                m->epoch++;
            }
            return 0;
        }
        if (maps_overlap(io_start, size, m->win[i].io_start, m->win[i].size))
            return -1;
    }
    if (free_slot < 0)
        return -1;
    m->win[free_slot].io_start = io_start;
    m->win[free_slot].ea_start = ea_start;
    m->win[free_slot].size = size;
    m->win[free_slot].gen++;
    m->win[free_slot].live = 1;
    m->count++;
    m->epoch++;
    return 0;
}

int rsx_nr_maps_unmap(rsx_nr_maps* m, u32 io_start)
{
    for (u32 i = 0; i < RSX_NR_MAX_IOMAPS; i++) {
        if (m->win[i].live && m->win[i].io_start == io_start) {
            m->win[i].live = 0;
            m->win[i].gen++;
            m->count--;
            m->epoch++;
            return 0;
        }
    }
    return -1;
}

int rsx_nr_maps_resolve(const rsx_nr_maps* m, u32 io, u32 size,
                        u32* ea, u32* win_gen)
{
    for (u32 i = 0; i < RSX_NR_MAX_IOMAPS; i++) {
        if (!m->win[i].live)
            continue;
        if (io >= m->win[i].io_start &&
            io - m->win[i].io_start <= m->win[i].size &&
            size <= m->win[i].size - (io - m->win[i].io_start)) {
            if (ea)
                *ea = m->win[i].ea_start + (io - m->win[i].io_start);
            if (win_gen)
                *win_gen = m->win[i].gen;
            return 1;
        }
    }
    return 0;
}

/* ---- resource cache ---------------------------------------------------- */

static void key_pack(const rsx_nr_res_key* k, u64* lo, u64* hi)
{
    /* lo mixes fmt so lo==0 && hi==0 (the empty marker) is unreachable for
     * a real key: hi always carries kind+1 in its top bits. */
    *lo = ((u64)k->offset << 32) | k->size;
    *hi = ((u64)(k->kind + 1u) << 48) | ((u64)(k->space & 0xFFFFu) << 32) |
          (u32)(k->fmt ^ (k->fmt >> 32));
    *lo ^= k->fmt * 0x9E3779B97F4A7C15ull;
}

static u32 key_hash(u64 lo, u64 hi, u32 mask)
{
    u64 h = lo ^ (hi * 0x100000001B3ull);
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    return (u32)h & mask;
}

int rsx_nr_res_cache_init(rsx_nr_res_cache* c, u32 cap, u32 arena_words,
                          rsx_guest_pages* pages, const rsx_nr_maps* maps)
{
    if (!cap || (cap & (cap - 1)))
        return -1;
    memset(c, 0, sizeof(*c));
    c->slots = calloc(cap, sizeof(rsx_nr_res));
    c->arena = calloc(arena_words ? arena_words : 1, 4);
    c->free_cap = 256;
    c->free_ofs = calloc(c->free_cap, 4);
    c->free_len = calloc(c->free_cap, 4);
    if (!c->slots || !c->arena || !c->free_ofs || !c->free_len) {
        rsx_nr_res_cache_destroy(c);
        return -1;
    }
    c->cap = cap;
    c->arena_cap = arena_words;
    c->pages = pages;
    c->maps = maps;
    return 0;
}

void rsx_nr_res_cache_destroy(rsx_nr_res_cache* c)
{
    free(c->slots);
    free(c->arena);
    free(c->free_ofs);
    free(c->free_len);
    memset(c, 0, sizeof(*c));
}

static u32 arena_alloc(rsx_nr_res_cache* c, u32 len)
{
    if (!len)
        return 0;
    /* first fit in the free list */
    for (u32 i = 0; i < c->free_count; i++) {
        if (c->free_len[i] >= len) {
            u32 ofs = c->free_ofs[i];
            c->free_ofs[i] += len;
            c->free_len[i] -= len;
            if (!c->free_len[i]) {
                c->free_ofs[i] = c->free_ofs[--c->free_count];
                c->free_len[i] = c->free_len[c->free_count];
            }
            return ofs;
        }
    }
    if (c->arena_bump + len > c->arena_cap)
        return ~0u;
    u32 ofs = c->arena_bump;
    c->arena_bump += len;
    return ofs;
}

static void arena_free(rsx_nr_res_cache* c, u32 ofs, u32 len)
{
    if (!len)
        return;
    if (c->free_count < c->free_cap) {
        c->free_ofs[c->free_count] = ofs;
        c->free_len[c->free_count] = len;
        c->free_count++;
    }
    /* free-list overflow leaks arena words until a sweep-all; acceptable
     * for the bounded offline model and counted implicitly by exhaustion */
}

static rsx_nr_res* find_slot(rsx_nr_res_cache* c, u64 lo, u64 hi, int for_insert)
{
    u32 mask = c->cap - 1;
    u32 idx = key_hash(lo, hi, mask);
    for (u32 probe = 0; probe <= mask; probe++, idx = (idx + probe) & mask) {
        rsx_nr_res* e = &c->slots[idx];
        if (e->live) {
            if (e->key_lo == lo && e->key_hi == hi)
                return e;
            continue;
        }
        /* dead slot: usable for insert, but lookups must keep probing
         * (tombstone semantics: key_lo/hi stay set until reuse) */
        if (for_insert)
            return e;
        if (!e->key_lo && !e->key_hi)
            return NULL;              /* never-used slot: probe chain ends */
    }
    return NULL;
}

rsx_nr_res* rsx_nr_res_lookup(rsx_nr_res_cache* c, const rsx_nr_res_key* k)
{
    u64 lo, hi;
    key_pack(k, &lo, &hi);
    rsx_nr_res* e = find_slot(c, lo, hi, 0);
    if (e && e->live) {
        e->last_use = c->frame;
        c->stats.hits++;
        return e;
    }
    c->stats.misses++;
    return NULL;
}

static void snapshot_entry(rsx_nr_res_cache* c, rsx_nr_res* e)
{
    if (e->snap_len)
        rsx_guest_pages_snapshot(c->pages, e->space, e->offset, e->size,
                                 c->arena + e->snap_ofs);
    e->map_epoch = c->maps ? c->maps->epoch : 0;
}

rsx_nr_res* rsx_nr_res_insert(rsx_nr_res_cache* c, const rsx_nr_res_key* k,
                              u64 backend_id)
{
    if (c->count >= c->cap - (c->cap >> 2)) {   /* keep load factor <= 3/4 */
        c->stats.table_full++;
        return NULL;
    }
    u64 lo, hi;
    key_pack(k, &lo, &hi);
    rsx_nr_res* e = find_slot(c, lo, hi, 1);
    if (!e) {
        c->stats.table_full++;
        return NULL;
    }
    if (e->live)
        return NULL;                             /* duplicate insert        */

    u32 snap_len = rsx_guest_pages_snapshot_len(c->pages, k->space,
                                                k->offset, k->size);
    u32 snap_ofs = arena_alloc(c, snap_len);
    if (snap_ofs == ~0u) {
        c->stats.arena_exhausted++;
        return NULL;
    }
    memset(e, 0, sizeof(*e));
    e->key_lo = lo;
    e->key_hi = hi;
    e->backend_id = backend_id;
    e->space = k->space;
    e->offset = k->offset;
    e->size = k->size;
    e->snap_ofs = snap_ofs;
    e->snap_len = snap_len;
    e->last_use = c->frame;
    e->live = 1;
    snapshot_entry(c, e);
    c->count++;
    c->stats.inserts++;
    return e;
}

int rsx_nr_res_current(rsx_nr_res_cache* c, const rsx_nr_res* e)
{
    if (c->maps && e->space == RSX_NR_SPACE_MAIN &&
        e->map_epoch != c->maps->epoch) {
        c->stats.stale_mapping++;
        return 0;
    }
    if (e->snap_len &&
        rsx_guest_pages_range_dirty(c->pages, e->space, e->offset, e->size,
                                    c->arena + e->snap_ofs)) {
        c->stats.stale_content++;
        return 0;
    }
    return 1;
}

void rsx_nr_res_revalidate(rsx_nr_res_cache* c, rsx_nr_res* e)
{
    snapshot_entry(c, e);
}

void rsx_nr_res_evict(rsx_nr_res_cache* c, rsx_nr_res* e,
                      rsx_nr_res_evict_fn cb, void* user)
{
    if (!e->live)
        return;
    if (cb)
        cb(user, e->backend_id);
    arena_free(c, e->snap_ofs, e->snap_len);
    e->live = 0;                     /* tombstone: keys stay for probing   */
    c->count--;
    c->stats.evictions++;
}

void rsx_nr_res_next_frame(rsx_nr_res_cache* c)
{
    c->frame++;
}

u32 rsx_nr_res_sweep(rsx_nr_res_cache* c, u32 max_age,
                     rsx_nr_res_evict_fn cb, void* user)
{
    u32 evicted = 0;
    for (u32 i = 0; i < c->cap; i++) {
        rsx_nr_res* e = &c->slots[i];
        if (!e->live)
            continue;
        u32 age = c->frame - e->last_use;
        if (age >= max_age) {
            rsx_nr_res_evict(c, e, cb, user);
            evicted++;
        }
    }
    c->stats.sweep_evictions += evicted;
    return evicted;
}

/* ---- PSO cache --------------------------------------------------------- */

int rsx_nr_pso_cache_init(rsx_nr_pso_cache* c, u32 cap)
{
    if (!cap || (cap & (cap - 1)))
        return -1;
    memset(c, 0, sizeof(*c));
    c->keys = calloc(cap, 8);
    c->values = calloc(cap, 8);
    if (!c->keys || !c->values) {
        free(c->keys);
        free(c->values);
        memset(c, 0, sizeof(*c));
        return -1;
    }
    c->cap = cap;
    return 0;
}

void rsx_nr_pso_cache_destroy(rsx_nr_pso_cache* c)
{
    free(c->keys);
    free(c->values);
    memset(c, 0, sizeof(*c));
}

static u32 pso_hash(u64 key, u32 mask)
{
    key ^= key >> 33;
    key *= 0xC2B2AE3D27D4EB4Full;
    key ^= key >> 29;
    return (u32)key & mask;
}

int rsx_nr_pso_lookup(rsx_nr_pso_cache* c, u64 key, u64* value)
{
    u32 mask = c->cap - 1;
    u32 idx = pso_hash(key, mask);
    for (u32 probe = 0; probe <= mask; probe++, idx = (idx + probe) & mask) {
        if (c->keys[idx] == key) {
            *value = c->values[idx];
            c->stats.hits++;
            return 1;
        }
        if (!c->keys[idx])
            break;
    }
    c->stats.misses++;
    return 0;
}

int rsx_nr_pso_insert(rsx_nr_pso_cache* c, u64 key, u64 value)
{
    if (!key)
        key = 1;                     /* 0 is the empty marker              */
    if (c->count >= c->cap - (c->cap >> 2)) {
        c->stats.table_full++;
        return -1;
    }
    u32 mask = c->cap - 1;
    u32 idx = pso_hash(key, mask);
    for (u32 probe = 0; probe <= mask; probe++, idx = (idx + probe) & mask) {
        if (!c->keys[idx]) {
            c->keys[idx] = key;
            c->values[idx] = value;
            c->count++;
            c->stats.inserts++;
            return 0;
        }
        if (c->keys[idx] == key) {
            c->values[idx] = value;  /* refresh                            */
            return 0;
        }
    }
    c->stats.table_full++;
    return -1;
}

u64 rsx_nr_hash_fold(u64 seed, const void* data, u32 bytes)
{
    const unsigned char* p = data;
    u64 h = seed ? seed : 0xCBF29CE484222325ull;
    for (u32 i = 0; i < bytes; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h ? h : 1;
}

/* ---- timeline fences --------------------------------------------------- */

void rsx_nr_fence_init(rsx_nr_fence* f)
{
    f->completed = 0;
    f->next_value = 0;
}

u64 rsx_nr_fence_next(rsx_nr_fence* f)
{
    return ++f->next_value;
}

void rsx_nr_fence_complete(rsx_nr_fence* f, u64 value)
{
    nrr_max64(&f->completed, (long long)value);
}

int rsx_nr_fence_done(const rsx_nr_fence* f, u64 value)
{
    return (unsigned long long)nrr_load64(&f->completed) >= value;
}

u64 rsx_nr_fence_completed(const rsx_nr_fence* f)
{
    return (u64)nrr_load64(&f->completed);
}
