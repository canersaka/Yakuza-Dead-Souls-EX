/*
 * ps3recomp - native-render resource model: mapping epochs, O(1) resource
 * lifetime cache, persistent PSO cache, and timeline fences.
 *
 * This is the backend-agnostic bookkeeping half of milestone 4. It answers,
 * without any GPU API and without linear searches:
 *
 *   - "is this guest range still the bytes my cached GPU resource was built
 *     from?"  ->  rsx_guest_pages dirty snapshot + IO-mapping epoch check.
 *   - "does a cached resource/PSO exist for this identity?"  ->  fixed-
 *     capacity open-addressing hash tables, O(1) expected.
 *   - "when may the upload-ring slice / retired resource be reused?"  ->
 *     timeline fences (D3D12 fence semantics modeled offline).
 *
 * Mapping epochs. The RSX IO window (main-memory space) is a *mapping*, not
 * an identity: cellGcmMapMainMemory-class operations rebind io offsets to
 * different EAs. A cached resource built from io bytes is invalidated by a
 * remap even when no guest write was published. rsx_nr_maps tracks the io
 * windows; every map/unmap bumps the global epoch and the touched windows'
 * generations. Resources snapshot the epoch at build time; validity
 * requires the epoch unchanged (conservative: any remap invalidates io-
 * space resources; exact per-window checks can tighten this later).
 *
 * Everything here is single-consumer (the render thread) except the fence
 * completion side, which any thread may signal.
 */

#ifndef PS3RECOMP_RSX_NR_RESOURCES_H
#define PS3RECOMP_RSX_NR_RESOURCES_H

#include "rsx_guest_pages.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Address spaces, matching rsx_dispatch.h RSX_LOCATION_* (kept local so
 * this module does not depend on the dispatcher). */
#define RSX_NR_SPACE_LOCAL 0
#define RSX_NR_SPACE_MAIN  1

/* ---------------------------------------------------------------------------
 * IO-mapping epochs
 * -----------------------------------------------------------------------*/

#define RSX_NR_MAX_IOMAPS 32

typedef struct rsx_nr_maps {
    struct {
        u32 io_start, size, ea_start;
        u32 gen;                     /* bumps every time this slot changes */
        int live;
    } win[RSX_NR_MAX_IOMAPS];
    u32 count;
    u64 epoch;                       /* bumps on every map/unmap           */
} rsx_nr_maps;

void rsx_nr_maps_init(rsx_nr_maps* m);

/* Map [io_start, io_start+size) -> ea_start. Replaces/splits nothing:
 * an exact-start remap reuses the slot (gen bump); overlapping windows
 * are rejected (-1). Returns 0 on success. */
int rsx_nr_maps_map(rsx_nr_maps* m, u32 io_start, u32 ea_start, u32 size);

/* Unmap the window starting at io_start. Returns 0, or -1 if absent. */
int rsx_nr_maps_unmap(rsx_nr_maps* m, u32 io_start);

static inline u64 rsx_nr_maps_epoch(const rsx_nr_maps* m) { return m->epoch; }

/* Resolve an io offset to a guest EA. Returns 1 and fills *ea (and the
 * window generation when win_gen is non-NULL); 0 when unmapped. The whole
 * [io, io+size) range must lie inside one window. */
int rsx_nr_maps_resolve(const rsx_nr_maps* m, u32 io, u32 size,
                        u32* ea, u32* win_gen);

/* ---------------------------------------------------------------------------
 * Resource lifetime cache (textures, vertex ranges, index ranges, ...)
 *
 * Identity = (kind, space, offset, size, fmt) — fmt folds any descriptor
 * bits that change the GPU resource (texture format/pitch/dims, index
 * width, ...). The cache owns per-entry guest-page snapshots (arena) and
 * the mapping-epoch stamp; the caller owns the backend object named by
 * backend_id and frees it from the evict callback.
 * -----------------------------------------------------------------------*/

typedef struct rsx_nr_res_key {
    u32 kind;                        /* caller-defined resource class      */
    u32 space;                       /* RSX_LOCATION_*                     */
    u32 offset;
    u32 size;
    u64 fmt;                         /* descriptor-identity fold           */
} rsx_nr_res_key;

typedef struct rsx_nr_res {
    u64 key_lo, key_hi;              /* packed key; 0/0 = empty slot       */
    u64 map_epoch;                   /* rsx_nr_maps epoch at (re)build     */
    u64 backend_id;                  /* caller's handle (opaque)           */
    u32 space, offset, size;         /* unpacked for dirty checks          */
    u32 snap_ofs, snap_len;          /* snapshot arena slice               */
    u32 last_use;                    /* frame stamp                        */
    u32 live;
} rsx_nr_res;

typedef struct rsx_nr_res_stats {
    u64 hits, misses, inserts, evictions, sweep_evictions;
    u64 stale_content, stale_mapping;/* revalidation failures by cause     */
    u64 arena_exhausted, table_full;
} rsx_nr_res_stats;

typedef struct rsx_nr_res_cache {
    rsx_nr_res* slots;               /* capacity power-of-two              */
    u32 cap, count;
    u32* arena;                      /* snapshot words                     */
    u32 arena_cap;
    u32* free_ofs;                   /* simple first-fit free list         */
    u32* free_len;
    u32 free_count, free_cap;
    u32 arena_bump;
    u32 frame;
    rsx_guest_pages* pages;
    const rsx_nr_maps* maps;         /* may be NULL (no epoch checks)      */
    rsx_nr_res_stats stats;
} rsx_nr_res_cache;

/* cap must be a power of two. Returns 0 on success. */
int  rsx_nr_res_cache_init(rsx_nr_res_cache* c, u32 cap, u32 arena_words,
                           rsx_guest_pages* pages, const rsx_nr_maps* maps);
void rsx_nr_res_cache_destroy(rsx_nr_res_cache* c);

/* O(1) expected lookup; NULL on miss. Hit stamps last_use. */
rsx_nr_res* rsx_nr_res_lookup(rsx_nr_res_cache* c, const rsx_nr_res_key* k);

/* Insert (key must not be present). Snapshots the range's current page
 * generations and the mapping epoch. Returns NULL when the table or the
 * snapshot arena is full (counted; the caller falls back to uncached). */
rsx_nr_res* rsx_nr_res_insert(rsx_nr_res_cache* c, const rsx_nr_res_key* k,
                              u64 backend_id);

/* 1 when the entry's guest bytes and mapping are unchanged since the last
 * (re)build — safe to use the cached backend resource as-is. */
int rsx_nr_res_current(rsx_nr_res_cache* c, const rsx_nr_res* e);

/* After the caller re-uploaded the backend resource from current guest
 * bytes: re-snapshot content + epoch so the entry is current again. */
void rsx_nr_res_revalidate(rsx_nr_res_cache* c, rsx_nr_res* e);

typedef void (*rsx_nr_res_evict_fn)(void* user, u64 backend_id);

/* Remove one entry (frees its snapshot; calls evict_cb for the backend). */
void rsx_nr_res_evict(rsx_nr_res_cache* c, rsx_nr_res* e,
                      rsx_nr_res_evict_fn cb, void* user);

/* Advance the frame stamp (call once per presented frame). */
void rsx_nr_res_next_frame(rsx_nr_res_cache* c);

/* Evict every live entry not used in the last max_age frames (0 = evict
 * everything). Returns the eviction count. */
u32 rsx_nr_res_sweep(rsx_nr_res_cache* c, u32 max_age,
                     rsx_nr_res_evict_fn cb, void* user);

/* ---------------------------------------------------------------------------
 * Persistent PSO / layout cache: content-hash key -> opaque handle.
 * Entries never go stale (a PSO's identity is its key); the table is a
 * plain open-addressing map with quadratic-ish probing and no deletion.
 * -----------------------------------------------------------------------*/

typedef struct rsx_nr_pso_stats {
    u64 hits, misses, inserts, table_full;
} rsx_nr_pso_stats;

typedef struct rsx_nr_pso_cache {
    u64* keys;                       /* 0 = empty (0 key is reserved)      */
    u64* values;
    u32 cap, count;
    rsx_nr_pso_stats stats;
} rsx_nr_pso_cache;

int  rsx_nr_pso_cache_init(rsx_nr_pso_cache* c, u32 cap);
void rsx_nr_pso_cache_destroy(rsx_nr_pso_cache* c);

/* Returns 1 and fills *value on hit. */
int  rsx_nr_pso_lookup(rsx_nr_pso_cache* c, u64 key, u64* value);
/* Returns 0, or -1 when full (counted; caller uses the value uncached). */
int  rsx_nr_pso_insert(rsx_nr_pso_cache* c, u64 key, u64 value);

/* FNV-1a fold helper for building PSO keys from state words. `seed` 0
 * starts a new hash; chain calls to fold more words. Never returns 0. */
u64 rsx_nr_hash_fold(u64 seed, const void* data, u32 bytes);

/* ---------------------------------------------------------------------------
 * Timeline fences (offline model of ID3D12Fence): a monotonically
 * increasing completed value; producers take signal values, any thread
 * completes, waiters test. Backs upload-ring reuse and native sync.
 * -----------------------------------------------------------------------*/

typedef struct rsx_nr_fence {
    volatile long long completed;
    unsigned long long next_value;   /* producer-side (single producer)    */
} rsx_nr_fence;

void rsx_nr_fence_init(rsx_nr_fence* f);
/* Producer: reserve the next signal value (to be completed later). */
u64  rsx_nr_fence_next(rsx_nr_fence* f);
/* Completion (any thread): completed = max(completed, value). */
void rsx_nr_fence_complete(rsx_nr_fence* f, u64 value);
/* 1 when `value` has completed. value 0 is always complete. */
int  rsx_nr_fence_done(const rsx_nr_fence* f, u64 value);
u64  rsx_nr_fence_completed(const rsx_nr_fence* f);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_NR_RESOURCES_H */
