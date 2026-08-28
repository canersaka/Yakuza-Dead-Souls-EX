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
    int create, void** resource, u32* dxgi_format,
    u32* resource_width, u32* resource_height);
typedef int (*rsx_nr_d3d12_borrow_depth_fn)(
    void* user, u32 space, u32 offset, u32 depth_format,
    u32 width, u32 height, int create, void** resource, u32* resource_format,
    u32* dsv_format, u32* srv_format, void** sample_resource,
    u32* sample_srv_format, int* publication_required);
/* Record the established renderer's depth-to-color snapshot on the same
 * ordered command list. A zero result guarantees sample_resource contains the
 * representation legacy rendering would bind; nonzero makes the backend use
 * its already-preflighted guest-memory fallback for that draw. */
typedef int (*rsx_nr_d3d12_resolve_depth_sample_fn)(
    void* user, u32 space, u32 offset, u32 width, u32 height);
typedef void (*rsx_nr_d3d12_publish_write_fn)(
    void* user, u32 space, u32 offset, u32 size);
/* Resolve and read CellGcmReportData.value for a condition captured by
 * NV4097 SET_RENDER_ENABLE. Return zero with *value filled, or nonzero when
 * the exact report mapping is unavailable. */
typedef int (*rsx_nr_d3d12_render_condition_fn)(
    void* user, u32 dma_report, u32 offset, u32* value);

/* Optional live command-timeline broker.  acquire() leases the application's
 * currently open DIRECT list until release() and reports the fence value that
 * will retire that list generation.  flush() closes, submits, waits, and
 * resets that same list.  The backend never closes or resets a borrowed list
 * itself.  Offline/WARP users leave this unset and retain the private queue. */
typedef int (*rsx_nr_d3d12_timeline_acquire_fn)(
    void* user, void** command_list, unsigned long long* generation,
    unsigned long long* recording_fence,
    unsigned long long* completed_fence);
typedef void (*rsx_nr_d3d12_timeline_release_fn)(void* user);
typedef int (*rsx_nr_d3d12_timeline_flush_fn)(void* user);

/* Optional live content cache. Shader blobs are opaque ID3DBlob pointers with
 * one caller-owned reference. Driver PSO blobs are malloc-like byte arrays
 * released through pso_free. Offline/WARP callers may leave all callbacks
 * unset and retain direct D3D compilation/creation. */
typedef void* (*rsx_nr_d3d12_compile_shader_fn)(
    void* user, u32 stage, const char* source, u32 source_length,
    u32 compiler_flags, int* cache_hit, int* compiled);
typedef int (*rsx_nr_d3d12_pso_load_fn)(
    void* user, u64 pso_key, u64 vertex_bytecode_hash,
    u64 pixel_bytecode_hash, void** data, u32* size);
typedef int (*rsx_nr_d3d12_pso_store_fn)(
    void* user, u64 pso_key, u64 vertex_bytecode_hash,
    u64 pixel_bytecode_hash, const void* data, u32 size);
typedef void (*rsx_nr_d3d12_pso_free_fn)(void* user, void* data);

typedef enum rsx_nr_d3d12_submit_cause {
    RSX_NR_D3D12_SUBMIT_DESCRIPTOR_RECYCLE = 0,
    RSX_NR_D3D12_SUBMIT_UPLOAD_ROLLOVER,
    RSX_NR_D3D12_SUBMIT_SEMAPHORE_PUBLICATION,
    RSX_NR_D3D12_SUBMIT_REFERENCE_PUBLICATION,
    RSX_NR_D3D12_SUBMIT_REPORT_PUBLICATION,
    RSX_NR_D3D12_SUBMIT_BARRIER_PUBLICATION,
    RSX_NR_D3D12_SUBMIT_TRANSFER_READBACK,
    RSX_NR_D3D12_SUBMIT_RESOURCE_REFRESH,
    RSX_NR_D3D12_SUBMIT_RESIDENCY_RETRY,
    RSX_NR_D3D12_SUBMIT_REFUSAL_RETIREMENT,
    RSX_NR_D3D12_SUBMIT_PRESENT,
    RSX_NR_D3D12_SUBMIT_DIAGNOSTIC_READBACK,
    RSX_NR_D3D12_SUBMIT_SHUTDOWN_RESET,
    RSX_NR_D3D12_SUBMIT_OTHER,
    RSX_NR_D3D12_SUBMIT_CAUSE_COUNT
} rsx_nr_d3d12_submit_cause;

typedef struct rsx_nr_d3d12_submit_cause_stats {
    unsigned long long submissions;
    unsigned long long cpu_wait_ticks;
    unsigned long long gpu_ticks;
    unsigned long long gpu_intervals;
    unsigned long long draws;
    unsigned long long draw_batches;
    unsigned long long descriptor_tables;
    unsigned long long upload_bytes;
    unsigned long long readback_bytes;
} rsx_nr_d3d12_submit_cause_stats;

#define RSX_NR_D3D12_TAIL_BUCKET_CAP 512u

/* One diagnostics-only, per-present delta. The ring is fixed at creation and
 * is copied only during orderly shutdown. GPU ticks are folded into the same
 * bucket after the final asynchronous query resolve. */
typedef struct rsx_nr_d3d12_tail_bucket {
    unsigned long long present_sequence;
    unsigned long long end_qpc;
    unsigned long long adaptation_calls;
    unsigned long long adaptation_ticks;
    unsigned long long draws;
    unsigned long long fence_ticks;
    unsigned long long flush_ticks;
    unsigned long long transfer_readback_ticks;
    unsigned long long transfer_readback_bytes;
    unsigned long long transfer_upload_ticks;
    unsigned long long transfer_upload_bytes;
    unsigned long long residency_prepare_ticks;
    unsigned long long residency_stabilize_ticks;
    unsigned long long preflight_draw_ticks;
    unsigned long long draw_ticks;
    unsigned long long fp_resolve_ticks;
    unsigned long long pso_lookup_ticks;
    unsigned long long pso_key_lookup_ticks;
    unsigned long long shader_compile_ticks;
    unsigned long long shader_cache_ticks;
    unsigned long long driver_pso_ticks;
    unsigned long long texture_prepare_ticks;
    unsigned long long batch_prepare_ticks;
    unsigned long long command_record_ticks;
    unsigned long long submit_count[RSX_NR_D3D12_SUBMIT_CAUSE_COUNT];
    unsigned long long submit_cpu_ticks[RSX_NR_D3D12_SUBMIT_CAUSE_COUNT];
    unsigned long long submit_gpu_ticks[RSX_NR_D3D12_SUBMIT_CAUSE_COUNT];
    unsigned long long submit_gpu_intervals[RSX_NR_D3D12_SUBMIT_CAUSE_COUNT];
} rsx_nr_d3d12_tail_bucket;

typedef struct rsx_nr_d3d12_stats {
    unsigned long long clears, draws, draw_batches, presents, transfers;
    unsigned long long present_failures;
    unsigned int first_present_failure_stage; /* 1=no RT, 2=handoff */
    unsigned int first_present_failure_buffer;
    unsigned int first_present_failure_display_valid;
    unsigned int first_present_failure_space;
    unsigned int first_present_failure_offset;
    unsigned int first_present_failure_width;
    unsigned int first_present_failure_height;
    unsigned int first_present_failure_format;
    unsigned int first_present_failure_dxgi;
    unsigned long long first_present_failure_generation;
    unsigned long long first_present_failure_recording_fence;
    unsigned long long first_present_failure_completed_fence;
    unsigned long long transfer_gpu_readbacks;
    unsigned long long transfer_gpu_uploads;
    unsigned long long queue_submissions;   /* fence-retired command lists */
    unsigned long long descriptor_table_hits;
    unsigned long long descriptor_table_builds;
    unsigned long long snapshot_islands;
    unsigned long long snapshot_draws_prepared;
    unsigned long long snapshot_draws_executed;
    unsigned long long snapshot_prepare_restarts;
    unsigned long long snapshot_prepare_failures;
    unsigned long long
        snapshot_prepare_submissions[RSX_NR_D3D12_SUBMIT_CAUSE_COUNT];
    unsigned int first_snapshot_prepare_failure_cause;
    unsigned int first_snapshot_prepare_failure_prior_draws;
    unsigned int first_snapshot_prepare_failure_attempt;
    unsigned long long snapshot_execute_failures;
    unsigned int first_snapshot_execute_failure_stage;
    unsigned int first_snapshot_execute_id;
    unsigned int first_snapshot_execute_count;
    unsigned int first_snapshot_execute_valid;
    unsigned int first_snapshot_execute_condition_dma;
    unsigned int first_snapshot_execute_condition_offset;
    unsigned long long pso_hits, pso_builds;
    unsigned long long vertex_shader_builds;
    unsigned long long pixel_shader_builds;
    unsigned long long vertex_shader_cache_hits;
    unsigned long long pixel_shader_cache_hits;
    unsigned long long driver_pso_creates;
    unsigned long long driver_pso_cache_hits;
    unsigned long long driver_pso_cache_writes;
    unsigned long long driver_pso_cache_rejects;
    unsigned long long unsupported_draws;    /* refused to the core (sum)  */
    unsigned long long conditional_draws_skipped;
    unsigned long long unsup_draw_topology;  /* fan/loop/quads/polygon     */
    unsigned long long unsup_draw_rt;        /* surface format/target      */
    unsigned long long unsup_draw_plan;      /* pull plan unsupported      */
    unsigned long long unsup_draw_pso;       /* compile/build failed       */
    unsigned long long unsup_draw_index;     /* index list unreadable      */
    unsigned long long unsup_draw_fp;        /* FP unreadable/unsupported  */
    unsigned long long unsup_draw_texture;   /* FP texture lane pending    */
    unsigned long long unsup_upload_index;   /* host IB arena reservation   */
    unsigned long long unsup_upload_pull;    /* vertex-pull CB reservation  */
    unsigned long long unsup_upload_vp;      /* vertex constants reservation */
    unsigned long long unsup_upload_fp;      /* fragment constants reserve  */
    unsigned long long first_upload_used;
    unsigned long long first_upload_budget;
    unsigned long long first_upload_request;
    unsigned int first_upload_batches;
    unsigned int first_upload_stage;         /* 1=index 2=pull 3=VP 4=FP   */
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
    /* First exact execution-time texture refusal. Populated only on failure,
     * so the clean path pays no diagnostic timing or I/O. Stage:
     * 1=disabled, 2=color self-alias, 3=depth self-alias,
     * 4=private-depth resolve, 5=guest texture resolve,
     * 6=vertex texture resolve, 7=descriptor capacity, 8=cube mismatch,
     * 9=rollover retire/open. */
    unsigned int first_texture_failure_stage;
    unsigned int first_texture_failure_unit;
    int first_texture_failure_result;
    unsigned int first_texture_failure_mask;
    unsigned int first_texture_failure_location;
    unsigned int first_texture_failure_offset;
    unsigned int first_texture_failure_format;
    unsigned int first_texture_failure_width;
    unsigned int first_texture_failure_height;
    unsigned int first_texture_failure_pitch;
    unsigned int first_texture_failure_mipmaps;
    unsigned int first_texture_failure_cubemap;
    unsigned int first_texture_cache_count;
    unsigned long long first_texture_cache_table_full;
    unsigned long long first_texture_cache_arena_exhausted;
    /* First exact execution-time fragment-program refusal. Populated only
     * on the failure branch; accepted draws pay no diagnostic clock, copy,
     * allocation, or I/O. Stage: 1=resolve/read failure, 2=unsupported
     * instruction/modifier. Reason bits: 1=opcode, 2=source modifier. */
    unsigned int first_fp_failure_stage;
    int first_fp_failure_result;
    unsigned int first_fp_failure_location;
    unsigned int first_fp_failure_offset;
    unsigned int first_fp_failure_control;
    unsigned int first_fp_failure_size;
    unsigned int first_fp_failure_texture_mask;
    unsigned int first_fp_failure_unsupported_count;
    unsigned int first_fp_failure_instruction_offset;
    unsigned int first_fp_failure_opcode;
    unsigned int first_fp_failure_reason;
    unsigned long long first_fp_failure_structural_hash;
    unsigned long long first_fp_failure_byte_hash;
    unsigned int first_fp_failure_words[16];
    unsigned int texture_cache_count;
    unsigned int texture_cache_capacity;
    unsigned long long texture_cache_table_full;
    unsigned long long texture_cache_arena_exhausted;
    unsigned int pso_cache_count;
    unsigned int pso_cache_capacity;
    unsigned long long pso_cache_table_full;
    unsigned long long rt_alias_binds;       /* current native RT sampled  */
    unsigned long long compile_failures;
    unsigned long long rt_builds;
    unsigned long long rt_refreshes;     /* broker replaced same identity */
    unsigned long long depth_builds;
    unsigned long long depth_refreshes;  /* broker replaced same identity */
    unsigned long long depth_snapshot_builds;
    unsigned long long depth_snapshot_resolves;
    unsigned long long resident_pages[2];
    unsigned long long watched_guest_pages[2];
    unsigned long long residency_failures;
    unsigned long long mirror_resyncs;
    unsigned long long mirror_rollovers;
    unsigned long long mirror_syncs;
    unsigned long long mirror_uploads;
    unsigned long long mirror_upload_bytes;
    unsigned long long mirror_upload_rejects;
    unsigned long long mirror_deferred_syncs;
    unsigned long long mirror_resolver_failures;
    unsigned long long mirror_exact_patches;
    unsigned long long mirror_exact_patch_bytes;
    unsigned long long mirror_exact_patch_retries;
    unsigned int first_residency_failure_stage; /* 1=plan 2=span 3=stabilize */
    unsigned int first_residency_failure_result;
    unsigned int first_residency_required_count;
    unsigned int first_residency_space;
    unsigned int first_residency_offset;
    unsigned int first_residency_size;
    unsigned int first_residency_first_page;
    unsigned int first_residency_last_page;
    unsigned int first_residency_first_gen;
    unsigned int first_residency_last_gen;
    unsigned long long forced_draw_input_refreshes;
    unsigned long long upload_rollovers;  /* safe pre-draw arena retires   */
    unsigned long long shared_timeline_acquires;
    unsigned long long shared_timeline_generations;
    unsigned long long shared_timeline_forced_submissions;
    /* Default-off shutdown aggregate. Ticks use stall_qpc_frequency and
     * overlap intentionally: a transfer readback includes its fence drain,
     * while explicit flush calls identify synchronization boundaries. */
    unsigned long long stall_qpc_frequency;
    unsigned long long stall_fence_drain_count;
    unsigned long long stall_fence_drain_ticks;
    unsigned long long stall_flush_count;
    unsigned long long stall_flush_ticks;
    unsigned long long stall_transfer_readback_count;
    unsigned long long stall_transfer_readback_ticks;
    unsigned long long stall_transfer_readback_bytes;
    unsigned long long stall_transfer_upload_count;
    unsigned long long stall_transfer_upload_ticks;
    unsigned long long stall_transfer_upload_bytes;
    unsigned long long stall_residency_prepare_count;
    unsigned long long stall_residency_prepare_ticks;
    unsigned long long stall_residency_stabilize_count;
    unsigned long long stall_residency_stabilize_ticks;
    unsigned long long stall_preflight_draw_count;
    unsigned long long stall_preflight_draw_ticks;
    unsigned long long stall_draw_count;
    unsigned long long stall_draw_ticks;
    unsigned long long stall_fp_resolve_count;
    unsigned long long stall_fp_resolve_ticks;
    unsigned long long stall_pso_lookup_count;
    unsigned long long stall_pso_lookup_ticks;
    unsigned long long stall_pso_key_lookup_count;
    unsigned long long stall_pso_key_lookup_ticks;
    unsigned long long stall_vertex_compile_count;
    unsigned long long stall_vertex_compile_ticks;
    unsigned long long stall_vertex_cache_count;
    unsigned long long stall_vertex_cache_ticks;
    unsigned long long stall_pixel_compile_count;
    unsigned long long stall_pixel_compile_ticks;
    unsigned long long stall_pixel_cache_count;
    unsigned long long stall_pixel_cache_ticks;
    unsigned long long stall_driver_pso_create_count;
    unsigned long long stall_driver_pso_create_ticks;
    unsigned long long stall_texture_prepare_count;
    unsigned long long stall_texture_prepare_ticks;
    unsigned long long stall_batch_prepare_count;
    unsigned long long stall_batch_prepare_ticks;
    unsigned long long stall_command_record_count;
    unsigned long long stall_command_record_ticks;
    /* Default-off fixed-memory submission attribution.  Enabled only by
     * YZ_NR_SUBMIT_ATTRIBUTION=1 and emitted once during orderly shutdown. */
    unsigned long long submit_attribution_qpc_frequency;
    rsx_nr_d3d12_submit_cause_stats
        submit_cause[RSX_NR_D3D12_SUBMIT_CAUSE_COUNT];
    unsigned long long submit_transfer_readback_count;
    unsigned long long submit_transfer_readback_ticks;
    unsigned long long submit_transfer_readback_bytes;
    unsigned long long submit_transfer_upload_count;
    unsigned long long submit_transfer_upload_ticks;
    unsigned long long submit_transfer_upload_bytes;
    /* Unified default-off RSX-tail diagnostic. GPU timestamps are resolved
     * into a fixed readback buffer on the command list which they measure;
     * the CPU maps that buffer only during orderly shutdown. */
    unsigned long long tail_gpu_frequency;
    unsigned long long tail_gpu_intervals_recorded;
    unsigned long long tail_gpu_intervals_dropped;
} rsx_nr_d3d12_stats;

/* Fixed-memory, default-off scanout provenance.  This deliberately exposes
 * only stable identities and aggregate write/present counts; it never reads
 * a render target back or emits output from the backend. */
typedef struct rsx_nr_d3d12_rt_provenance {
    unsigned long long resource_identity;
    unsigned long long write_serial;
    unsigned long long color_clear_writes;
    unsigned long long draw_writes;
    unsigned long long present_count;
    unsigned int space, offset, format, width, height;
    unsigned int resource_state;
    unsigned int external;
} rsx_nr_d3d12_rt_provenance;

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
/* Default-off diagnostic finalization.  When the Hana input oracle was
 * enabled at creation, retire its already-recorded copies and emit the fixed
 * shutdown aggregate without destroying process-lifetime renderer objects. */
int rsx_nr_d3d12_dump_hana_input(rsx_nr_d3d12* b);

/* The guest-write tracker feeding the mirror; the embedder publishes guest
 * writes here (tests call note_write after touching arenas). */
rsx_guest_pages* rsx_nr_d3d12_pages(rsx_nr_d3d12* b);
int rsx_nr_d3d12_depth_bounds_supported(const rsx_nr_d3d12* b);

/* Install the live process-wide content cache before the first PSO request. */
int rsx_nr_d3d12_set_content_cache(
    rsx_nr_d3d12* b, rsx_nr_d3d12_compile_shader_fn compile_shader,
    rsx_nr_d3d12_pso_load_fn pso_load,
    rsx_nr_d3d12_pso_store_fn pso_store,
    rsx_nr_d3d12_pso_free_fn pso_free, void* user);

/* Fill the GPU half of the exec ops (clear/draw/transfer/present/flush).
 * The embedder fills the host half before rsx_nr_backend_init. */
void rsx_nr_d3d12_get_exec_ops(rsx_nr_d3d12* b, rsx_nr_exec_ops* out);

/* Capture one draw's immutable resources at decoder/recording time while its
 * FIFO GET remains withheld. Island prepare is then a constant-time commit
 * gate; neither path decodes a FIFO method or executes a draw. */
int rsx_nr_d3d12_record_snapshot_draw(
    rsx_nr_d3d12* b, const rsx_nr_backend* recorded_state,
    rsx_nir_stream* stream, u32 op_index);
int rsx_nr_d3d12_prepare_snapshot_island(
    rsx_nr_d3d12* b, const rsx_nr_backend* initial_state,
    rsx_nir_stream* stream);
void rsx_nr_d3d12_finish_snapshot_island(
    rsx_nr_d3d12* b, rsx_nr_backend* backend, int committed);

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
/* Same side-effect-free gate, additionally returning the exact fragment
 * texture-unit usage decoded from TEX/TXP instructions. The output is valid
 * only on success. */
int rsx_nr_d3d12_validate_draw_program_usage(
    rsx_nr_d3d12* b, const rsx_nir_pipeline* st,
    const u32* vp_words, u32 vp_word_count, u32* texture_mask);
/* Validate/import an already-existing depth target for texture sampling.
 * The broker lookup is exact and lookup-only: it may retain the resource in
 * the backend cache, but must not create, clear, submit, or wait for one. */
int rsx_nr_d3d12_validate_depth_sample_alias(
    rsx_nr_d3d12* b, const rsx_nir_texture* texture);
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
/* Enable the shutdown-only scanout provenance counters before the first GPU
 * operation. The default-off path is one predictable branch at successful
 * color clears, draws, and presents, with no clocks, allocation, or I/O. */
int rsx_nr_d3d12_set_scanout_provenance(rsx_nr_d3d12* b, int enabled);
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
    rsx_nr_d3d12_borrow_depth_fn depth,
    rsx_nr_d3d12_resolve_depth_sample_fn resolve_depth_sample,
    void* broker_user);

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

/* Move recording from the backend's private queue to one ordered host list.
 * Must be installed before the first GPU operation.  Returns nonzero if the
 * broker cannot provide a valid open list/fence generation. */
int rsx_nr_d3d12_set_shared_timeline(
    rsx_nr_d3d12* b, rsx_nr_d3d12_timeline_acquire_fn acquire,
    rsx_nr_d3d12_timeline_release_fn release,
    rsx_nr_d3d12_timeline_flush_fn flush, void* user);
int rsx_nr_d3d12_shared_timeline_enabled(const rsx_nr_d3d12* b);
/* Retire the currently leased native list for an exact pending GPU-report
 * consumer. This is the only safe way to flush from inside draw execution:
 * it releases the backend's shared-timeline lease before invoking the host
 * flush and attributes the forced submission as report publication. */
int rsx_nr_d3d12_flush_report_dependency(rsx_nr_d3d12* b);

/* Declare that every draw submitted to this backend is owned as part of a
 * completely preflighted render section. This admits the captured combined
 * flow+vertex-texture family whose old failure came from mixing its native
 * depth work with later legacy draws. Default is disabled. It must be set
 * before any preflight/draw/PSO build; returns nonzero if too late. */
int rsx_nr_d3d12_set_coherent_section_mode(rsx_nr_d3d12* b, int enabled);

/* Diagnostic-only exact draw-input refresh. When enabled, execution marks
 * only the already-preflighted vertex/index spans dirty immediately before
 * their mirror synchronization. This distinguishes a missed guest-writer
 * notification from shader/raster defects without broad memory scanning.
 * Default is disabled and the disabled path is one predictable branch. */
int rsx_nr_d3d12_set_force_draw_input_refresh(rsx_nr_d3d12* b,
                                              int enabled);
/* Advance the diagnostic's fixed-memory page-deduplication epoch at one
 * complete ownership boundary. Repeated draws of the same page inside that
 * section then produce one refresh, not one upload per draw. */
void rsx_nr_d3d12_begin_draw_input_refresh_section(rsx_nr_d3d12* b);

/* Publish a completed guest write to the lock-free generation tracker. */
void rsx_nr_d3d12_note_guest_write(rsx_nr_d3d12* b, u32 space,
                                   u32 offset, u32 size);

/* Read back a rendered RT registered by (space, offset): w*h*4 BGRA bytes
 * into out. Returns 0, or -1 when no such RT exists. Flushes first. */
int rsx_nr_d3d12_read_rt(rsx_nr_d3d12* b, u32 space, u32 offset,
                         u32 w, u32 h, u8* out);

/* Offline validation readback for one exact depth target. Writes w*h
 * float32 depth values in row-major order and flushes first. The lookup is
 * exact, so diagnostics cannot accidentally create or substitute a target. */
int rsx_nr_d3d12_read_depth(rsx_nr_d3d12* b, u32 space, u32 offset,
                            u32 format, u32 w, u32 h, float* out);

/* Enumerate live render targets for deterministic offline capture validation.
 * Ordinals are dense over the currently live target table.
 * Returns 0 on success or -1 when ordinal is past the last live target. */
int rsx_nr_d3d12_rt_info(const rsx_nr_d3d12* b, u32 ordinal,
                         u32* space, u32* offset, u32* format,
                         u32* width, u32* height);
int rsx_nr_d3d12_get_rt_provenance(
    const rsx_nr_d3d12* b, u32 ordinal,
    rsx_nr_d3d12_rt_provenance* out);

void rsx_nr_d3d12_get_stats(const rsx_nr_d3d12* b, rsx_nr_d3d12_stats* out);

/* Retire and fold the optional asynchronous GPU timestamp ring.  This is a
 * shutdown-only operation; ordinary execution and disabled diagnostics never
 * map a readback resource or wait for a diagnostic query. */
int rsx_nr_d3d12_finalize_tail_breakdown(rsx_nr_d3d12* b);
u32 rsx_nr_d3d12_tail_bucket_count(const rsx_nr_d3d12* b);
int rsx_nr_d3d12_get_tail_bucket(
    const rsx_nr_d3d12* b, u32 chronological_index,
    rsx_nr_d3d12_tail_bucket* out);
void rsx_nr_d3d12_tail_note_adaptation(
    rsx_nr_d3d12* b, unsigned long long ticks);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_NR_BACKEND_D3D12_H */
