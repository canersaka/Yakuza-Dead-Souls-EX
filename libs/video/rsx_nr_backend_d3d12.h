/*
 * ps3recomp - native-render D3D12 execution sink.
 *
 * Implements the GPU half of rsx_nr_exec_ops on D3D12 (offscreen; a WARP
 * device serves the offline tests, the live integration passes the shared
 * hardware device): render targets from folded surface state, clears,
 * PSO-cached vertex-pulling draws over the persistent guest-memory mirror,
 * host-semantic transfers, and fence-tracked presents. Host synchronization
 * callbacks (semaphores/reports/reference/user) are NOT provided here —
 * the embedder supplies those against its label window and merges the two
 * halves into one rsx_nr_exec_ops.
 *
 * Vertex path: true GPU vertex pulling (rsx_vertex_pull codegen over the
 * rsx_gpu_mirror ByteAddressBuffers at t20/t21). When the folded state
 * carries a vertex program, it is decompiled with pulled inputs
 * (rsx_vp_decompile_pull_ex); with no program (word_count 0) a clip-space
 * passthrough of ATTR0 is generated — the offline pixel tests use that
 * mode. Draw shapes the sink cannot yet execute faithfully (unsupported
 * primitive/format, primitive restart, scaled blits with real scaling,
 * fragment programs) return failure to the core, which counts them; nothing
 * is silently approximated. Fragment programs execute only when their exact
 * guest bytes are readable and every opcode/resource family is supported.
 *
 * Upload hazards: per-draw constants come from a fence-gated upload ring;
 * mirror staging reuse is governed by the mirror backend's three-slice
 * fence contract. The offline execution model is execute-and-wait per
 * present, which keeps the hazard machinery exercised and the validation
 * deterministic.
 */

#ifndef PS3RECOMP_RSX_NR_BACKEND_D3D12_H
#define PS3RECOMP_RSX_NR_BACKEND_D3D12_H

#include "rsx_nr_backend.h"
#include "rsx_guest_pages.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_nr_d3d12 rsx_nr_d3d12;

/* Optional live presentation handoff. The native queue is fully retired
 * before this callback, and `texture` is left in RENDER_TARGET state. The
 * callback may copy it to the application's swap chain, but must restore that
 * state before returning. `format` is the numeric DXGI_FORMAT value, kept
 * opaque here so this public header remains windows.h-free. */
typedef int (*rsx_nr_d3d12_present_fn)(void* user, void* texture, u32 format,
                                       u32 width, u32 height, u32 buffer_id);

/* Called exactly once when a guest page first becomes GPU-resident.  A live
 * embedder uses it to arm the VM write hook for that exact page.  Returning
 * nonzero refuses residency (and therefore the native draw) safely. */
typedef int (*rsx_nr_d3d12_watch_page_fn)(void* user, u32 space,
                                          u32 page_offset);

typedef struct rsx_nr_d3d12_stats {
    unsigned long long clears, draws, draw_batches, presents, transfers;
    unsigned long long pso_hits, pso_builds;
    unsigned long long unsupported_draws;    /* refused to the core (sum)  */
    unsigned long long unsup_draw_topology;  /* fan/loop/quads/polygon     */
    unsigned long long unsup_draw_rt;        /* surface format/target      */
    unsigned long long unsup_draw_plan;      /* pull plan unsupported      */
    unsigned long long unsup_draw_pso;       /* compile/build failed       */
    unsigned long long unsup_draw_index;     /* index list unreadable      */
    unsigned long long unsup_draw_fp;        /* FP unreadable/unsupported  */
    unsigned long long unsup_draw_texture;   /* FP texture lane pending    */
    unsigned long long unsup_topology_id[16];/* exact primitive census     */
    unsigned long long unsup_rt_format[32];  /* exact surface-format census */
    unsigned long long restart_draws;        /* executed via strip-cut IB  */
    unsigned long long unsupported_clears;
    unsigned long long unsupported_transfers;
    unsigned long long real_fp_draws;        /* real guest FP executed     */
    unsigned long long texture_draws;        /* real sampled draws         */
    unsigned long long texture_builds;       /* persistent guest uploads   */
    unsigned long long texture_hits;         /* unchanged cached resources */
    unsigned long long texture_refreshes;    /* dirty guest resource rebuilt */
    unsigned long long texture_failures;     /* exact texture refusal      */
    unsigned long long rt_alias_binds;       /* current native RT sampled  */
    unsigned long long compile_failures;
    unsigned long long rt_builds;
    unsigned long long resident_pages[2];
    unsigned long long residency_failures;
} rsx_nr_d3d12_stats;

/* guest_ptr resolves (space, offset, min_bytes) like rsx_live_guest_ptr_fn;
 * writable_ptr resolves the same bytes writable (transfers write guest
 * memory). `device` = existing ID3D12Device* or NULL to create a WARP
 * device (offline tests). Returns NULL on failure (no device / OOM). */
rsx_nr_d3d12* rsx_nr_d3d12_create(void* device, u32 local_size, u32 main_size,
                                  const u8* (*guest_ptr)(void* user, u32 space,
                                                         u32 offset,
                                                         u32 min_bytes),
                                  u8* (*writable_ptr)(void* user, u32 space,
                                                      u32 offset,
                                                      u32 min_bytes),
                                  void* user);
void rsx_nr_d3d12_destroy(rsx_nr_d3d12* b);

/* The guest-write tracker feeding the mirror; the embedder publishes guest
 * writes here (tests call note_write after touching arenas). */
rsx_guest_pages* rsx_nr_d3d12_pages(rsx_nr_d3d12* b);

/* Fill the GPU half of the exec ops (clear/draw/transfer/present/flush).
 * The embedder fills the host half before rsx_nr_backend_init. */
void rsx_nr_d3d12_get_exec_ops(rsx_nr_d3d12* b, rsx_nr_exec_ops* out);

/* Configure live scanout before the first native render target is created.
 * rgba_targets selects R8G8B8A8 for direct compatibility with the title's
 * swap chain; offline validation retains the default B8G8R8A8 targets. */
int rsx_nr_d3d12_set_live_output(rsx_nr_d3d12* b, int rgba_targets,
                                 rsx_nr_d3d12_present_fn present,
                                 void* present_user);
void rsx_nr_d3d12_set_display_buffer(rsx_nr_d3d12* b, u32 buffer_id,
                                     u32 location, u32 offset,
                                     u32 width, u32 height);

/* Install the exact-page VM hook before executing native draws.  NULL is
 * valid for offline arenas whose writers publish directly through
 * rsx_nr_d3d12_note_guest_write(). */
void rsx_nr_d3d12_set_watch_page(rsx_nr_d3d12* b,
                                 rsx_nr_d3d12_watch_page_fn watch,
                                 void* watch_user);

/* Publish a completed guest write to the lock-free generation tracker. */
void rsx_nr_d3d12_note_guest_write(rsx_nr_d3d12* b, u32 space,
                                   u32 offset, u32 size);

/* Read back a rendered RT registered by (space, offset): w*h*4 BGRA bytes
 * into out. Returns 0, or -1 when no such RT exists. Flushes first. */
int rsx_nr_d3d12_read_rt(rsx_nr_d3d12* b, u32 space, u32 offset,
                         u32 w, u32 h, u8* out);

void rsx_nr_d3d12_get_stats(const rsx_nr_d3d12* b, rsx_nr_d3d12_stats* out);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_NR_BACKEND_D3D12_H */
