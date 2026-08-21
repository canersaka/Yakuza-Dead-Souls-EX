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
 * fragment programs — the solid test PS stands in) return failure to the
 * core, which counts them; nothing is silently approximated without a
 * counter.
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

typedef struct rsx_nr_d3d12_stats {
    unsigned long long clears, draws, draw_batches, presents, transfers;
    unsigned long long pso_hits, pso_builds;
    unsigned long long unsupported_draws;    /* refused to the core (sum)  */
    unsigned long long unsup_draw_topology;  /* fan/loop/quads/polygon     */
    unsigned long long unsup_draw_rt;        /* surface format/target      */
    unsigned long long unsup_draw_plan;      /* pull plan unsupported      */
    unsigned long long unsup_draw_pso;       /* compile/build failed       */
    unsigned long long unsup_draw_index;     /* index list unreadable      */
    unsigned long long restart_draws;        /* executed via strip-cut IB  */
    unsigned long long unsupported_clears;
    unsigned long long unsupported_transfers;
    unsigned long long approx_fp_draws;      /* solid PS stood in for FP   */
    unsigned long long compile_failures;
    unsigned long long rt_builds;
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

/* Read back a rendered RT registered by (space, offset): w*h*4 BGRA bytes
 * into out. Returns 0, or -1 when no such RT exists. Flushes first. */
int rsx_nr_d3d12_read_rt(rsx_nr_d3d12* b, u32 space, u32 offset,
                         u32 w, u32 h, u8* out);

void rsx_nr_d3d12_get_stats(const rsx_nr_d3d12* b, rsx_nr_d3d12_stats* out);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_NR_BACKEND_D3D12_H */
