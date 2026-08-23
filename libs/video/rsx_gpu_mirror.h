/*
 * ps3recomp - persistent GPU mirror of guest graphics memory
 *
 * Maintains GPU-resident copies of the two guest graphics address spaces
 * (RSX local and IO-mapped main memory) so shaders can read raw guest bytes
 * (true vertex pulling) without the CPU re-fetching and converting per draw.
 *
 * The mirror is deliberately incremental and bounded:
 *   - Only *registered* ranges (guest spans referenced by live graphics
 *     resources) are kept current; whole-space copies never happen.
 *   - Only pages the rsx_guest_pages tracker reports dirty are uploaded,
 *     coalesced into spans, optionally under a per-sync byte budget.
 *   - Uploads happen exclusively inside rsx_gpu_mirror_sync(), which the
 *     renderer calls at a safe synchronization point (while recording the
 *     frame's command list, before any draw that consumes the mirror).
 *     The backend owns staging-ring/fence hazards and may reject an upload
 *     (`ring full`); rejected pages simply stay dirty and retry.
 *
 * The core is backend-agnostic (no D3D12 types) so its registration, dirty
 * propagation, budget, and lifetime behavior are unit-testable offline; the
 * D3D12 backend lives in rsx_gpu_mirror_d3d12.c.
 *
 * Threading: unlike the tracker (whose writer side is lock-free and called
 * from guest-write paths), the mirror itself is single-consumer: register/
 * unregister/sync/queries must run on one thread (the render thread).
 */
#ifndef PS3RECOMP_RSX_GPU_MIRROR_H
#define PS3RECOMP_RSX_GPU_MIRROR_H

#include "rsx_guest_pages.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Backend interface.  `guest_ptr` matches rsx_live_guest_ptr_fn: resolve
 * `min_bytes` of readable guest memory for (space, offset) or NULL.
 * `upload` copies `size` guest bytes into the persistent GPU buffer of
 * `space` at the same byte offset; it is called only inside sync and must
 * return 0 on success or nonzero to reject (the pages stay dirty). */
typedef struct rsx_gpu_mirror_ops {
    void* user;
    const u8* (*guest_ptr)(void* user, u32 space, u32 offset, u32 min_bytes);
    int (*upload)(void* user, u32 space, u32 offset, const u8* src, u32 size);
} rsx_gpu_mirror_ops;

/* Nonzero opaque range handle; 0 is invalid. */
typedef u32 rsx_gpu_mirror_range;

typedef struct rsx_gpu_mirror_stats {
    u64 syncs;
    u64 epoch_skips;        /* whole space skipped: epoch unchanged        */
    u64 uploads;            /* backend upload calls that succeeded         */
    u64 upload_bytes;
    u64 upload_rejects;     /* backend said ring-full                      */
    u64 deferred_syncs;     /* syncs that left dirty pages behind          */
    u64 resolver_failures;  /* guest_ptr returned NULL for a dirty span    */
} rsx_gpu_mirror_stats;

typedef struct rsx_gpu_mirror rsx_gpu_mirror;   /* opaque */

/* `pages` must outlive the mirror.  Space sizes come from the tracker. */
rsx_gpu_mirror* rsx_gpu_mirror_create(rsx_guest_pages* pages,
                                      const rsx_gpu_mirror_ops* ops);
void rsx_gpu_mirror_destroy(rsx_gpu_mirror* m);

/* Reserve range slots before entering a live render loop.  Registering up to
 * `capacity` simultaneous ranges after this succeeds performs no allocation.
 * The call is only valid before the first registration; it returns 0 on
 * success and -1 on invalid input/allocation failure. */
int rsx_gpu_mirror_reserve_ranges(rsx_gpu_mirror* m, u32 capacity);

/* Register a guest range a graphics resource reads.  Overlapping ranges
 * share pages (each page is uploaded once no matter how many ranges cover
 * it).  Returns 0 on failure (empty/out-of-space range, allocation).  The
 * range starts stale; it becomes current after a sync uploads it. */
rsx_gpu_mirror_range rsx_gpu_mirror_register(rsx_gpu_mirror* m, u32 space,
                                             u32 offset, u32 size);

/* Drop a registration.  Pages no longer covered by any range keep their
 * bytes but stop being maintained; stale handles (already unregistered or
 * recycled) are ignored and return 0. */
int rsx_gpu_mirror_unregister(rsx_gpu_mirror* m, rsx_gpu_mirror_range r);

/* 1 when every page of the range is uploaded and no guest write has been
 * published against it since its last upload — i.e. a shader reading the
 * mirror sees exactly the current guest bytes.  Draw setup uses this to
 * fall back to the CPU path instead of consuming a stale mirror. */
int rsx_gpu_mirror_range_current(const rsx_gpu_mirror* m,
                                 rsx_gpu_mirror_range r);

/* Upload every dirty page of every registered range, coalesced, at most
 * `budget_bytes` per call when nonzero.  Returns the number of bytes
 * uploaded.  Call once per frame at the safe synchronization point. */
u32 rsx_gpu_mirror_sync(rsx_gpu_mirror* m, u32 budget_bytes);

void rsx_gpu_mirror_get_stats(const rsx_gpu_mirror* m,
                              rsx_gpu_mirror_stats* out);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_GPU_MIRROR_H */
