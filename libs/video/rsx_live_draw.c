/*
 * ps3recomp - Track B live NV4097 draw path (implementation)
 *
 * See rsx_live_draw.h. This is the validated capture-replay D3D12 engine
 * (libs/video/tests/replay_main.c) lifted into a runtime module and driven by
 * the live FIFO consumer instead of an .rxs file:
 *   - rsx_dispatch register-file model (shared, unchanged)
 *   - NV40 VP/FP -> HLSL decompilers (shared, unchanged)
 *   - B1 render/sampler state -> D3D12 PSO + dynamic samplers + mip chains
 *
 * Differences from the harness:
 *   - guest memory comes from an injected resolver (the runtime's vm_base map),
 *     not a private arena;
 *   - present goes to a swap chain bound to the runtime's window, not a PPM
 *     readback;
 *   - the whole engine is gated behind YZ_RSX_DRAW (default ON; "0" = off).
 *
 * Clean-room: NV40 ISA/register facts from envytools rnndb + Mesa nv30 +
 * psdevwiki; RPCS3 as a read-only fact oracle only.
 */

#include "rsx_live_draw.h"

#if !defined(_WIN32)

/* Non-Windows: the whole path is a no-op (D3D12 is Windows-only). */
int  rsx_live_draw_enabled(void) { return 0; }
int  rsx_live_draw_init(void* hwnd, u32 w, u32 h, rsx_live_guest_ptr_fn f, void* u)
{ (void)hwnd; (void)w; (void)h; (void)f; (void)u; return 0; }
void rsx_live_draw_seed_registers(const u32* r, u32 n) { (void)r; (void)n; }
void rsx_live_draw_seed_transform_program(const u32* w, u32 n) { (void)w; (void)n; }
void rsx_live_draw_method(u32 m, u32 a) { (void)m; (void)a; }
void rsx_live_draw_flush(void) {}
void rsx_live_draw_present(u32 b) { (void)b; }
void rsx_live_draw_set_movie_mode(int on) { (void)on; }
void rsx_live_draw_present_rgba(const uint8_t* r, u32 w, u32 h) { (void)r; (void)w; (void)h; }
void rsx_live_draw_a010_probe_begin(void) {}
int  rsx_live_draw_a010_probe_active(void) { return 0; }
void rsx_live_draw_shutdown(void) {}

#else /* _WIN32 */

#define _CRT_SECURE_NO_WARNINGS

#include "rsx_dispatch.h"
#include "rsx_fp_decompiler.h"
#include "rsx_restart_cuts.h"
#include "rsx_vp_decompiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include "rsx_vertex_formats.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* ---------------------------------------------------------------------------
 * Engine state (module-static; single live RSX)
 * -----------------------------------------------------------------------*/

#define LD_SWAP_BUFFERS  2
#define MAX_SURFACES     64
#define MAX_TEXTURES     128
#define MAX_VTEX         64
#define MAX_SAMPLERS     128
#define MAX_PSOS         256
#define UPLOAD_SIZE      (64u * 1024 * 1024)

#define SRV_WHITE        0
#define SRV_SURFACE_BASE 1
#define SRV_ZDEPTH_BASE  (SRV_SURFACE_BASE + MAX_SURFACES)
#define SRV_TEXTURE_BASE (SRV_ZDEPTH_BASE + MAX_SURFACES)
#define SRV_VTEX_BASE    (SRV_TEXTURE_BASE + MAX_TEXTURES)
#define SRV_HEAP_SLOTS   (SRV_VTEX_BASE + MAX_VTEX)
#define SRV_TABLE_SIZE   16
#define SRV_RING_TABLES  4096

#define SMP_DEFAULT      0
#define SMP_CACHE_SLOTS  MAX_SAMPLERS
#define SMP_TABLE_SIZE   16
/* A shader-visible SAMPLER heap is hard-capped at 2048 descriptors by D3D12
 * (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE). SMP_RING_TABLES*SMP_TABLE_SIZE
 * must stay <= 2048, else CreateDescriptorHeap fails -> NULL heap -> crash in
 * sampler_table. 128*16 = 2048 = the max (= up to 128 sampler tables/frame). */
#define SMP_RING_TABLES  128

#define CB_BLOCK_BYTES   ((512 + 2) * 16)
#define CB_BLOCK_ALIGNED ((CB_BLOCK_BYTES + 255) & ~255u)
#define CB_RING_BYTES    (CB_BLOCK_ALIGNED * SRV_RING_TABLES)

#define VERT_STRIDE      (16 * 4 * 4)   /* 16 attrs * float4                  */
#define MAX_VERTS        (256 * 1024)
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
    u32 location, offset;
    ID3D12Resource* tex;
    u32 w, h;
    int cleared;
    int had_write;
} zdepth_t;
typedef struct {
    u32 location, offset, format, width, height, pitch, remap;
    u32 cubemap;
    ID3D12Resource* tex;
    u64 content_hash;
    u32 last_hash_frame;
} texcache_t;
typedef struct {
    u32 location, offset, format, width, height, pitch;
    ID3D12Resource* tex;
    u64 content_hash;
    u32 last_hash_frame;
} vtexcache_t;
typedef struct { u64 key; ID3D12PipelineState* pso; } psocache_t;

typedef struct {
    int              enabled;    /* YZ_RSX_DRAW resolved                     */
    int              ready;      /* device + resources up                    */

    ID3D12Device*              dev;
    ID3D12CommandQueue*        queue;
    ID3D12CommandAllocator*    alloc;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence*               fence;
    HANDLE                     fence_event;
    u64                        fence_value;

    IDXGISwapChain3*           swap;
    ID3D12Resource*            backbuf[LD_SWAP_BUFFERS];
    ID3D12DescriptorHeap*      rtv_heap;
    u32                        rtv_step;

    surface_t                  surfaces[MAX_SURFACES];
    u32                        n_surfaces;

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
    u32                        n_textures;
    vtexcache_t                vtex[MAX_VTEX];
    u32                        n_vtex;
    ID3D12Resource*            retired_textures[MAX_TEXTURES];
    u32                        n_retired_textures;
    ID3D12Resource*            upload;
    u8*                        upload_mapped;
    u32                        upload_used;

    /* Optional host-movie UI compositor. The guest keeps rendering captions
     * into its ordinary offscreen surface. Each guest flip reads back the
     * latest sparse overlay for blending over each 30 Hz host movie frame. */
    ID3D12Resource*            movie_upload;
    u8*                        movie_upload_mapped;
    ID3D12Resource*            movie_overlay_readback;
    u8*                        movie_overlay_rgba;
    u8*                        movie_overlay_mask;
    u32                        movie_overlay_pitch;
    int                        movie_overlay_valid;
    u64                        movie_overlay_frames;

    ID3D12DescriptorHeap*      smp_cpu_heap;
    ID3D12DescriptorHeap*      smp_heap;
    u32                        smp_step, smp_ring_used;
    u32                        smp_keys[SMP_CACHE_SLOTS];
    u32                        n_samplers;

    ID3D12RootSignature*       rootsig_x;
    psocache_t                 psos[MAX_PSOS];
    u32                        n_psos;

    ID3D12Resource*            cb;
    u8*                        cb_mapped;
    u32                        cb_used;

    ID3D12Resource*            vb;
    u8*                        vb_mapped;
    u32                        vb_used;

    u32                        width, height;
    rsx_live_guest_ptr_fn      guest_ptr;
    void*                      guest_user;

    rsx_dispatch               rsx;
} ld_state;

static ld_state g;
static u32 g_ld_frames = 0;
static u64 g_ld_texture_cache_full = 0;
static u64 g_ld_texture_decode_fail = 0;
static u64 g_ld_zdepth_srv_binds = 0;
static u64 g_ld_zdepth_srv_reject_no_write = 0;
static u64 g_ld_vtex_binds = 0;
static u64 g_ld_vtex_uploads = 0;
static u64 g_ld_vtex_refreshes = 0;
static u64 g_ld_vtex_unsupported = 0;
static u64 g_ld_vtex_enabled = 0;
static u64 g_ld_vtex_missing_for_txl = 0;
static u64 g_ld_divider_fetches = 0;
static volatile LONG g_ld_a010_probe_active = 0;
static u32 g_ld_a010_probe_start_frame = 0;
static u32 g_ld_a010_probe_sample = 0;
static u64 g_ld_a010_probe_touched = 0;
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
static u64 fnv1a(const void* data, u32 n, u64 h);

/* ---------------------------------------------------------------------------
 * enable gate
 * -----------------------------------------------------------------------*/
int rsx_live_draw_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("YZ_RSX_DRAW");
        cached = (e && e[0] == '0') ? 0 : 1;   /* default ON, "0" disables   */
    }
    return cached;
}

/* ---------------------------------------------------------------------------
 * B1 render/sampler state decode (identical facts to replay_main.c)
 * -----------------------------------------------------------------------*/
#define M_BLEND_ENABLE       0x0310
#define M_ALPHA_TEST_ENABLE  0x0304
#define M_ALPHA_FUNC         0x0308
#define M_ALPHA_REF          0x030C
#define M_BLEND_SFACTOR      0x0314
#define M_BLEND_DFACTOR      0x0318
#define M_BLEND_EQUATION     0x0320
#define M_DEPTH_FUNC         0x0A6C
#define M_DEPTH_WRITE        0x0A70
#define M_DEPTH_TEST_ENABLE  0x0A74
#define M_CULL_FACE          0x1830
#define M_FRONT_FACE         0x1834
#define M_CULL_FACE_ENABLE   0x183C
#define M_COLOR_MASK         0x0324

static D3D12_COMPARISON_FUNC gcm_cmp(u32 f)
{
    switch (f) {
    case 0x0200: return D3D12_COMPARISON_FUNC_NEVER;
    case 0x0201: return D3D12_COMPARISON_FUNC_LESS;
    case 0x0202: return D3D12_COMPARISON_FUNC_EQUAL;
    case 0x0203: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case 0x0204: return D3D12_COMPARISON_FUNC_GREATER;
    case 0x0205: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case 0x0206: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    default:     return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}
static D3D12_BLEND gcm_blend_factor(u32 f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D12_BLEND_ZERO;
    case 0x0001: return D3D12_BLEND_ONE;
    case 0x0300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D12_BLEND_SRC_ALPHA;
    case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D12_BLEND_DEST_ALPHA;
    case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D12_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D12_BLEND_BLEND_FACTOR;
    case 0x8002: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default:     return D3D12_BLEND_ONE;
    }
}
static D3D12_BLEND_OP gcm_blend_op(u32 e)
{
    switch (e) {
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
    default:     return D3D12_BLEND_OP_ADD;
    }
}

typedef struct {
    u32 alpha_test_enable, alpha_func, alpha_ref_raw, alpha_ref_format;
    u32 blend_enable, sf_rgb, df_rgb, sf_a, df_a, eq_rgb, eq_a;
    u32 depth_test, depth_write, depth_func;
    u32 cull_enable, cull_face, front_face;
    u32 color_mask;
} render_state_t;

static void decode_render_state(render_state_t* rs)
{
    memset(rs, 0, sizeof(*rs));
    rs->alpha_test_enable = rsx_dsp_reg(&g.rsx, M_ALPHA_TEST_ENABLE) & 1;
    rs->alpha_func = rsx_dsp_reg(&g.rsx, M_ALPHA_FUNC);
    rs->alpha_ref_raw = rsx_dsp_reg(&g.rsx, M_ALPHA_REF);
    rsx_dsp_surface alpha_surface;
    rsx_dsp_get_surface(&g.rsx, &alpha_surface);
    rs->alpha_ref_format = alpha_surface.color_format;
    rs->blend_enable = rsx_dsp_reg(&g.rsx, M_BLEND_ENABLE) & 1;
    const u32 sf = rsx_dsp_reg(&g.rsx, M_BLEND_SFACTOR);
    const u32 df = rsx_dsp_reg(&g.rsx, M_BLEND_DFACTOR);
    const u32 eq = rsx_dsp_reg(&g.rsx, M_BLEND_EQUATION);
    rs->sf_rgb = sf & 0xFFFF; rs->sf_a = sf >> 16;
    rs->df_rgb = df & 0xFFFF; rs->df_a = df >> 16;
    rs->eq_rgb = eq & 0xFFFF; rs->eq_a = eq >> 16;
    rs->depth_test  = rsx_dsp_reg(&g.rsx, M_DEPTH_TEST_ENABLE) & 1;
    rs->depth_write = rsx_dsp_reg(&g.rsx, M_DEPTH_WRITE) & 1;
    rs->depth_func  = rsx_dsp_reg(&g.rsx, M_DEPTH_FUNC);
    rs->cull_enable = rsx_dsp_reg(&g.rsx, M_CULL_FACE_ENABLE) & 1;
    rs->cull_face   = rsx_dsp_reg(&g.rsx, M_CULL_FACE);
    rs->front_face  = rsx_dsp_reg(&g.rsx, M_FRONT_FACE);
    /* s31 (scratch/s31_blue_emitter.md): honor the RAW register — 0 is a
     * legitimate game-written "write no color channels" (the character
     * shadow-mask depth-prime pass). rsx_dispatch_init seeds the nv40
     * reset default (0x01010101), so never-written reads as all-on. */
    rs->color_mask  = rsx_dsp_reg(&g.rsx, M_COLOR_MASK);
}

static void apply_render_state(D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd,
                               const render_state_t* rs)
{
    D3D12_RENDER_TARGET_BLEND_DESC* b = &pd->BlendState.RenderTarget[0];
    /* nv40_3d COLOR_MASK byte layout (Mesa/nouveau nv40_3d.xml.h, MIT/X11):
     * B=[0:7] G=[8:15] R=[16:23] A=[24:31]; any nonzero byte = channel on.
     * (s31: was hardcoded ENABLE_ALL — the blue-character class' live twin.) */
    b->RenderTargetWriteMask =
        (((rs->color_mask       ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_BLUE  : 0) |
        (((rs->color_mask >>  8 ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_GREEN : 0) |
        (((rs->color_mask >> 16 ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_RED   : 0) |
        (((rs->color_mask >> 24 ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_ALPHA : 0);
    if (rs->blend_enable) {
        b->BlendEnable   = TRUE;
        b->SrcBlend      = gcm_blend_factor(rs->sf_rgb, 0);
        b->DestBlend     = gcm_blend_factor(rs->df_rgb, 0);
        b->BlendOp       = gcm_blend_op(rs->eq_rgb);
        b->SrcBlendAlpha = gcm_blend_factor(rs->sf_a, 1);
        b->DestBlendAlpha= gcm_blend_factor(rs->df_a, 1);
        b->BlendOpAlpha  = gcm_blend_op(rs->eq_a);
    }
    pd->RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    if (rs->cull_enable && rs->cull_face) {
        const u32 f = rs->cull_face;
        pd->RasterizerState.CullMode = (f == 0x0404) ? D3D12_CULL_MODE_FRONT
                                     : (f == 0x0405) ? D3D12_CULL_MODE_BACK
                                                     : D3D12_CULL_MODE_NONE;
    } else {
        pd->RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    }
    /* Y-negating viewport epilogue mirrors winding; take the front sense
     * straight from the guest value (validated in B1). */
    pd->RasterizerState.FrontCounterClockwise = (rs->front_face == 0x0901);
    if (g.depth) {
        pd->DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        pd->DepthStencilState.DepthEnable = rs->depth_test ? TRUE : FALSE;
        pd->DepthStencilState.DepthWriteMask =
            rs->depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pd->DepthStencilState.DepthFunc = gcm_cmp(rs->depth_func);
        pd->DepthStencilState.StencilEnable = FALSE;
    }
}

static D3D12_TEXTURE_ADDRESS_MODE gcm_wrap(u32 w)
{
    switch (w & 0xF) {
    case 1: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 6:
    case 7:
    case 8: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

static D3D12_SAMPLER_DESC decode_sampler(const rsx_dsp_texture* t)
{
    D3D12_SAMPLER_DESC sd = {0};
    const u32 minf = (t->filter >> 16) & 0x7;
    const u32 magf = (t->filter >> 24) & 0x7;
    const int min_linear = (minf == 2 || minf == 4 || minf == 6);
    const int mag_linear = (magf == 2);
    const int mip_linear = (minf == 5 || minf == 6);
    const int mip_present = (minf >= 3);
    D3D12_FILTER_TYPE mnf = min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mgf = mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mpf = mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_ENCODE_BASIC_FILTER(mnf, mgf, mpf, D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    sd.AddressU = gcm_wrap(t->wrap);
    sd.AddressV = gcm_wrap(t->wrap >> 8);
    sd.AddressW = gcm_wrap(t->wrap >> 16);
    const u32 max_lod_fx = (t->control0 >> 7)  & 0xFFF;
    const u32 min_lod_fx = (t->control0 >> 19) & 0xFFF;
    sd.MinLOD = (float)min_lod_fx / 256.0f;
    sd.MaxLOD = mip_present ? (float)max_lod_fx / 256.0f : 0.0f;
    if (sd.MaxLOD < sd.MinLOD) sd.MaxLOD = sd.MinLOD;
    sd.MaxAnisotropy = 1;
    return sd;
}
static u32 sampler_key(const rsx_dsp_texture* t)
{
    const u32 minf = (t->filter >> 16) & 0x7;
    const u32 magf = (t->filter >> 24) & 0x7;
    const u32 wrap = t->wrap & 0xFFF;
    const u32 lod  = (t->control0 >> 7) & 0x1FFFFF;
    return minf | (magf << 3) | (wrap << 6) | (lod << 18);
}

/* ---------------------------------------------------------------------------
 * descriptor heap helpers
 * -----------------------------------------------------------------------*/
static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(u32 idx)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.rtv_heap, &h);
    h.ptr += (size_t)idx * g.rtv_step;
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.srv_cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.srv_cpu_heap, &h);
    h.ptr += (size_t)slot * g.srv_step;
    return h;
}
static void srv_write(u32 slot, ID3D12Resource* tex)
{
    g.dev->lpVtbl->CreateShaderResourceView(g.dev, tex, NULL, srv_cpu(slot));
}
static void srv_write_zdepth(u32 slot, ID3D12Resource* tex)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    /* The replay-proven depth snapshot broadcasts depth to RGB. Mirror that
     * mapping directly in the native SRV and force alpha to one. */
    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    sd.Texture2D.MipLevels = 1;
    g.dev->lpVtbl->CreateShaderResourceView(
        g.dev, tex, &sd, srv_cpu(slot));
}
static D3D12_GPU_DESCRIPTOR_HANDLE srv_table(const u32 slots[SRV_TABLE_SIZE])
{
    if (g.srv_ring_used >= SRV_RING_TABLES) g.srv_ring_used = 0;
    const u32 base = g.srv_ring_used++ * SRV_TABLE_SIZE;
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    g.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.srv_heap, &dst);
    dst.ptr += (size_t)base * g.srv_step;
    for (u32 i = 0; i < SRV_TABLE_SIZE; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE d = dst;
        d.ptr += (size_t)i * g.srv_step;
        g.dev->lpVtbl->CopyDescriptorsSimple(g.dev, 1, d, srv_cpu(slots[i]),
                                             D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    g.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(g.srv_heap, &h);
    h.ptr += (u64)base * g.srv_step;
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE smp_cpu(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.smp_cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.smp_cpu_heap, &h);
    h.ptr += (size_t)slot * g.smp_step;
    return h;
}
static u32 sampler_slot(const rsx_dsp_texture* t, u32 key)
{
    for (u32 i = 0; i < g.n_samplers; i++)
        if (g.smp_keys[i] == key) return 1 + i;
    if (g.n_samplers >= SMP_CACHE_SLOTS) return SMP_DEFAULT;
    D3D12_SAMPLER_DESC sd = decode_sampler(t);
    const u32 slot = 1 + g.n_samplers;
    g.dev->lpVtbl->CreateSampler(g.dev, &sd, smp_cpu(slot));
    g.smp_keys[g.n_samplers++] = key;
    return slot;
}
static D3D12_GPU_DESCRIPTOR_HANDLE sampler_table(const u32 slots[SMP_TABLE_SIZE])
{
    if (g.smp_ring_used >= SMP_RING_TABLES) g.smp_ring_used = 0;
    const u32 base = g.smp_ring_used++ * SMP_TABLE_SIZE;
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    g.smp_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.smp_heap, &dst);
    dst.ptr += (size_t)base * g.smp_step;
    for (u32 i = 0; i < SMP_TABLE_SIZE; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE d = dst;
        d.ptr += (size_t)i * g.smp_step;
        g.dev->lpVtbl->CopyDescriptorsSimple(g.dev, 1, d, smp_cpu(slots[i]),
                                             D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    g.smp_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(g.smp_heap, &h);
    h.ptr += (u64)base * g.smp_step;
    return h;
}

/* ---------------------------------------------------------------------------
 * command list submit/wait (simple synchronous model, like the harness)
 * -----------------------------------------------------------------------*/
static void ld_flush(void)
{
    g.list->lpVtbl->Close(g.list);
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    const u64 v = ++g.fence_value;
    g.queue->lpVtbl->Signal(g.queue, g.fence, v);
    if (g.fence->lpVtbl->GetCompletedValue(g.fence) < v) {
        g.fence->lpVtbl->SetEventOnCompletion(g.fence, v, g.fence_event);
        WaitForSingleObject(g.fence_event, INFINITE);
    }
    /* Dynamic guest textures can replace a cached D3D resource while an
     * earlier draw in this command list still references the old one.  The
     * fence above is the first safe point at which those old resources may be
     * released. */
    for (u32 i = 0; i < g.n_retired_textures; i++)
        g.retired_textures[i]->lpVtbl->Release(g.retired_textures[i]);
    g.n_retired_textures = 0;
    g.alloc->lpVtbl->Reset(g.alloc);
    g.list->lpVtbl->Reset(g.list, g.alloc, NULL);
    g.upload_used = 0;
}

/* Public wrapper for the RSX SET_REFERENCE / sync fence: block until the GPU
 * has finished all queued draws (mirrors RPCS3 nv406e::set_reference's sync(),
 * RSXThread.cpp), so the game's REF poll advances only after the GPU has really
 * caught up. Without it our async consumer writes REF instantly and races ahead
 * of real GPU time (measured: ours skips the fence wait RPCS3 performs). Gated
 * at the call site by YZ_RSX_FENCE_SYNC. */
void rsx_live_draw_flush(void) { if (g.ready) ld_flush(); }

static void retire_texture(ID3D12Resource* tex)
{
    if (!tex) return;
    if (g.n_retired_textures >= MAX_TEXTURES)
        ld_flush();
    g.retired_textures[g.n_retired_textures++] = tex;
}

/* ---------------------------------------------------------------------------
 * texture upload (single-level + mip)
 * -----------------------------------------------------------------------*/
static u32 log2_u32(u32 v) { u32 n = 0; while (v > 1) { v >>= 1; n++; } return n; }
static u32 morton_index(u32 x, u32 y, u32 lw, u32 lh)
{
    u32 idx = 0, shift = 0;
    while (lw || lh) {
        if (lw) { idx |= (x & 1) << shift; x >>= 1; shift++; lw--; }
        if (lh) { idx |= (y & 1) << shift; y >>= 1; shift++; lh--; }
    }
    return idx;
}
static u8 remap_comp(const u8 s[4], u32 remap, u32 comp)
{
    const u32 op  = (remap >> (8 + comp * 2)) & 3;
    const u32 sel = (remap >> (comp * 2)) & 3;
    if (op == 0) return 0;
    if (op == 1) return 255;
    return s[sel];
}
static void decode_texel(u32 base_fmt, const u8* p, u32 remap, u8 d[4])
{
    u8 s[4];
    switch (base_fmt) {
    case TEX_FMT_B8: s[0] = 255; s[1] = s[2] = s[3] = p[0]; break;
    case TEX_FMT_A4R4G4B4: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        s[0] = (u8)(((v >> 12) & 0xF) * 17); s[1] = (u8)(((v >> 8) & 0xF) * 17);
        s[2] = (u8)(((v >> 4) & 0xF) * 17);  s[3] = (u8)((v & 0xF) * 17); break;
    }
    case TEX_FMT_A1R5G5B5: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        s[0] = (v & 0x8000) ? 255 : 0;
        s[1] = (u8)(((v >> 10) & 0x1F) * 255 / 31);
        s[2] = (u8)(((v >> 5) & 0x1F) * 255 / 31);
        s[3] = (u8)((v & 0x1F) * 255 / 31); break;
    }
    case TEX_FMT_R5G6B5: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        s[0] = 255;
        s[1] = (u8)(((v >> 11) & 0x1F) * 255 / 31);
        s[2] = (u8)(((v >> 5) & 0x3F) * 255 / 63);
        s[3] = (u8)((v & 0x1F) * 255 / 31); break;
    }
    case TEX_FMT_G8B8:
        s[0] = 255;
        s[1] = s[2] = p[0];
        s[3] = p[1];
        break;
    case TEX_FMT_DEPTH24_D8: s[0] = 255; s[1] = s[2] = s[3] = p[0]; break;
    default: s[0] = p[0]; s[1] = p[1]; s[2] = p[2]; s[3] = p[3]; break;
    }
    d[0] = remap_comp(s, remap, 1);
    d[1] = remap_comp(s, remap, 2);
    d[2] = remap_comp(s, remap, 3);
    d[3] = remap_comp(s, remap, 0);
}

typedef struct { u32 w, h; const u8* data; u32 row_bytes, rows; } tex_level_t;

static ID3D12Resource* create_texture_mipped(DXGI_FORMAT fmt, const tex_level_t* lv, u32 n)
{
    if (n == 0) return NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = lv[0].w; rd.Height = lv[0].h; rd.DepthOrArraySize = 1;
    rd.MipLevels = (u16)n; rd.Format = fmt; rd.SampleDesc.Count = 1;
    ID3D12Resource* tex = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                      &IID_ID3D12Resource, (void**)&tex)))
        return NULL;
    for (u32 m = 0; m < n; m++) {
        const u32 pitch = (lv[m].row_bytes + 255) & ~255u;
        const u32 start = (g.upload_used + 511) & ~511u;
        if ((u64)start + (u64)pitch * lv[m].rows > UPLOAD_SIZE) break;
        for (u32 y = 0; y < lv[m].rows; y++)
            memcpy(g.upload_mapped + start + (size_t)y * pitch,
                   lv[m].data + (size_t)y * lv[m].row_bytes, lv[m].row_bytes);
        D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
        src.pResource = g.upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = start;
        src.PlacedFootprint.Footprint.Format = fmt;
        src.PlacedFootprint.Footprint.Width = lv[m].w;
        src.PlacedFootprint.Footprint.Height = lv[m].h;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = pitch;
        dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = m;
        g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);
        g.upload_used = start + pitch * lv[m].rows;
    }
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    return tex;
}

/* Cubemap faces are stored face-major in guest memory. D3D12 subresources use
 * that same order: face * mip_count + mip. */
static ID3D12Resource* create_texture_cube(
    DXGI_FORMAT fmt, const tex_level_t* lv, u32 n_mips)
{
    if (!n_mips) return NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = lv[0].w; rd.Height = lv[0].h; rd.DepthOrArraySize = 6;
    rd.MipLevels = (u16)n_mips; rd.Format = fmt; rd.SampleDesc.Count = 1;
    ID3D12Resource* tex = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&tex)))
        return NULL;
    for (u32 face = 0; face < 6; face++) {
        for (u32 mip = 0; mip < n_mips; mip++) {
            const tex_level_t* level = &lv[face * n_mips + mip];
            const u32 pitch = (level->row_bytes + 255) & ~255u;
            const u32 start = (g.upload_used + 511) & ~511u;
            if ((u64)start + (u64)pitch * level->rows > UPLOAD_SIZE) {
                tex->lpVtbl->Release(tex);
                return NULL;
            }
            for (u32 y = 0; y < level->rows; y++)
                memcpy(g.upload_mapped + start + (size_t)y * pitch,
                       level->data + (size_t)y * level->row_bytes,
                       level->row_bytes);
            D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
            src.pResource = g.upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = start;
            src.PlacedFootprint.Footprint.Format = fmt;
            src.PlacedFootprint.Footprint.Width = level->w;
            src.PlacedFootprint.Footprint.Height = level->h;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = pitch;
            dst.pResource = tex;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = face * n_mips + mip;
            g.list->lpVtbl->CopyTextureRegion(
                g.list, &dst, 0, 0, 0, &src, NULL);
            g.upload_used = start + pitch * level->rows;
        }
    }
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    return tex;
}

static ID3D12Resource* create_texture_rgba(const u8* rgba, u32 w, u32 h)
{
    tex_level_t lv = { w, h, rgba, w * 4, h };
    return create_texture_mipped(DXGI_FORMAT_R8G8B8A8_UNORM, &lv, 1);
}

static u32 texture_source_span(const rsx_dsp_texture* t)
{
    const u32 base_fmt = t->format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
    const int linear = (t->format & TEX_FMT_LINEAR) != 0;
    u32 texel_size = 0, block_size = 0;
    switch (base_fmt) {
    case TEX_FMT_DXT1:  block_size = 8; break;
    case TEX_FMT_DXT23:
    case TEX_FMT_DXT45: block_size = 16; break;
    case TEX_FMT_B8: texel_size = 1; break;
    case TEX_FMT_A4R4G4B4:
    case TEX_FMT_A1R5G5B5:
    case TEX_FMT_R5G6B5:
    case TEX_FMT_G8B8: texel_size = 2; break;
    case TEX_FMT_A8R8G8B8:
    case TEX_FMT_DEPTH24_D8: texel_size = 4; break;
    default: return 0;
    }
    if (!t->width || !t->height || t->width > 4096 || t->height > 4096 ||
        t->dimension != 2)
        return 0;
    if (t->cubemap && !block_size)
        return 0;
    u32 n_mips = t->mipmaps ? t->mipmaps : 1;
    if (n_mips > 14) n_mips = 14;
    if (t->cubemap) {
        n_mips = 1;
        for (u32 d = (t->width < t->height ? t->width : t->height) / 4;
             d > 1; d >>= 1)
            n_mips++;
        if (t->mipmaps && n_mips > t->mipmaps)
            n_mips = t->mipmaps;
        if (n_mips > 14) n_mips = 14;
    }
    u32 mw = t->width, mh = t->height, span = 0;
    for (u32 m = 0; m < n_mips; m++) {
        if (block_size)
            span += ((mw + 3) / 4) * block_size * ((mh + 3) / 4);
        else {
            const u32 pitch = (m == 0 && linear && t->pitch)
                ? t->pitch : mw * texel_size;
            span += pitch * mh;
        }
        if (mw == 1 && mh == 1) break;
        mw = mw > 1 ? mw >> 1 : 1;
        mh = mh > 1 ? mh >> 1 : 1;
    }
    return t->cubemap ? span * 6 : span;
}

static u64 texture_content_hash(const rsx_dsp_texture* t, int* readable)
{
    const u32 span = texture_source_span(t);
    const u8* src = span ? guest_ptr(t->location, t->offset, span) : NULL;
    if (!src) {
        *readable = 0;
        return 0;
    }
    /* One hash per cached texture per presented frame.  Word-at-a-time FNV is
     * deliberately cheap; this is a mutation detector, not a content ID. */
    u64 hash = 1469598103934665603ull;
    u32 i = 0;
    for (; i + 8 <= span; i += 8) {
        u64 word;
        memcpy(&word, src + i, sizeof(word));
        hash ^= word;
        hash *= 1099511628211ull;
    }
    for (; i < span; i++) {
        hash ^= src[i];
        hash *= 1099511628211ull;
    }
    *readable = 1;
    return hash;
}

static ID3D12Resource* decode_guest_texture(const rsx_dsp_texture* t, u32 remap)
{
    const u32 base_fmt = t->format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
    const int linear = (t->format & TEX_FMT_LINEAR) != 0;
    const u32 w = t->width, h = t->height;
    if (!w || !h || w > 4096 || h > 4096 || t->dimension != 2)
        return NULL;
    u32 n_mips = t->mipmaps ? t->mipmaps : 1;
    if (n_mips > 14) n_mips = 14;

    if (base_fmt == TEX_FMT_DXT1 || base_fmt == TEX_FMT_DXT23 ||
        base_fmt == TEX_FMT_DXT45) {
        const DXGI_FORMAT dxgi = base_fmt == TEX_FMT_DXT1 ? DXGI_FORMAT_BC1_UNORM
                               : base_fmt == TEX_FMT_DXT23 ? DXGI_FORMAT_BC2_UNORM
                                                           : DXGI_FORMAT_BC3_UNORM;
        const u32 block = base_fmt == TEX_FMT_DXT1 ? 8 : 16;
        const u32 total = texture_source_span(t);
        const u8* src = guest_ptr(t->location, t->offset, total);
        if (!src) return NULL;
        if (t->cubemap) {
            n_mips = 1;
            for (u32 d = (w < h ? w : h) / 4; d > 1; d >>= 1)
                n_mips++;
            if (t->mipmaps && n_mips > t->mipmaps)
                n_mips = t->mipmaps;
            if (n_mips > 14) n_mips = 14;
            const u32 face_span = total / 6;
            tex_level_t cube_levels[6 * 14];
            u32 level = 0;
            for (u32 face = 0; face < 6; face++) {
                u32 mw = w, mh = h, off = 0;
                for (u32 mip = 0; mip < n_mips; mip++) {
                    const u32 bw = (mw + 3) / 4, bh = (mh + 3) / 4;
                    cube_levels[level].w = (mw + 3) & ~3u;
                    cube_levels[level].h = (mh + 3) & ~3u;
                    cube_levels[level].data =
                        src + (size_t)face * face_span + off;
                    cube_levels[level].row_bytes = bw * block;
                    cube_levels[level].rows = bh;
                    level++;
                    off += bw * block * bh;
                    mw = mw > 1 ? mw >> 1 : 1;
                    mh = mh > 1 ? mh >> 1 : 1;
                }
            }
            return create_texture_cube(dxgi, cube_levels, n_mips);
        }
        tex_level_t levels[14];
        u32 mw = w, mh = h, off = 0, n = 0;
        for (u32 m = 0; m < n_mips; m++) {
            const u32 bw = (mw + 3) / 4, bh = (mh + 3) / 4;
            levels[n].w = (mw + 3) & ~3u;
            levels[n].h = (mh + 3) & ~3u;
            levels[n].data = src + off;
            levels[n].row_bytes = bw * block;
            levels[n].rows = bh;
            n++;
            off += bw * block * bh;
            if (mw == 1 && mh == 1) break;
            mw = mw > 1 ? mw >> 1 : 1;
            mh = mh > 1 ? mh >> 1 : 1;
        }
        return create_texture_mipped(dxgi, levels, n);
    }

    u32 texel_size;
    switch (base_fmt) {
    case TEX_FMT_B8: texel_size = 1; break;
    case TEX_FMT_A4R4G4B4:
    case TEX_FMT_A1R5G5B5:
    case TEX_FMT_R5G6B5:
    case TEX_FMT_G8B8: texel_size = 2; break;
    case TEX_FMT_A8R8G8B8:
    case TEX_FMT_DEPTH24_D8: texel_size = 4; break;
    default: return NULL;
    }
    if (!linear && ((w & (w - 1)) || (h & (h - 1)))) return NULL;
    const u32 span = texture_source_span(t);
    const u8* src = guest_ptr(t->location, t->offset, span);
    if (!src) return NULL;

    u8* rgba[14] = {0};
    tex_level_t levels[14];
    u32 mw = w, mh = h, off = 0, n = 0;
    int oom = 0;
    for (u32 m = 0; m < n_mips; m++) {
        const u32 pitch = (m == 0 && linear && t->pitch)
            ? t->pitch : mw * texel_size;
        rgba[n] = (u8*)malloc((size_t)mw * mh * 4);
        if (!rgba[n]) { oom = 1; break; }
        const u32 lw = log2_u32(mw), lh = log2_u32(mh);
        const u8* level_src = src + off;
        for (u32 y = 0; y < mh; y++)
            for (u32 x = 0; x < mw; x++) {
                const u8* pixel = linear
                    ? level_src + (size_t)y * pitch + (size_t)x * texel_size
                    : level_src + (size_t)morton_index(x, y, lw, lh) * texel_size;
                decode_texel(base_fmt, pixel, remap,
                             rgba[n] + ((size_t)y * mw + x) * 4);
            }
        levels[n].w = mw;
        levels[n].h = mh;
        levels[n].data = rgba[n];
        levels[n].row_bytes = mw * 4;
        levels[n].rows = mh;
        n++;
        off += pitch * mh;
        if (mw == 1 && mh == 1) break;
        mw = mw > 1 ? mw >> 1 : 1;
        mh = mh > 1 ? mh >> 1 : 1;
    }
    ID3D12Resource* resource = (!oom && n)
        ? create_texture_mipped(DXGI_FORMAT_R8G8B8A8_UNORM, levels, n) : NULL;
    for (u32 m = 0; m < n; m++) free(rgba[m]);
    return resource;
}

static void write_texture_srv(u32 index, const texcache_t* entry)
{
    const u32 base_fmt = entry->format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
    const int compressed = base_fmt == TEX_FMT_DXT1 ||
                           base_fmt == TEX_FMT_DXT23 ||
                           base_fmt == TEX_FMT_DXT45;
    if (entry->cubemap) {
        static const u32 sel2d3d[4] = { 3, 0, 1, 2 };
        static const u32 out2comp[4] = { 1, 2, 3, 0 };
        u32 mapping[4];
        for (u32 out = 0; out < 4; out++) {
            const u32 comp = out2comp[out];
            const u32 op = (entry->remap >> (8 + comp * 2)) & 3;
            const u32 sel = (entry->remap >> (comp * 2)) & 3;
            mapping[out] = op == 0 ? 4 : op == 1 ? 5 : sel2d3d[sel];
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
        desc.Format = base_fmt == TEX_FMT_DXT1 ? DXGI_FORMAT_BC1_UNORM
                    : base_fmt == TEX_FMT_DXT23 ? DXGI_FORMAT_BC2_UNORM
                                                : DXGI_FORMAT_BC3_UNORM;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        desc.Shader4ComponentMapping = entry->remap == 0xAAE4
            ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
            : mapping[0] | (mapping[1] << 3) | (mapping[2] << 6) |
              (mapping[3] << 9) | (1u << 12);
        desc.TextureCube.MipLevels = (UINT)-1;
        g.dev->lpVtbl->CreateShaderResourceView(
            g.dev, entry->tex, &desc, srv_cpu(SRV_TEXTURE_BASE + index));
    } else if (compressed && entry->remap != 0xAAE4) {
        static const u32 sel2d3d[4] = { 3, 0, 1, 2 };
        static const u32 out2comp[4] = { 1, 2, 3, 0 };
        u32 mapping[4];
        for (u32 out = 0; out < 4; out++) {
            const u32 comp = out2comp[out];
            const u32 op = (entry->remap >> (8 + comp * 2)) & 3;
            const u32 sel = (entry->remap >> (comp * 2)) & 3;
            mapping[out] = op == 0 ? 4 : op == 1 ? 5 : sel2d3d[sel];
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
        desc.Format = base_fmt == TEX_FMT_DXT1 ? DXGI_FORMAT_BC1_UNORM
                    : base_fmt == TEX_FMT_DXT23 ? DXGI_FORMAT_BC2_UNORM
                                                : DXGI_FORMAT_BC3_UNORM;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = mapping[0] | (mapping[1] << 3) |
                                       (mapping[2] << 6) | (mapping[3] << 9) |
                                       (1u << 12);
        desc.Texture2D.MipLevels = (UINT)-1;
        g.dev->lpVtbl->CreateShaderResourceView(
            g.dev, entry->tex, &desc, srv_cpu(SRV_TEXTURE_BASE + index));
    } else {
        srv_write(SRV_TEXTURE_BASE + index, entry->tex);
    }
}

/* Decode a guest texture descriptor into a cached SRV slot.  Unlike the old
 * descriptor-only cache, re-hash the source once per frame and refresh the
 * D3D resource when the game rewrites the same guest address. */
static u32 texture_srv_slot(const rsx_dsp_texture* t)
{
    const u32 remap = t->remap & 0xFFFF;
    static int refresh_enabled = -1;
    if (refresh_enabled < 0)
        refresh_enabled = getenv("YZ_RSX_NO_TEX_REFRESH") ? 0 : 1;

    for (u32 i = 0; i < g.n_textures; i++) {
        texcache_t* entry = &g.textures[i];
        if (entry->location != t->location || entry->offset != t->offset ||
            entry->format != t->format || entry->width != t->width ||
            entry->height != t->height || entry->pitch != t->pitch ||
            entry->remap != remap || entry->cubemap != t->cubemap)
            continue;
        if (refresh_enabled && entry->tex &&
            entry->last_hash_frame != g_ld_frames) {
            int readable = 0;
            const u64 hash = texture_content_hash(t, &readable);
            entry->last_hash_frame = g_ld_frames;
            if (readable && hash != entry->content_hash) {
                ID3D12Resource* replacement = decode_guest_texture(t, remap);
                if (replacement) {
                    ID3D12Resource* old = entry->tex;
                    entry->tex = replacement;
                    entry->content_hash = hash;
                    write_texture_srv(i, entry);
                    retire_texture(old);
                    static u32 refresh_count = 0;
                    refresh_count++;
                    if (refresh_count <= 64 || (refresh_count & 255) == 0)
                        fprintf(stderr,
                                "[tex-refresh] n=%u frame=%u unit-src=%u:0x%08X "
                                "fmt=0x%02X %ux%u\n",
                                refresh_count, g_ld_frames, t->location, t->offset,
                                t->format, t->width, t->height);
                }
            }
        }
        return entry->tex ? SRV_TEXTURE_BASE + i : SRV_WHITE;
    }

    if (g.n_textures >= MAX_TEXTURES) {
        g_ld_texture_cache_full++;
        if (g_ld_texture_cache_full <= 16 ||
            (g_ld_texture_cache_full & (g_ld_texture_cache_full - 1)) == 0)
            fprintf(stderr,
                    "[texture-cache] FULL cap=%u white-fallbacks=%llu\n",
                    MAX_TEXTURES,
                    (unsigned long long)g_ld_texture_cache_full);
        return SRV_WHITE;
    }
    const u32 index = g.n_textures++;
    texcache_t* entry = &g.textures[index];
    memset(entry, 0, sizeof(*entry));
    entry->location = t->location;
    entry->offset = t->offset;
    entry->format = t->format;
    entry->width = t->width;
    entry->height = t->height;
    entry->pitch = t->pitch;
    entry->remap = remap;
    entry->cubemap = t->cubemap;
    entry->last_hash_frame = g_ld_frames;
    {
        int readable = 0;
        entry->content_hash = texture_content_hash(t, &readable);
    }
    entry->tex = decode_guest_texture(t, remap);
    if (entry->tex) {
        write_texture_srv(index, entry);
    } else {
        g_ld_texture_decode_fail++;
        if (g_ld_texture_decode_fail <= 16 ||
            (g_ld_texture_decode_fail & (g_ld_texture_decode_fail - 1)) == 0)
            fprintf(stderr,
                    "[texture-cache] decode failed n=%llu src=%u:0x%08X "
                    "fmt=0x%02X %ux%u pitch=%u\n",
                    (unsigned long long)g_ld_texture_decode_fail,
                    t->location, t->offset, t->format, t->width, t->height,
                    t->pitch);
    }
    return entry->tex ? SRV_TEXTURE_BASE + index : SRV_WHITE;
}

static int vertex_texture_supported(const rsx_dsp_vertex_texture* vt)
{
    return vt->dimension == 2 && !vt->cubemap &&
           (vt->format & TEX_FMT_BASE_MASK) ==
               RSX_TEX_FMT_W32Z32Y32X32_FLOAT;
}

static u32 vertex_texture_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        if (!vt.enabled) continue;
        g_ld_vtex_enabled++;
        if (vertex_texture_supported(&vt)) {
            mask |= 1u << u;
        } else {
            g_ld_vtex_unsupported++;
            static u32 warned = 0;
            if (warned++ < 16)
                fprintf(stderr,
                        "[vtex] enabled but unsupported unit=%u off=0x%08X "
                        "fmt=0x%02X dim=%u cube=%u %ux%u pitch=%u ctl=0x%08X\n",
                        u, vt.offset, vt.format, vt.dimension, vt.cubemap,
                        vt.width, vt.height, vt.pitch, vt.control0);
        }
    }
    return mask;
}

static u64 vertex_texture_hash(const rsx_dsp_vertex_texture* vt,
                               const u8** out_src, u32* out_pitch)
{
    const u32 pitch = vt->pitch ? vt->pitch : vt->width * 16u;
    const u32 span = pitch * vt->height;
    const u8* src = span ? guest_ptr(vt->location, vt->offset, span) : NULL;
    if (out_src) *out_src = src;
    if (out_pitch) *out_pitch = pitch;
    return src ? fnv1a(src, span, 1469598103934665603ull) : 0;
}

static ID3D12Resource* decode_vertex_texture(
    const rsx_dsp_vertex_texture* vt, u64* out_hash)
{
    if (!vertex_texture_supported(vt) || !vt->width || !vt->height ||
        vt->width > 4096 || vt->height > 4096)
        return NULL;
    const u8* src = NULL;
    u32 pitch = 0;
    const u64 hash = vertex_texture_hash(vt, &src, &pitch);
    if (!src) return NULL;
    const u32 row_bytes = vt->width * 16u;
    u8* staging = (u8*)malloc((size_t)row_bytes * vt->height);
    if (!staging) return NULL;
    for (u32 y = 0; y < vt->height; y++) {
        const u8* srow = src + (size_t)y * pitch;
        u8* drow = staging + (size_t)y * row_bytes;
        for (u32 c = 0; c < vt->width * 4u; c++) {
            const u8* p = srow + (size_t)c * 4;
            const u32 v = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                          ((u32)p[2] << 8) | (u32)p[3];
            memcpy(drow + (size_t)c * 4, &v, 4);
        }
    }
    tex_level_t level = {
        vt->width, vt->height, staging, row_bytes, vt->height
    };
    ID3D12Resource* tex = create_texture_mipped(
        DXGI_FORMAT_R32G32B32A32_FLOAT, &level, 1);
    free(staging);
    if (tex) {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
        if (out_hash) *out_hash = hash;
    }
    return tex;
}

static void write_vertex_texture_srv(u32 index, ID3D12Resource* tex)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    g.dev->lpVtbl->CreateShaderResourceView(
        g.dev, tex, &sd, srv_cpu(SRV_VTEX_BASE + index));
}

static u32 vertex_texture_srv_slot(const rsx_dsp_vertex_texture* vt)
{
    for (u32 i = 0; i < g.n_vtex; i++) {
        vtexcache_t* e = &g.vtex[i];
        if (e->location != vt->location || e->offset != vt->offset ||
            e->format != vt->format || e->width != vt->width ||
            e->height != vt->height || e->pitch != vt->pitch)
            continue;
        if (e->tex && e->last_hash_frame != g_ld_frames) {
            const u64 hash = vertex_texture_hash(vt, NULL, NULL);
            e->last_hash_frame = g_ld_frames;
            if (hash && hash != e->content_hash) {
                u64 replacement_hash = 0;
                ID3D12Resource* replacement =
                    decode_vertex_texture(vt, &replacement_hash);
                if (replacement) {
                    ID3D12Resource* old = e->tex;
                    e->tex = replacement;
                    e->content_hash = replacement_hash;
                    write_vertex_texture_srv(i, e->tex);
                    retire_texture(old);
                    g_ld_vtex_refreshes++;
                }
            }
        }
        return e->tex ? SRV_VTEX_BASE + i : SRV_WHITE;
    }
    if (g.n_vtex >= MAX_VTEX) return SRV_WHITE;
    const u32 index = g.n_vtex++;
    vtexcache_t* e = &g.vtex[index];
    memset(e, 0, sizeof(*e));
    e->location = vt->location;
    e->offset = vt->offset;
    e->format = vt->format;
    e->width = vt->width;
    e->height = vt->height;
    e->pitch = vt->pitch;
    e->last_hash_frame = g_ld_frames;
    e->tex = decode_vertex_texture(vt, &e->content_hash);
    if (e->tex) {
        write_vertex_texture_srv(index, e->tex);
        g_ld_vtex_uploads++;
        fprintf(stderr,
                "[vtex] upload unit-data %u:0x%08X fmt=0x%02X %ux%u pitch=%u\n",
                vt->location, vt->offset, vt->format,
                vt->width, vt->height, vt->pitch);
    }
    return e->tex ? SRV_VTEX_BASE + index : SRV_WHITE;
}

/* ---------------------------------------------------------------------------
 * surfaces (color RTs keyed by location/offset), rendered into then presented
 * -----------------------------------------------------------------------*/
static u32 surface_get(u32 location, u32 offset, u32 want_w, u32 want_h)
{
    if (!want_w) want_w = g.width;
    if (!want_h) want_h = g.height;
    u32 slot = MAX_SURFACES;
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].location == location && g.surfaces[i].offset == offset) {
            if (g.surfaces[i].w == want_w && g.surfaces[i].h == want_h)
                return i;
            slot = i;
            break;
        }
    if (slot == MAX_SURFACES) {
        if (g.n_surfaces >= MAX_SURFACES) return LD_INVALID_SURFACE;
        slot = g.n_surfaces;
    } else {
        surface_t* old = &g.surfaces[slot];
        fprintf(stderr,
                "[surfsz] live surface 0x%X redeclared %ux%u -> %ux%u "
                "(content dropped)\n",
                offset, old->w, old->h, want_w, want_h);
        if (old->tex) {
            old->tex->lpVtbl->Release(old->tex);
            old->tex = NULL;
        }
    }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = want_w; rd.Height = want_h; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0}; cv.Format = rd.Format;
    surface_t* s = &g.surfaces[slot];
    const HRESULT create_hr = g.dev->lpVtbl->CreateCommittedResource(
        g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
        &IID_ID3D12Resource, (void**)&s->tex);
    if (FAILED(create_hr)) {
        static u32 surface_fail_logs = 0;
        if (surface_fail_logs++ < 32) {
            const HRESULT removed = g.dev->lpVtbl->GetDeviceRemovedReason(g.dev);
            fprintf(stderr,
                    "[surface-fail] color %u:0x%X %ux%u hr=0x%08lX "
                    "removed=0x%08lX slot=%u count=%u\n",
                    location, offset, want_w, want_h,
                    (unsigned long)create_hr, (unsigned long)removed,
                    slot, g.n_surfaces);
        }
        return LD_INVALID_SURFACE;
    }
    s->location = location; s->offset = offset; s->w = want_w; s->h = want_h;
    /* RTVs for surfaces live above the swap-chain backbuffer RTVs */
    g.dev->lpVtbl->CreateRenderTargetView(g.dev, s->tex,
        NULL, rtv_handle(LD_SWAP_BUFFERS + slot));
    srv_write(SRV_SURFACE_BASE + slot, s->tex);
    if (slot == g.n_surfaces) g.n_surfaces++;
    return slot;
}

static u32 current_surface(void)
{
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    return surface_get(sf.color_location[0], sf.color_offset[0],
                       sf.clip_w, sf.clip_h);
}

/* On RSX each zeta (depth) address is distinct memory. The offline renderer's
 * s31 fix proved that sharing one D3D depth resource across all of this game's
 * shadow, scene and post-processing passes cross-contaminates later depth
 * tests. Keep the legacy shared target only as a bounded allocation fallback. */
static int honor_zeta_track(void)
{
    static int initialized = 0;
    static int enabled = 1;
    if (!initialized) {
        initialized = 1;
        enabled = getenv("RSX_NO_ZETA_TRACK") ? 0 : 1;
        fprintf(stderr, "[zetatrack] live per-zeta depth %s\n",
                enabled ? "ON" : "OFF (legacy shared target)");
    }
    return enabled;
}

static D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.dsv_heap, &h);
    h.ptr += (size_t)slot * g.dsv_step;
    return h;
}

/* Returns DSV slot 1+i. Slot 0 is the legacy shared depth fallback. */
static u32 zdepth_get(u32 location, u32 offset, u32 rt_w, u32 rt_h)
{
    if (!honor_zeta_track()) return 0;
    /* The DSV must cover the complete live framebuffer/viewport it is bound
     * with.  Some early passes declare a smaller clip than the active canvas;
     * allocating only that clip caused the D3D device to fail subsequent
     * surface and PSO creation.  Shadow-map sampling dimensions must therefore
     * be represented separately from the backing DSV allocation. */
    u32 want_w = rt_w > g.width ? rt_w : g.width;
    u32 want_h = rt_h > g.height ? rt_h : g.height;
    u32 slot = MAX_SURFACES;
    for (u32 i = 0; i < g.n_zdepths; i++) {
        zdepth_t* z = &g.zdepths[i];
        if (z->location == location && z->offset == offset) {
            if (z->w >= want_w && z->h >= want_h) return 1 + i;
            slot = i;
            break;
        }
    }
    if (slot == MAX_SURFACES) {
        if (g.n_zdepths >= MAX_SURFACES) {
            fprintf(stderr, "[zetatrack] live cache full; shared fallback\n");
            return 0;
        }
        slot = g.n_zdepths;
    } else {
        zdepth_t* old = &g.zdepths[slot];
        if (want_w < old->w) want_w = old->w;
        if (want_h < old->h) want_h = old->h;
        fprintf(stderr,
                "[zetatrack] live zeta %u:0x%X outgrown %ux%u -> %ux%u\n",
                location, offset, old->w, old->h, want_w, want_h);
        if (old->tex) {
            old->tex->lpVtbl->Release(old->tex);
            old->tex = NULL;
        }
    }

    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = want_w;
    rd.Height = want_h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {0};
    /* Optimized clear values must use the typed DSV format even though the
     * resource itself is typeless so it can also be exposed through an SRV. */
    cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    cv.DepthStencil.Depth = 1.0f;
    zdepth_t* z = &g.zdepths[slot];
    HRESULT create_hr = g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
            &IID_ID3D12Resource, (void**)&z->tex);
    if (FAILED(create_hr)) {
        fprintf(stderr,
                "[zetatrack] live create failed %u:0x%X hr=0x%08lX; shared fallback\n",
                location, offset, (unsigned long)create_hr);
        z->tex = NULL;
        return 0;
    }
    z->location = location;
    z->offset = offset;
    z->w = want_w;
    z->h = want_h;
    z->had_write = 0;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvd = {0};
    dsvd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    g.dev->lpVtbl->CreateDepthStencilView(
        g.dev, z->tex, &dsvd, dsv_handle(1 + slot));
    srv_write_zdepth(SRV_ZDEPTH_BASE + slot, z->tex);
    g.list->lpVtbl->ClearDepthStencilView(
        g.list, dsv_handle(1 + slot),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, NULL);
    z->cleared = 1;
    fprintf(stderr,
            "[zetatrack] live new #%u %u:0x%X backing=%ux%u "
            "clip=%ux%u canvas=%ux%u\n",
            slot, location, offset, want_w, want_h,
            rt_w, rt_h, g.width, g.height);
    if (slot == g.n_zdepths) g.n_zdepths++;
    return 1 + slot;
}

/* ---------------------------------------------------------------------------
 * PSO cache (VP+FP+render-state keyed)
 * -----------------------------------------------------------------------*/
static u64 fnv1a(const void* data, u32 n, u64 h)
{
    const u8* p = (const u8*)data;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static u32 vp_txl_unit_mask(const u8* ucode, u32 instrs)
{
    u32 mask = 0;
    for (u32 i = 0; i < instrs; i++) {
        const u8* p = ucode + i * 16;
        const u32 d1 = (u32)p[4] | ((u32)p[5] << 8) |
                       ((u32)p[6] << 16) | ((u32)p[7] << 24);
        if (((d1 >> 22) & 0x1Fu) != 0x19u) continue;
        const u32 d2 = (u32)p[8] | ((u32)p[9] << 8) |
                       ((u32)p[10] << 16) | ((u32)p[11] << 24);
        mask |= 1u << ((d2 >> 8) & 3u);
    }
    return mask;
}

static ID3D12PipelineState* build_pso(const char* vs_hlsl, const char* ps_hlsl,
                                      const render_state_t* rs)
{
    ID3DBlob *vs = NULL, *ps = NULL, *err = NULL;
    static u32 compile_fail_logs = 0;
    static u32 create_fail_logs = 0;
    if (FAILED(D3DCompile(vs_hlsl, strlen(vs_hlsl), "xvs", NULL, NULL, "main",
                          "vs_5_0", 0, 0, &vs, &err))) {
        if (compile_fail_logs++ < 32) {
            const char* msg = err ? (const char*)err->lpVtbl->GetBufferPointer(err)
                                  : "no compiler diagnostic";
            fprintf(stderr, "[pso-fail] vertex compile: %.768s\n", msg);
        }
        if (err) err->lpVtbl->Release(err); return NULL;
    }
    err = NULL;
    if (FAILED(D3DCompile(ps_hlsl, strlen(ps_hlsl), "xps", NULL, NULL, "main",
                          "ps_5_0", 0, 0, &ps, &err))) {
        if (compile_fail_logs++ < 32) {
            const char* msg = err ? (const char*)err->lpVtbl->GetBufferPointer(err)
                                  : "no compiler diagnostic";
            fprintf(stderr, "[pso-fail] pixel compile: %.768s\n", msg);
        }
        if (err) err->lpVtbl->Release(err);
        vs->lpVtbl->Release(vs); return NULL;
    }
    D3D12_INPUT_ELEMENT_DESC il[16];
    for (u32 i = 0; i < 16; i++) {
        D3D12_INPUT_ELEMENT_DESC e = {"ATTR", i, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                                      i * 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
        il[i] = e;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = g.rootsig_x;
    pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
    pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
    pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 16;
    apply_render_state(&pd, rs);
    pd.SampleMask = 0xFFFFFFFFu;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    HRESULT hr = g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd,
                                                            &IID_ID3D12PipelineState,
                                                            (void**)&pso);
    if (FAILED(hr) && create_fail_logs++ < 32) {
        const HRESULT removed = g.dev->lpVtbl->GetDeviceRemovedReason(g.dev);
        fprintf(stderr,
                "[pso-fail] create hr=0x%08lX removed=0x%08lX "
                "depth{test=%u write=%u func=%u} blend=%u "
                "cull{enable=%u face=%u}\n",
                (unsigned long)hr, (unsigned long)removed,
                rs->depth_test, rs->depth_write, rs->depth_func,
                rs->blend_enable, rs->cull_enable, rs->cull_face);
    }
    vs->lpVtbl->Release(vs); ps->lpVtbl->Release(ps);
    return SUCCEEDED(hr) ? pso : NULL;
}

static ID3D12PipelineState* get_pso(void)
{
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start >= RSX_DSP_VP_INSTR) return NULL;
    const u8* vp_uc = (const u8*)(g.rsx.vp + start * 4);
    const u32 vp_instrs = rsx_vp_program_size_instrs(vp_uc, (RSX_DSP_VP_INSTR - start) * 16);
    if (!vp_instrs) return NULL;

    u32 fp_loc = 0;
    const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
    const u8* fp_uc = guest_ptr(fp_loc, fp_off, 16);
    if (!fp_uc) return NULL;
    const u32 fp_size = rsx_fp_program_size(fp_uc, 0x10000);
    if (!fp_size) return NULL;
    /* re-resolve with the true size to validate the whole program is mapped */
    fp_uc = guest_ptr(fp_loc, fp_off, fp_size);
    if (!fp_uc) return NULL;

    /* Fragment output register mode (fp16 h0 vs fp32 r0) is driven by the
     * SHADER_CONTROL word bit 0x40 (same fix as the replay harness — the
     * AUTO heuristic returned stale fp32 scratch for h0-writing materials);
     * fold the deciding bit into the cache key so a program reused under a
     * different export mode gets its own PSO. Kill-switch YZ_FP_CTRL_AUTO=1
     * restores the old heuristic for the A/B. */
    static int ctrl_auto = -1;
    if (ctrl_auto < 0) ctrl_auto = getenv("YZ_FP_CTRL_AUTO") ? 1 : 0;
    const u32 fp_ctrl = ctrl_auto ? RSX_FP_CTRL_AUTO : rsx_dsp_shader_control(&g.rsx);
    u32 cube_mask = 0;
    for (u32 unit = 0; unit < RSX_DSP_NUM_TEXTURES; unit++) {
        rsx_dsp_texture texture;
        rsx_dsp_get_texture(&g.rsx, unit, &texture);
        if (texture.enabled && texture.cubemap)
            cube_mask |= 1u << unit;
    }
    const u32 vtex_mask = vertex_texture_mask();
    const u32 txl_mask = vp_txl_unit_mask(vp_uc, vp_instrs);
    if (txl_mask && !(txl_mask & vtex_mask)) {
        g_ld_vtex_missing_for_txl++;
        static u32 warned = 0;
        if (warned++ < 32) {
            fprintf(stderr,
                    "[vtex] TXL shader has no supported binding txl=0x%X "
                    "bound=0x%X start=%u instrs=%u\n",
                    txl_mask, vtex_mask, start, vp_instrs);
            for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
                if (!((txl_mask >> u) & 1u)) continue;
                const u32 base = 0x0900 + u * 0x20;
                fprintf(stderr,
                        "[vtex] raw u%u off=%08X fmt=%08X wrap=%08X "
                        "ctl0=%08X ctl3=%08X filter=%08X rect=%08X\n",
                        u, rsx_dsp_reg(&g.rsx, base),
                        rsx_dsp_reg(&g.rsx, base + 4),
                        rsx_dsp_reg(&g.rsx, base + 8),
                        rsx_dsp_reg(&g.rsx, base + 12),
                        rsx_dsp_reg(&g.rsx, base + 16),
                        rsx_dsp_reg(&g.rsx, base + 20),
                        rsx_dsp_reg(&g.rsx, base + 24));
            }
        }
    }

    u64 key = fnv1a(vp_uc, vp_instrs * 16, 1469598103934665603ull);
    key = fnv1a(fp_uc, fp_size, key);
    const u32 fp_ctrl_key = fp_ctrl & 0x40u;
    key = fnv1a(&fp_ctrl_key, sizeof(fp_ctrl_key), key);
    key = fnv1a(&cube_mask, sizeof(cube_mask), key);
    key = fnv1a(&vtex_mask, sizeof(vtex_mask), key);
    render_state_t rs; decode_render_state(&rs);
    key = fnv1a(&rs, sizeof(rs), key);

    for (u32 i = 0; i < g.n_psos; i++)
        if (g.psos[i].key == key) return g.psos[i].pso;
    if (g.n_psos >= MAX_PSOS) return NULL;

    static char vs_hlsl[256 * 1024];
    static char ps_hlsl[256 * 1024];
    ID3D12PipelineState* pso = NULL;
    const int vi = rsx_vp_decompile_ex(
        vp_uc, vp_instrs * 16, vtex_mask, vs_hlsl, sizeof(vs_hlsl));
    int fi = rsx_fp_decompile_ex(
        fp_uc, fp_size, fp_ctrl, cube_mask, ps_hlsl, sizeof(ps_hlsl));
    if (fi > 0 && rs.alpha_test_enable &&
        rsx_fp_apply_alpha_test(ps_hlsl, sizeof(ps_hlsl), rs.alpha_func,
            rsx_fp_alpha_ref(rs.alpha_ref_raw, rs.alpha_ref_format)) < 0)
        fi = -1;
    if (vi > 0 && fi > 0) pso = build_pso(vs_hlsl, ps_hlsl, &rs);

    g.psos[g.n_psos].key = key;
    g.psos[g.n_psos].pso = pso;
    g.n_psos++;
    return pso;
}

/* ---------------------------------------------------------------------------
 * Draw accumulation sink (mirrors the harness sink)
 * -----------------------------------------------------------------------*/
typedef struct { float a[16][4]; } vtx_t;
typedef struct { u32 first, count; } batch_t;

typedef struct {
    batch_t arr[256]; u32 n_arr;
    batch_t idx[256]; u32 n_idx;
    u32     n_packets;
    vtx_t*  verts; u32 n_verts, cap_verts;
    int     fetch_ok;
    /* Primitive-restart cut points (s25 port of the replay-harness fix):
     * a guest index equal to the RSX restart sentinel is a cut marker, not
     * a vertex reference (RPCS3 RSXThread.cpp:398); fetch_batches records
     * the n_verts position at each one and the STRIP/FAN expansion must
     * not bridge across a cut. */
    u32*    cuts; u32 n_cuts, cap_cuts;
} draw_ctx;

static draw_ctx dc;

static void dc_reset(void)
{
    dc.n_arr = dc.n_idx = dc.n_verts = 0;
    dc.n_packets = 0;
    dc.n_cuts = 0;
    dc.fetch_ok = 1;
}
static void push_vert(const vtx_t* v)
{
    if (dc.n_verts >= dc.cap_verts) {
        u32 nc = dc.cap_verts ? dc.cap_verts * 2 : 4096;
        vtx_t* nv = (vtx_t*)realloc(dc.verts, (size_t)nc * sizeof(vtx_t));
        if (!nv) { dc.fetch_ok = 0; return; }
        dc.verts = nv; dc.cap_verts = nc;
    }
    dc.verts[dc.n_verts++] = *v;
}

static float ld_be_f32(const u8* p)
{
    const u32 v = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static float ld_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0)
        out = sign;
    else if (exp == 31)
        out = sign | 0x7F800000u | (man << 13);
    else
        out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, 4);
    return f;
}

static int fetch_attr(u32 i, u32 base, u32 vertex_id, u32 base_index,
                      float out[4])
{
    rsx_dsp_vertex_attr a;
    rsx_dsp_get_vertex_attr(&g.rsx, i, &a);
    if (!a.type || !a.size) return 0;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    const u32 elem_size = rsx_vertex_attrib_size(a.type, a.size);
    const u32 stride = a.stride ? a.stride : elem_size;
    const u32 divider_mask = rsx_dsp_reg(&g.rsx, 0x1FC0);
    if (a.frequency >= 2)
        g_ld_divider_fetches++;
    const u32 source_element = rsx_vertex_element_index(
        vertex_id, base_index, a.frequency,
        (divider_mask >> i) & 1u);
    const u8* p = guest_ptr(
        a.location, base + a.offset + source_element * stride, elem_size);
    if (!p) return 0;
    for (u32 c = 0; c < a.size && c < 4; c++) {
        switch (a.type) {
        case RSX_VTX_TYPE_FLOAT:
            out[c] = ld_be_f32(p + c * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            out[c] = ld_be_f16(p + c * 2);
            break;
        case RSX_VTX_TYPE_UNORM8:
            out[c] = p[c] / 255.0f;
            break;
        case RSX_VTX_TYPE_UINT8:
            out[c] = (float)p[c];
            break;
        case RSX_VTX_TYPE_SNORM16: {
            const s16 v = (s16)((p[c * 2] << 8) | p[c * 2 + 1]);
            out[c] = v / 32767.0f;
            break;
        }
        case RSX_VTX_TYPE_SINT16: {
            const s16 v = (s16)((p[c * 2] << 8) | p[c * 2 + 1]);
            out[c] = (float)v;
            break;
        }
        case RSX_VTX_TYPE_CMP32: {
            const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                          ((u32)p[2] << 8) | (u32)p[3];
            s32 x = (s32)(w & 0x7FF);          if (x & 0x400) x -= 0x800;
            s32 y = (s32)((w >> 11) & 0x7FF); if (y & 0x400) y -= 0x800;
            s32 z = (s32)((w >> 22) & 0x3FF); if (z & 0x200) z -= 0x400;
            out[0] = (float)x / 1023.0f;
            out[1] = (float)y / 1023.0f;
            out[2] = (float)z / 511.0f;
            out[3] = 1.0f;
            return 1;
        }
        default:
            return 0;
        }
    }
    return 1;
}

static void fetch_one(u32 base, u32 vertex_id, u32 base_index)
{
    vtx_t v;
    for (u32 i = 0; i < 16; i++) {
        rsx_dsp_vertex_attr a;
        rsx_dsp_get_vertex_attr(&g.rsx, i, &a);
        if (a.type && a.size &&
            fetch_attr(i, base, vertex_id, base_index, v.a[i])) continue;
        if (i == 0) { dc.fetch_ok = 0; return; }
        rsx_dsp_vertex_default(&g.rsx, i, v.a[i]);
        if (i == 3 && v.a[3][0] == 0 && v.a[3][1] == 0 && v.a[3][2] == 0 && v.a[3][3] == 1)
            v.a[3][0] = v.a[3][1] = v.a[3][2] = 1.0f;
    }
    push_vert(&v);
}

static void fetch_batches(void)
{
    const u32 base = rsx_dsp_vertex_data_base_offset(&g.rsx);
    for (u32 r = 0; r < dc.n_arr && dc.fetch_ok; r++)
        for (u32 i = 0; i < dc.arr[r].count && dc.fetch_ok; i++)
            fetch_one(base, dc.arr[r].first + i, 0);
    if (!dc.n_idx) return;
    const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
    rsx_dsp_index_array ia; rsx_dsp_get_index_array(&g.rsx, &ia);
    /* Restart sentinel handling, same rule as the replay harness (RPCS3
     * RSXThread.cpp:398 "if (value == restart) continue" + rsx_methods.h
     * restart_index_enabled()/restart_index()): record a cut, never fetch
     * the phantom vertex. Kill-switch RSX_NO_RESTART shared with the
     * harness for byte-exact A/B. */
    static int s_no_restart = -1;
    if (s_no_restart < 0) s_no_restart = getenv("RSX_NO_RESTART") ? 1 : 0;
    const int restart_en = !s_no_restart && rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32);
    const u32 restart_val = rsx_dsp_restart_index(&g.rsx);
    for (u32 r = 0; r < dc.n_idx && dc.fetch_ok; r++)
        for (u32 i = 0; i < dc.idx[r].count && dc.fetch_ok; i++) {
            const u32 esz = ia.is_u32 ? 4 : 2;
            const u8* ip = guest_ptr(ia.location, ia.offset + (dc.idx[r].first + i) * esz, esz);
            if (!ip) { dc.fetch_ok = 0; return; }
            const u32 index = ia.is_u32
                ? (((u32)ip[0] << 24) | ((u32)ip[1] << 16) | ((u32)ip[2] << 8) | ip[3])
                : (u32)((ip[0] << 8) | ip[1]);
            if (restart_en && index == restart_val) {
                if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                                          dc.n_verts)) {
                    dc.fetch_ok = 0;
                    return;
                }
                continue;
            }
            fetch_one(base, index, base_index);
        }
}

/* primitive ids = raw NV4097 VERTEX_BEGIN_END arg (rsx_dispatch stores it raw;
 * matches the replay harness). These were off by one (4/5/6/7), which dropped
 * EVERY quad/triangle draw through the switch's default: return -> black. */
#define PRIM_TRIANGLES       5
#define PRIM_TRIANGLE_STRIP  6
#define PRIM_TRIANGLE_FAN    7
#define PRIM_QUADS           8

/* Live-draw activity counters (verification: is real geometry flowing, or only
 * clears?). Reported per presented frame in rsx_live_draw_present. */
/* DRAW_ARRAYS/DRAW_INDEX_ARRAY writes are packets. Multiple packets between a
 * single BEGIN/END are deliberately coalesced into one D3D12 DrawInstanced,
 * so comparing packet count directly with executed D3D draws was invalid.
 * Keep two independently balanced ledgers instead:
 *
 *   packets_seen = packets_queued + packets_movie + packets_queue_full
 *   groups_seen  = groups_executed + every group_drop_* outcome
 *
 * An END with no accepted packet is counted separately as groups_empty and is
 * not a render attempt. This makes every early return explicit. */
typedef struct ld_stats {
    unsigned long long packets_seen;
    unsigned long long packets_queued;
    unsigned long long packets_movie;
    unsigned long long packets_queue_full;
    unsigned long long groups_seen;
    unsigned long long groups_empty;
    unsigned long long groups_executed;
    unsigned long long group_drop_fetch;
    unsigned long long group_drop_degenerate;
    unsigned long long group_drop_primitive;
    unsigned long long group_drop_alloc;
    unsigned long long group_drop_pso;
    unsigned long long group_drop_ring;
    unsigned long long group_drop_surface;
    unsigned long long clears;
    unsigned long long clear_drop_surface;
    unsigned long long implicit_depth_clears;
} ld_stats;

static ld_stats g_ld_stats;

void rsx_live_draw_a010_probe_begin(void)
{
    if (!getenv("YZ_RSX_A010_PROBE") || !g.ready)
        return;
    CreateDirectoryA("scratch\\a010_probe", NULL);
    g_ld_a010_probe_start_frame = g_ld_frames;
    g_ld_a010_probe_sample = 0;
    g_ld_a010_probe_touched = 0;
    InterlockedExchange(&g_ld_a010_probe_active, 1);
    fprintf(stderr,
            "[a010-probe] BEGIN live_frame=%u surfaces=%u packets=%llu groups=%llu\n",
            g_ld_frames, g.n_surfaces, g_ld_stats.packets_seen,
            g_ld_stats.groups_seen);
    fflush(stderr);
}

int rsx_live_draw_a010_probe_active(void)
{
    return InterlockedCompareExchange(&g_ld_a010_probe_active, 0, 0) != 0;
}

static unsigned long long ld_groups_accounted(void)
{
    return g_ld_stats.groups_executed + g_ld_stats.group_drop_fetch +
           g_ld_stats.group_drop_degenerate + g_ld_stats.group_drop_primitive +
           g_ld_stats.group_drop_alloc + g_ld_stats.group_drop_pso +
           g_ld_stats.group_drop_ring + g_ld_stats.group_drop_surface;
}

static int ld_target_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("YZ_RSX_TARGET_TRACE") ? 1 : 0;
    return enabled;
}

static void ld_trace_target(const char* event, u32 target, u32 mask)
{
    if (!ld_target_trace_enabled()) return;
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    fprintf(stderr,
            "[rsx-target] frame=%u event=%s target=%u loc=%u off=0x%08X "
            "fmt=0x%X pitch=%u clip=%u,%u+%ux%u mask=0x%02X\n",
            g_ld_frames, event, target, sf.color_location[0], sf.color_offset[0],
            sf.color_format, sf.color_pitch[0], sf.clip_x, sf.clip_y,
            sf.clip_w, sf.clip_h, mask);
}

/* Movie mode: while a host-decoded movie owns the window
 * (rsx_live_draw_present_rgba), do not make the default boot process the
 * guest's otherwise invisible RSX stream.  That experiment materially changes
 * CRI/movie handoff scheduling and regressed the proven title/menu path.
 * YZ_MOVIE_TRACK_RSX keeps the state-tracking experiment available for a
 * focused post-movie A/B once the transition itself is deterministic. */
static volatile int g_ld_movie_mode = 0;
static int g_ld_movie_track_rsx = -1;
static int g_ld_movie_composite_ui = -1;

static int ld_movie_composite_ui_enabled(void)
{
    if (g_ld_movie_composite_ui < 0)
        g_ld_movie_composite_ui = getenv("YZ_MOVIE_COMPOSITE_UI") ? 1 : 0;
    return g_ld_movie_composite_ui;
}

static void ld_movie_reset_rings(void)
{
    g.vb_used = 0;
    g.cb_used = 0;
    g.srv_ring_used = 0;
    g.smp_ring_used = 0;
    g.depth_cleared = 0;
}

static int ld_movie_overlay_ensure(void)
{
    if (g.movie_upload && g.movie_upload_mapped &&
        g.movie_overlay_readback && g.movie_overlay_rgba &&
        g.movie_overlay_mask)
        return 1;

    g.movie_overlay_pitch = (g.width * 4 + 255) & ~255u;
    const UINT64 rb_size = (UINT64)g.movie_overlay_pitch * g.height;
    D3D12_HEAP_PROPERTIES hp = {0};
    D3D12_RESOURCE_DESC bd = {0};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = rb_size;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&g.movie_overlay_readback))) {
        fprintf(stderr, "[movie-ui] readback allocation failed\n");
        return 0;
    }
    g.movie_overlay_rgba = (u8*)malloc((size_t)g.width * g.height * 4);
    g.movie_overlay_mask = (u8*)malloc((size_t)g.width * g.height);
    if (!g.movie_overlay_rgba || !g.movie_overlay_mask) {
        free(g.movie_overlay_rgba);
        free(g.movie_overlay_mask);
        g.movie_overlay_rgba = NULL;
        g.movie_overlay_mask = NULL;
        g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
        g.movie_overlay_readback = NULL;
        fprintf(stderr, "[movie-ui] CPU overlay allocation failed\n");
        return 0;
    }

    /* Do not reuse the general guest upload ring for host frames. Guest draw
     * commands recorded between flips can still reference that memory; a
     * dedicated upload buffer lets host presentation append to the same
     * command list without a second fence/wait on every movie frame. */
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&g.movie_upload))) {
        free(g.movie_overlay_rgba);
        free(g.movie_overlay_mask);
        g.movie_overlay_rgba = NULL;
        g.movie_overlay_mask = NULL;
        g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
        g.movie_overlay_readback = NULL;
        fprintf(stderr, "[movie-ui] host upload allocation failed\n");
        return 0;
    }
    D3D12_RANGE no_read = {0, 0};
    if (FAILED(g.movie_upload->lpVtbl->Map(
            g.movie_upload, 0, &no_read, (void**)&g.movie_upload_mapped))) {
        g.movie_upload->lpVtbl->Release(g.movie_upload);
        g.movie_upload = NULL;
        free(g.movie_overlay_rgba);
        free(g.movie_overlay_mask);
        g.movie_overlay_rgba = NULL;
        g.movie_overlay_mask = NULL;
        g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
        g.movie_overlay_readback = NULL;
        fprintf(stderr, "[movie-ui] host upload map failed\n");
        return 0;
    }
    return 1;
}

/* Start movies from a known transparent guest surface. This removes the stale
 * pre-movie frame before the game begins drawing captions, while leaving the
 * swap chain entirely owned by the 30 Hz host presenter. */
static void ld_movie_overlay_begin(void)
{
    g.movie_overlay_valid = 0;
    g.movie_overlay_frames = 0;
    if (!ld_movie_overlay_ensure()) return;

    ld_flush();
    const float transparent[4] = {0, 0, 0, 0};
    for (u32 i = 0; i < g.n_surfaces; i++)
        g.list->lpVtbl->ClearRenderTargetView(
            g.list, rtv_handle(LD_SWAP_BUFFERS + i), transparent, 0, NULL);
    if (g.n_surfaces) ld_flush();
    ld_movie_reset_rings();
    fprintf(stderr, "[movie-ui] compositor armed (%ux%u, %u guest surfaces)\n",
            g.width, g.height, g.n_surfaces);
    fflush(stderr);
}

/* Capture the guest's latest offscreen result without presenting it. The
 * guest target is not a true transparent overlay: depending on the auth
 * sequence it can contain black, a fade, or a complete rendered scene.
 * Extract only low-saturation bright glyphs from the subtitle-safe lower band
 * and synthesize a small dark outline. The decoded movie therefore remains the
 * background even when the guest rendered an opaque full-screen image. */
static void ld_movie_capture_overlay(void)
{
    if (!ld_movie_overlay_ensure()) return;
    const u32 target = current_surface();
    if (target == LD_INVALID_SURFACE) {
        g.movie_overlay_valid = 0;
        return;
    }
    ID3D12Resource* rt = g.surfaces[target].tex;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = rt;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = g.movie_overlay_readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = g.width;
    dst.PlacedFootprint.Footprint.Height = g.height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = g.movie_overlay_pitch;
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    ld_flush();

    const SIZE_T rb_size = (SIZE_T)g.movie_overlay_pitch * g.height;
    u8* mapped = NULL;
    D3D12_RANGE rr = {0, rb_size};
    if (FAILED(g.movie_overlay_readback->lpVtbl->Map(
            g.movie_overlay_readback, 0, &rr, (void**)&mapped))) {
        g.movie_overlay_valid = 0;
        return;
    }
    for (u32 y = 0; y < g.height; y++)
        memcpy(g.movie_overlay_rgba + (size_t)y * g.width * 4,
               mapped + (size_t)y * g.movie_overlay_pitch,
               (size_t)g.width * 4);
    D3D12_RANGE wr = {0, 0};
    g.movie_overlay_readback->lpVtbl->Unmap(
        g.movie_overlay_readback, 0, &wr);

    const u64 total = (u64)g.width * g.height;
    const u32 band_y0 = g.height * 52 / 100;
    const u32 band_y1 = g.height * 96 / 100;
    const u32 band_x0 = g.width * 5 / 100;
    const u32 band_x1 = g.width * 95 / 100;
    const u64 band_pixels =
        (u64)(band_y1 - band_y0) * (band_x1 - band_x0);
    u64 glyph_pixels = 0;
    memset(g.movie_overlay_mask, 0, (size_t)total);

    for (u32 y = band_y0; y < band_y1; y++) {
        for (u32 x = band_x0; x < band_x1; x++) {
            const u64 i = (u64)y * g.width + x;
            const u8* p = g.movie_overlay_rgba + i * 4;
            const int hi = p[0] > p[1]
                ? (p[0] > p[2] ? p[0] : p[2])
                : (p[1] > p[2] ? p[1] : p[2]);
            const int lo = p[0] < p[1]
                ? (p[0] < p[2] ? p[0] : p[2])
                : (p[1] < p[2] ? p[1] : p[2]);
            if (hi < 145 || hi - lo > 52)
                continue;
            int coverage = (hi - 96) * 255 / 159;
            if (coverage < 64) coverage = 64;
            if (coverage > 255) coverage = 255;
            g.movie_overlay_mask[i] = (u8)coverage;
            glyph_pixels++;
        }
    }

    /* A value of 1 denotes the synthetic outline; 64..255 are glyph
     * coverage. Work from a copy condition (>=64) so dilation does not grow
     * recursively. */
    if (glyph_pixels >= 8 && glyph_pixels < band_pixels / 6) {
        for (u32 y = band_y0; y < band_y1; y++) {
            for (u32 x = band_x0; x < band_x1; x++) {
                const u64 i = (u64)y * g.width + x;
                if (g.movie_overlay_mask[i] < 64)
                    continue;
                const u32 ya = y > 1 ? y - 2 : 0;
                const u32 yb = y + 2 < g.height ? y + 2 : g.height - 1;
                const u32 xa = x > 1 ? x - 2 : 0;
                const u32 xb = x + 2 < g.width ? x + 2 : g.width - 1;
                for (u32 oy = ya; oy <= yb; oy++)
                    for (u32 ox = xa; ox <= xb; ox++) {
                        u8* m = &g.movie_overlay_mask[(u64)oy * g.width + ox];
                        if (!*m) *m = 1;
                    }
            }
        }
    }
    g.movie_overlay_valid =
        glyph_pixels >= 8 && glyph_pixels < band_pixels / 6;
    g.movie_overlay_frames++;
    if (g.movie_overlay_frames <= 16 ||
        (g.movie_overlay_frames & 63) == 0 ||
        (!g.movie_overlay_valid && glyph_pixels)) {
        fprintf(stderr,
                "[movie-ui] overlay=%llu glyphs=%llu/%llu %s\n",
                (unsigned long long)g.movie_overlay_frames,
                (unsigned long long)glyph_pixels,
                (unsigned long long)band_pixels,
                g.movie_overlay_valid ? "accepted" : "rejected");
        fflush(stderr);
    }

    /* A guest flip still marks a new texture-generation boundary even though
     * the guest surface is not sent to the swap chain. */
    g_ld_frames++;
    ld_movie_reset_rings();
}

static void sink_begin(void* u, const rsx_dispatch* r, u32 prim) { (void)u; (void)r; (void)prim; dc_reset(); }
static void sink_draw_arrays(void* u, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)u; (void)r; g_ld_stats.packets_seen++;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) {
        g_ld_stats.packets_movie++;
        return;
    }
    dc.n_packets++;
    if (dc.n_arr >= 256) { g_ld_stats.packets_queue_full++; return; }
    dc.arr[dc.n_arr].first = first; dc.arr[dc.n_arr].count = count; dc.n_arr++;
    g_ld_stats.packets_queued++;
}
static void sink_draw_index(void* u, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)u; (void)r; g_ld_stats.packets_seen++;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) {
        g_ld_stats.packets_movie++;
        return;
    }
    dc.n_packets++;
    if (dc.n_idx >= 256) { g_ld_stats.packets_queue_full++; return; }
    dc.idx[dc.n_idx].first = first; dc.idx[dc.n_idx].count = count; dc.n_idx++;
    g_ld_stats.packets_queued++;
}

static void sink_end(void* user, const rsx_dispatch* r)
{
    (void)user; (void)r;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) return;
    if (!dc.n_packets) { g_ld_stats.groups_empty++; return; }
    g_ld_stats.groups_seen++;
    const u32 prim = g.rsx.current_primitive;
    fetch_batches();
    if (!dc.n_verts || !dc.fetch_ok) { g_ld_stats.group_drop_fetch++; return; }

    /* Segment table from the restart cuts (port of the replay-harness s25c/
     * s25d fixes): STRIP/FAN never bridge a cut, strips alternate winding
     * per LOCAL triangle index (odd triangles flip vertex order to keep
     * face orientation under backface culling), fans anchor to their OWN
     * segment's first vertex. */
    const u32 n_seg = dc.n_cuts + 1;

    vtx_t* tri = NULL; u32 n_tri = 0;
    switch (prim) {
    case PRIM_TRIANGLES: tri = dc.verts; n_tri = dc.n_verts - dc.n_verts % 3; break;
    case PRIM_TRIANGLE_STRIP: {
        if (dc.n_verts < 3) { g_ld_stats.group_drop_degenerate++; return; }
        u32 total = 0;
        for (u32 s = 0; s < n_seg; s++) {
            u32 sb, cnt;
            rsx_restart_segment_bounds(dc.cuts, dc.n_cuts, dc.n_verts,
                                       s, &sb, &cnt);
            if (cnt >= 3) total += (cnt - 2) * 3;
        }
        if (!total) { g_ld_stats.group_drop_degenerate++; return; }
        n_tri = total; tri = (vtx_t*)malloc(n_tri * sizeof(vtx_t));
        if (!tri) { g_ld_stats.group_drop_alloc++; return; }
        u32 w = 0;
        for (u32 s = 0; s < n_seg; s++) {
            u32 sb, cnt;
            rsx_restart_segment_bounds(dc.cuts, dc.n_cuts, dc.n_verts,
                                       s, &sb, &cnt);
            if (cnt < 3) continue;
            for (u32 i = 0; i + 2 < cnt; i++) {
                if (i & 1) {
                    tri[w*3+0] = dc.verts[sb+i+1]; tri[w*3+1] = dc.verts[sb+i];   tri[w*3+2] = dc.verts[sb+i+2];
                } else {
                    tri[w*3+0] = dc.verts[sb+i];   tri[w*3+1] = dc.verts[sb+i+1]; tri[w*3+2] = dc.verts[sb+i+2];
                }
                w++;
            }
        }
        break;
    }
    case PRIM_TRIANGLE_FAN: {
        if (dc.n_verts < 3) { g_ld_stats.group_drop_degenerate++; return; }
        u32 total = 0;
        for (u32 s = 0; s < n_seg; s++) {
            u32 sb, cnt;
            rsx_restart_segment_bounds(dc.cuts, dc.n_cuts, dc.n_verts,
                                       s, &sb, &cnt);
            if (cnt >= 3) total += (cnt - 2) * 3;
        }
        if (!total) { g_ld_stats.group_drop_degenerate++; return; }
        n_tri = total; tri = (vtx_t*)malloc(n_tri * sizeof(vtx_t));
        if (!tri) { g_ld_stats.group_drop_alloc++; return; }
        u32 w = 0;
        for (u32 s = 0; s < n_seg; s++) {
            u32 sb, cnt;
            rsx_restart_segment_bounds(dc.cuts, dc.n_cuts, dc.n_verts,
                                       s, &sb, &cnt);
            if (cnt < 3) continue;
            for (u32 i = 1; i + 1 < cnt; i++) {
                tri[w*3+0] = dc.verts[sb]; tri[w*3+1] = dc.verts[sb+i]; tri[w*3+2] = dc.verts[sb+i+1];
                w++;
            }
        }
        break;
    }
    case PRIM_QUADS: {
        const u32 quads = dc.n_verts / 4;
        if (!quads) { g_ld_stats.group_drop_degenerate++; return; }
        n_tri = quads * 6; tri = (vtx_t*)malloc(n_tri * sizeof(vtx_t));
        if (!tri) { g_ld_stats.group_drop_alloc++; return; }
        for (u32 q = 0; q < quads; q++) {
            const vtx_t* v = &dc.verts[q*4]; vtx_t* t = &tri[q*6];
            t[0]=v[0]; t[1]=v[1]; t[2]=v[2]; t[3]=v[2]; t[4]=v[3]; t[5]=v[0];
        }
        break;
    }
    default: g_ld_stats.group_drop_primitive++; return;
    }

    if (!n_tri) {
        g_ld_stats.group_drop_degenerate++;
        if (tri != dc.verts) free(tri);
        return;
    }

    if (g.vb_used + n_tri * VERT_STRIDE > MAX_VERTS * VERT_STRIDE) {
        g_ld_stats.group_drop_ring++;
        if (tri != dc.verts) free(tri); return;
    }
    memcpy(g.vb_mapped + g.vb_used, tri, (size_t)n_tri * VERT_STRIDE);

    ID3D12PipelineState* pso = get_pso();
    if (!pso) {
        g_ld_stats.group_drop_pso++;
        if (tri != dc.verts) free(tri); return;   /* no fallback in live path */
    }
    if (g.cb_used + CB_BLOCK_ALIGNED > CB_RING_BYTES) {
        g_ld_stats.group_drop_ring++;
        if (tri != dc.verts) free(tri); return;   /* no fallback in live path */
    }

    const u32 target = current_surface();
    if (target == LD_INVALID_SURFACE) {
        g_ld_stats.group_drop_surface++;
        if (tri != dc.verts) free(tri);
        return;
    }
    if (rsx_live_draw_a010_probe_active() && target < 64)
        g_ld_a010_probe_touched |= 1ull << target;
    { static u32 last_target = LD_INVALID_SURFACE;
      if (target != last_target) { ld_trace_target("draw", target, 0); last_target = target; } }
    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    const u32 current_zslot = g.depth
        ? zdepth_get(sf.zeta_location, sf.zeta_offset, sf.clip_w, sf.clip_h)
        : 0;
    u32 slots[SRV_TABLE_SIZE], smp_slots[SMP_TABLE_SIZE];
    u32 surf_used[SRV_TABLE_SIZE], n_surf_used = 0;
    u32 zdepth_used[SRV_TABLE_SIZE], n_zdepth_used = 0;
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++) slots[u] = SRV_WHITE;
    for (u32 u = 0; u < SMP_TABLE_SIZE; u++) smp_slots[u] = SMP_DEFAULT;
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++) {
        rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
        if (!t.enabled) continue;
        smp_slots[u] = sampler_slot(&t, sampler_key(&t));
        int sampled = -1;
        for (u32 i = 0; i < g.n_surfaces; i++)
            if (g.surfaces[i].location == t.location && g.surfaces[i].offset == t.offset && i != target)
            { sampled = (int)i; break; }
        if (sampled >= 0) {
            slots[u] = SRV_SURFACE_BASE + sampled;
            int seen = 0;
            for (u32 k = 0; k < n_surf_used; k++) if (surf_used[k] == (u32)sampled) seen = 1;
            if (!seen && n_surf_used < SRV_TABLE_SIZE) surf_used[n_surf_used++] = (u32)sampled;
        } else {
            int sampled_depth = -1;
            const u32 texture_base_fmt =
                t.format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
            /* Mirror the replay-proven depth-RT contract: address and format
             * must match, and the pass must have executed a depth-writing draw.
             * Clear-only zetas intentionally fall through to guest VRAM. */
            if (texture_base_fmt == TEX_FMT_DEPTH24_D8)
                for (u32 i = 0; i < g.n_zdepths; i++)
                    if (g.zdepths[i].location == t.location &&
                        g.zdepths[i].offset == t.offset &&
                        current_zslot != 1 + i) {
                        if (g.zdepths[i].had_write)
                            sampled_depth = (int)i;
                        else
                            g_ld_zdepth_srv_reject_no_write++;
                        break;
                    }
            if (sampled_depth >= 0) {
                slots[u] = SRV_ZDEPTH_BASE + sampled_depth;
                g_ld_zdepth_srv_binds++;
                int seen = 0;
                for (u32 k = 0; k < n_zdepth_used; k++)
                    if (zdepth_used[k] == (u32)sampled_depth) seen = 1;
                if (!seen && n_zdepth_used < SRV_TABLE_SIZE)
                    zdepth_used[n_zdepth_used++] = (u32)sampled_depth;
            } else {
                slots[u] = texture_srv_slot(&t);
            }
        }
    }

    D3D12_RESOURCE_BARRIER bar = {0};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    for (u32 k = 0; k < n_surf_used; k++) {
        bar.Transition.pResource = g.surfaces[surf_used[k]].tex;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &bar);
    }
    for (u32 k = 0; k < n_zdepth_used; k++) {
        bar.Transition.pResource = g.zdepths[zdepth_used[k]].tex;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &bar);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(LD_SWAP_BUFFERS + target);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv; int have_dsv = 0;
    if (g.depth) {
        dsv = dsv_handle(current_zslot);
        have_dsv = 1;
        if (current_zslot) {
            zdepth_t* z = &g.zdepths[current_zslot - 1];
            if (!z->cleared) {
                g.list->lpVtbl->ClearDepthStencilView(
                    g.list, dsv,
                    D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                    1.0f, 0, 0, NULL);
                z->cleared = 1;
                z->had_write = 0;
                g_ld_stats.implicit_depth_clears++;
            }
        } else if (!g.depth_cleared) {
            g.list->lpVtbl->ClearDepthStencilView(g.list, dsv,
                D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
            g.depth_cleared = 1;
            g_ld_stats.implicit_depth_clears++;
        }
    }
    g.list->lpVtbl->OMSetRenderTargets(g.list, 1, &rtv, FALSE, have_dsv ? &dsv : NULL);
    ID3D12DescriptorHeap* heaps[] = {g.srv_heap, g.smp_heap};
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE table = srv_table(slots);
    const D3D12_GPU_DESCRIPTOR_HANDLE stbl = sampler_table(smp_slots);

    const float W = sf.clip_w ? (float)sf.clip_w : (float)g.width;
    const float H = sf.clip_h ? (float)sf.clip_h : (float)g.height;
    float xf[8] = {1, 1, 1, 0, 0, 0, 0, 0};
    if (vp.scale[0] != 0.0f || vp.translate[0] != 0.0f) {
        xf[0] = vp.scale[0] / (W * 0.5f);
        xf[1] = -(vp.scale[1] / (H * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.translate[0] - W * 0.5f) / (W * 0.5f);
        xf[5] = -((vp.translate[1] - H * 0.5f) / (H * 0.5f));
        xf[6] = vp.translate[2];
    }
    u8* cbdst = g.cb_mapped + g.cb_used;
    memcpy(cbdst, g.rsx.constants, RSX_DSP_NUM_CONSTANTS * 16);
    memcpy(cbdst + 512 * 16, xf, sizeof(xf));

    g.list->lpVtbl->SetPipelineState(g.list, pso);
    g.list->lpVtbl->SetGraphicsRootSignature(g.list, g.rootsig_x);
    g.list->lpVtbl->SetGraphicsRootConstantBufferView(
        g.list, 0, g.cb->lpVtbl->GetGPUVirtualAddress(g.cb) + g.cb_used);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 1, table);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 2, stbl);
    const u32 vtex_mask = vertex_texture_mask();
    if (vtex_mask) {
        u32 vtex_slots[SRV_TABLE_SIZE];
        for (u32 u = 0; u < SRV_TABLE_SIZE; u++)
            vtex_slots[u] = SRV_WHITE;
        for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
            if (!((vtex_mask >> u) & 1u)) continue;
            rsx_dsp_vertex_texture vt;
            rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
            vtex_slots[u] = vertex_texture_srv_slot(&vt);
            if (vtex_slots[u] != SRV_WHITE)
                g_ld_vtex_binds++;
            else
                g_ld_vtex_unsupported++;
        }
        const D3D12_GPU_DESCRIPTOR_HANDLE vtex_table =
            srv_table(vtex_slots);
        g.list->lpVtbl->SetGraphicsRootDescriptorTable(
            g.list, 3, vtex_table);
    }
    g.cb_used += CB_BLOCK_ALIGNED;

    D3D12_VIEWPORT dvp = {0, 0, W, H, 0.0f, 1.0f};
    D3D12_RECT sc = {
        0, 0,
        (LONG)(g.surfaces[target].w ? g.surfaces[target].w : g.width),
        (LONG)(g.surfaces[target].h ? g.surfaces[target].h : g.height)
    };
    g.list->lpVtbl->RSSetViewports(g.list, 1, &dvp);
    g.list->lpVtbl->RSSetScissorRects(g.list, 1, &sc);

    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation = g.vb->lpVtbl->GetGPUVirtualAddress(g.vb) + g.vb_used;
    vbv.StrideInBytes = VERT_STRIDE; vbv.SizeInBytes = n_tri * VERT_STRIDE;
    g.list->lpVtbl->IASetVertexBuffers(g.list, 0, 1, &vbv);
    g.list->lpVtbl->IASetPrimitiveTopology(g.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->lpVtbl->DrawInstanced(g.list, n_tri, 1, 0, 0);
    if (current_zslot) {
        render_state_t depth_state;
        decode_render_state(&depth_state);
        /* A write-enable bit alone does not prove that this draw produced a
         * usable depth map.  With depth testing disabled RSX does not execute
         * the depth pass represented by this tracked zeta. */
        if (depth_state.depth_test && depth_state.depth_write)
            g.zdepths[current_zslot - 1].had_write = 1;
    }
    g_ld_stats.groups_executed++;

    for (u32 k = 0; k < n_surf_used; k++) {
        bar.Transition.pResource = g.surfaces[surf_used[k]].tex;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &bar);
    }
    for (u32 k = 0; k < n_zdepth_used; k++) {
        bar.Transition.pResource = g.zdepths[zdepth_used[k]].tex;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &bar);
    }
    g.vb_used += n_tri * VERT_STRIDE;
    if (tri != dc.verts) free(tri);
}

static void sink_clear(void* user, const rsx_dispatch* r, u32 mask)
{
    (void)user; (void)r;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) return;
    g_ld_stats.clears++;
    const u32 target = current_surface();
    if (target == LD_INVALID_SURFACE) { g_ld_stats.clear_drop_surface++; return; }
    if (rsx_live_draw_a010_probe_active() && target < 64)
        g_ld_a010_probe_touched |= 1ull << target;
    if (ld_target_trace_enabled()) {
        static u32 last_clear_target = LD_INVALID_SURFACE;
        const int changed = target != last_clear_target;
        if (changed) {
            ld_trace_target("clear-target", target, mask);
            last_clear_target = target;
        }
        /* A broken scene can issue tens of thousands of clears. Preserve the
         * opening sequence and a periodic heartbeat without turning tracing
         * itself into a scheduler perturbation. Target changes are always
         * emitted above. */
        if (g_ld_stats.clears <= 256 || (g_ld_stats.clears & 1023) == 0) {
            const u32 z = rsx_dsp_clear_zstencil(&g.rsx);
            fprintf(stderr,
                    "[rsx-clear] frame=%u n=%llu target=%u mask=0x%02X "
                    "argb=0x%08X z24=0x%06X stencil=0x%02X\n",
                    g_ld_frames, g_ld_stats.clears, target, mask,
                    rsx_dsp_clear_color(&g.rsx), z >> 8, z & 0xFF);
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(LD_SWAP_BUFFERS + target);
    if (mask & (RSX_CLEAR_COLOR_R | RSX_CLEAR_COLOR_G | RSX_CLEAR_COLOR_B | RSX_CLEAR_COLOR_A)) {
        const u32 c = rsx_dsp_clear_color(&g.rsx);
        const float col[4] = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
                               (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f };
        g.list->lpVtbl->ClearRenderTargetView(g.list, rtv, col, 0, NULL);
    }
    if ((mask & (RSX_CLEAR_DEPTH | RSX_CLEAR_STENCIL)) && g.depth) {
        rsx_dsp_surface sf;
        rsx_dsp_get_surface(&g.rsx, &sf);
        const u32 zslot = zdepth_get(sf.zeta_location, sf.zeta_offset,
                                     sf.clip_w, sf.clip_h);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_handle(zslot);
        D3D12_CLEAR_FLAGS clear_flags = (D3D12_CLEAR_FLAGS)0;
        if (mask & RSX_CLEAR_DEPTH)
            clear_flags = (D3D12_CLEAR_FLAGS)(clear_flags | D3D12_CLEAR_FLAG_DEPTH);
        if (mask & RSX_CLEAR_STENCIL)
            clear_flags = (D3D12_CLEAR_FLAGS)(clear_flags | D3D12_CLEAR_FLAG_STENCIL);
        g.list->lpVtbl->ClearDepthStencilView(g.list, dsv,
            clear_flags, 1.0f, 0, 0, NULL);
        if (zslot) {
            g.zdepths[zslot - 1].cleared = 1;
            if (mask & RSX_CLEAR_DEPTH)
                g.zdepths[zslot - 1].had_write = 0;
        } else {
            if (mask & RSX_CLEAR_DEPTH)
                g.depth_cleared = 1;
        }
    }
}

static void sink_flip(void* user, const rsx_dispatch* r, u32 arg)
{
    (void)user; (void)r;
    if (g_ld_movie_mode) {
        if (ld_movie_composite_ui_enabled()) ld_movie_capture_overlay();
        return;
    }
    rsx_live_draw_present(arg & 7);
}

/* ---------------------------------------------------------------------------
 * device / resource setup
 * -----------------------------------------------------------------------*/
static int make_root_signature(void)
{
    D3D12_DESCRIPTOR_RANGE xrange = {0};
    xrange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    xrange.NumDescriptors = SRV_TABLE_SIZE;
    D3D12_DESCRIPTOR_RANGE srange = {0};
    srange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    srange.NumDescriptors = SMP_TABLE_SIZE;
    D3D12_DESCRIPTOR_RANGE vrange = {0};
    vrange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    vrange.NumDescriptors = RSX_DSP_NUM_VERTEX_TEXTURES;
    vrange.BaseShaderRegister = 16;
    D3D12_ROOT_PARAMETER xp[4] = {0};
    xp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    xp[0].Descriptor.ShaderRegister = 0;
    xp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    xp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    xp[1].DescriptorTable.NumDescriptorRanges = 1;
    xp[1].DescriptorTable.pDescriptorRanges = &xrange;
    xp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    xp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    xp[2].DescriptorTable.NumDescriptorRanges = 1;
    xp[2].DescriptorTable.pDescriptorRanges = &srange;
    xp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    xp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    xp[3].DescriptorTable.NumDescriptorRanges = 1;
    xp[3].DescriptorTable.pDescriptorRanges = &vrange;
    xp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_STATIC_SAMPLER_DESC vsmp[RSX_DSP_NUM_VERTEX_TEXTURES] = {0};
    for (u32 i = 0; i < RSX_DSP_NUM_VERTEX_TEXTURES; i++) {
        vsmp[i].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        vsmp[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        vsmp[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        vsmp[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        vsmp[i].MaxLOD = D3D12_FLOAT32_MAX;
        vsmp[i].ShaderRegister = i;
        vsmp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd = {0};
    rsd.NumParameters = 4; rsd.pParameters = xp;
    rsd.NumStaticSamplers = RSX_DSP_NUM_VERTEX_TEXTURES;
    rsd.pStaticSamplers = vsmp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        if (err) err->lpVtbl->Release(err); return -1;
    }
    HRESULT hr = g.dev->lpVtbl->CreateRootSignature(g.dev, 0, sig->lpVtbl->GetBufferPointer(sig),
                                                    sig->lpVtbl->GetBufferSize(sig),
                                                    &IID_ID3D12RootSignature, (void**)&g.rootsig_x);
    sig->lpVtbl->Release(sig);
    return SUCCEEDED(hr) ? 0 : -1;
}

int rsx_live_draw_init(void* hwnd, u32 width, u32 height,
                       rsx_live_guest_ptr_fn guest_fn, void* guest_user)
{
    if (!rsx_live_draw_enabled()) return 0;
    if (g.ready) return 0;
    g.width = width; g.height = height;
    g.guest_ptr = guest_fn; g.guest_user = guest_user;

    IDXGIFactory4* factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory))) return -1;
    if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&g.dev))) {
        factory->lpVtbl->Release(factory); return -1;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {0};
    g.dev->lpVtbl->CreateCommandQueue(g.dev, &qd, &IID_ID3D12CommandQueue, (void**)&g.queue);
    g.dev->lpVtbl->CreateCommandAllocator(g.dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          &IID_ID3D12CommandAllocator, (void**)&g.alloc);
    g.dev->lpVtbl->CreateCommandList(g.dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, NULL,
                                     &IID_ID3D12GraphicsCommandList, (void**)&g.list);
    g.dev->lpVtbl->CreateFence(g.dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&g.fence);
    g.fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    /* swap chain bound to the runtime's HWND */
    DXGI_SWAP_CHAIN_DESC1 scd = {0};
    scd.Width = width; scd.Height = height; scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1; scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = LD_SWAP_BUFFERS; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = NULL;
    if (FAILED(factory->lpVtbl->CreateSwapChainForHwnd(factory, (IUnknown*)g.queue,
            (HWND)hwnd, &scd, NULL, NULL, &sc1))) {
        factory->lpVtbl->Release(factory); return -1;
    }
    sc1->lpVtbl->QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&g.swap);
    sc1->lpVtbl->Release(sc1);
    factory->lpVtbl->Release(factory);

    /* RTV heap: [0..1] backbuffers, [2..] surface cache */
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = LD_SWAP_BUFFERS + MAX_SURFACES;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g.rtv_heap);
    g.rtv_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (u32 i = 0; i < LD_SWAP_BUFFERS; i++) {
        g.swap->lpVtbl->GetBuffer(g.swap, i, &IID_ID3D12Resource, (void**)&g.backbuf[i]);
        g.dev->lpVtbl->CreateRenderTargetView(g.dev, g.backbuf[i], NULL, rtv_handle(i));
    }

    /* upload arena */
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = UPLOAD_SIZE;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&g.upload);
    D3D12_RANGE rr = {0, 0};
    g.upload->lpVtbl->Map(g.upload, 0, &rr, (void**)&g.upload_mapped);

    bd.Width = MAX_VERTS * VERT_STRIDE;
    g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&g.vb);
    g.vb->lpVtbl->Map(g.vb, 0, &rr, (void**)&g.vb_mapped);

    bd.Width = CB_RING_BYTES;
    g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&g.cb);
    g.cb->lpVtbl->Map(g.cb, 0, &rr, (void**)&g.cb_mapped);

    /* SRV heaps */
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = SRV_HEAP_SLOTS; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g.srv_cpu_heap);
    hd.NumDescriptors = SRV_RING_TABLES * SRV_TABLE_SIZE;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g.srv_heap);
    g.srv_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    /* sampler heaps */
    D3D12_DESCRIPTOR_HEAP_DESC shd = {0};
    shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    shd.NumDescriptors = SMP_CACHE_SLOTS + 1; shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &shd, &IID_ID3D12DescriptorHeap, (void**)&g.smp_cpu_heap);
    shd.NumDescriptors = SMP_RING_TABLES * SMP_TABLE_SIZE;
    shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &shd, &IID_ID3D12DescriptorHeap, (void**)&g.smp_heap);
    g.smp_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    /* Fail init (fall back to null present) rather than crash later if any
     * shader-visible/CPU descriptor heap didn't create -- e.g. an over-limit
     * sampler heap would return NULL here and fault in sampler_table. */
    if (!g.srv_cpu_heap || !g.srv_heap || !g.smp_cpu_heap || !g.smp_heap) {
        fprintf(stderr, "[live-draw] descriptor heap alloc failed "
                "(srv_cpu=%p srv=%p smp_cpu=%p smp=%p)\n",
                (void*)g.srv_cpu_heap, (void*)g.srv_heap,
                (void*)g.smp_cpu_heap, (void*)g.smp_heap);
        return -1;
    }
    {
        D3D12_SAMPLER_DESC def = {0};
        def.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        def.AddressU = def.AddressV = def.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        def.MaxLOD = D3D12_FLOAT32_MAX; def.MaxAnisotropy = 1;
        g.dev->lpVtbl->CreateSampler(g.dev, &def, smp_cpu(SMP_DEFAULT));
    }

    /* depth */
    D3D12_DESCRIPTOR_HEAP_DESC dhd = {0};
    dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dhd.NumDescriptors = 1 + MAX_SURFACES;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &dhd, &IID_ID3D12DescriptorHeap, (void**)&g.dsv_heap);
    g.dsv_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(
        g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    {
        D3D12_HEAP_PROPERTIES dhp = {0}; dhp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC drd = {0};
        drd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        drd.Width = width; drd.Height = height; drd.DepthOrArraySize = 1;
        drd.MipLevels = 1; drd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; drd.SampleDesc.Count = 1;
        drd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE dcv = {0}; dcv.Format = drd.Format; dcv.DepthStencil.Depth = 1.0f;
        if (SUCCEEDED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &dhp, D3D12_HEAP_FLAG_NONE, &drd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv, &IID_ID3D12Resource, (void**)&g.depth))) {
            D3D12_CPU_DESCRIPTOR_HANDLE dh;
            g.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.dsv_heap, &dh);
            g.dev->lpVtbl->CreateDepthStencilView(g.dev, g.depth, NULL, dh);
        } else {
            g.depth = NULL;
        }
    }

    if (make_root_signature() != 0) return -1;

    static const u8 white[4] = {255, 255, 255, 255};
    g.white_tex = create_texture_rgba(white, 1, 1);
    if (g.white_tex) srv_write(SRV_WHITE, g.white_tex);

    /* dispatcher + sink */
    rsx_dispatch_sink sink = {0};
    sink.user = &g;
    sink.clear = sink_clear;
    sink.begin = sink_begin;
    sink.end = sink_end;
    sink.draw_arrays = sink_draw_arrays;
    sink.draw_index_array = sink_draw_index;
    sink.flip = sink_flip;
    rsx_dispatch_init(&g.rsx, &sink);

    g.ready = 1;
    g.enabled = 1;
    return 0;
}

void rsx_live_draw_seed_registers(const u32* regs, u32 count)
{
    if (g.ready) rsx_dispatch_seed_registers(&g.rsx, regs, count);
}
void rsx_live_draw_seed_transform_program(const u32* words, u32 count)
{
    if (g.ready) rsx_dispatch_seed_transform_program(&g.rsx, words, count);
}

void rsx_live_draw_method(u32 method, u32 arg)
{
    const int composite = ld_movie_composite_ui_enabled();
    if (composite) {
        for (;;) {
            if (g_ld_movie_mode || g_ld_host_waiting) {
                while (g_ld_host_waiting)
                    SwitchToThread();
                AcquireSRWLockExclusive(&g_ld_access_lock);
                if (!g.ready) {
                    ReleaseSRWLockExclusive(&g_ld_access_lock);
                    return;
                }
                rsx_dispatch_method(&g.rsx, method, arg);
                ReleaseSRWLockExclusive(&g_ld_access_lock);
                return;
            }
            InterlockedIncrement(&g_ld_guest_active);
            MemoryBarrier();
            if (!g_ld_movie_mode && !g_ld_host_waiting) {
                if (g.ready)
                    rsx_dispatch_method(&g.rsx, method, arg);
                InterlockedDecrement(&g_ld_guest_active);
                return;
            }
            InterlockedDecrement(&g_ld_guest_active);
        }
    }

    if (!g.ready) return;
    if (g_ld_movie_mode) {
        if (g_ld_movie_track_rsx < 0)
            g_ld_movie_track_rsx = getenv("YZ_MOVIE_TRACK_RSX") ? 1 : 0;
        if (!g_ld_movie_track_rsx) return;
    }
    rsx_dispatch_method(&g.rsx, method, arg);
}

void rsx_live_draw_set_movie_mode(int on)
{
    static unsigned long long suppressed_at_start = 0;
    const int composite = ld_movie_composite_ui_enabled();
    if (on && rsx_live_draw_a010_probe_active()) {
        fprintf(stderr,
                "[a010-probe] END movie-mode live_frame=%u elapsed_frames=%u "
                "surfaces=%u samples=%u touched=0x%016llX\n",
                g_ld_frames, g_ld_frames - g_ld_a010_probe_start_frame,
                g.n_surfaces, g_ld_a010_probe_sample,
                (unsigned long long)g_ld_a010_probe_touched);
        fflush(stderr);
        InterlockedExchange(&g_ld_a010_probe_active, 0);
    }
    if (composite) {
        InterlockedExchange(&g_ld_host_waiting, 1);
        if (on) InterlockedExchange((volatile LONG*)&g_ld_movie_mode, 1);
        while (InterlockedCompareExchange(&g_ld_guest_active, 0, 0) != 0)
            SwitchToThread();
        AcquireSRWLockExclusive(&g_ld_access_lock);
    }
    if (on) {
        suppressed_at_start = g_ld_stats.packets_movie;
        if (composite) ld_movie_overlay_begin();
        else g_ld_movie_mode = 1;
    } else {
        if (composite) {
            g.movie_overlay_valid = 0;
            /* Do not expose a partially rendered auth/fade surface between
             * the last host movie frame and the next clean guest scene. */
            ld_flush();
            const float black[4] = {0, 0, 0, 1};
            for (u32 i = 0; i < g.n_surfaces; i++)
                g.list->lpVtbl->ClearRenderTargetView(
                    g.list, rtv_handle(LD_SWAP_BUFFERS + i), black, 0, NULL);
            if (g.n_surfaces) ld_flush();
            ld_movie_reset_rings();
            InterlockedExchange((volatile LONG*)&g_ld_movie_mode, 0);
            fprintf(stderr, "[movie-ui] compositor disarmed after %llu guest overlays\n",
                    (unsigned long long)g.movie_overlay_frames);
            fflush(stderr);
        } else {
            g_ld_movie_mode = 0;
        }
        if (!composite && g_ld_movie_track_rsx > 0) {
            fprintf(stderr,
                    "[live-draw] movie handoff: tracked RSX state, suppressed %llu guest draw packets\n",
                    g_ld_stats.packets_movie - suppressed_at_start);
            fflush(stderr);
        }
    }
    if (composite) {
        ReleaseSRWLockExclusive(&g_ld_access_lock);
        InterlockedExchange(&g_ld_host_waiting, 0);
    }
}

u32 rsx_live_draw_get_frames(void) { return g_ld_frames; }

static u32 g_ld_last_frame_draws = 0;
/* Draws in the last COMPLETED frame (title-bar telemetry: distinguishes
 * "presenting fresh content" from "flipping a static image" -- the dead
 * journal-consumer limp state renders ~0 draws/frame while flips tick). */
u32 rsx_live_draw_get_last_draws(void) { return g_ld_last_frame_draws; }

/* Present a host-decoded RGBA8 frame to the window: copy it straight into the
 * swap-chain backbuffer (both R8G8B8A8_UNORM at the swap size) and Present.
 * The frame is clamped to the backbuffer size. Call from a single thread with
 * movie mode on (so guest draws don't touch g.list). */
void rsx_live_draw_present_rgba(const uint8_t* rgba, u32 w, u32 h)
{
    const int composite = ld_movie_composite_ui_enabled();
    if (composite) {
        InterlockedExchange(&g_ld_host_waiting, 1);
        AcquireSRWLockExclusive(&g_ld_access_lock);
    }
    if (!g.ready || !rgba) {
        if (composite) {
            ReleaseSRWLockExclusive(&g_ld_access_lock);
            InterlockedExchange(&g_ld_host_waiting, 0);
        }
        return;
    }
    if (composite && !ld_movie_overlay_ensure()) {
        ReleaseSRWLockExclusive(&g_ld_access_lock);
        InterlockedExchange(&g_ld_host_waiting, 0);
        return;
    }
    if (w > g.width)  w = g.width;
    if (h > g.height) h = g.height;
    const u32 pitch = (w * 4 + 255) & ~255u;          /* D3D12 copy pitch align */
    if ((UINT64)pitch * h > UPLOAD_SIZE) {
        if (composite) {
            ReleaseSRWLockExclusive(&g_ld_access_lock);
            InterlockedExchange(&g_ld_host_waiting, 0);
        }
        return;
    }

    u8* host_upload = composite ? g.movie_upload_mapped : g.upload_mapped;
    for (u32 y = 0; y < h; y++) {
        u8* dstrow = host_upload + (size_t)y * pitch;
        memcpy(dstrow, rgba + (size_t)y * w * 4, (size_t)w * 4);
        if (composite && g.movie_overlay_valid && g.movie_overlay_rgba &&
            g.movie_overlay_mask) {
            const u8* ov = g.movie_overlay_rgba + (size_t)y * g.width * 4;
            const u8* mask =
                g.movie_overlay_mask + (size_t)y * g.width;
            for (u32 x = 0; x < w; x++, ov += 4) {
                const int coverage = mask[x];
                if (!coverage) continue;
                u8* out = dstrow + (size_t)x * 4;
                if (coverage == 1) {
                    out[0] = (u8)((out[0] * 64 + 127) / 255);
                    out[1] = (u8)((out[1] * 64 + 127) / 255);
                    out[2] = (u8)((out[2] * 64 + 127) / 255);
                    continue;
                }
                const int white = ov[0] > ov[1]
                    ? (ov[0] > ov[2] ? ov[0] : ov[2])
                    : (ov[1] > ov[2] ? ov[1] : ov[2]);
                for (int c = 0; c < 3; c++) {
                    const int v =
                        (white * coverage +
                         out[c] * (255 - coverage) + 127) / 255;
                    out[c] = (u8)v;
                }
                out[3] = 255;
            }
        }
    }

    const u32 bbi = g.swap->lpVtbl->GetCurrentBackBufferIndex(g.swap);
    ID3D12Resource* bb = g.backbuf[bbi];

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = bb; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
    src.pResource = composite ? g.movie_upload : g.upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = w;
    src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    ld_flush();
    g.swap->lpVtbl->Present(g.swap, 1, 0);
    if (composite) {
        ReleaseSRWLockExclusive(&g_ld_access_lock);
        InterlockedExchange(&g_ld_host_waiting, 0);
    }
}

/* Env-gated (YZ_RSX_DUMP) framebuffer dump: read the current color surface back
 * and write a binary PPM. Self-contained -- creates + releases its own readback
 * buffer, so no init/struct changes. Uses g.list which ld_flush leaves open. */
static void ld_dump_surface_ppm(const char* path, ID3D12Resource* rt)
{
    if (!rt) return;
    const u32 pitch = (g.width * 4 + 255) & ~255u;              /* 256-align */
    const UINT64 rb_size = (UINT64)pitch * g.height;

    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = rb_size;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&rb)))
        return;

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = g.width;
    dst.PlacedFootprint.Footprint.Height = g.height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    ld_flush();                                    /* copy completes on the GPU */

    u8* px = NULL; D3D12_RANGE rr = {0, (SIZE_T)rb_size};
    if (SUCCEEDED(rb->lpVtbl->Map(rb, 0, &rr, (void**)&px))) {
        FILE* f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", g.width, g.height);
            for (u32 y = 0; y < g.height; y++) {
                const u8* row = px + (SIZE_T)y * pitch;
                for (u32 x = 0; x < g.width; x++) fwrite(row + x * 4, 1, 3, f);  /* RGBA->RGB */
            }
            fclose(f);
            fprintf(stderr, "[live-draw] wrote %s\n", path);
        }
        D3D12_RANGE wr = {0, 0};
        rb->lpVtbl->Unmap(rb, 0, &wr);
    }
    rb->lpVtbl->Release(rb);
}

void rsx_live_draw_present(u32 buffer_id)
{
    if (!g.ready) return;
    (void)buffer_id;
    /* transition the presented surface -> backbuffer copy -> present.
     * The current color target holds this frame's composite; copy it into the
     * swap-chain backbuffer and present. */
    const u32 target = current_surface();
    if (target == LD_INVALID_SURFACE) {
        fprintf(stderr, "[live-draw] frame present skipped: no color surface\n");
        return;
    }
    { static u32 last_present_target = LD_INVALID_SURFACE;
      if (target != last_present_target) {
          ld_trace_target("present", target, buffer_id);
          last_present_target = target;
      } }
    ID3D12Resource* srcimg = g.surfaces[target].tex;
    const u32 bbi = g.swap->lpVtbl->GetCurrentBackBufferIndex(g.swap);
    ID3D12Resource* bb = g.backbuf[bbi];

    D3D12_RESOURCE_BARRIER bar[2] = {0};
    bar[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[0].Transition.pResource = srcimg;
    bar[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[1].Transition.pResource = bb;
    bar[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g.list->lpVtbl->ResourceBarrier(g.list, 2, bar);

    g.list->lpVtbl->CopyResource(g.list, bb, srcimg);

    bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g.list->lpVtbl->ResourceBarrier(g.list, 2, bar);

    ld_flush();
    g.swap->lpVtbl->Present(g.swap, 1, 0);

    { static unsigned long long packets_at_last_frame = 0;
      g_ld_last_frame_draws = (u32)(g_ld_stats.packets_seen - packets_at_last_frame);
      packets_at_last_frame = g_ld_stats.packets_seen; }
    g_ld_frames++;
    /* First 32 frames verbatim, then every 32nd: keeps the log bounded while
     * making the TRUE frame count measurable from the log. (The old hard cap
     * at 32 made "stalls at frame ~32" unfalsifiable from the .err alone.) */
    if (g_ld_frames <= 32 || (g_ld_frames & 31) == 0)
        fprintf(stderr,
                "[live-draw] frame %u packets[seen=%llu queued=%llu movie=%llu qfull=%llu] "
                "groups[seen=%llu exec=%llu empty=%llu drop{fetch=%llu degen=%llu prim=%llu "
                "alloc=%llu pso=%llu ring=%llu surface=%llu}] clears[guest=%llu badsurf=%llu implicitZ=%llu] "
                "textures[cached=%u/%u full=%llu decodefail=%llu depthSRV=%llu rejectZ=%llu] "
                "vtex[cached=%u enabled=%llu binds=%llu uploads=%llu refresh=%llu "
                "unsupported=%llu missingTXL=%llu] divider=%llu "
                "balance[p=%s g=%s] (cumulative)\n",
                g_ld_frames,
                g_ld_stats.packets_seen, g_ld_stats.packets_queued,
                g_ld_stats.packets_movie, g_ld_stats.packets_queue_full,
                g_ld_stats.groups_seen, g_ld_stats.groups_executed,
                g_ld_stats.groups_empty, g_ld_stats.group_drop_fetch,
                g_ld_stats.group_drop_degenerate, g_ld_stats.group_drop_primitive,
                g_ld_stats.group_drop_alloc, g_ld_stats.group_drop_pso,
                g_ld_stats.group_drop_ring, g_ld_stats.group_drop_surface,
                g_ld_stats.clears, g_ld_stats.clear_drop_surface,
                g_ld_stats.implicit_depth_clears,
                g.n_textures, MAX_TEXTURES,
                (unsigned long long)g_ld_texture_cache_full,
                (unsigned long long)g_ld_texture_decode_fail,
                (unsigned long long)g_ld_zdepth_srv_binds,
                (unsigned long long)g_ld_zdepth_srv_reject_no_write,
                g.n_vtex,
                (unsigned long long)g_ld_vtex_enabled,
                (unsigned long long)g_ld_vtex_binds,
                (unsigned long long)g_ld_vtex_uploads,
                (unsigned long long)g_ld_vtex_refreshes,
                (unsigned long long)g_ld_vtex_unsupported,
                (unsigned long long)g_ld_vtex_missing_for_txl,
                (unsigned long long)g_ld_divider_fetches,
                g_ld_stats.packets_seen == g_ld_stats.packets_queued +
                    g_ld_stats.packets_movie + g_ld_stats.packets_queue_full ? "ok" : "BAD",
                g_ld_stats.groups_seen == ld_groups_accounted() ? "ok" : "BAD");
    /* [fps] heartbeat: direct frame-rate logging, one line per ~5s wall.
     * Exists because pace repeatedly had to be inferred from frame-counter
     * arithmetic and log ordering, and two such inferences were wrong in one
     * night (s42). Volume-bounded per LESSONS #6c. */
    { static ULONGLONG fps_t0 = 0; static u32 fps_f0 = 0;
      ULONGLONG now = GetTickCount64();
      if (fps_t0 == 0) { fps_t0 = now; fps_f0 = g_ld_frames; }
      else if (now - fps_t0 >= 5000) {
          fprintf(stderr, "[fps] %.1f (frames %u..%u over %.1fs)\n",
                  (g_ld_frames - fps_f0) * 1000.0 / (double)(now - fps_t0),
                  fps_f0, g_ld_frames, (now - fps_t0) / 1000.0);
          fps_t0 = now; fps_f0 = g_ld_frames;
      } }
    if (getenv("YZ_RSX_DUMP") && g_ld_frames <= 8) {
        /* Dump the presented color surface (RENDER_TARGET state -> safe). */
        const u32 cur = current_surface();
        if (cur != LD_INVALID_SURFACE) {
            char path[256];
            snprintf(path, sizeof(path), "scratch\\ld_frame_%02u.ppm", g_ld_frames);
            ld_dump_surface_ppm(path, g.surfaces[cur].tex);
        }
    }
    if (rsx_live_draw_a010_probe_active()) {
        const u32 elapsed = g_ld_frames - g_ld_a010_probe_start_frame;
        /* Loading consumes roughly 150 fast flips before a010. Sampling every
         * 16 frames spans the load and complete AUTH window while keeping the
         * synchronous readbacks from becoming the scene's clock. */
        if ((elapsed & 15u) == 0) {
            u64 mask = g_ld_a010_probe_touched;
            const u32 cur = current_surface();
            if (cur < 64) mask |= 1ull << cur;
            fprintf(stderr,
                    "[a010-probe] SAMPLE n=%u live_frame=%u elapsed=%u "
                    "present=%u buffer=%u touched=0x%016llX surfaces=%u\n",
                    g_ld_a010_probe_sample, g_ld_frames, elapsed, cur,
                    buffer_id, (unsigned long long)mask, g.n_surfaces);
            for (u32 i = 0; i < g.n_surfaces && i < 64; i++) {
                if (!(mask & (1ull << i))) continue;
                char path[256];
                snprintf(path, sizeof(path),
                         "scratch\\a010_probe\\sample_%03u_frame_%05u_surface_%02u.ppm",
                         g_ld_a010_probe_sample, g_ld_frames, i);
                ld_dump_surface_ppm(path, g.surfaces[i].tex);
                fprintf(stderr,
                        "[a010-probe] SURFACE sample=%u index=%u location=%u "
                        "offset=0x%08X role=%s\n",
                        g_ld_a010_probe_sample, i, g.surfaces[i].location,
                        g.surfaces[i].offset, i == cur ? "present" : "offscreen");
            }
            g_ld_a010_probe_touched = 0;
            g_ld_a010_probe_sample++;
            fflush(stderr);
        }
        if (elapsed >= 640) {
            fprintf(stderr,
                    "[a010-probe] END frame-cap live_frame=%u samples=%u\n",
                    g_ld_frames, g_ld_a010_probe_sample);
            fflush(stderr);
            InterlockedExchange(&g_ld_a010_probe_active, 0);
        }
    }

    /* new frame: reset per-frame ring cursors */
    g.vb_used = 0; g.cb_used = 0;
    g.srv_ring_used = 0; g.smp_ring_used = 0;
    g.depth_cleared = 0;
    for (u32 i = 0; i < g.n_zdepths; i++)
        g.zdepths[i].cleared = 0;
}

void rsx_live_draw_shutdown(void)
{
    if (!g.ready) return;
    /* let the GPU drain, then release. (Best-effort; process teardown also
     * reclaims.) */
    ld_flush();
    for (u32 i = 0; i < g.n_psos; i++) if (g.psos[i].pso) g.psos[i].pso->lpVtbl->Release(g.psos[i].pso);
    for (u32 i = 0; i < g.n_textures; i++) if (g.textures[i].tex) g.textures[i].tex->lpVtbl->Release(g.textures[i].tex);
    for (u32 i = 0; i < g.n_vtex; i++) if (g.vtex[i].tex) g.vtex[i].tex->lpVtbl->Release(g.vtex[i].tex);
    for (u32 i = 0; i < g.n_surfaces; i++) if (g.surfaces[i].tex) g.surfaces[i].tex->lpVtbl->Release(g.surfaces[i].tex);
    for (u32 i = 0; i < g.n_zdepths; i++) if (g.zdepths[i].tex) g.zdepths[i].tex->lpVtbl->Release(g.zdepths[i].tex);
    if (g.movie_upload) g.movie_upload->lpVtbl->Release(g.movie_upload);
    if (g.movie_overlay_readback) g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
    if (g.movie_overlay_rgba) free(g.movie_overlay_rgba);
    if (g.movie_overlay_mask) free(g.movie_overlay_mask);
    if (g.white_tex) g.white_tex->lpVtbl->Release(g.white_tex);
    if (g.depth) g.depth->lpVtbl->Release(g.depth);
    if (g.rootsig_x) g.rootsig_x->lpVtbl->Release(g.rootsig_x);
    if (g.swap) g.swap->lpVtbl->Release(g.swap);
    if (g.dev) g.dev->lpVtbl->Release(g.dev);
    memset(&g, 0, sizeof(g));
    if (dc.verts) { free(dc.verts); dc.verts = NULL; dc.cap_verts = 0; }
    if (dc.cuts) { free(dc.cuts); dc.cuts = NULL; dc.cap_cuts = 0; }
}

#endif /* _WIN32 */
