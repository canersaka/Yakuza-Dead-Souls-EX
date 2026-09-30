/*
 * ps3recomp - live NV4097 draw path (implementation)
 *
 * See rsx_live_draw.h. The D3D12 engine driven by the live FIFO consumer:
 *   - rsx_dispatch register-file model
 *   - NV40 VP/FP -> HLSL decompilers
 *   - render/sampler state -> D3D12 PSO + dynamic samplers + mip chains
 *   - guest memory through an injected resolver (the runtime's vm_base map)
 *   - present to a swap chain bound to the runtime's window
 *
 * Clean-room: NV40 ISA/register facts from envytools rnndb, Mesa nv30 and
 * psdevwiki.
 */

#include "rsx_live_draw.h"
#include "rsx_commands.h"

#if !defined(_WIN32)
int rsx_live_draw_set_early_geometry_cache(int enabled)
{ (void)enabled; return 0; }

/* Non-Windows: the whole path is a no-op (D3D12 is Windows-only). */
int  rsx_live_draw_enabled(void) { return 0; }
int  rsx_live_draw_init(void* hwnd, u32 w, u32 h, rsx_live_guest_ptr_fn f, void* u)
{ (void)hwnd; (void)w; (void)h; (void)f; (void)u; return 0; }
void rsx_live_draw_seed_registers(const u32* r, u32 n) { (void)r; (void)n; }
void rsx_live_draw_seed_transform_program(const u32* w, u32 n) { (void)w; (void)n; }
void rsx_live_draw_seed_transform_constants(const u32* w, u32 n) { (void)w; (void)n; }
void rsx_live_draw_set_display_buffer(
    u32 b, u32 l, u32 o, u32 p, u32 w, u32 h)
{ (void)b; (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_live_draw_method(u32 m, u32 a) { (void)m; (void)a; }
int rsx_live_draw_prepared_packet_supported(u32 h) { (void)h; return 0; }
int rsx_live_draw_graphics_packet(u32 h,const u32* a,struct rsx_state* s)
{ (void)h; (void)a; (void)s; return 0; }
void rsx_live_draw_flush(void) {}
void rsx_live_draw_present(u32 b) { (void)b; }
void rsx_live_draw_set_movie_mode(int on) { (void)on; }
void rsx_live_draw_present_rgba(const uint8_t* r, u32 w, u32 h) { (void)r; (void)w; (void)h; }
u32  rsx_live_draw_get_frames(void) { return 0; }
u64  rsx_live_draw_get_completed_draws(void) { return 0; }
u32  rsx_live_draw_get_last_draws(void) { return 0; }
double rsx_live_draw_get_present_fps(void) { return 0.0; }
int rsx_live_draw_set_geometry_cache(int enabled) { (void)enabled; return 0; }
void rsx_live_draw_dump_geometry_cache(void) {}
void rsx_live_draw_shutdown(void) {}

#else /* _WIN32 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "rsx_dispatch.h"
#include "rsx_fp_decompiler.h"
#include "rsx_hash.h"
#include "rsx_restart_cuts.h"
#include "rsx_vertex_compact.h"
#include "rsx_vp_decompiler.h"
#include "rsx_vp_metadata_cache.h"

#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>

#define LD_PRESENT_INTERVAL 1u

#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include "rsx_vertex_formats.h"
#include "rsx_texture_lookup.h"
#include "rsx_material_cache.h"
#include "rsx_prepared_packet.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* ---------------------------------------------------------------------------
 * Engine state (module-static; single live RSX)
 * -----------------------------------------------------------------------*/

#define LD_SWAP_BUFFERS  2
#define MAX_SURFACES     64
#define MAX_TEXTURES     RSX_TEXTURE_LOOKUP_CAPACITY
#define MAX_RETIRED_TEXTURES 4096
#define MAX_VTEX         64
#define MAX_SAMPLERS     128
/* Distinct pipeline keys seen across a full boot exceed 2,000; the cap is the
 * next power of two. */
#define MAX_PSOS         8192
#define MAX_SHADER_BLOBS 8192
#define UPLOAD_SIZE      (64u * 1024 * 1024)

#define SRV_WHITE        0
#define SRV_SURFACE_BASE 1
#define SRV_ZDEPTH_BASE  (SRV_SURFACE_BASE + MAX_SURFACES)
#define SRV_TEXTURE_BASE (SRV_ZDEPTH_BASE + MAX_SURFACES)
#define SRV_VTEX_BASE    (SRV_TEXTURE_BASE + MAX_TEXTURES)
#define SRV_HEAP_SLOTS   (SRV_VTEX_BASE + MAX_VTEX)
#define SRV_DEPTH_SOURCE_BASE SRV_HEAP_SLOTS
#define UAV_ZDEPTH_BASE   (SRV_DEPTH_SOURCE_BASE + MAX_SURFACES)
#define SRV_CPU_HEAP_SLOTS (UAV_ZDEPTH_BASE + MAX_SURFACES)
#define SRV_TABLE_SIZE   16
#define SRV_RING_TABLES  4096

#define SMP_DEFAULT      0
#define SMP_CACHE_SLOTS  MAX_SAMPLERS
#define SMP_TABLE_SIZE   16
/* A shader-visible SAMPLER heap is hard-capped at 2048 descriptors by D3D12
 * (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE). SMP_RING_TABLES*SMP_TABLE_SIZE
 * must stay <= 2048, else CreateDescriptorHeap fails. Identical sampler
 * tables share entries, so this is 128 unique tables between GPU flushes. */
#define SMP_RING_TABLES  128

#define CB_BLOCK_BYTES   ((512 + 3) * 16)
#define CB_BLOCK_ALIGNED ((CB_BLOCK_BYTES + 255) & ~255u)
#define CB_RING_BYTES    (CB_BLOCK_ALIGNED * SRV_RING_TABLES)
#define PS_CB_RING_BYTES CB_RING_BYTES

#define VERT_STRIDE      (16 * 4 * 4)   /* 16 attrs * float4                  */
#define VERT_BUFFER_SIZE (256u * 1024u * 1024u)
#define INDEX_BUFFER_SIZE (64u * 1024u * 1024u)
#define LD_INVALID_SURFACE 0xFFFFFFFFu

/* gcm texture format bytes (mirror rsx_dispatch.h) */
#define TEX_FMT_B8         0x81
#define TEX_FMT_A1R5G5B5   0x82
#define TEX_FMT_A4R4G4B4   0x83
#define TEX_FMT_R5G6B5     0x84
#define TEX_FMT_A8R8G8B8   0x85
#define TEX_FMT_DXT1       0x86
#define TEX_FMT_DXT23      0x87
#define TEX_FMT_DXT45      0x88
#define TEX_FMT_G8B8       0x8B
#define TEX_FMT_DEPTH24_D8 0x90
#define TEX_FMT_LINEAR     0x20
#define TEX_FMT_UNNORM     0x40
#define TEX_FMT_BASE_MASK  0x9F

typedef struct {
    u32 location, offset;
    ID3D12Resource* tex;
    u32 w, h;
} surface_t;
typedef struct {
    u32 location, offset, pitch, width, height;
    int valid;
} display_buffer_t;
typedef struct {
    u32 location, offset;
    ID3D12Resource* tex;
    ID3D12Resource* snapshot;
    u32 w, h;
    u32 snapshot_w, snapshot_h;
    D3D12_RESOURCE_STATES snapshot_state;
    int cleared;
    int had_write;
    int snapshot_valid;
    u64 draws;
    u64 depth_test_draws;
    u64 depth_write_draws;
    u64 depth_both_draws;
    u64 resource_generation;
    u64 last_depth_write_generation;
    u64 last_resolve_generation;
    u32 last_depth_write_owner;
} zdepth_t;
typedef struct {
    u32 location, offset, format, width, height, pitch, remap;
    u32 cubemap;
    ID3D12Resource* tex;
    u64 content_hash;
    u32 last_hash_frame;
    u64 last_use_serial;
} texcache_t;
typedef struct {
    u32 location, offset, format, width, height, pitch;
    ID3D12Resource* tex;
    u64 content_hash;
    u32 last_hash_frame;
} vtexcache_t;
typedef struct { u64 key; ID3D12PipelineState* pso; } psocache_t;
typedef struct {
    u64 hash;
    u32 source_length;
    char* source;
    ID3DBlob* blob;
} shader_blob_t;
typedef struct {
    shader_blob_t entries[MAX_SHADER_BLOBS];
    u32 count;
    u64 retained_source_bytes;
    u64 retained_blob_bytes;
} shader_blob_cache_t;

#include "rsx_legacy_frame_types.h"

typedef struct {
    int              ready;      /* device + resources up                    */

    ID3D12Device*              dev;
    ID3D12CommandQueue*        queue;
    ID3D12CommandAllocator*    alloc;
    ID3D12CommandAllocator*    alloc_ring[2];
    u32                        legacy_frame_slot;
    ld_legacy_frame            legacy_frames[2];
    u64                        legacy_async_submissions, legacy_reuse_waits;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence*               fence;
    HANDLE                     fence_event;
    u64                        fence_value;
    u64                        list_generation;

    IDXGISwapChain3*           swap;
    ID3D12Resource*            backbuf[LD_SWAP_BUFFERS];
    ID3D12DescriptorHeap*      rtv_heap;
    u32                        rtv_step;

    surface_t                  surfaces[MAX_SURFACES];
    u32                        n_surfaces;
    display_buffer_t           display_buffers[8];

    ID3D12DescriptorHeap*      dsv_heap;
    u32                        dsv_step;
    ID3D12Resource*            depth;
    int                        depth_cleared;
    zdepth_t                   zdepths[MAX_SURFACES];
    u32                        n_zdepths;

    ID3D12DescriptorHeap*      srv_cpu_heap;
    ID3D12DescriptorHeap*      srv_heap;
    u32                        srv_step, srv_ring_used;
    ID3D12Resource*            white_tex;
    texcache_t                 textures[MAX_TEXTURES];
    rsx_texture_lookup         texture_lookup;
    u32                        n_textures;
    vtexcache_t                vtex[MAX_VTEX];
    u32                        n_vtex;
    ID3D12Resource*            retired_textures[MAX_RETIRED_TEXTURES];
    u32                        n_retired_textures;
    ID3D12Resource*            upload;
    u8*                        upload_mapped;
    u32                        upload_used;

    ID3D12DescriptorHeap*      smp_cpu_heap;
    ID3D12DescriptorHeap*      smp_heap;
    u32                        smp_step, smp_ring_used;
    u32                        smp_ring_slots[SMP_RING_TABLES][SMP_TABLE_SIZE];
    u32                        smp_keys[SMP_CACHE_SLOTS];
    u32                        n_samplers;

    ID3D12RootSignature*       rootsig_x;
    ID3D12RootSignature*       depth_snapshot_rootsig;
    ID3D12PipelineState*       depth_snapshot_pso;
    psocache_t                 psos[MAX_PSOS];
    u32                        n_psos;
    shader_blob_cache_t        vs_blobs;
    shader_blob_cache_t        ps_blobs;

    ID3D12Resource*            cb;
    u8*                        cb_mapped;
    u32                        cb_used;

    /* Separate transient b1 arena for buffered fragment constants. It uses
     * the existing constant-buffer ring budget rather than changing any
     * texture or vertex upload-ring capacity. */
    ID3D12Resource*            ps_cb;
    u8*                        ps_cb_mapped;
    u32                        ps_cb_used;
    rsx_fp_constant_block      fp_constants;
    float                      fp_alpha_ref;

    ID3D12Resource*            vb;
    u8*                        vb_mapped;
    u32                        vb_used;
    ID3D12Resource*            ib;
    u32*                       ib_mapped;
    u32                        ib_used;

    u32                        width, height;
    rsx_live_guest_ptr_fn      guest_ptr;
    void*                      guest_user;

    rsx_dispatch               rsx;
} ld_state;

static ld_state g;
static volatile int g_ld_movie_mode = 0;

static HRESULT ld_profiled_swap_present(void)
{
    return g.swap->lpVtbl->Present(g.swap, LD_PRESENT_INTERVAL, 0);
}

static int g_ld_dred_dumped = 0;
static u32 g_ld_frames = 0;
static u32 g_ld_last_frame_draws = 0;
static ID3D12InfoQueue* g_ld_info_queue = NULL;
static int g_ld_debug_layer_enabled = 0;

typedef struct {
    int valid;
    u64 key;
    u64 vp_hash;
    u32 vp_start, vp_instrs;
    u32 fp_location, fp_offset, fp_size, fp_control;
    u32 cube_mask, vtex_mask, txl_mask;
    u32 input_mask, input_stride;
    u32 packed_offsets;
} ld_pso_metadata;

static ld_pso_metadata g_ld_current_pso;
static u32 g_ld_last_present_target = UINT32_MAX;

#define LD_LAYOUT_CACHE_CAP 256u
typedef struct {
    int valid;
    rsx_vertex_layout_plan plan;
} ld_layout_cache_entry;

static ld_layout_cache_entry g_ld_layout_cache[LD_LAYOUT_CACHE_CAP];
static u32 g_ld_layout_cache_count = 0;

static void ld_layout_plan_get(u32 mask, rsx_vertex_layout_plan* out)
{
    mask &= 0xFFFFu;
    for (u32 i = 0; i < g_ld_layout_cache_count; i++) {
        if (g_ld_layout_cache[i].valid &&
            g_ld_layout_cache[i].plan.mask == mask) {
            *out = g_ld_layout_cache[i].plan;
            return;
        }
    }
    rsx_vertex_layout_plan_init(out, mask);
    if (g_ld_layout_cache_count < LD_LAYOUT_CACHE_CAP) {
        ld_layout_cache_entry* entry =
            &g_ld_layout_cache[g_ld_layout_cache_count++];
        entry->valid = 1;
        entry->plan = *out;
    }
}

#define LD_RECENT_DRAW_CAP 128u
typedef struct {
    u64 serial;
    u64 pso_key;
    u64 descriptor_signature;
    u32 frame;
    u32 vp_start, vp_instrs;
    u32 fp_location, fp_offset, fp_size, fp_control;
    u32 cube_mask, vtex_mask, txl_mask, texture_mask;
    u32 primitive, vertices, target, zslot;
    u32 clip_x, clip_y, clip_w, clip_h;
    u32 srv_ring_used, sampler_ring_used, cb_used, vb_used;
} ld_recent_draw;

static ld_recent_draw g_ld_recent_draws[LD_RECENT_DRAW_CAP];
static u64 g_ld_recent_draw_total = 0;

typedef enum {
    LD_FLUSH_PRESENT = 0,
    LD_FLUSH_GUEST_REFERENCE,
    LD_FLUSH_VERTEX_RING,
    LD_FLUSH_VERTEX_CONSTANT_RING,
    LD_FLUSH_RETIRE_QUEUE,
    LD_FLUSH_MOVIE,
    LD_FLUSH_MOVIE_PRESENT,
    LD_FLUSH_READBACK,
    LD_FLUSH_PIXEL_CONSTANT_RING,
    LD_FLUSH_DESCRIPTOR_RING,
    LD_FLUSH_SHUTDOWN,
    LD_FLUSH_REASON_COUNT
} ld_flush_reason;

/* Recent successful presents, for the presented-FPS figure in the window
 * title. */
#define LD_PRESENT_RING_CAP 1024u
typedef struct {
    u64 present_id;
    LONGLONG qpc;
} ld_present_sample;
static ld_present_sample g_ld_present_ring[LD_PRESENT_RING_CAP];
static u64 g_ld_present_total = 0;
static LONGLONG g_ld_qpc_frequency = 0;

static int g_ld_reuse_vp_hash;
static int g_ld_use_vp_metadata;
static int g_ld_fast_indices;
static rsx_material_cache g_ld_materials;
static rsx_prepared_packet_cache* g_ld_prepared_packets;
static void ld_pipeline_records_reset(void);
static int ld_set_prepared_meshes(int enabled);
static u64 g_ld_srv_versions[SRV_CPU_HEAP_SLOTS];
static u64 g_ld_material_surface_epoch = 1, g_ld_material_table_epoch = 1;
static void ld_material_reset_tables(void)
{
    rsx_material_advance_epoch(&g_ld_materials, &g_ld_material_table_epoch);
}
static void ld_material_descriptor_written(u32 slot)
{
    if (slot >= SRV_CPU_HEAP_SLOTS) return;
    rsx_material_advance_epoch(&g_ld_materials, &g_ld_srv_versions[slot]);
    if (slot >= SRV_SURFACE_BASE && slot < SRV_ZDEPTH_BASE)
        rsx_material_advance_epoch(&g_ld_materials, &g_ld_material_surface_epoch);
}
static int g_ld_batch_fetch;
static vp_metadata_cache g_ld_vp_metadata;

static void ld_present_measure_init(void)
{
    g_ld_use_vp_metadata = 1;
    memset(&g_ld_vp_metadata, 0, sizeof(g_ld_vp_metadata));
    g_ld_fast_indices = 1;
    ld_pipeline_records_reset();
    rsx_prepared_packet_destroy(g_ld_prepared_packets);
    g_ld_prepared_packets = rsx_prepared_packet_create();
    memset(&g_ld_materials, 0, sizeof(g_ld_materials));
    memset(g_ld_srv_versions, 0, sizeof(g_ld_srv_versions));
    g_ld_material_surface_epoch = g_ld_material_table_epoch = 1;
    rsx_vertex_fetch_set_reuse_ranges(1);
    {
        LARGE_INTEGER frequency;
        memset(g_ld_present_ring, 0, sizeof(g_ld_present_ring));
        g_ld_present_total = 0;
        g_ld_qpc_frequency =
            QueryPerformanceFrequency(&frequency) ? frequency.QuadPart : 0;
    }
}

static void ld_present_measure_record(u32 guest_frame)
{
    LARGE_INTEGER now;
    (void)guest_frame;
    if (!QueryPerformanceCounter(&now))
        return;
    const u64 present_id = g_ld_present_total + 1u;
    ld_present_sample* sample =
        &g_ld_present_ring[(present_id - 1u) & (LD_PRESENT_RING_CAP - 1u)];
    sample->present_id = present_id;
    sample->qpc = now.QuadPart;
    g_ld_present_total = present_id;
}

static u64 g_ld_texture_cache_full = 0;
static u64 g_ld_texture_use_serial = 0;

static const char* ld_d3d_severity_name(D3D12_MESSAGE_SEVERITY severity)
{
    switch (severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: return "corruption";
        case D3D12_MESSAGE_SEVERITY_ERROR: return "error";
        case D3D12_MESSAGE_SEVERITY_WARNING: return "warning";
        case D3D12_MESSAGE_SEVERITY_INFO: return "info";
        default: return "message";
    }
}

static void ld_drain_info_queue(const char* where)
{
    if (!g_ld_info_queue) return;
    const u64 count =
        g_ld_info_queue->lpVtbl->GetNumStoredMessages(g_ld_info_queue);
    for (u64 i = 0; i < count; i++) {
        SIZE_T bytes = 0;
        if (FAILED(g_ld_info_queue->lpVtbl->GetMessage(
                g_ld_info_queue, i, NULL, &bytes)) ||
            !bytes || bytes > 1024u * 1024u)
            continue;
        D3D12_MESSAGE* message = (D3D12_MESSAGE*)malloc(bytes);
        if (!message) break;
        if (SUCCEEDED(g_ld_info_queue->lpVtbl->GetMessage(
                g_ld_info_queue, i, message, &bytes))) {
            fprintf(stderr,
                    "[d3d-debug] where=%s frame=%u severity=%s id=%u %s\n",
                    where, g_ld_frames,
                    ld_d3d_severity_name(message->Severity),
                    (unsigned)message->ID,
                    message->pDescription
                        ? message->pDescription : "<no description>");
        }
        free(message);
    }
    if (count)
        g_ld_info_queue->lpVtbl->ClearStoredMessages(g_ld_info_queue);
}

static void ld_dump_recent_draws(void)
{
    const u64 first =
        g_ld_recent_draw_total > LD_RECENT_DRAW_CAP
            ? g_ld_recent_draw_total - LD_RECENT_DRAW_CAP + 1u : 1u;
    fprintf(stderr,
            "[draw-history] total=%llu retained=%llu..%llu\n",
            (unsigned long long)g_ld_recent_draw_total,
            (unsigned long long)first,
            (unsigned long long)g_ld_recent_draw_total);
    for (u64 serial = first; serial <= g_ld_recent_draw_total; serial++) {
        const ld_recent_draw* draw =
            &g_ld_recent_draws[(serial - 1u) & (LD_RECENT_DRAW_CAP - 1u)];
        if (draw->serial != serial) continue;
        fprintf(stderr,
                "[draw-history] serial=%llu frame=%u key=%016llX "
                "vp=%u+%u fp=%u:0x%X+%u ctrl=0x%X "
                "mask{tex=%04X cube=%04X vtex=%04X txl=%04X} "
                "prim=%u verts=%u target=%u z=%u "
                "clip=%u,%u %ux%u desc=%016llX "
                "ring{srv=%u smp=%u cb=%u vb=%u}\n",
                (unsigned long long)draw->serial, draw->frame,
                (unsigned long long)draw->pso_key,
                draw->vp_start, draw->vp_instrs,
                draw->fp_location, draw->fp_offset, draw->fp_size,
                draw->fp_control, draw->texture_mask, draw->cube_mask,
                draw->vtex_mask, draw->txl_mask, draw->primitive,
                draw->vertices, draw->target, draw->zslot,
                draw->clip_x, draw->clip_y, draw->clip_w, draw->clip_h,
                (unsigned long long)draw->descriptor_signature,
                draw->srv_ring_used, draw->sampler_ring_used,
                draw->cb_used, draw->vb_used);
    }
}

static const char* ld_dred_op_name(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op) {
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRTV";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDSV";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
        case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
        case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
        default: return "other";
    }
}

static void ld_enable_dred(void)
{
    ID3D12DeviceRemovedExtendedDataSettings* settings = NULL;
    const HRESULT hr = D3D12GetDebugInterface(
        &IID_ID3D12DeviceRemovedExtendedDataSettings, (void**)&settings);
    if (FAILED(hr) || !settings) {
        fprintf(stderr, "[dred] settings unavailable hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }
    settings->lpVtbl->SetAutoBreadcrumbsEnablement(
        settings, D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->lpVtbl->SetPageFaultEnablement(
        settings, D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->lpVtbl->Release(settings);
    fprintf(stderr, "[dred] auto-breadcrumbs and page-fault tracking enabled\n");
}

static void ld_enable_debug_layer(void)
{
    if (!getenv("RSX_D3D_DEBUG")) return;
    ID3D12Debug* debug = NULL;
    const HRESULT hr = D3D12GetDebugInterface(
        &IID_ID3D12Debug, (void**)&debug);
    if (FAILED(hr) || !debug) {
        fprintf(stderr,
                "[d3d-debug] validation layer unavailable hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }
    debug->lpVtbl->EnableDebugLayer(debug);
    debug->lpVtbl->Release(debug);
    g_ld_debug_layer_enabled = 1;
    fprintf(stderr, "[d3d-debug] validation layer enabled\n");
}

static void ld_open_info_queue(void)
{
    if (!g_ld_debug_layer_enabled || !g.dev) return;
    const HRESULT hr = g.dev->lpVtbl->QueryInterface(
        g.dev, &IID_ID3D12InfoQueue, (void**)&g_ld_info_queue);
    if (FAILED(hr) || !g_ld_info_queue) {
        fprintf(stderr,
                "[d3d-debug] info queue unavailable hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }
    D3D12_MESSAGE_SEVERITY severities[] = {
        D3D12_MESSAGE_SEVERITY_CORRUPTION,
        D3D12_MESSAGE_SEVERITY_ERROR,
        D3D12_MESSAGE_SEVERITY_WARNING
    };
    D3D12_INFO_QUEUE_FILTER filter = {0};
    filter.AllowList.NumSeverities =
        (UINT)(sizeof(severities) / sizeof(severities[0]));
    filter.AllowList.pSeverityList = severities;
    g_ld_info_queue->lpVtbl->AddStorageFilterEntries(
        g_ld_info_queue, &filter);
    g_ld_info_queue->lpVtbl->SetMessageCountLimit(
        g_ld_info_queue, 65536u);
    fprintf(stderr,
            "[d3d-debug] info queue attached (warning/error/corruption)\n");
}

static void ld_dump_dred(const char* where, HRESULT trigger_hr)
{
    if (g_ld_dred_dumped++ || !g.dev) return;
    ld_drain_info_queue(where);
    ld_dump_recent_draws();
    const HRESULT removed = g.dev->lpVtbl->GetDeviceRemovedReason(g.dev);
    fprintf(stderr,
            "[dred] trigger=%s hr=0x%08lX removed=0x%08lX frame=%u "
            "surfaces=%u zetas=%u textures=%u psos=%u\n",
            where, (unsigned long)trigger_hr, (unsigned long)removed,
            g_ld_frames, g.n_surfaces, g.n_zdepths, g.n_textures, g.n_psos);

    ID3D12DeviceRemovedExtendedData1* dred = NULL;
    HRESULT hr = g.dev->lpVtbl->QueryInterface(
        g.dev, &IID_ID3D12DeviceRemovedExtendedData1, (void**)&dred);
    if (FAILED(hr) || !dred) {
        fprintf(stderr, "[dred] query failed hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {0};
    hr = dred->lpVtbl->GetAutoBreadcrumbsOutput1(dred, &breadcrumbs);
    fprintf(stderr, "[dred] breadcrumbs hr=0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        const D3D12_AUTO_BREADCRUMB_NODE1* node =
            breadcrumbs.pHeadAutoBreadcrumbNode;
        for (u32 node_index = 0; node && node_index < 16;
             node = node->pNext, node_index++) {
            const u32 last = node->pLastBreadcrumbValue
                ? *node->pLastBreadcrumbValue : 0;
            fprintf(stderr,
                    "[dred] node=%u list=%s queue=%s count=%u last=%u\n",
                    node_index,
                    node->pCommandListDebugNameA
                        ? node->pCommandListDebugNameA : "<unnamed>",
                    node->pCommandQueueDebugNameA
                        ? node->pCommandQueueDebugNameA : "<unnamed>",
                    node->BreadcrumbCount, last);
            if (!node->pCommandHistory || !node->BreadcrumbCount) continue;
            const u32 first = last > 8 ? last - 8 : 0;
            u32 end = last + 8;
            if (end >= node->BreadcrumbCount)
                end = node->BreadcrumbCount - 1;
            for (u32 i = first; i <= end; i++) {
                const D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[i];
                fprintf(stderr,
                        "[dred]   op[%u]%s=%u(%s)\n",
                        i, i == last ? "*" : "",
                        (unsigned)op, ld_dred_op_name(op));
            }
        }
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {0};
    hr = dred->lpVtbl->GetPageFaultAllocationOutput1(dred, &fault);
    fprintf(stderr,
            "[dred] page-fault hr=0x%08lX va=0x%016llX\n",
            (unsigned long)hr,
            (unsigned long long)fault.PageFaultVA);
    if (SUCCEEDED(hr)) {
        const D3D12_DRED_ALLOCATION_NODE1* lists[2] = {
            fault.pHeadExistingAllocationNode,
            fault.pHeadRecentFreedAllocationNode
        };
        const char* labels[2] = {"existing", "recent-freed"};
        for (u32 list = 0; list < 2; list++) {
            const D3D12_DRED_ALLOCATION_NODE1* allocation = lists[list];
            for (u32 i = 0; allocation && i < 16;
                 allocation = allocation->pNext, i++) {
                fprintf(stderr,
                        "[dred] %s[%u] type=%u name=%s object=%p\n",
                        labels[list], i, (unsigned)allocation->AllocationType,
                        allocation->ObjectNameA
                            ? allocation->ObjectNameA : "<unnamed>",
                        (const void*)allocation->pObject);
            }
        }
    }
    dred->lpVtbl->Release(dred);
    fflush(stderr);
}
static u64 g_ld_texture_cache_evictions = 0;
static u64 g_ld_texture_decode_fail = 0;
static char g_ld_shader_disk_dir[MAX_PATH];
static int g_ld_shader_disk_ready = -1;
static u64 g_ld_shader_disk_hits[2] = {0, 0};
static u64 g_ld_shader_disk_misses[2] = {0, 0};
static u64 g_ld_shader_disk_writes[2] = {0, 0};
static u64 g_ld_shader_disk_rejects = 0;

/* Host movie presentation and the FIFO consumer live on different threads but
 * share one D3D12 command list. Normal gameplay remains single-producer and
 * bypasses this lock; the active-reader handshake closes the movie-mode
 * transition race without putting every ordinary RSX method behind an SRW
 * lock. Host-frame priority prevents a flood of guest caption commands from
 * starving Present when the window is backgrounded. */
static SRWLOCK g_ld_access_lock = SRWLOCK_INIT;
static volatile LONG g_ld_guest_active = 0;
static volatile LONG g_ld_host_waiting = 0;

static const u8* guest_ptr(u32 location, u32 offset, u32 min_bytes)
{
    if (!g.guest_ptr) return NULL;
    return g.guest_ptr(g.guest_user, location, offset, min_bytes);
}
static const u8* ld_fragment_bytes(u32 location,u32 offset,u32 bytes)
{
    return guest_ptr(location,offset,bytes);
}

/* ---------------------------------------------------------------------------
 * enable gate
 * -----------------------------------------------------------------------*/
int rsx_live_draw_enabled(void)
{
    return 1;
}

/* The renderer is one translation unit: the parts below share the module
 * state above and are included once, in dependency order. */
#include "rsx_ld_render_state.inc"
#include "rsx_ld_descriptors.inc"
#include "rsx_ld_submit.inc"
#include "rsx_ld_textures.inc"
#include "rsx_ld_surfaces.inc"
#include "rsx_ld_pipelines.inc"
#include "rsx_ld_draw.inc"
#include "rsx_ld_device.inc"
#include "rsx_ld_entry.inc"
#include "rsx_ld_present.inc"
#include "rsx_ld_shutdown.inc"

#endif /* _WIN32 */
