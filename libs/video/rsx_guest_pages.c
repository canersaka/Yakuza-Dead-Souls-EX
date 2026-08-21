/*
 * ps3recomp - guest graphics-memory page-generation (dirty) tracker
 *
 * See rsx_guest_pages.h for the model and the seqlock-style concurrency
 * contract.  MSVC builds need /experimental:c11atomics (already set by the
 * runtime target, mirroring runtime/spu/spu_channels.c).
 */
#include "rsx_guest_pages.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef _Atomic uint32_t gp_a32;
typedef _Atomic uint64_t gp_a64;

#define GP_PAGES_PER_BLOCK (RSX_GUEST_BLOCK_SIZE / RSX_GUEST_PAGE_SIZE)

static rsx_guest_pages_space* gp_space(rsx_guest_pages* t, u32 space)
{
    if (!t || space >= RSX_GUEST_NUM_SPACES)
        return NULL;
    rsx_guest_pages_space* s = &t->space[space];
    return s->npages ? s : NULL;
}

static const rsx_guest_pages_space* gp_space_c(const rsx_guest_pages* t,
                                               u32 space)
{
    return gp_space((rsx_guest_pages*)t, space);
}

/* Clip [offset, offset+size) to the space; 0 if empty. */
static int gp_clip(const rsx_guest_pages_space* s, u32* offset, u32* size)
{
    if (!s || !*size || *offset >= s->size)
        return 0;
    if (s->size - *offset < *size)
        *size = s->size - *offset;
    return 1;
}

static int gp_space_alloc(rsx_guest_pages_space* s, u32 size)
{
    memset(s, 0, sizeof(*s));
    if (!size)
        return 0;
    const u64 rounded =
        ((u64)size + RSX_GUEST_PAGE_SIZE - 1u) & ~(u64)(RSX_GUEST_PAGE_SIZE - 1u);
    if (rounded > 0x100000000ull)
        return -1;
    const u32 npages = (u32)(rounded >> RSX_GUEST_PAGE_SHIFT);
    const u32 nblocks =
        (u32)((rounded + RSX_GUEST_BLOCK_SIZE - 1u) >> RSX_GUEST_BLOCK_SHIFT);
    s->page_gen = calloc(npages, sizeof(gp_a32));
    s->block_gen = calloc(nblocks, sizeof(gp_a32));
    s->epoch = calloc(1, sizeof(gp_a64));
    if (!s->page_gen || !s->block_gen || !s->epoch) {
        free(s->page_gen);
        free(s->block_gen);
        free(s->epoch);
        memset(s, 0, sizeof(*s));
        return -1;
    }
    s->size = rounded == 0x100000000ull ? 0xFFFFFFFFu : (u32)rounded;
    s->npages = npages;
    s->nblocks = nblocks;
    return 0;
}

int rsx_guest_pages_init(rsx_guest_pages* t, u32 local_size, u32 main_size)
{
    if (!t)
        return -1;
    memset(t, 0, sizeof(*t));
    if (gp_space_alloc(&t->space[0], local_size) != 0)
        return -1;
    if (gp_space_alloc(&t->space[1], main_size) != 0) {
        rsx_guest_pages_destroy(t);
        return -1;
    }
    return 0;
}

void rsx_guest_pages_destroy(rsx_guest_pages* t)
{
    if (!t)
        return;
    for (u32 i = 0; i < RSX_GUEST_NUM_SPACES; i++) {
        free(t->space[i].page_gen);
        free(t->space[i].block_gen);
        free(t->space[i].epoch);
    }
    memset(t, 0, sizeof(*t));
}

void rsx_guest_pages_note_write(rsx_guest_pages* t, u32 space, u32 offset,
                                u32 size)
{
    rsx_guest_pages_space* s = gp_space(t, space);
    if (!gp_clip(s, &offset, &size))
        return;
    gp_a32* pages = (gp_a32*)s->page_gen;
    gp_a32* blocks = (gp_a32*)s->block_gen;
    const u32 first_page = offset >> RSX_GUEST_PAGE_SHIFT;
    const u32 last_page = (u32)(((u64)offset + size - 1u) >> RSX_GUEST_PAGE_SHIFT);
    /* Pages first, then blocks, then the epoch: a reader that skips via an
     * unchanged block counter can never miss a page bump (contract in the
     * header). */
    for (u32 p = first_page; p <= last_page; p++)
        atomic_fetch_add_explicit(&pages[p], 1u, memory_order_release);
    const u32 first_block = offset >> RSX_GUEST_BLOCK_SHIFT;
    const u32 last_block = (u32)(((u64)offset + size - 1u) >> RSX_GUEST_BLOCK_SHIFT);
    for (u32 b = first_block; b <= last_block; b++)
        atomic_fetch_add_explicit(&blocks[b], 1u, memory_order_release);
    atomic_fetch_add_explicit((gp_a64*)s->epoch, 1u, memory_order_release);
}

u64 rsx_guest_pages_space_epoch(const rsx_guest_pages* t, u32 space)
{
    const rsx_guest_pages_space* s = gp_space_c(t, space);
    if (!s)
        return 0;
    return atomic_load_explicit((gp_a64*)s->epoch, memory_order_acquire);
}

u32 rsx_guest_pages_page_gen(const rsx_guest_pages* t, u32 space, u32 page)
{
    const rsx_guest_pages_space* s = gp_space_c(t, space);
    if (!s || page >= s->npages)
        return 0;
    return atomic_load_explicit(&((gp_a32*)s->page_gen)[page],
                                memory_order_acquire);
}

u32 rsx_guest_pages_block_gen(const rsx_guest_pages* t, u32 space, u32 block)
{
    const rsx_guest_pages_space* s = gp_space_c(t, space);
    if (!s || block >= s->nblocks)
        return 0;
    return atomic_load_explicit(&((gp_a32*)s->block_gen)[block],
                                memory_order_acquire);
}

/* Page/block index bounds of a clipped range. */
typedef struct gp_bounds {
    u32 first_page, last_page;    /* inclusive */
    u32 first_block, last_block;  /* inclusive */
} gp_bounds;

static int gp_bounds_of(const rsx_guest_pages_space* s, u32 offset, u32 size,
                        gp_bounds* b)
{
    if (!gp_clip(s, &offset, &size))
        return 0;
    b->first_page = offset >> RSX_GUEST_PAGE_SHIFT;
    b->last_page = (u32)(((u64)offset + size - 1u) >> RSX_GUEST_PAGE_SHIFT);
    b->first_block = offset >> RSX_GUEST_BLOCK_SHIFT;
    b->last_block = (u32)(((u64)offset + size - 1u) >> RSX_GUEST_BLOCK_SHIFT);
    return 1;
}

u32 rsx_guest_pages_snapshot_len(const rsx_guest_pages* t, u32 space,
                                 u32 offset, u32 size)
{
    const rsx_guest_pages_space* s = gp_space_c(t, space);
    gp_bounds b;
    if (!gp_bounds_of(s, offset, size, &b))
        return 0;
    return (b.last_block - b.first_block + 1u) +
           (b.last_page - b.first_page + 1u);
}

void rsx_guest_pages_snapshot(const rsx_guest_pages* t, u32 space,
                              u32 offset, u32 size, u32* snap)
{
    const rsx_guest_pages_space* s = gp_space_c(t, space);
    gp_bounds b;
    if (!snap || !gp_bounds_of(s, offset, size, &b))
        return;
    const gp_a32* pages = (const gp_a32*)s->page_gen;
    const gp_a32* blocks = (const gp_a32*)s->block_gen;
    u32 w = 0;
    for (u32 bi = b.first_block; bi <= b.last_block; bi++)
        snap[w++] = atomic_load_explicit(&blocks[bi], memory_order_acquire);
    for (u32 p = b.first_page; p <= b.last_page; p++)
        snap[w++] = atomic_load_explicit(&pages[p], memory_order_acquire);
}

int rsx_guest_pages_range_dirty(const rsx_guest_pages* t, u32 space,
                                u32 offset, u32 size, const u32* snap)
{
    const rsx_guest_pages_space* s = gp_space_c(t, space);
    gp_bounds b;
    if (!snap || !gp_bounds_of(s, offset, size, &b))
        return 0;
    const gp_a32* pages = (const gp_a32*)s->page_gen;
    const gp_a32* blocks = (const gp_a32*)s->block_gen;
    const u32 nblocks = b.last_block - b.first_block + 1u;
    const u32* page_snap = snap + nblocks;
    for (u32 bi = b.first_block; bi <= b.last_block; bi++) {
        if (atomic_load_explicit(&blocks[bi], memory_order_acquire) ==
            snap[bi - b.first_block])
            continue;   /* no write hit this block since the snapshot */
        u32 p0 = bi << (RSX_GUEST_BLOCK_SHIFT - RSX_GUEST_PAGE_SHIFT);
        u32 p1 = p0 + GP_PAGES_PER_BLOCK - 1u;
        if (p0 < b.first_page) p0 = b.first_page;
        if (p1 > b.last_page) p1 = b.last_page;
        for (u32 p = p0; p <= p1; p++)
            if (atomic_load_explicit(&pages[p], memory_order_acquire) !=
                page_snap[p - b.first_page])
                return 1;
    }
    return 0;
}

u32 rsx_guest_pages_collect_dirty(const rsx_guest_pages* t, u32 space,
                                  u32 offset, u32 size, u32* snap,
                                  rsx_guest_span* out, u32 max_spans,
                                  u32 budget_bytes, int* more)
{
    if (more)
        *more = 0;
    const rsx_guest_pages_space* s = gp_space_c(t, space);
    gp_bounds b;
    u32 clip_off = offset, clip_size = size;
    if (!snap || !out || !max_spans || !gp_bounds_of(s, offset, size, &b))
        return 0;
    gp_clip(s, &clip_off, &clip_size);
    const u32 range_end = clip_off + clip_size;   /* clipped, cannot wrap */
    const gp_a32* pages = (const gp_a32*)s->page_gen;
    const gp_a32* blocks = (const gp_a32*)s->block_gen;
    const u32 nblocks = b.last_block - b.first_block + 1u;
    u32* page_snap = snap + nblocks;

    u32 n_spans = 0;
    u64 collected = 0;
    int stopped = 0;

    for (u32 bi = b.first_block; bi <= b.last_block && !stopped; bi++) {
        /* Read the block counter BEFORE its pages (mirror of the writer's
         * page-then-block order); refresh the slot only if every dirty
         * page of the block was acknowledged below. */
        const u32 bgen =
            atomic_load_explicit(&blocks[bi], memory_order_acquire);
        if (bgen == snap[bi - b.first_block])
            continue;
        u32 p0 = bi << (RSX_GUEST_BLOCK_SHIFT - RSX_GUEST_PAGE_SHIFT);
        u32 p1 = p0 + GP_PAGES_PER_BLOCK - 1u;
        if (p0 < b.first_page) p0 = b.first_page;
        if (p1 > b.last_page) p1 = b.last_page;
        int block_complete = 1;
        for (u32 p = p0; p <= p1; p++) {
            const u32 gen =
                atomic_load_explicit(&pages[p], memory_order_acquire);
            if (gen == page_snap[p - b.first_page])
                continue;
            /* Clip the page to the requested range. */
            u32 span_off = p << RSX_GUEST_PAGE_SHIFT;
            u32 span_end = span_off + RSX_GUEST_PAGE_SIZE;
            if (span_off < clip_off) span_off = clip_off;
            if (span_end > range_end) span_end = range_end;
            const u32 span_size = span_end - span_off;
            if (budget_bytes && collected + span_size > budget_bytes) {
                block_complete = 0;
                stopped = 1;
                break;
            }
            if (n_spans && out[n_spans - 1].offset + out[n_spans - 1].size ==
                               span_off) {
                out[n_spans - 1].size += span_size;
            } else {
                if (n_spans == max_spans) {
                    block_complete = 0;
                    stopped = 1;
                    break;
                }
                out[n_spans].offset = span_off;
                out[n_spans].size = span_size;
                n_spans++;
            }
            collected += span_size;
            page_snap[p - b.first_page] = gen;
        }
        if (block_complete)
            snap[bi - b.first_block] = bgen;
    }
    if (more && (stopped ||
                 rsx_guest_pages_range_dirty(t, space, offset, size, snap)))
        *more = 1;
    return n_spans;
}

void rsx_guest_pages_snapshot_undo(const rsx_guest_pages* t, u32 space,
                                   u32 offset, u32 size, u32* snap,
                                   u32 span_offset, u32 span_size)
{
    const rsx_guest_pages_space* s = gp_space_c(t, space);
    gp_bounds b;
    if (!snap || !span_size || !gp_bounds_of(s, offset, size, &b))
        return;
    const u32 nblocks = b.last_block - b.first_block + 1u;
    u32* page_snap = snap + nblocks;
    u32 p0 = span_offset >> RSX_GUEST_PAGE_SHIFT;
    u32 p1 = (u32)(((u64)span_offset + span_size - 1u) >> RSX_GUEST_PAGE_SHIFT);
    if (p0 < b.first_page) p0 = b.first_page;
    if (p1 > b.last_page) p1 = b.last_page;
    for (u32 p = p0; p <= p1; p++)
        page_snap[p - b.first_page] -= 1u;   /* != current until re-collected */
    /* The affected block slots must rescan too. */
    u32 b0 = p0 >> (RSX_GUEST_BLOCK_SHIFT - RSX_GUEST_PAGE_SHIFT);
    u32 b1 = p1 >> (RSX_GUEST_BLOCK_SHIFT - RSX_GUEST_PAGE_SHIFT);
    for (u32 bi = b0; bi <= b1; bi++)
        snap[bi - b.first_block] -= 1u;
}

void rsx_guest_pages_debug_set_page_gen(rsx_guest_pages* t, u32 space,
                                        u32 page, u32 value)
{
    rsx_guest_pages_space* s = gp_space(t, space);
    if (!s || page >= s->npages)
        return;
    atomic_store_explicit(&((gp_a32*)s->page_gen)[page], value,
                          memory_order_release);
    const u32 block = page >> (RSX_GUEST_BLOCK_SHIFT - RSX_GUEST_PAGE_SHIFT);
    atomic_store_explicit(&((gp_a32*)s->block_gen)[block], value,
                          memory_order_release);
}
