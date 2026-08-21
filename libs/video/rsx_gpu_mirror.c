/*
 * ps3recomp - persistent GPU mirror of guest graphics memory (core)
 *
 * See rsx_gpu_mirror.h.  The core tracks registration refcounts per page and
 * per 64 KiB block, and syncs by walking only blocks that contain registered
 * pages, using the tracker's block counters to skip clean blocks in one
 * compare.  Sync cost is therefore proportional to registered memory (with a
 * 64 KiB-granular fast path), never to the whole guest space, and nothing
 * here runs per draw.
 */
#include "rsx_gpu_mirror.h"

#include <stdlib.h>
#include <string.h>

#define GM_PAGE_SHIFT  RSX_GUEST_PAGE_SHIFT
#define GM_PAGE_SIZE   RSX_GUEST_PAGE_SIZE
#define GM_BLOCK_SHIFT RSX_GUEST_BLOCK_SHIFT
#define GM_PB_SHIFT    (GM_BLOCK_SHIFT - GM_PAGE_SHIFT)
#define GM_PAGES_PER_BLOCK (1u << GM_PB_SHIFT)

/* Longest contiguous upload run buffered before a flush (256 pages = 1 MiB);
 * longer dirty runs simply flush in slices. */
#define GM_RUN_CAP 256u

#define GM_STATE_VALID 1u

typedef struct gm_space {
    u32  size;
    u32  npages;
    u32  nblocks;
    u32* page_snap;     /* generation last uploaded (valid only w/ VALID)   */
    u8*  page_state;
    u32* page_ref;      /* registrations covering the page                  */
    u32* block_snap;    /* tracker block gen as of the last completed scan  */
    u32* block_ref;     /* registered pages in the block                    */
    u32* block_stale;   /* registered-but-never-uploaded pages in the block */
    u64  last_epoch;
    u32  stale_total;
    int  have_epoch;
} gm_space;

typedef struct gm_range_slot {
    u32 offset;
    u32 size;
    u16 gen;
    u8  space;
    u8  active;
} gm_range_slot;

struct rsx_gpu_mirror {
    rsx_guest_pages* pages;
    rsx_gpu_mirror_ops ops;
    gm_space space[RSX_GUEST_NUM_SPACES];
    gm_range_slot* slots;
    u32 slot_count;
    u32 slot_cap;
    int deferred;          /* a prior sync left dirty pages behind */
    rsx_gpu_mirror_stats stats;
};

static int gm_space_init(gm_space* s, const rsx_guest_pages_space* ts)
{
    memset(s, 0, sizeof(*s));
    if (!ts->npages)
        return 0;
    s->size = ts->size;
    s->npages = ts->npages;
    s->nblocks = ts->nblocks;
    s->page_snap = (u32*)calloc(s->npages, sizeof(u32));
    s->page_state = (u8*)calloc(s->npages, 1);
    s->page_ref = (u32*)calloc(s->npages, sizeof(u32));
    s->block_snap = (u32*)calloc(s->nblocks, sizeof(u32));
    s->block_ref = (u32*)calloc(s->nblocks, sizeof(u32));
    s->block_stale = (u32*)calloc(s->nblocks, sizeof(u32));
    if (!s->page_snap || !s->page_state || !s->page_ref ||
        !s->block_snap || !s->block_ref || !s->block_stale)
        return -1;
    return 0;
}

rsx_gpu_mirror* rsx_gpu_mirror_create(rsx_guest_pages* pages,
                                      const rsx_gpu_mirror_ops* ops)
{
    if (!pages || !ops || !ops->guest_ptr || !ops->upload)
        return NULL;
    rsx_gpu_mirror* m = (rsx_gpu_mirror*)calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->pages = pages;
    m->ops = *ops;
    for (u32 i = 0; i < RSX_GUEST_NUM_SPACES; i++) {
        if (gm_space_init(&m->space[i], &pages->space[i]) != 0) {
            rsx_gpu_mirror_destroy(m);
            return NULL;
        }
    }
    return m;
}

void rsx_gpu_mirror_destroy(rsx_gpu_mirror* m)
{
    if (!m)
        return;
    for (u32 i = 0; i < RSX_GUEST_NUM_SPACES; i++) {
        gm_space* s = &m->space[i];
        free(s->page_snap);
        free(s->page_state);
        free(s->page_ref);
        free(s->block_snap);
        free(s->block_ref);
        free(s->block_stale);
    }
    free(m->slots);
    free(m);
}

static gm_space* gm_get_space(rsx_gpu_mirror* m, u32 space)
{
    if (!m || space >= RSX_GUEST_NUM_SPACES || !m->space[space].npages)
        return NULL;
    return &m->space[space];
}

rsx_gpu_mirror_range rsx_gpu_mirror_register(rsx_gpu_mirror* m, u32 space,
                                             u32 offset, u32 size)
{
    gm_space* s = gm_get_space(m, space);
    if (!s || !size || offset >= s->size || s->size - offset < size)
        return 0;

    /* Slot allocation (reuse inactive slots, keep their generation). */
    u32 slot = m->slot_count;
    for (u32 i = 0; i < m->slot_count; i++) {
        if (!m->slots[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == m->slot_count) {
        if (m->slot_count == m->slot_cap) {
            const u32 cap = m->slot_cap ? m->slot_cap * 2u : 32u;
            if (cap > 0xFFFFFu)
                return 0;
            gm_range_slot* grown = (gm_range_slot*)realloc(
                m->slots, (size_t)cap * sizeof(*grown));
            if (!grown)
                return 0;
            m->slots = grown;
            m->slot_cap = cap;
        }
        memset(&m->slots[m->slot_count], 0, sizeof(m->slots[0]));
        m->slot_count++;
    }

    gm_range_slot* r = &m->slots[slot];
    r->offset = offset;
    r->size = size;
    r->space = (u8)space;
    r->active = 1;
    r->gen = (u16)((r->gen + 1u) & 0xFFFu);
    if (!r->gen)
        r->gen = 1;

    const u32 first_page = offset >> GM_PAGE_SHIFT;
    const u32 last_page =
        (u32)(((u64)offset + size - 1u) >> GM_PAGE_SHIFT);
    for (u32 p = first_page; p <= last_page; p++) {
        if (s->page_ref[p]++ != 0)
            continue;
        const u32 b = p >> GM_PB_SHIFT;
        s->block_ref[b]++;
        /* The page starts stale: force it dirty against the tracker so the
         * next sync uploads it even if it was valid under an older,
         * since-unregistered resource. */
        s->page_state[p] &= (u8)~GM_STATE_VALID;
        s->page_snap[p] = rsx_guest_pages_page_gen(m->pages, space, p) - 1u;
        s->block_stale[b]++;
        s->stale_total++;
    }
    return ((u32)r->gen << 20) | (slot + 1u);
}

static gm_range_slot* gm_resolve(const rsx_gpu_mirror* m,
                                 rsx_gpu_mirror_range handle)
{
    if (!m || !handle)
        return NULL;
    const u32 slot = (handle & 0xFFFFFu) - 1u;
    const u16 gen = (u16)(handle >> 20);
    if (slot >= m->slot_count)
        return NULL;
    gm_range_slot* r = (gm_range_slot*)&m->slots[slot];
    if (!r->active || r->gen != gen)
        return NULL;
    return r;
}

int rsx_gpu_mirror_unregister(rsx_gpu_mirror* m, rsx_gpu_mirror_range handle)
{
    gm_range_slot* r = gm_resolve(m, handle);
    if (!r)
        return 0;
    gm_space* s = gm_get_space(m, r->space);
    const u32 first_page = r->offset >> GM_PAGE_SHIFT;
    const u32 last_page =
        (u32)(((u64)r->offset + r->size - 1u) >> GM_PAGE_SHIFT);
    for (u32 p = first_page; p <= last_page; p++) {
        if (--s->page_ref[p] != 0)
            continue;
        const u32 b = p >> GM_PB_SHIFT;
        s->block_ref[b]--;
        if (!(s->page_state[p] & GM_STATE_VALID)) {
            s->block_stale[b]--;
            s->stale_total--;
        }
    }
    r->active = 0;
    return 1;
}

int rsx_gpu_mirror_range_current(const rsx_gpu_mirror* m,
                                 rsx_gpu_mirror_range handle)
{
    const gm_range_slot* r = gm_resolve(m, handle);
    if (!r)
        return 0;
    const gm_space* s = &m->space[r->space];
    const u32 first_page = r->offset >> GM_PAGE_SHIFT;
    const u32 last_page =
        (u32)(((u64)r->offset + r->size - 1u) >> GM_PAGE_SHIFT);
    const u32 first_block = first_page >> GM_PB_SHIFT;
    const u32 last_block = last_page >> GM_PB_SHIFT;
    for (u32 b = first_block; b <= last_block; b++) {
        /* Block counter read BEFORE its pages (tracker contract). */
        const u32 bgen = rsx_guest_pages_block_gen(m->pages, r->space, b);
        if (!s->block_stale[b] && bgen == s->block_snap[b])
            continue;   /* completed scan, nothing written since */
        u32 p0 = b << GM_PB_SHIFT;
        u32 p1 = p0 + GM_PAGES_PER_BLOCK - 1u;
        if (p0 < first_page) p0 = first_page;
        if (p1 > last_page) p1 = last_page;
        for (u32 p = p0; p <= p1; p++) {
            if (!(s->page_state[p] & GM_STATE_VALID))
                return 0;
            if (rsx_guest_pages_page_gen(m->pages, r->space, p) !=
                s->page_snap[p])
                return 0;
        }
    }
    return 1;
}

/* One buffered contiguous upload run. */
typedef struct gm_run {
    u32 first_page;
    u32 count;
    u32 gens[GM_RUN_CAP];
} gm_run;

/* Flush the run: resolve guest memory, hand it to the backend, and
 * acknowledge the pages on success.  Returns 0 on success, -1 when the sync
 * must stop (backend reject), leaving the run's pages dirty. */
static int gm_flush_run(rsx_gpu_mirror* m, u32 space, gm_run* run,
                        u32* uploaded)
{
    if (!run->count)
        return 0;
    gm_space* s = &m->space[space];
    const u32 offset = run->first_page << GM_PAGE_SHIFT;
    u32 size = run->count << GM_PAGE_SHIFT;
    if (offset + (u64)size > s->size)
        size = s->size - offset;   /* final partial page of the space */
    const u8* src = m->ops.guest_ptr(m->ops.user, space, offset, size);
    int rc;
    if (!src) {
        m->stats.resolver_failures++;
        rc = -1;   /* unreadable guest span: stays dirty, retried later */
    } else {
        rc = m->ops.upload(m->ops.user, space, offset, src, size);
        if (rc != 0)
            m->stats.upload_rejects++;
    }
    if (rc != 0) {
        run->count = 0;
        return -1;
    }
    for (u32 i = 0; i < run->count; i++) {
        const u32 p = run->first_page + i;
        const u32 b = p >> GM_PB_SHIFT;
        if (!(s->page_state[p] & GM_STATE_VALID)) {
            s->page_state[p] |= GM_STATE_VALID;
            s->block_stale[b]--;
            s->stale_total--;
        }
        s->page_snap[p] = run->gens[i];
    }
    m->stats.uploads++;
    m->stats.upload_bytes += size;
    *uploaded += size;
    run->count = 0;
    return 0;
}

u32 rsx_gpu_mirror_sync(rsx_gpu_mirror* m, u32 budget_bytes)
{
    if (!m)
        return 0;
    m->stats.syncs++;
    u32 uploaded = 0;
    int deferred = 0;

    for (u32 space = 0; space < RSX_GUEST_NUM_SPACES && !deferred; space++) {
        gm_space* s = &m->space[space];
        if (!s->npages)
            continue;
        /* Epoch read BEFORE the scan: writes landing during the scan keep
         * epoch != last_epoch and re-trigger the next sync. */
        const u64 epoch = rsx_guest_pages_space_epoch(m->pages, space);
        if (s->have_epoch && epoch == s->last_epoch && !s->stale_total &&
            !m->deferred) {
            m->stats.epoch_skips++;
            continue;
        }

        gm_run run = {0, 0, {0}};
        int complete = 1;
        for (u32 b = 0; b < s->nblocks; b++) {
            if (!s->block_ref[b])
                continue;
            const u32 bgen = rsx_guest_pages_block_gen(m->pages, space, b);
            if (!s->block_stale[b] && bgen == s->block_snap[b])
                continue;
            const u32 p0 = b << GM_PB_SHIFT;
            u32 p1 = p0 + GM_PAGES_PER_BLOCK - 1u;
            if (p1 >= s->npages)
                p1 = s->npages - 1u;
            int block_complete = 1;
            for (u32 p = p0; p <= p1; p++) {
                if (!s->page_ref[p])
                    continue;
                const u32 gen =
                    rsx_guest_pages_page_gen(m->pages, space, p);
                if ((s->page_state[p] & GM_STATE_VALID) &&
                    gen == s->page_snap[p])
                    continue;
                if (budget_bytes &&
                    uploaded + (run.count + 1u) * GM_PAGE_SIZE >
                        budget_bytes) {
                    block_complete = 0;
                    deferred = 1;
                    break;
                }
                if (run.count &&
                    (run.first_page + run.count != p ||
                     run.count == GM_RUN_CAP)) {
                    if (gm_flush_run(m, space, &run, &uploaded) != 0) {
                        block_complete = 0;
                        deferred = 1;
                        break;
                    }
                }
                if (!run.count)
                    run.first_page = p;
                run.gens[run.count++] = gen;
            }
            if (block_complete && run.count &&
                run.first_page + run.count - 1u >= p0) {
                /* Flush before crediting the block so a reject cannot
                 * mark it clean with pages still dirty. */
                if (gm_flush_run(m, space, &run, &uploaded) != 0)
                    block_complete = 0;
            }
            if (!block_complete) {
                complete = 0;
                deferred = 1;
                break;
            }
            s->block_snap[b] = bgen;
        }
        if (run.count && gm_flush_run(m, space, &run, &uploaded) != 0) {
            complete = 0;
            deferred = 1;
        }
        if (complete) {
            s->last_epoch = epoch;
            s->have_epoch = 1;
        }
    }

    m->deferred = deferred;
    if (deferred)
        m->stats.deferred_syncs++;
    return uploaded;
}

void rsx_gpu_mirror_get_stats(const rsx_gpu_mirror* m,
                              rsx_gpu_mirror_stats* out)
{
    if (!m || !out)
        return;
    *out = m->stats;
}
