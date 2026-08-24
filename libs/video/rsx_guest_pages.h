/*
 * ps3recomp - guest graphics-memory page-generation (dirty) tracker
 *
 * Foundation for the persistent GPU guest-memory mirror: writers publish
 * "these guest bytes changed" at page granularity, and consumers (the mirror,
 * cached vertex/texture resources) detect changes against a snapshot without
 * any global scan.
 *
 * Model
 *   - Two independent address spaces, matching the RSX location selector:
 *     RSX_LOCATION_LOCAL (0, the GDDR carve) and RSX_LOCATION_MAIN (1, the
 *     IO-mapped XDR window).  Offsets are the same byte offsets the draw
 *     path already uses with rsx_live_guest_ptr_fn.
 *   - Every 1 KiB mirror page carries a 32-bit generation counter that
 *     increments on every write notification covering the page. The external
 *     VM-write router remains sparse at the host's 4 KiB page granularity;
 *     this finer internal split prevents unrelated dynamic vertex slices in
 *     one watched host page from invalidating each other. Every 64 KiB block
 *     carries a second counter so a range query can skip 64 clean pages
 *     with one compare.  A per-space 64-bit epoch counts all notifications,
 *     giving consumers an O(1) "nothing changed anywhere" early-out.
 *   - Consumers store per-range snapshots of the counters.  "Changed" is a
 *     pure equality test, so counter wrap-around is harmless as long as a
 *     single page does not receive an exact multiple of 2^32 notifications
 *     between two queries of the same snapshot (physically unreachable
 *     between frames).
 *
 * Concurrency contract (seqlock-style; the writer side is lock-free)
 *   - Writers MUST call rsx_guest_pages_note_write AFTER the guest bytes
 *     are written.  The counter increments are releases: a reader that
 *     observes the bumped counter also observes the payload.
 *   - Readers MUST read counters (all reads here are acquires) BEFORE
 *     copying the payload, and store the pre-read value as the snapshot.
 *     A write racing the copy leaves the stored snapshot stale, so the
 *     following query sees the page dirty again and the copy is repaired.
 *     Torn intermediate copies are therefore transient, never final.
 *   - note_write order per notification is page counters, then block
 *     counters, then the epoch; queries compare blocks before pages so the
 *     block fast path can never hide a page bump.
 */
#ifndef PS3RECOMP_RSX_GUEST_PAGES_H
#define PS3RECOMP_RSX_GUEST_PAGES_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_GUEST_PAGE_SHIFT   10u
#define RSX_GUEST_PAGE_SIZE    (1u << RSX_GUEST_PAGE_SHIFT)
#define RSX_GUEST_BLOCK_SHIFT  16u
#define RSX_GUEST_BLOCK_SIZE   (1u << RSX_GUEST_BLOCK_SHIFT)
#define RSX_GUEST_NUM_SPACES   2u   /* RSX_LOCATION_LOCAL / RSX_LOCATION_MAIN */

typedef struct rsx_guest_pages_space {
    void* page_gen;    /* _Atomic u32[npages]  */
    void* block_gen;   /* _Atomic u32[nblocks] */
    void* epoch;       /* _Atomic u64          */
    u32   size;
    u32   npages;
    u32   nblocks;
} rsx_guest_pages_space;

typedef struct rsx_guest_pages {
    rsx_guest_pages_space space[RSX_GUEST_NUM_SPACES];
} rsx_guest_pages;

typedef struct rsx_guest_span {
    u32 offset;
    u32 size;
} rsx_guest_span;

/* Sizes are rounded up to whole pages.  A zero size disables that space
 * (note_write ignores it, queries report clean).  Returns 0 on success. */
int  rsx_guest_pages_init(rsx_guest_pages* t, u32 local_size, u32 main_size);
void rsx_guest_pages_destroy(rsx_guest_pages* t);

/* Publish a guest write of `size` bytes at `offset`.  Any alignment, any
 * size (zero is a no-op), cross-page and multi-page ranges included; the
 * range is clamped to the space.  Thread-safe against everything here. */
void rsx_guest_pages_note_write(rsx_guest_pages* t, u32 space, u32 offset,
                                u32 size);

/* O(1) whole-space write counter (acquire). */
u64 rsx_guest_pages_space_epoch(const rsx_guest_pages* t, u32 space);

/* Raw counter access for consumers that keep their own snapshot layout
 * (the GPU mirror keeps full-space arrays).  Acquire loads.  Out-of-range
 * indices read 0. */
u32 rsx_guest_pages_page_gen(const rsx_guest_pages* t, u32 space, u32 page);
u32 rsx_guest_pages_block_gen(const rsx_guest_pages* t, u32 space, u32 block);

/* ---- per-range snapshot convenience API --------------------------------
 * The snapshot for range [offset, offset+size) is a caller-owned u32 array
 * laid out as [covered block generations][covered page generations]. */

/* Number of u32 slots the caller must allocate for this range (0 when the
 * range is empty or outside the space). */
u32 rsx_guest_pages_snapshot_len(const rsx_guest_pages* t, u32 space,
                                 u32 offset, u32 size);

/* Fill `snap` with the current generations (blocks first, then pages). */
void rsx_guest_pages_snapshot(const rsx_guest_pages* t, u32 space,
                              u32 offset, u32 size, u32* snap);

/* 1 if any page overlapping the range changed since `snap` was recorded.
 * Uses the block counters to skip clean blocks. */
int rsx_guest_pages_range_dirty(const rsx_guest_pages* t, u32 space,
                                u32 offset, u32 size, const u32* snap);

/* Collect the dirty pages of the range as coalesced spans (clipped to the
 * range) and acknowledge exactly the collected pages in `snap` with the
 * generation read before returning (so the caller may copy the payload
 * after this call, seqlock contract above).  Collection stops when either
 * `max_spans` would be exceeded or the collected bytes exceed a nonzero
 * `budget_bytes`; unacknowledged pages simply stay dirty for a later call.
 * A block's snapshot slot is refreshed only when every dirty page of that
 * block inside the range was acknowledged.  Returns the span count and
 * sets *more to 1 when dirty pages remain. */
u32 rsx_guest_pages_collect_dirty(const rsx_guest_pages* t, u32 space,
                                  u32 offset, u32 size, u32* snap,
                                  rsx_guest_span* out, u32 max_spans,
                                  u32 budget_bytes, int* more);

/* Un-acknowledge one collected span (e.g. its upload was rejected): the
 * covered pages compare dirty again on the next query. */
void rsx_guest_pages_snapshot_undo(const rsx_guest_pages* t, u32 space,
                                   u32 offset, u32 size, u32* snap,
                                   u32 span_offset, u32 span_size);

/* TEST-ONLY: preload one page counter (and its block counter) to exercise
 * wrap-around.  Not for production use. */
void rsx_guest_pages_debug_set_page_gen(rsx_guest_pages* t, u32 space,
                                        u32 page, u32 value);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_GUEST_PAGES_H */
