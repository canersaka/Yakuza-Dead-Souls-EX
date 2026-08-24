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
 * fence contract. Draws and clears stay on one ordered command list until
 * present, an explicit barrier, capacity rollover, or a legacy ownership
 * handoff retires it.
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
typedef int (*rsx_nr_d3d12_borrow_color_fn)(
    void* user, u32 space, u32 offset, u32 width, u32 height,
    void** resource, u32* dxgi_format);
typedef int (*rsx_nr_d3d12_borrow_depth_fn)(
    void* user, u32 space, u32 offset, u32 depth_format,
    u32 width, u32 height, void** resource, u32* resource_format,
    u32* dsv_format, u32* srv_format, int* publication_required);
typedef void (*rsx_nr_d3d12_publish_write_fn)(
    void* user, u32 space, u32 offset, u32 size);
/* Resolve and read CellGcmReportData.value for a condition captured by
 * NV4097 SET_RENDER_ENABLE. Return zero with *value filled, or nonzero when
 * the exact report mapping is unavailable. */
typedef int (*rsx_nr_d3d12_render_condition_fn)(
    void* user, u32 dma_report, u32 offset, u32* value);

typedef struct rsx_nr_d3d12_stats {
    unsigned long long clears, draws, draw_batches, presents, transfers;
    unsigned long long queue_submissions;   /* fence-retired command lists */
    unsigned long long descriptor_table_hits;
    unsigned long long descriptor_table_builds;
    unsigned long long pso_hits, pso_builds;
    unsigned long long unsupported_draws;    /* refused to the core (sum)  */
    unsigned long long conditional_draws_skipped;
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
    unsigned long long rt_refreshes;     /* broker replaced same identity */
    unsigned long long depth_builds;
    unsigned long long depth_refreshes;  /* broker replaced same identity */
    unsigned long long resident_pages[2];
    unsigned long long residency_failures;
    unsigned long long mirror_resyncs;
    unsigned long long mirror_rollovers;
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
int rsx_nr_d3d12_depth_bounds_supported(const rsx_nr_d3d12* b);

/* Fill the GPU half of the exec ops (clear/draw/transfer/present/flush).
 * The embedder fills the host half before rsx_nr_backend_init. */
void rsx_nr_d3d12_get_exec_ops(rsx_nr_d3d12* b, rsx_nr_exec_ops* out);

/* Transactional section preflight. These routines may populate persistent
 * resource/PSO/page-watch caches, but never open/submit a command list,
 * modify guest memory, publish a report/label, or present. A live section is
 * eligible only when every action passes these checks before execution. */
enum rsx_nr_d3d12_draw_preflight_reason {
    RSX_NR_DRAW_PF_BAD_ARGUMENT = 1,
    RSX_NR_DRAW_PF_SURFACE_TARGET,
    RSX_NR_DRAW_PF_TOPOLOGY,
    RSX_NR_DRAW_PF_COLOR_TARGET,
    RSX_NR_DRAW_PF_DEPTH_TARGET,
    RSX_NR_DRAW_PF_FRAGMENT_RESOLVE,
    RSX_NR_DRAW_PF_FRAGMENT_UNSUPPORTED,
    RSX_NR_DRAW_PF_TEXTURE_DISABLED,
    RSX_NR_DRAW_PF_TEXTURE_INVALID,
    RSX_NR_DRAW_PF_VERTEX_PLAN,
    RSX_NR_DRAW_PF_PSO,
    RSX_NR_DRAW_PF_RESIDENCY,
    RSX_NR_DRAW_PF_UPLOAD_SCRATCH,
    RSX_NR_DRAW_PF_VERTEX_PROGRAM,
    RSX_NR_DRAW_PF_VERTEX_TEXTURE,
    RSX_NR_DRAW_PF_STENCIL_STATE,
    RSX_NR_DRAW_PF_DEPTH_BOUNDS,
    RSX_NR_DRAW_PF_RENDER_CONDITION,
};
int rsx_nr_d3d12_preflight_clear(rsx_nr_d3d12* b,
                                 const rsx_nir_pipeline* st,
                                 const rsx_nir_clear* clear);
int rsx_nr_d3d12_preflight_draw(rsx_nr_d3d12* b,
                                const rsx_nir_pipeline* st,
                                const u32* vp_words, u32 vp_word_count,
                                 const rsx_nir_draw* draw,
                                 const u32* batches);
/* Side-effect-free first gate for shader/program compatibility. It performs
 * no resource creation, page registration, command-list recording, PSO build,
 * allocation, submission, or guest write. */
int rsx_nr_d3d12_validate_draw_program(rsx_nr_d3d12* b,
                                       const rsx_nir_pipeline* st,
                                       const u32* vp_words,
                                       u32 vp_word_count);
int rsx_nr_d3d12_preflight_transfer(rsx_nr_d3d12* b,
                                    const rsx_nir_pipeline* st,
                                    const rsx_nir_transfer* transfer,
                                    const u32* words);
int rsx_nr_d3d12_preflight_present(rsx_nr_d3d12* b, u32 buffer_id);

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
void rsx_nr_d3d12_set_resource_broker(
    rsx_nr_d3d12* b, rsx_nr_d3d12_borrow_color_fn color,
    rsx_nr_d3d12_borrow_depth_fn depth, void* broker_user);

/* Optional post-publication route for host-memory transfer writes. When set,
 * the callback owns generation notification after the bytes are visible; the
 * backend's private page tracker is used directly only when no callback is
 * installed (the offline default). */
void rsx_nr_d3d12_set_publish_write(
    rsx_nr_d3d12* b, rsx_nr_d3d12_publish_write_fn publish, void* user);

/* Install the exact guest-report reader used by conditional draws. NULL
 * keeps such draws transactionally unsupported. */
void rsx_nr_d3d12_set_render_condition_reader(
    rsx_nr_d3d12* b, rsx_nr_d3d12_render_condition_fn read, void* user);

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
