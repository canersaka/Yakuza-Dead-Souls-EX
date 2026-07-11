/*
 * ps3recomp - Track B capture-replay harness
 *
 * Feeds an exported .rxs replay stream (tools/rrc_export.py) through the
 * clean-room NV4097 dispatcher (../rsx_dispatch.c) into an OFFSCREEN D3D12
 * renderer (WARP by default, so it runs headless on any machine), reads the
 * render target back, and writes one .ppm per flip plus a method-coverage
 * report (names from the exporter's .names.txt sidecar).
 *
 * Rendering scope (staged, matches SUMMARY.md Track B):
 *   stage 1: CLEAR_BUFFERS with the capture's clear color
 *   stage 2: vertex-color geometry — position/color fetched from the
 *            capture's guest-memory blocks at draw time (BE -> LE), quads
 *            and fans expanded to triangle lists on the CPU. No vertex
 *            program execution yet: positions pass through the RSX viewport
 *            scale/translate INVERSE only when they look like clip-space
 *            output is unavailable; see fetch_draw() comments.
 *   stage 3: texturing on unit 0. When the bound texture's (location,
 *            offset) matches a surface this replay already rendered, the
 *            surface's RT is bound as the SRV (render-to-texture chains,
 *            incl. the final composite draw); otherwise the texture is
 *            decoded from guest memory (linear + swizzled A8R8G8B8, B8;
 *            anything else falls back to a 1x1 white). The pixel shader
 *            modulates the sample by the vertex color (diffuse defaults
 *            to white), matching the capture's no-blend composite draw.
 *   stage 4: NV40 shader translation. The active transform program (from
 *            VP_START_FROM_ID) and fragment program (guest memory at
 *            SHADER_PROGRAM) are decompiled to HLSL (../rsx_vp_decompiler.c,
 *            ../rsx_fp_decompiler.c), D3DCompiled and cached as PSOs keyed
 *            on the combined ucode hash. Vertices carry all 16 attributes
 *            (CPU-fetched, disabled attrs read their VTX_ATTR_4F default),
 *            512 transform constants + the RSX viewport transform ride a
 *            per-draw CBV ring, and all 16 texture units bind through a
 *            16-descriptor SRV table ring. Untranslatable pairs fall back
 *            to the stage-3 fixed pipeline. Still ignored: blending, depth
 *            test, per-unit samplers, VP condition codes / flow control.
 *
 * Build (see build_replay.cmd):
 *   cl /std:c17 /O2 /I..\..\..\include replay_main.c ..\rsx_dispatch.c
 *      /link d3d12.lib dxgi.lib d3dcompiler.lib
 *
 * Usage:
 *   replay.exe <stream.rxs> [--hw] [--out <dir>]
 */

#ifndef _WIN32
#error Track B replay harness is Windows-only (D3D12)
#endif

#define _CRT_SECURE_NO_WARNINGS

#include "../rsx_dispatch.h"
#include "../rsx_fp_decompiler.h"
#include "../rsx_vp_decompiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>   /* before the D3D headers so the IIDs get defined */
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* ---------------------------------------------------------------------------
 * .rxs stream container (see tools/rrc_export.py)
 * -----------------------------------------------------------------------*/

typedef struct {
    u32 location, offset, size, data_off;
} rxs_block;

typedef struct {
    u32 w, h, pitch, offset;
} rxs_display_buf;

typedef struct {
    u32  n_blocks, n_records, reg_words, vp_words, disp_w, disp_h;
    u32  disp_count;
    rxs_display_buf disp[8];
    u32* regs;
    u32* vp;
    rxs_block* blocks;
    u8*  data;      /* concatenated block payloads */
    u32* records;   /* n_records * 2 words         */
} rxs_stream;

static int rxs_load(const char* path, rxs_stream* s)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return -1; }

    u32 hdr[8];
    if (fread(hdr, 4, 8, f) != 8 || memcmp(hdr, "RXS1", 4) != 0 || hdr[1] != 2) {
        printf("%s: not an RXS1 v2 stream (re-run tools/rrc_export.py)\n", path);
        fclose(f);
        return -1;
    }
    s->n_blocks  = hdr[2];
    s->n_records = hdr[3];
    s->reg_words = hdr[4];
    s->vp_words  = hdr[5];
    s->disp_w    = hdr[6];
    s->disp_h    = hdr[7];
    if (fread(&s->disp_count, 4, 1, f) != 1 ||
        fread(s->disp, sizeof(rxs_display_buf), 8, f) != 8) {
        printf("%s: truncated display table\n", path);
        fclose(f);
        return -1;
    }

    s->regs   = malloc((size_t)s->reg_words * 4);
    s->vp     = malloc((size_t)s->vp_words * 4);
    s->blocks = malloc((size_t)s->n_blocks * sizeof(rxs_block));
    if (fread(s->regs, 4, s->reg_words, f) != s->reg_words ||
        fread(s->vp, 4, s->vp_words, f) != s->vp_words ||
        fread(s->blocks, sizeof(rxs_block), s->n_blocks, f) != s->n_blocks) {
        printf("%s: truncated header sections\n", path);
        fclose(f);
        return -1;
    }
    size_t data_size = 0;
    for (u32 i = 0; i < s->n_blocks; i++) {
        size_t end = (size_t)s->blocks[i].data_off + s->blocks[i].size;
        if (end > data_size) data_size = end;
    }
    s->data    = malloc(data_size ? data_size : 1);
    s->records = malloc((size_t)s->n_records * 8);
    if (fread(s->data, 1, data_size, f) != data_size ||
        fread(s->records, 8, s->n_records, f) != s->n_records) {
        printf("%s: truncated data/records\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Guest memory arenas (local VRAM + main IO), populated by apply-block records
 * -----------------------------------------------------------------------*/

#define ARENA_SIZE (256u * 1024 * 1024)

static u8* g_arena[2]; /* [RSX_LOCATION_LOCAL], [RSX_LOCATION_MAIN] */

static const u8* guest_ptr(u32 location, u32 offset)
{
    if (location > 1 || offset >= ARENA_SIZE)
        return NULL;
    return g_arena[location] + offset;
}

static void apply_block(const rxs_stream* s, u32 index)
{
    if (index >= s->n_blocks)
        return;
    const rxs_block* b = &s->blocks[index];
    if (b->location > 1 || (u64)b->offset + b->size > ARENA_SIZE)
        return;
    memcpy(g_arena[b->location] + b->offset, s->data + b->data_off, b->size);
}

/* ---------------------------------------------------------------------------
 * Offscreen D3D12 renderer (WARP by default)
 * -----------------------------------------------------------------------*/

/* Stage 4: every vertex carries all 16 RSX attributes as float4 (the CPU
 * fetch converts each attribute's declared type); the translated vertex
 * program reads them as ATTR0..ATTR15, the fallback shaders read ATTR0
 * (position), ATTR3 (diffuse) and ATTR8 (texcoord0) out of the same layout. */
#define MAX_VERTS   (256 * 1024)
#define VERT_STRIDE (16 * 16)   /* 16 attributes x float4 */

/* The capture renders through many offscreen surfaces (render-to-texture
 * passes) before one final composite draw into the scanout buffer. Model
 * each distinct (location, color0 offset) as its own render target; the
 * flip picks the display buffer's surface for readback. */
#define MAX_SURFACES 64

/* Uploaded-texture cache (guest textures decoded once, keyed on the
 * descriptor; block re-applies over texture memory are NOT tracked).
 * One capture frame references well over the old 128-entry cap; once the
 * cache filled, every further distinct texture fell back to the white slot
 * (visible as flat-white UI panels and untextured composites). Sized to
 * hold a whole frame's texture set. The cache heap is CPU-side (non
 * shader-visible) so its descriptor count is unconstrained. */
#define MAX_TEXTURES 4096
#define UPLOAD_SIZE  (64u * 1024 * 1024)

/* CPU-side SRV cache heap layout: [0] 1x1 white fallback, [1..] surfaces,
 * then uploaded guest textures. Draws copy from here into a shader-visible
 * ring of 16-descriptor tables (one table per draw, t0..t15). */
#define SRV_WHITE        0
#define SRV_SURFACE_BASE 1
#define SRV_TEXTURE_BASE (SRV_SURFACE_BASE + MAX_SURFACES)
/* s27 part 3: depth-target snapshots get their own slot range, separate
 * from both color surfaces and the guest-texture cache (RSX_NO_DEPTH_RT) */
#define SRV_DEPTH_BASE   (SRV_TEXTURE_BASE + MAX_TEXTURES)
/* s29 (scratch/s29_x820_band.md): a color surface later sampled as a
 * texture at a smaller GAME-DECLARED size than the physical canvas every
 * surface_get() texture is allocated at (g.width x g.height) gets its own
 * cropped snapshot slot range, one per physical surface index, mirroring
 * the depth-RT snapshot range above (RSX_SURF_CROP). */
#define SRV_CROP_BASE    (SRV_DEPTH_BASE + MAX_SURFACES)
#define SRV_HEAP_SLOTS   (SRV_CROP_BASE + MAX_SURFACES)

#define SRV_TABLE_SIZE   16      /* descriptors per draw table              */
#define SRV_RING_TABLES  4096

/* B1: sampler heap. [0] is the default linear-clamp fallback; distinct
 * decoded sampler states are interned after it. Each draw fills a
 * 16-descriptor table (s0..s15) in a parallel shader-visible ring. */
#define SMP_DEFAULT      0
#define SMP_CACHE_SLOTS  MAX_TEXTURES
#define SMP_TABLE_SIZE   16
#define SMP_RING_TABLES  SRV_RING_TABLES

/* Per-draw constant block for translated shaders: 512 vec4 transform
 * constants + viewport-derived position scale/offset (see VP decompiler). */
#define CB_BLOCK_BYTES   ((512 + 2) * 16)
#define CB_BLOCK_ALIGNED ((CB_BLOCK_BYTES + 255) & ~255u)
#define CB_RING_BYTES    (CB_BLOCK_ALIGNED * SRV_RING_TABLES)

/* Translated-shader PSO cache */
#define MAX_PSOS         256

typedef struct {
    u32             location, offset;
    ID3D12Resource* tex;
    /* s31 FIX 2 (scratch/s31_render_fixes.md, the x=820 band): allocated at
     * the surface's LOGICAL (game-declared clip) size, not a fixed physical
     * canvas -- on real RSX a render target IS its declared size, so a
     * later draw sampling it as a texture gets UV [0,1] spanning exactly
     * the rendered columns/rows. The old uniform-canvas allocation made
     * UV span never-rendered padding (hard cutoff at declared/canvas of
     * the destination width, MEASURED x=819.2 for the 1024-wide 0xE40000
     * source, scratch/s29_x820_band.md). */
    u32             w, h;
    u32             draw_hits;   /* draws that targeted this surface this run */
    /* s29: cropped snapshot cache (RSX_SURF_CROP), NULL until this
     * surface is first sampled as a texture at a size smaller than the
     * physical canvas. Reused/replaced across draws by (crop_w, crop_h). */
    ID3D12Resource* crop_tex;
    u32             crop_w, crop_h;
} surface_t;

/* s27 part 3 (scratch/s26_fp_bisect.md, RSX_NO_DEPTH_RT fix): a depth/zeta
 * render target that later gets sampled as a texture (the tex15-class
 * shadow-map read) needs its own snapshot the same way color surfaces do --
 * but there is only ONE shared D3D12 depth-stencil resource (g.depth,
 * reused by every draw's depth test), so unlike a color surface a snapshot
 * has to be an explicit CPU-readback-and-reupload copy taken at the moment
 * the zeta target changes to something else (the pass boundary), not a
 * live view of the shared buffer. */
typedef struct {
    u32             location, offset;
    ID3D12Resource* tex;        /* RGBA8 snapshot, NULL until first flush   */
} depth_surface_t;

/* s31 FIX 1 (scratch/s31_render_fixes.md, s29-5a "pass-boundary depth
 * tracking"): on real RSX every zeta target (location, offset) is its own
 * piece of memory -- depth written while zeta=0x2310000 is simply not there
 * when the game later renders with zeta=0xB40000. The harness's single
 * shared g.depth resource merged ALL passes' depth (MEASURED: 80% of the
 * canvas carried residual cross-pass depth before draw 803,
 * scratch/s29_draw803_occluder.md mechanism 1), so later same-canvas draws
 * failed their depth test against unrelated earlier-pass content (the
 * player-character occluder class). Model each distinct zeta target as its
 * OWN depth resource, lazily created and far-cleared INLINE in the command
 * list (constraint from scratch/s29_x820_band.md: no Close/Execute/Wait/
 * Reset may be introduced inside the draw path -- resource creation and
 * ClearDepthStencilView are both flushless). Content persists across pass
 * revisits (the capture returns to zeta 0xB40000 at draw 803 after a
 * 57-draw 0x20C0000 interlude WITHOUT re-clearing -- deliberate depth
 * continuity that a clear-on-switch scheme would destroy). */
typedef struct {
    u32             location, offset;
    ID3D12Resource* tex;        /* dedicated D32_FLOAT_S8 buffer            */
    u32             w, h;       /* s31 FIX 2: >= every RT it's bound with   */
    int             cleared;    /* per-frame first-use clear latch          */
} zdepth_t;

typedef struct {
    u32             location, offset, format, width, height, pitch, remap;
    ID3D12Resource* tex;        /* NULL = undecodable, use the white slot */
} texcache_t;

typedef struct {
    u64                  key;   /* combined VP+FP ucode hash                */
    ID3D12PipelineState* pso;   /* NULL = translation failed, use fallback  */
    int                  skinned; /* [skin] diag: vp_c[467] literal seen in
                                    * the decompiled HLSL (4-bone skinning
                                    * scale-const pattern), cached so a PSO
                                    * cache HIT can still be tagged without
                                    * re-decompiling every draw            */
} psocache_t;

typedef struct {
    ID3D12Device*              dev;
    ID3D12CommandQueue*        queue;
    ID3D12CommandAllocator*    alloc;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence*               fence;
    HANDLE                     fence_event;
    u64                        fence_value;

    ID3D12DescriptorHeap*      rtv_heap;
    u32                        rtv_step;
    surface_t                  surfaces[MAX_SURFACES];
    u32                        n_surfaces;
    ID3D12Resource*            readback;
    u32                        width, height, rb_pitch;

    /* B1: shared depth-stencil target (one D32_FLOAT_S8 buffer bound with
     * every color surface; depth test/write honor the captured state) */
    ID3D12DescriptorHeap*      dsv_heap;
    u32                        dsv_step;
    ID3D12Resource*            depth;
    int                        depth_cleared;   /* per-frame clear latch      */

    /* s31 FIX 1: per-zeta-target depth buffers (DSV heap slot 1+i; slot 0
     * stays the shared fallback g.depth for RSX_NO_ZETA_TRACK) */
    zdepth_t                   zdepths[MAX_SURFACES];
    u32                        n_zdepths;

    /* s27 part 3: depth-target-as-texture snapshots (RSX_NO_DEPTH_RT kill
     * switch reverts to the pre-existing raw-guest-memory fallback) */
    depth_surface_t            depth_surfaces[MAX_SURFACES];
    u32                        n_depth_surfaces;
    ID3D12Resource*            depth_readback;  /* dedicated CPU-readable copy target */
    u32                        cur_zeta_loc, cur_zeta_off;
    int                        cur_zeta_valid, cur_zeta_had_write;
    u32                        depth_snapshot_count;

    /* B1: dynamic sampler heap (per-unit filter/wrap/LOD from the capture).
     * Samplers are interned by their decoded key; a per-draw table of 16
     * sampler descriptors mirrors the SRV table ring. */
    ID3D12DescriptorHeap*      smp_cpu_heap;   /* interned sampler cache      */
    ID3D12DescriptorHeap*      smp_heap;       /* shader-visible ring         */
    u32                        smp_step;
    u32                        smp_ring_used;
    u32                        smp_keys[MAX_TEXTURES];
    u32                        n_samplers;

    ID3D12DescriptorHeap*      srv_cpu_heap; /* CPU-only cache descriptors   */
    ID3D12DescriptorHeap*      srv_heap;   /* shader-visible table ring      */
    u32                        srv_step;
    u32                        srv_ring_used; /* tables handed out this frame */
    ID3D12Resource*            white_tex;
    texcache_t                 textures[MAX_TEXTURES];
    u32                        n_textures;
    ID3D12Resource*            upload;     /* linear texture-upload arena    */
    u8*                        upload_mapped;
    u32                        upload_used;

    ID3D12RootSignature*       rootsig;    /* fallback fixed pipeline        */
    ID3D12PipelineState*       pso_tri;
    ID3D12PipelineState*       pso_tex;
    ID3D12PipelineState*       pso_line;

    ID3D12RootSignature*       rootsig_x;  /* translated: CBV b0 + t0..t15   */
    psocache_t                 psos[MAX_PSOS];
    u32                        n_psos;

    ID3D12Resource*            cb;         /* per-draw constant ring         */
    u8*                        cb_mapped;
    u32                        cb_used;

    ID3D12Resource*            vb;
    u8*                        vb_mapped;
    u32                        vb_used;    /* bytes */
} gpu_t;

static gpu_t g;

static void srv_write(u32 slot, ID3D12Resource* tex);
static ID3D12Resource* create_texture_rgba(const u8* rgba, u32 w, u32 h);
static void gpu_wait(void);   /* s29: surface_crop_flush() needs this ahead of its definition */
static D3D12_SAMPLER_DESC decode_sampler(const rsx_dsp_texture* t);
static D3D12_CPU_DESCRIPTOR_HANDLE smp_cpu(u32 slot);

static int gpu_init(u32 width, u32 height, int use_hw)
{
    HRESULT hr;
    /* s29 DEBUG: RSX_D3D_DEBUG=1 enables the D3D12 validation layer for
     * bisecting the surface-crop corruption -- temporary, not part of the
     * fix itself. */
    if (getenv("RSX_D3D_DEBUG")) {
        ID3D12Debug* dbg = NULL;
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&dbg)) && dbg) {
            dbg->lpVtbl->EnableDebugLayer(dbg);
            dbg->lpVtbl->Release(dbg);
            printf("[gpu] D3D12 debug layer ENABLED (RSX_D3D_DEBUG)\n");
        } else {
            printf("[gpu] D3D12 debug layer unavailable\n");
        }
    }
    IDXGIFactory4* factory = NULL;
    hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory);
    if (FAILED(hr)) { printf("dxgi factory failed 0x%08lX\n", hr); return -1; }

    IDXGIAdapter* adapter = NULL;
    if (!use_hw) {
        hr = factory->lpVtbl->EnumWarpAdapter(factory, &IID_IDXGIAdapter, (void**)&adapter);
        if (FAILED(hr)) { printf("no WARP adapter 0x%08lX\n", hr); return -1; }
    }
    hr = D3D12CreateDevice((IUnknown*)adapter, D3D_FEATURE_LEVEL_11_0,
                           &IID_ID3D12Device, (void**)&g.dev);
    if (adapter) adapter->lpVtbl->Release(adapter);
    factory->lpVtbl->Release(factory);
    if (FAILED(hr)) { printf("device create failed 0x%08lX\n", hr); return -1; }
    printf("[gpu] %s device created\n", use_hw ? "hardware" : "WARP");

    D3D12_COMMAND_QUEUE_DESC qd = {0};
    if (FAILED(g.dev->lpVtbl->CreateCommandQueue(g.dev, &qd, &IID_ID3D12CommandQueue, (void**)&g.queue)))
        return -1;
    if (FAILED(g.dev->lpVtbl->CreateCommandAllocator(g.dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     &IID_ID3D12CommandAllocator, (void**)&g.alloc)))
        return -1;
    if (FAILED(g.dev->lpVtbl->CreateCommandList(g.dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, NULL,
                                                &IID_ID3D12GraphicsCommandList, (void**)&g.list)))
        return -1;
    if (FAILED(g.dev->lpVtbl->CreateFence(g.dev, 0, D3D12_FENCE_FLAG_NONE,
                                          &IID_ID3D12Fence, (void**)&g.fence)))
        return -1;
    g.fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    g.width = width;
    g.height = height;
    g.rb_pitch = (width * 4 + 255) & ~255u;

    /* RTV heap for the surface cache (surfaces are created on demand) */
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.NumDescriptors = MAX_SURFACES;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g.rtv_heap)))
        return -1;
    g.rtv_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    /* readback buffer -- s31 FIX 2: logical-size surfaces can exceed the
     * display canvas (this capture declares 1024x768 and 1024x1024 passes
     * on a 1280x720 display), so give the scratch readback enough room for
     * any surface up to 2048x2048 RGBA8. */
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (u64)g.rb_pitch * height;
    if (bd.Width < 16u * 1024 * 1024)
        bd.Width = 16u * 1024 * 1024;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                      &IID_ID3D12Resource, (void**)&g.readback)))
        return -1;

    /* upload vertex buffer */
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    bd.Width = (u64)MAX_VERTS * VERT_STRIDE;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                      &IID_ID3D12Resource, (void**)&g.vb)))
        return -1;
    D3D12_RANGE rr = {0, 0};
    g.vb->lpVtbl->Map(g.vb, 0, &rr, (void**)&g.vb_mapped);

    /* texture-upload arena (persistently mapped; regions handed out once) */
    bd.Width = UPLOAD_SIZE;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                      &IID_ID3D12Resource, (void**)&g.upload)))
        return -1;
    g.upload->lpVtbl->Map(g.upload, 0, &rr, (void**)&g.upload_mapped);

    /* CPU-only SRV cache heap (copy source) + shader-visible table ring */
    hd.NumDescriptors = SRV_HEAP_SLOTS;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g.srv_cpu_heap)))
        return -1;
    hd.NumDescriptors = SRV_RING_TABLES * SRV_TABLE_SIZE;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g.srv_heap)))
        return -1;
    g.srv_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    /* B1: sampler heaps (CPU cache + shader-visible ring) mirroring the SRVs */
    {
        D3D12_DESCRIPTOR_HEAP_DESC shd = {0};
        shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        shd.NumDescriptors = SMP_CACHE_SLOTS + 1;   /* +1 default fallback   */
        shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &shd, &IID_ID3D12DescriptorHeap, (void**)&g.smp_cpu_heap)))
            return -1;
        shd.NumDescriptors = SMP_RING_TABLES * SMP_TABLE_SIZE;
        shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &shd, &IID_ID3D12DescriptorHeap, (void**)&g.smp_heap)))
            return -1;
        g.smp_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        /* default sampler at cache slot 0: linear/clamp, full mip range */
        D3D12_SAMPLER_DESC def = {0};
        def.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        def.AddressU = def.AddressV = def.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        def.MaxLOD = D3D12_FLOAT32_MAX;
        def.MaxAnisotropy = 1;
        g.dev->lpVtbl->CreateSampler(g.dev, &def, smp_cpu(SMP_DEFAULT));
    }

    /* B1: shared depth-stencil target + DSV heap (D32_FLOAT_S8) */
    {
        D3D12_DESCRIPTOR_HEAP_DESC dhd = {0};
        dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dhd.NumDescriptors = 1 + MAX_SURFACES;   /* s31: slot 0 = shared fallback, 1+i = per-zeta-target */
        if (FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &dhd, &IID_ID3D12DescriptorHeap, (void**)&g.dsv_heap)))
            return -1;
        g.dsv_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        D3D12_HEAP_PROPERTIES dhp = {0};
        dhp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC drd = {0};
        drd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        drd.Width = width;
        drd.Height = height;
        drd.DepthOrArraySize = 1;
        drd.MipLevels = 1;
        drd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        drd.SampleDesc.Count = 1;
        drd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE dcv = {0};
        dcv.Format = drd.Format;
        dcv.DepthStencil.Depth = 1.0f;
        if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &dhp, D3D12_HEAP_FLAG_NONE, &drd,
                                                          D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv,
                                                          &IID_ID3D12Resource, (void**)&g.depth))) {
            printf("[gpu] depth buffer create failed; depth test disabled\n");
            g.depth = NULL;
        } else {
            D3D12_CPU_DESCRIPTOR_HANDLE dh;
            g.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.dsv_heap, &dh);
            g.dev->lpVtbl->CreateDepthStencilView(g.dev, g.depth, NULL, dh);
        }
    }

    /* s27 part 3: dedicated CPU-readable copy target for depth-target
     * snapshots (RSX_NO_DEPTH_RT). Only the DEPTH plane (subresource 0 of
     * the planar D32_FLOAT_S8X24_UINT resource) is copied -- R32_FLOAT,
     * 4 bytes/texel; row pitch padded to the mandatory 256-byte D3D12 copy
     * alignment. */
    if (g.depth) {
        D3D12_HEAP_PROPERTIES dhp2 = {0};
        dhp2.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC dbd = {0};
        dbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        const u32 depth_pitch = (width * 4 + 255) & ~255u;
        dbd.Width = (u64)depth_pitch * height;
        dbd.Height = 1;
        dbd.DepthOrArraySize = 1;
        dbd.MipLevels = 1;
        dbd.SampleDesc.Count = 1;
        dbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &dhp2, D3D12_HEAP_FLAG_NONE, &dbd,
                                                          D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                          &IID_ID3D12Resource, (void**)&g.depth_readback))) {
            printf("[gpu] depth readback buffer create failed; RSX_NO_DEPTH_RT-equivalent fallback\n");
            g.depth_readback = NULL;
        }
    }

    /* per-draw constant ring for the translated shaders */
    bd.Width = CB_RING_BYTES;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                      &IID_ID3D12Resource, (void**)&g.cb)))
        return -1;
    g.cb->lpVtbl->Map(g.cb, 0, &rr, (void**)&g.cb_mapped);

    /* root signature: 16 root constants = 4x4 transform at b0 (VS),
     * one SRV table at t0 (PS), one static linear-clamp sampler at s0 */
    D3D12_DESCRIPTOR_RANGE range = {0};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER rp[2] = {0};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 16;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &range;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC smp = {0};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {0};
    rsd.NumParameters = 2;
    rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &smp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = NULL;
    ID3DBlob* err = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        printf("rootsig: %s\n", err ? (char*)err->lpVtbl->GetBufferPointer(err) : "?");
        return -1;
    }
    hr = g.dev->lpVtbl->CreateRootSignature(g.dev, 0, sig->lpVtbl->GetBufferPointer(sig),
                                            sig->lpVtbl->GetBufferSize(sig),
                                            &IID_ID3D12RootSignature, (void**)&g.rootsig);
    sig->lpVtbl->Release(sig);
    if (FAILED(hr)) return -1;

    /* translated-shader root signature: b0 root CBV (VS constants+viewport),
     * one 16-descriptor SRV table t0..t15 (PS), one 16-descriptor dynamic
     * sampler table s0..s15 (PS). B1: samplers are per-unit descriptors that
     * carry the captured filter/wrap/LOD, not baked static samplers. */
    {
        D3D12_DESCRIPTOR_RANGE xrange = {0};
        xrange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        xrange.NumDescriptors = SRV_TABLE_SIZE;
        D3D12_DESCRIPTOR_RANGE srange = {0};
        srange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        srange.NumDescriptors = SMP_TABLE_SIZE;
        D3D12_ROOT_PARAMETER xp[3] = {0};
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
        D3D12_ROOT_SIGNATURE_DESC xrsd = {0};
        xrsd.NumParameters = 3;
        xrsd.pParameters = xp;
        xrsd.NumStaticSamplers = 0;
        xrsd.pStaticSamplers = NULL;
        xrsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob* xsig = NULL;
        if (FAILED(D3D12SerializeRootSignature(&xrsd, D3D_ROOT_SIGNATURE_VERSION_1, &xsig, &err))) {
            printf("rootsig_x: %s\n", err ? (char*)err->lpVtbl->GetBufferPointer(err) : "?");
            return -1;
        }
        hr = g.dev->lpVtbl->CreateRootSignature(g.dev, 0, xsig->lpVtbl->GetBufferPointer(xsig),
                                                xsig->lpVtbl->GetBufferSize(xsig),
                                                &IID_ID3D12RootSignature, (void**)&g.rootsig_x);
        xsig->lpVtbl->Release(xsig);
        if (FAILED(hr)) return -1;
    }

    /* fallback pass-through shaders reading the wide 16-attribute vertex;
     * transform columns arrive at b0. The textured pixel shader modulates by
     * the vertex color (white when no diffuse). */
    static const char vs_src[] =
        "cbuffer cb : register(b0) { float4 c0, c1, c2, c3; };\n"
        "struct I { float4 p : ATTR0; float4 c : ATTR3; float4 t : ATTR8; };\n"
        "struct O { float4 p : SV_POSITION; float4 c : COLOR; float2 t : TEXCOORD; };\n"
        "O main(I i) {\n"
        "  O o;\n"
        "  o.p = c0 * i.p.x + c1 * i.p.y + c2 * i.p.z + c3 * 1.0;\n"
        "  o.c = i.c;\n"
        "  o.t = i.t.xy;\n"
        "  return o;\n"
        "}\n";
    static const char ps_src[] =
        "struct I { float4 p : SV_POSITION; float4 c : COLOR; float2 t : TEXCOORD; };\n"
        "float4 main(I i) : SV_TARGET { return i.c; }\n";
    static const char ps_tex_src[] =
        "Texture2D tex0 : register(t0);\n"
        "SamplerState smp0 : register(s0);\n"
        "struct I { float4 p : SV_POSITION; float4 c : COLOR; float2 t : TEXCOORD; };\n"
        "float4 main(I i) : SV_TARGET { return tex0.Sample(smp0, i.t) * i.c; }\n";
    ID3DBlob *vs = NULL, *ps = NULL, *ps_tex = NULL;
    if (FAILED(D3DCompile(vs_src, sizeof(vs_src) - 1, "vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vs, &err))) {
        printf("vs: %s\n", err ? (char*)err->lpVtbl->GetBufferPointer(err) : "?");
        return -1;
    }
    if (FAILED(D3DCompile(ps_src, sizeof(ps_src) - 1, "ps", NULL, NULL, "main", "ps_5_0", 0, 0, &ps, &err))) {
        printf("ps: %s\n", err ? (char*)err->lpVtbl->GetBufferPointer(err) : "?");
        return -1;
    }
    if (FAILED(D3DCompile(ps_tex_src, sizeof(ps_tex_src) - 1, "ps_tex", NULL, NULL, "main", "ps_5_0", 0, 0, &ps_tex, &err))) {
        printf("ps_tex: %s\n", err ? (char*)err->lpVtbl->GetBufferPointer(err) : "?");
        return -1;
    }

    D3D12_INPUT_ELEMENT_DESC il[] = {
        {"ATTR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0 * 16,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"ATTR", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 3 * 16,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"ATTR", 8, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8 * 16,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = g.rootsig;
    pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
    pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
    pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    pd.InputLayout.pInputElementDescs = il;
    pd.InputLayout.NumElements = 3;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    hr = g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd, &IID_ID3D12PipelineState,
                                                    (void**)&g.pso_tri);
    if (FAILED(hr)) { printf("pso tri failed 0x%08lX\n", hr); return -1; }
    pd.PS.pShaderBytecode = ps_tex->lpVtbl->GetBufferPointer(ps_tex);
    pd.PS.BytecodeLength = ps_tex->lpVtbl->GetBufferSize(ps_tex);
    hr = g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd, &IID_ID3D12PipelineState,
                                                    (void**)&g.pso_tex);
    if (FAILED(hr)) { printf("pso tex failed 0x%08lX\n", hr); return -1; }
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
    pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd, &IID_ID3D12PipelineState,
                                               (void**)&g.pso_line);
    vs->lpVtbl->Release(vs);
    ps->lpVtbl->Release(ps);
    ps_tex->lpVtbl->Release(ps_tex);

    /* 1x1 white fallback texture at SRV slot 0: draws whose unit-0 texture
     * cannot be resolved modulate by white, i.e. degrade to vertex color */
    static const u8 white[4] = {255, 255, 255, 255};
    g.white_tex = create_texture_rgba(white, 1, 1);
    if (!g.white_tex)
        return -1;
    srv_write(SRV_WHITE, g.white_tex);

    /* open the list for the first frame */
    D3D12_VIEWPORT vp = {0, 0, (float)width, (float)height, 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, (LONG)width, (LONG)height};
    g.list->lpVtbl->RSSetViewports(g.list, 1, &vp);
    g.list->lpVtbl->RSSetScissorRects(g.list, 1, &sc);
    return 0;
}

static D3D12_CPU_DESCRIPTOR_HANDLE surface_rtv(u32 idx)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.rtv_heap, &h);
    h.ptr += (size_t)idx * g.rtv_step;
    return h;
}

/* CPU-side cache descriptor (copy source for the per-draw tables) */
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

/* Allocate the next 16-descriptor table in the shader-visible ring, fill it
 * from the given cache slots, and return its GPU handle for the root table. */
static D3D12_GPU_DESCRIPTOR_HANDLE srv_table(const u32 slots[SRV_TABLE_SIZE])
{
    if (g.srv_ring_used >= SRV_RING_TABLES) {
        printf("[gpu] SRV table ring full; reusing table 0\n");
        g.srv_ring_used = 0;
    }
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

/* ---- B1 sampler heap (parallels the SRV heap/ring) -------------------- */

static D3D12_CPU_DESCRIPTOR_HANDLE smp_cpu(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.smp_cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.smp_cpu_heap, &h);
    h.ptr += (size_t)slot * g.smp_step;
    return h;
}

/* Intern a decoded sampler by key; return its CPU-cache slot. */
static u32 sampler_slot(const rsx_dsp_texture* t, u32 key)
{
    for (u32 i = 0; i < g.n_samplers; i++)
        if (g.smp_keys[i] == key)
            return 1 + i;                 /* slot 0 = default fallback       */
    if (g.n_samplers >= SMP_CACHE_SLOTS)
        return SMP_DEFAULT;
    D3D12_SAMPLER_DESC sd = decode_sampler(t);
    const u32 slot = 1 + g.n_samplers;
    g.dev->lpVtbl->CreateSampler(g.dev, &sd, smp_cpu(slot));
    g.smp_keys[g.n_samplers++] = key;
    return slot;
}

/* Fill the next 16-descriptor sampler table from cache slots; return GPU handle. */
static D3D12_GPU_DESCRIPTOR_HANDLE sampler_table(const u32 slots[SMP_TABLE_SIZE])
{
    if (g.smp_ring_used >= SMP_RING_TABLES)
        g.smp_ring_used = 0;
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

/* Create an immutable texture from CPU-prepared data: stage the rows in the
 * persistently-mapped upload arena, record the copy on the open list, and
 * leave the resource in PIXEL_SHADER_RESOURCE state. `rows`/`row_bytes`
 * describe the CPU layout (for BC formats: block rows of 4 texel lines). */
static ID3D12Resource* create_texture_ex(DXGI_FORMAT fmt, u32 w, u32 h,
                                         const u8* data, u32 row_bytes, u32 rows)
{
    const u32 pitch = (row_bytes + 255) & ~255u; /* D3D12 placed-footprint row alignment */
    const u32 start = (g.upload_used + 511) & ~511u;
    if ((u64)start + (u64)pitch * rows > UPLOAD_SIZE) {
        printf("[gpu] texture upload arena full\n");
        return NULL;
    }
    for (u32 y = 0; y < rows; y++)
        memcpy(g.upload_mapped + start + (size_t)y * pitch, data + (size_t)y * row_bytes, row_bytes);

    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    ID3D12Resource* tex = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                      &IID_ID3D12Resource, (void**)&tex))) {
        printf("[gpu] texture create failed (%ux%u)\n", w, h);
        return NULL;
    }
    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = g.upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = start;
    src.PlacedFootprint.Footprint.Format = fmt;
    src.PlacedFootprint.Footprint.Width = w;
    src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    g.upload_used = start + pitch * rows;
    return tex;
}

static ID3D12Resource* create_texture_rgba(const u8* rgba, u32 w, u32 h)
{
    return create_texture_ex(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, rgba, w * 4, h);
}

/* B1: one mip level's CPU-side layout (top level at index 0). */
typedef struct {
    u32       w, h;         /* level dimensions (texels)                    */
    const u8* data;         /* CPU source                                   */
    u32       row_bytes;    /* bytes per source row (block row for BC)      */
    u32       rows;         /* source row count (block rows for BC)         */
} tex_level_t;

/* Create an immutable MipLevels=n texture and upload every level. Each level
 * is staged into the upload arena at its own 256-aligned pitch and copied to
 * its subresource. Leaves the resource in PIXEL_SHADER_RESOURCE state. */
static ID3D12Resource* create_texture_mipped(DXGI_FORMAT fmt, const tex_level_t* lv, u32 n)
{
    if (n == 0)
        return NULL;
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = lv[0].w;
    rd.Height = lv[0].h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = (u16)n;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    ID3D12Resource* tex = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                      &IID_ID3D12Resource, (void**)&tex))) {
        printf("[gpu] mipped texture create failed (%ux%u x%u)\n", lv[0].w, lv[0].h, n);
        return NULL;
    }
    for (u32 m = 0; m < n; m++) {
        const u32 pitch = (lv[m].row_bytes + 255) & ~255u;
        const u32 start = (g.upload_used + 511) & ~511u;
        if ((u64)start + (u64)pitch * lv[m].rows > UPLOAD_SIZE) {
            printf("[gpu] upload arena full at mip %u\n", m);
            break;
        }
        for (u32 y = 0; y < lv[m].rows; y++)
            memcpy(g.upload_mapped + start + (size_t)y * pitch,
                   lv[m].data + (size_t)y * lv[m].row_bytes, lv[m].row_bytes);
        D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
        src.pResource = g.upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = start;
        src.PlacedFootprint.Footprint.Format = fmt;
        src.PlacedFootprint.Footprint.Width = lv[m].w;
        src.PlacedFootprint.Footprint.Height = lv[m].h;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = pitch;
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = m;
        g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);
        g.upload_used = start + pitch * lv[m].rows;
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

/* Find or create the render target for a (location, color offset) pair.
 * s31 FIX 2: allocated at the LOGICAL (want_w x want_h) size -- the game's
 * declared clip dims at bind time (0 = fall back to the display canvas).
 * A (location, offset) redeclared at different dims is REALLOCATED in place
 * (same cache slot, content dropped, logged): the game re-purposed that
 * memory, and one slot per (location, offset) keeps the sampled-surface
 * lookup unambiguous. MEASURED on cap_user3d.rxs: every offset is bound
 * with exactly one size across the whole capture, so the realloc path
 * never fires there. */
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
        if (g.n_surfaces >= MAX_SURFACES) {
            printf("[gpu] surface cache full; reusing surface 0\n");
            return 0;
        }
        slot = g.n_surfaces;
    } else {
        surface_t* olds = &g.surfaces[slot];
        printf("[surfsz] surface 0x%X redeclared %ux%u -> %ux%u (content dropped)\n",
               offset, olds->w, olds->h, want_w, want_h);
        if (olds->tex) { olds->tex->lpVtbl->Release(olds->tex); olds->tex = NULL; }
    }

    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = want_w;
    rd.Height = want_h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = rd.Format;
    surface_t* s = &g.surfaces[slot];
    HRESULT hr = g.dev->lpVtbl->CreateCommittedResource(
        g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
        &IID_ID3D12Resource, (void**)&s->tex);
    if (FAILED(hr)) {
        printf("[gpu] surface create failed 0x%08lX\n", hr);
        return 0;
    }
    s->location = location;
    s->offset = offset;
    s->w = want_w;
    s->h = want_h;
    g.dev->lpVtbl->CreateRenderTargetView(g.dev, s->tex, NULL, surface_rtv(slot));
    srv_write(SRV_SURFACE_BASE + slot, s->tex);
    if (slot == g.n_surfaces)
        g.n_surfaces++;
    return slot;
}

/* s29 (scratch/s29_x820_band.md): every surface_get() texture is allocated
 * at the fixed physical canvas size (g.width x g.height), regardless of the
 * GCM surface's own logical clip/pitch dims -- a deliberate uniform-canvas
 * simplification so any (location,offset) can alias any texture without
 * reallocation. On real RSX hardware a render target IS its declared size,
 * so a game-authored blit/downsample pass whose texcoords are normalized
 * against its OWN declared texture width (t.width/t.height, this draw's
 * captured texture-unit format registers) samples correctly. In our
 * uniform-canvas replay, binding the raw oversized physical resource as
 * that texture unit's SRV makes UV [0,1] span the FULL physical width
 * instead of the declared one -- for a source narrower than the canvas,
 * anything past declared_w/physical_w of the destination silently samples
 * the never-rendered padding columns (black), producing a hard vertical
 * cutoff at declared_w/physical_w of the destination's own width (measured
 * root of the x=820 band: source 0xE40000 declared 1024 wide, physical
 * canvas 1280, dest viewport 1024 -> cutoff at 1024*(1024/1280)=819.2 ~=
 * 820, scratch/s29_x820_band.md).
 *
 * MECHANISM CONFIRMED, FIX NOT SHIP-SAFE (2026-07-10): the CPU-round-trip
 * crop below is architecturally correct but MEASURED to regress worse than
 * the defect it fixes -- inserting the extra Close/Execute/Wait/Reset cycle
 * anywhere inside a draw's texture-unit resolution loop (i.e. AFTER
 * `target`'s own prior CLEAR was already recorded but BEFORE this draw's
 * OWN paint of that same target is recorded) causes the paint to be LOST,
 * leaving only the clear color -- reproduced with the crop's GPU work
 * stripped out entirely (a bare flush with no barrier/copy/recreate still
 * breaks it, scratch/s29_x820_band.md "flush-only isolation"). Root cause
 * of THAT is still open. Default OFF pending a fix for the flush-ordering
 * bug; RSX_SURF_CROP=1 opts in for further debugging only -- do not flip
 * this default without first re-verifying the flush-only repro is gone. */
static int honor_surf_crop(void)
{
    static int inited = 0, on = 0;
    if (!inited) {
        inited = 1;
        on = getenv("RSX_SURF_CROP") ? 1 : 0;
        printf("[surfcrop] RSX_SURF_CROP %s -> surface-as-texture size crop %s\n",
               getenv("RSX_SURF_CROP") ? "SET" : "unset",
               on ? "ON (opt-in, s29 fix -- KNOWN REGRESSIVE, debug only)" : "OFF (default, safe)");
    }
    return on;
}

/* Crop physical surface g.surfaces[phys_idx]'s top-left (log_w x log_h)
 * region into a tightly-sized RGBA8 texture, via a CPU round trip (same
 * close/execute/wait/reopen shape as depth_snapshot_flush()): the surface
 * is read back through the existing g.readback scratch buffer (already
 * sized g.width x g.height RGBA8 for the per-flip scanout dump), the
 * declared sub-rect is copied out, and create_texture_rgba() re-uploads it
 * as an independent, correctly-sized resource. Called from inside the
 * per-draw texture-unit resolution loop, BEFORE this draw's own vertex/
 * constant-buffer/SRV-table writes (g.vb_used/g.cb_used/g.srv_ring_used/
 * g.upload_used are all still at their pre-draw values at that point, same
 * precondition gpu_flush() relies on a few hundred lines down), so
 * resetting those ring counters here is safe. Returns NULL (caller falls
 * back to the raw oversized bind) if anything about the source is
 * unusable. */
static ID3D12Resource* surface_crop_flush(u32 phys_idx, u32 log_w, u32 log_h)
{
    if (phys_idx >= g.n_surfaces || !g.surfaces[phys_idx].tex || !g.readback)
        return NULL;
    surface_t* sp = &g.surfaces[phys_idx];
    /* s31 FIX 2: the "physical" size is now the surface's own logical size */
    const u32 phys_w = sp->w ? sp->w : g.width;
    const u32 phys_h = sp->h ? sp->h : g.height;
    const u32 phys_pitch = (phys_w * 4 + 255) & ~255u;
    if (log_w == 0 || log_h == 0 || log_w > phys_w || log_h > phys_h)
        return NULL;
    static u32 s29_crop_calls = 0;
    s29_crop_calls++;
    printf("[s29crop] call #%u phys_idx=%u off=0x%X log=%ux%u phys=%ux%u\n",
           s29_crop_calls, phys_idx, sp->offset, log_w, log_h, phys_w, phys_h);
    if (sp->crop_tex && sp->crop_w == log_w && sp->crop_h == log_h) {
        /* Reuse the existing crop resource's SLOT but content must still be
         * refreshed every call (the physical surface may have been
         * re-rendered since the last crop) -- fall through to redo the
         * copy, only the CreateCommittedResource is skippable in principle;
         * not optimized here, correctness over perf for this fix. */
    }

    /* s29 DEBUG FINDING (not fixed, see honor_surf_crop()'s comment): a bare
     * Close/Execute/Wait/Reset at this exact point in the caller's texture-
     * unit loop -- with NO barrier/copy/recreate at all -- was independently
     * confirmed (this session, not left as live code) to reproduce the same
     * "draw's paint lost, target shows only its clear color" regression.
     * The corruption is in the FLUSH ITSELF landing between `target`'s own
     * prior clear and this draw's own paint of that target, not in any of
     * the copy/barrier/texture-create work below. Do not re-litigate the
     * barrier states as the culprit -- also independently ruled out this
     * session (skipping both ResourceBarrier calls changed nothing). */
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = sp->tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = sp->tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    dst.pResource = g.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = phys_w;
    dst.PlacedFootprint.Footprint.Height = phys_h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = phys_pitch;
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    g.list->lpVtbl->Close(g.list);
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    gpu_wait();
    {
        HRESULT dr = g.dev->lpVtbl->GetDeviceRemovedReason(g.dev);
        if (FAILED(dr))
            printf("[s29crop] DEVICE REMOVED after flush: 0x%08lX\n", dr);
    }

    u8* src_px = NULL;
    D3D12_RANGE rr = {0, (size_t)phys_pitch * phys_h};
    g.readback->lpVtbl->Map(g.readback, 0, &rr, (void**)&src_px);
    u8* rgba = src_px ? malloc((size_t)log_w * log_h * 4) : NULL;
    if (rgba) {
        for (u32 y = 0; y < log_h; y++)
            memcpy(rgba + (size_t)y * log_w * 4,
                   src_px + (size_t)y * phys_pitch, (size_t)log_w * 4);
    }
    D3D12_RANGE wr = {0, 0};
    g.readback->lpVtbl->Unmap(g.readback, 0, &wr);

    g.alloc->lpVtbl->Reset(g.alloc);
    g.list->lpVtbl->Reset(g.list, g.alloc, NULL);
    g.vb_used = 0;
    g.cb_used = 0;
    g.srv_ring_used = 0;
    g.upload_used = 0;

    ID3D12Resource* tex = NULL;
    if (rgba) {
        tex = create_texture_rgba(rgba, log_w, log_h);
        free(rgba);
    }
    if (tex) {
        if (sp->crop_tex)
            sp->crop_tex->lpVtbl->Release(sp->crop_tex);
        sp->crop_tex = tex;
        sp->crop_w = log_w;
        sp->crop_h = log_h;
        srv_write(SRV_CROP_BASE + phys_idx, tex);
    }
    return tex;
}

/* Surface currently selected as color target 0 by the register state.
 * s31 FIX 2: sized by the surface state's own declared clip dims. */
static u32 current_surface(const rsx_dispatch* rsx)
{
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(rsx, &sf);
    return surface_get(sf.color_location[0], sf.color_offset[0], sf.clip_w, sf.clip_h);
}

static void gpu_wait(void)
{
    g.fence_value++;
    g.queue->lpVtbl->Signal(g.queue, g.fence, g.fence_value);
    if (g.fence->lpVtbl->GetCompletedValue(g.fence) < g.fence_value) {
        g.fence->lpVtbl->SetEventOnCompletion(g.fence, g.fence_value, g.fence_event);
        WaitForSingleObject(g.fence_event, INFINITE);
    }
}

/* ---- s31 FIX 1: per-zeta-target depth buffers (see zdepth_t) ----------- */

/* Kill switch: RSX_NO_ZETA_TRACK=1 restores the old single-shared-depth
 * behavior (every pass merged into g.depth). Default ON. */
static int honor_zeta_track(void)
{
    static int inited = 0, on = 1;
    if (!inited) {
        inited = 1;
        on = getenv("RSX_NO_ZETA_TRACK") ? 0 : 1;
        printf("[zetatrack] RSX_NO_ZETA_TRACK %s -> per-zeta-target depth buffers %s\n",
               on ? "unset" : "SET",
               on ? "ON (default, s31 fix)" : "OFF (legacy shared depth)");
    }
    return on;
}

static D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.dsv_heap, &h);
    h.ptr += (size_t)slot * g.dsv_step;
    return h;
}

/* Find or create the dedicated depth buffer for zeta target
 * (location, offset). Returns the DSV heap slot: 1+i for a per-target
 * buffer, 0 = fall back to the shared g.depth (table full / create failed).
 * A fresh committed resource's contents are undefined, so creation records
 * an inline far-plane ClearDepthStencilView -- flushless by design
 * (constraint: scratch/s29_x820_band.md's mid-draw-flush hazard).
 * s31 FIX 2: (rt_w, rt_h) is the CURRENT color target's logical size; the
 * buffer is sized to cover max(RT, canvas) so the DSV is never smaller
 * than the RT it's bound with (this capture declares 1024x768/1024x1024
 * passes on a 1280x720 canvas). If a later bind outgrows it, it is
 * recreated larger (fresh far clear, logged -- never fires on this
 * capture, where each zeta target pairs with fixed-size passes). */
static u32 zdepth_get(u32 location, u32 offset, u32 rt_w, u32 rt_h)
{
    u32 want_w = rt_w > g.width  ? rt_w : g.width;
    u32 want_h = rt_h > g.height ? rt_h : g.height;
    u32 idx = MAX_SURFACES;
    for (u32 i = 0; i < g.n_zdepths; i++)
        if (g.zdepths[i].location == location && g.zdepths[i].offset == offset) {
            if (g.zdepths[i].w >= want_w && g.zdepths[i].h >= want_h)
                return 1 + i;
            idx = i;   /* outgrown: recreate larger below */
            break;
        }
    if (idx == MAX_SURFACES) {
        if (g.n_zdepths >= MAX_SURFACES) {
            printf("[zetatrack] zdepth cache full; falling back to shared depth\n");
            return 0;
        }
        idx = g.n_zdepths;
    } else {
        zdepth_t* oz = &g.zdepths[idx];
        printf("[zetatrack] zdepth loc=%u off=0x%X outgrown %ux%u -> %ux%u"
               " (content dropped, re-cleared)\n",
               location, offset, oz->w, oz->h, want_w, want_h);
        if (want_w < oz->w) want_w = oz->w;
        if (want_h < oz->h) want_h = oz->h;
        if (oz->tex) { oz->tex->lpVtbl->Release(oz->tex); oz->tex = NULL; }
    }
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = want_w;
    rd.Height = want_h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = rd.Format;
    cv.DepthStencil.Depth = 1.0f;
    zdepth_t* z = &g.zdepths[idx];
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                      &IID_ID3D12Resource, (void**)&z->tex))) {
        printf("[zetatrack] zdepth create failed (loc=%u off=0x%X); shared fallback\n",
               location, offset);
        z->tex = NULL;
        return 0;
    }
    z->location = location;
    z->offset = offset;
    z->w = want_w;
    z->h = want_h;
    const u32 slot = 1 + idx;
    g.dev->lpVtbl->CreateDepthStencilView(g.dev, z->tex, NULL, dsv_handle(slot));
    D3D12_CPU_DESCRIPTOR_HANDLE dh = dsv_handle(slot);
    g.list->lpVtbl->ClearDepthStencilView(g.list, dh,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
    z->cleared = 1;
    printf("[zetatrack] new zeta depth target #%u loc=%u off=0x%X (%ux%u)\n",
           idx, location, offset, want_w, want_h);
    if (idx == g.n_zdepths)
        g.n_zdepths++;
    return slot;
}

/* Resource behind a zeta (location, offset), WITHOUT creating one: used by
 * the opt-in depth-readback diagnostics so they read the pass-correct
 * buffer under tracking. NULL -> caller uses the shared g.depth. */
static ID3D12Resource* zdepth_find_res(u32 location, u32 offset)
{
    if (!honor_zeta_track())
        return NULL;
    for (u32 i = 0; i < g.n_zdepths; i++)
        if (g.zdepths[i].location == location && g.zdepths[i].offset == offset)
            return g.zdepths[i].tex;
    return NULL;
}

/* The depth resource bound by the most recent draw (RSX_DEPTH_DUMP_* reads
 * this so its raw dumps stay meaningful under per-target tracking). */
static ID3D12Resource* g_zdump_res = NULL;

/* s27 part 3 (scratch/s26_fp_bisect.md, depth-target-as-texture snapshots).
 * s27 A/B CORRECTION (~07:20): default flipped to OFF — with the RSQ fix in,
 * the full-composite A/B showed this feature REGRESSES the room geometry
 * (walls/cabinets replaced by sky punch-through: scratch/s27_rsqfix vs
 * scratch/s27_rsq_nodrt) — its part-3 "zero regressions" diff did not cover
 * the composite. The mechanism is real (shadow-map reads sample live data)
 * but the readback/redirect corrupts some room passes; debug before
 * re-defaulting. Opt-in: RSX_DEPTH_RT=1. */
static int honor_depth_rt(void)
{
    static int inited = 0, on = 0;
    if (!inited) {
        inited = 1;
        on = getenv("RSX_DEPTH_RT") ? 1 : 0;
        printf("[depthrt] RSX_DEPTH_RT %s -> depth-target snapshotting %s\n",
               on ? "SET" : "unset", on ? "ON" : "OFF (default; s27 A/B regression)");
    }
    return on;
}

/* Find or lazily register a depth_surfaces[] entry for (location, offset).
 * Returns its index; a freshly-created entry has tex==NULL until the first
 * depth_snapshot_flush() populates it (mirrors surface_get()'s pattern). */
static u32 depth_surface_get(u32 location, u32 offset)
{
    for (u32 i = 0; i < g.n_depth_surfaces; i++)
        if (g.depth_surfaces[i].location == location && g.depth_surfaces[i].offset == offset)
            return i;
    if (g.n_depth_surfaces >= MAX_SURFACES)
        return 0;
    depth_surface_t* d = &g.depth_surfaces[g.n_depth_surfaces];
    d->location = location;
    d->offset = offset;
    d->tex = NULL;
    return g.n_depth_surfaces++;
}

/* Snapshot the shared depth buffer's CURRENT content into a dedicated,
 * sampleable RGBA8 texture tagged (location, offset) -- called at a zeta
 * pass boundary (the zeta target is about to change), so this captures the
 * just-finished pass's final depth state before anything overwrites it.
 * Structured like gpu_flush() (close/execute/wait/reopen) with the
 * depth-plane copy + CPU readback + reupload inserted in between; the
 * ring-buffer resets at the end mirror gpu_flush()'s own tail exactly,
 * since this executes and fully drains the command list the same way. */
static void depth_snapshot_flush(u32 location, u32 offset)
{
    if (!g.depth || !g.depth_readback || !honor_depth_rt())
        return;

    /* s31 FIX 1: under per-zeta-target tracking, this pass's depth lives in
     * ITS OWN buffer, not the shared g.depth -- snapshot the right one. */
    ID3D12Resource* zres = zdepth_find_res(location, offset);
    ID3D12Resource* dres = zres ? zres : g.depth;

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = dres;
    b.Transition.Subresource = 0;   /* depth plane only */
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    const u32 depth_pitch = (g.width * 4 + 255) & ~255u;
    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = dres;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;       /* plane 0 = depth for D32_FLOAT_S8X24_UINT */
    dst.pResource = g.depth_readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = g.width;
    dst.PlacedFootprint.Footprint.Height = g.height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = depth_pitch;
    /* s31 FIX 2: per-zeta-target buffers are sized >= the canvas; copy
     * exactly the canvas-sized top-left region the readback is sized for. */
    D3D12_BOX zbox = {0, 0, 0, g.width, g.height, 1};
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, &zbox);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    g.list->lpVtbl->Close(g.list);
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    gpu_wait();

    u8* src_px = NULL;
    D3D12_RANGE rr = {0, (size_t)depth_pitch * g.height};
    g.depth_readback->lpVtbl->Map(g.depth_readback, 0, &rr, (void**)&src_px);
    u8* rgba = malloc((size_t)g.width * g.height * 4);
    if (rgba && src_px) {
        for (u32 y = 0; y < g.height; y++) {
            const float* row = (const float*)(src_px + (size_t)y * depth_pitch);
            for (u32 x = 0; x < g.width; x++) {
                /* Same 8-bit approximation decode_texel()'s DEPTH24_D8 case
                 * already uses for guest-memory depth textures: broadcast
                 * one grayscale byte, opaque alpha -- keeps this snapshot's
                 * sampled value consistent with what the raw-memory path
                 * would have produced had the data actually been there. */
                float dv = row[x];
                if (dv < 0.0f) dv = 0.0f;
                if (dv > 1.0f) dv = 1.0f;
                const u8 d8 = (u8)(dv * 255.0f + 0.5f);
                u8* px = rgba + ((size_t)y * g.width + x) * 4;
                px[0] = px[1] = px[2] = d8;
                px[3] = 255;
            }
        }
    }
    D3D12_RANGE wr = {0, 0};
    g.depth_readback->lpVtbl->Unmap(g.depth_readback, 0, &wr);

    g.alloc->lpVtbl->Reset(g.alloc);
    g.list->lpVtbl->Reset(g.list, g.alloc, NULL);
    g.vb_used = 0;
    g.cb_used = 0;
    g.srv_ring_used = 0;
    g.upload_used = 0;

    if (rgba) {
        ID3D12Resource* tex = create_texture_rgba(rgba, g.width, g.height);
        free(rgba);
        if (tex) {
            const u32 idx = depth_surface_get(location, offset);
            if (g.depth_surfaces[idx].tex)
                g.depth_surfaces[idx].tex->lpVtbl->Release(g.depth_surfaces[idx].tex);
            g.depth_surfaces[idx].tex = tex;
            srv_write(SRV_DEPTH_BASE + idx, tex);
            g.depth_snapshot_count++;
            printf("[depthrt] snapshot #%u: zeta (loc=%u off=0x%X) -> %ux%u RGBA8"
                   " (srv slot %u)\n", g.depth_snapshot_count, location, offset,
                   g.width, g.height, SRV_DEPTH_BASE + idx);
        }
    }
}

/* ---------------------------------------------------------------------------
 * RSX_DEPTH_DUMP_PRE / RSX_DEPTH_DUMP_POST (session s29, coordinator's
 * mechanism-pin follow-up, scratch/s29_draw803_occluder.md): a raw
 * float32-per-texel depth-buffer readback, independent of RSX_DEPTH_RT
 * (which snapshots at zeta-boundary switches, only, and re-encodes to 8-bit
 * grayscale for texture reuse -- this dumps the RAW shared depth resource
 * at an EXACT draw-index boundary chosen by the caller, unencoded, so a
 * Python diff can name individual differing texels precisely).
 * `RSX_DEPTH_DUMP_PRE=idx:path[,idx:path...]` dumps immediately BEFORE that
 * draw index is processed (no GPU work for it recorded yet).
 * `RSX_DEPTH_DUMP_POST=idx:path[,idx:path...]` dumps immediately AFTER that
 * draw's DrawInstanced is recorded, using the exact same Close/Execute/Wait
 * flush depth_snapshot_flush() already uses in production (15d7900) so the
 * GPU has actually retired the draw before the copy -- this is a proven
 * pattern in this file, not a new synchronization primitive. Reuses the
 * existing g.depth_readback scratch resource (already sized for this exact
 * copy shape). Additive, env-gated, no effect unless armed. */
typedef struct { int idx; char path[260]; } depth_dump_t;
static depth_dump_t g_ddump_pre[8];
static int g_ddump_pre_n = 0;
static depth_dump_t g_ddump_post[8];
static int g_ddump_post_n = 0;

static void parse_ddump_list(const char* env, depth_dump_t* arr, int* n, int cap)
{
    if (!env) return;
    const char* p = env;
    while (*p && *n < cap) {
        const int idx = atoi(p);
        const char* colon = strchr(p, ':');
        if (!colon) break;
        const char* start = colon + 1;
        const char* comma = strchr(start, ',');
        size_t len = comma ? (size_t)(comma - start) : strlen(start);
        if (len >= sizeof(arr[*n].path)) len = sizeof(arr[*n].path) - 1;
        memcpy(arr[*n].path, start, len);
        arr[*n].path[len] = 0;
        arr[*n].idx = idx;
        (*n)++;
        p = comma ? comma + 1 : start + len;
    }
}

static const char* ddump_lookup(const depth_dump_t* arr, int n, u32 draw_idx)
{
    for (int i = 0; i < n; i++)
        if ((u32)arr[i].idx == draw_idx)
            return arr[i].path;
    return NULL;
}

/* Flushes the command list (same shape as depth_snapshot_flush()) and
 * writes the shared depth resource's RAW float32 texels, row-major,
 * g.width*g.height*4 bytes, no header -- a Python script does the diffing. */
static void dump_depth_raw(const char* path)
{
    if (!g.depth || !g.depth_readback) {
        printf("[ddump] no depth resource, skip %s\n", path);
        return;
    }
    /* s31 FIX 1: dump the buffer bound by the most recent draw (under
     * per-zeta-target tracking the shared g.depth is only a fallback). */
    ID3D12Resource* dres = g_zdump_res ? g_zdump_res : g.depth;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = dres;
    b.Transition.Subresource = 0;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    const u32 depth_pitch = (g.width * 4 + 255) & ~255u;
    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = dres;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    dst.pResource = g.depth_readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = g.width;
    dst.PlacedFootprint.Footprint.Height = g.height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = depth_pitch;
    /* s31 FIX 2: same canvas-sized box rationale as depth_snapshot_flush */
    D3D12_BOX zbox = {0, 0, 0, g.width, g.height, 1};
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, &zbox);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    g.list->lpVtbl->Close(g.list);
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    gpu_wait();

    u8* src_px = NULL;
    D3D12_RANGE rr = {0, (size_t)depth_pitch * g.height};
    g.depth_readback->lpVtbl->Map(g.depth_readback, 0, &rr, (void**)&src_px);
    if (src_px) {
        FILE* f = fopen(path, "wb");
        if (f) {
            for (u32 y = 0; y < g.height; y++)
                fwrite(src_px + (size_t)y * depth_pitch, 1, (size_t)g.width * 4, f);
            fclose(f);
            printf("[ddump] wrote %s (%ux%u float32, raw)\n", path, g.width, g.height);
        } else {
            printf("[ddump] cannot write %s\n", path);
        }
    } else {
        printf("[ddump] map failed for %s\n", path);
    }
    D3D12_RANGE wr = {0, 0};
    g.depth_readback->lpVtbl->Unmap(g.depth_readback, 0, &wr);

    g.alloc->lpVtbl->Reset(g.alloc);
    g.list->lpVtbl->Reset(g.list, g.alloc, NULL);
    g.vb_used = 0;
    g.cb_used = 0;
    g.srv_ring_used = 0;
    g.upload_used = 0;
}

/* depth_pass_track() (s27 part 3) is defined further down, right after
 * decode_render_state()/render_state_t -- it needs both and they're not
 * declared yet at this point in the file. */
static void depth_pass_track(const rsx_dispatch* rsx);

static void write_ppm(const char* path, const u8* px, u32 pitch, u32 w, u32 h)
{
    FILE* f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); return; }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (u32 y = 0; y < h; y++) {
        const u8* row = px + (size_t)y * pitch;
        for (u32 x = 0; x < w; x++)
            fwrite(row + x * 4, 1, 3, f); /* RGBA -> RGB */
    }
    fclose(f);
    printf("[gpu] wrote %s\n", path);
}

/* Flush the recorded list, read one surface back, emit a .ppm, reopen. */
static void gpu_readback_surface(u32 surf_idx, const char* path)
{
    ID3D12Resource* rt = g.surfaces[surf_idx].tex;
    /* s31 FIX 2: surfaces carry their own logical dims now. */
    const u32 sw = g.surfaces[surf_idx].w ? g.surfaces[surf_idx].w : g.width;
    const u32 sh = g.surfaces[surf_idx].h ? g.surfaces[surf_idx].h : g.height;
    const u32 spitch = (sw * 4 + 255) & ~255u;

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
    dst.pResource = g.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = sw;
    dst.PlacedFootprint.Footprint.Height = sh;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = spitch;
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    g.list->lpVtbl->Close(g.list);
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    gpu_wait();

    u8* px = NULL;
    D3D12_RANGE rr = {0, (size_t)spitch * sh};
    g.readback->lpVtbl->Map(g.readback, 0, &rr, (void**)&px);
    write_ppm(path, px, spitch, sw, sh);
    D3D12_RANGE wr = {0, 0};
    g.readback->lpVtbl->Unmap(g.readback, 0, &wr);

    /* reopen for further work */
    g.alloc->lpVtbl->Reset(g.alloc);
    g.list->lpVtbl->Reset(g.list, g.alloc, NULL);
    D3D12_VIEWPORT vp = {0, 0, (float)g.width, (float)g.height, 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, (LONG)g.width, (LONG)g.height};
    g.list->lpVtbl->RSSetViewports(g.list, 1, &vp);
    g.list->lpVtbl->RSSetScissorRects(g.list, 1, &sc);
    g.vb_used = 0;
    g.cb_used = 0;
    g.srv_ring_used = 0;
    g.upload_used = 0;   /* copies completed; recycle the staging arena */
}

/* Mid-frame flush: execute the accumulated command list, wait for the GPU,
 * then reopen it and recycle the per-flush ring buffers (vertex buffer,
 * constant ring, SRV table ring). Render/depth surfaces are committed
 * resources that keep their contents across the flush, and the clears
 * already recorded have executed, so this is transparent to the frame being
 * built. Used when the vertex upload buffer would overflow so that a capture
 * with more geometry than one buffer-full can still draw every batch. */
static u32 g_flush_count;
static void gpu_flush(void)
{
    g.list->lpVtbl->Close(g.list);
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    gpu_wait();

    g.alloc->lpVtbl->Reset(g.alloc);
    g.list->lpVtbl->Reset(g.list, g.alloc, NULL);
    D3D12_VIEWPORT vp = {0, 0, (float)g.width, (float)g.height, 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, (LONG)g.width, (LONG)g.height};
    g.list->lpVtbl->RSSetViewports(g.list, 1, &vp);
    g.list->lpVtbl->RSSetScissorRects(g.list, 1, &sc);
    g.vb_used = 0;
    g.cb_used = 0;
    g.srv_ring_used = 0;
    /* All texture-upload copies recorded before this flush have completed on
     * the GPU (gpu_wait above), so the staging arena can be recycled. Without
     * this the bump arena overflows once a frame references more texture bytes
     * than the arena holds. */
    g.upload_used = 0;
    g_flush_count++;
}

/* ---------------------------------------------------------------------------
 * Guest texture decode + cache
 *
 * Decodes NV40 fragment textures out of the capture's guest memory into
 * RGBA8 D3D12 textures. Linear (LN) images use the TEX_SIZE1 pitch;
 * non-linear power-of-two images are stored "swizzled" = Morton/Z-order
 * with the X coordinate in the even bit positions and the excess bits of
 * the longer dimension appended above the interleave (public layout, see
 * docs/PSDEVWIKI_REFS.md and Mesa's nvfx swizzle helpers).
 * -----------------------------------------------------------------------*/

static u32 log2_u32(u32 v)
{
    u32 n = 0;
    while (v > 1) { v >>= 1; n++; }
    return n;
}

/* Texel index of (x, y) inside a swizzled w x h (powers of two) image. */
static u32 morton_index(u32 x, u32 y, u32 log2w, u32 log2h)
{
    u32 idx = 0, shift = 0;
    while (log2w || log2h) {
        if (log2w) { idx |= (x & 1) << shift; x >>= 1; shift++; log2w--; }
        if (log2h) { idx |= (y & 1) << shift; y >>= 1; shift++; log2h--; }
    }
    return idx;
}

/* Decode one uncompressed texel to RGBA8 through the TEX_SWIZZLE remap
 * crossbar. Source components are indexed in gcm order A=0 R=1 G=2 B=3
 * (an A8R8G8B8 texel is a big-endian AARRGGBB word, i.e. bytes A,R,G,B);
 * the remap word's low-byte 2-bit fields select the source for out
 * A/R/G/B from bits [1:0]/[3:2]/[5:4]/[7:6], and the second byte carries
 * a per-component op in the same order (bits [9:8]/[11:10]/[13:12]/
 * [15:14]): 0 = force zero, 1 = force one, 2 = use the crossbar select.
 * Identity is 0xAAE4. The capture's CRI movie frame uses 0xAA93 (RGBA
 * byte order sampled through the ARGB format, all ops = remap). */
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
    case RSX_TEX_FMT_B8:
        s[0] = 255;
        s[1] = s[2] = s[3] = p[0];
        break;
    case RSX_TEX_FMT_A4R4G4B4: {                /* BE 16-bit ARGB nibbles */
        const u16 v = (u16)((p[0] << 8) | p[1]);
        s[0] = (u8)(((v >> 12) & 0xF) * 17);
        s[1] = (u8)(((v >> 8) & 0xF) * 17);
        s[2] = (u8)(((v >> 4) & 0xF) * 17);
        s[3] = (u8)((v & 0xF) * 17);
        break;
    }
    case RSX_TEX_FMT_A1R5G5B5: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        s[0] = (v & 0x8000) ? 255 : 0;
        s[1] = (u8)(((v >> 10) & 0x1F) * 255 / 31);
        s[2] = (u8)(((v >> 5) & 0x1F) * 255 / 31);
        s[3] = (u8)((v & 0x1F) * 255 / 31);
        break;
    }
    case RSX_TEX_FMT_R5G6B5: {                  /* BE 16-bit 565, no alpha */
        const u16 v = (u16)((p[0] << 8) | p[1]);
        s[0] = 255;
        s[1] = (u8)(((v >> 11) & 0x1F) * 255 / 31);
        s[2] = (u8)(((v >> 5)  & 0x3F) * 255 / 63);
        s[3] = (u8)((v & 0x1F) * 255 / 31);
        break;
    }
    case RSX_TEX_FMT_G8B8:                       /* two 8-bit channels G,B */
        /* NV40 G8B8: MSB-first byte pair = G (p[0]), B (p[1]); no native R
         * (replicate G so grayscale/identity reads are sane), opaque alpha.
         * The crossbar remap word then selects the shader-visible channels. */
        s[0] = 255;
        s[1] = p[0];
        s[2] = p[0];
        s[3] = p[1];
        break;
    case RSX_TEX_FMT_DEPTH24_D8:
        /* D24S8 sampled as a color: broadcast the depth (top byte of the
         * BE word's 24-bit depth field) — 8-bit approximation */
        s[0] = 255;
        s[1] = s[2] = s[3] = p[0];
        break;
    default:                                    /* A8R8G8B8 */
        s[0] = p[0];
        s[1] = p[1];
        s[2] = p[2];
        s[3] = p[3];
        break;
    }
    d[0] = remap_comp(s, remap, 1);             /* R */
    d[1] = remap_comp(s, remap, 2);             /* G */
    d[2] = remap_comp(s, remap, 3);             /* B */
    d[3] = remap_comp(s, remap, 0);             /* A */
}

/* ---------------------------------------------------------------------------
 * B1: NV4097 render / sampler state -> D3D12
 *
 * The register file already holds every state method the capture wrote; here
 * we decode the ones that gate the pixel result and translate them to D3D12
 * pipeline / sampler descriptors. Method numbers come from tools/nv40_methods.py
 * (envytools rnndb nv30-40_3d). The gcm enum values (blend factor/equation,
 * comparison func, texture filter/wrap) are hardware ISA facts documented in
 * envytools rnndb + the Mesa nv30 driver + psdevwiki; RPCS3 was consulted only
 * as a read-only fact oracle for the bitfield positions (no code copied).
 * -----------------------------------------------------------------------*/

/* Method offsets (byte address >> 0; used with rsx_dsp_reg's word index) */
#define M_ALPHA_TEST_ENABLE  0x0304
#define M_ALPHA_FUNC         0x0308
#define M_ALPHA_REF          0x030C
#define M_BLEND_ENABLE       0x0310
#define M_BLEND_SFACTOR      0x0314   /* rgb[0:15] a[16:31]                  */
#define M_BLEND_DFACTOR      0x0318
#define M_BLEND_EQUATION     0x0320   /* rgb[0:15] a[16:31]                  */
#define M_DEPTH_FUNC         0x0A6C
#define M_DEPTH_WRITE        0x0A70
#define M_DEPTH_TEST_ENABLE  0x0A74
#define M_CULL_FACE          0x1830   /* 0x404=FRONT 0x405=BACK 0x408=F&B    */
#define M_FRONT_FACE         0x1834   /* 0x900=CW 0x901=CCW                  */
#define M_CULL_FACE_ENABLE   0x183C
#define M_COLOR_MASK         0x0324   /* per-channel byte mask (nv40_3d.xml.h:
                                        * B=[0:7] G=[8:15] R=[16:23] A=[24:31],
                                        * Mesa/nouveau nv40 3D engine, MIT/X11) */

/* gcm comparison func (0x200..0x207) -> D3D12_COMPARISON_FUNC (1..8) */
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
    case 0x0207: default: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

/* gcm blend factor -> D3D12_BLEND (color form; the *_alpha row picks the
 * alpha-channel equivalents where D3D12 distinguishes them) */
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
    case 0x8001: return alpha ? D3D12_BLEND_BLEND_FACTOR : D3D12_BLEND_BLEND_FACTOR;   /* constant color */
    case 0x8002: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;   /* constant alpha (D3D12: single factor) */
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default:     return alpha ? D3D12_BLEND_ONE : D3D12_BLEND_ONE;
    }
}

/* gcm blend equation -> D3D12_BLEND_OP */
static D3D12_BLEND_OP gcm_blend_op(u32 e)
{
    switch (e) {
    case 0x8006: return D3D12_BLEND_OP_ADD;             /* FUNC_ADD          */
    case 0x8007: return D3D12_BLEND_OP_MIN;             /* MIN               */
    case 0x8008: return D3D12_BLEND_OP_MAX;             /* MAX               */
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;        /* FUNC_SUBTRACT     */
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;    /* FUNC_REV_SUBTRACT */
    default:     return D3D12_BLEND_OP_ADD;
    }
}

/* B1 state-group kill switches (env: set to "0" to disable a group).
 * Default all ON; used to bisect regressions and as documented flags. */
static int g_b1_blend = 1, g_b1_depth = 1, g_b1_cull = 1, g_b1_samp = 1;

static void b1_read_env(void)
{
    char* e;
    if ((e = getenv("YZ_B1_BLEND")) && e[0] == '0') g_b1_blend = 0;
    if ((e = getenv("YZ_B1_DEPTH")) && e[0] == '0') g_b1_depth = 0;
    if ((e = getenv("YZ_B1_CULL"))  && e[0] == '0') g_b1_cull  = 0;
    if ((e = getenv("YZ_B1_SAMP"))  && e[0] == '0') g_b1_samp  = 0;
}

/* Decoded render state that folds into the PSO (and thus the PSO cache key) */
typedef struct {
    u32 blend_enable;
    u32 sf_rgb, df_rgb, sf_a, df_a, eq_rgb, eq_a;
    u32 depth_test, depth_write;
    u32 depth_func;
    u32 cull_enable, cull_face, front_face;
    u32 color_mask;
} render_state_t;

static void decode_render_state(const rsx_dispatch* rsx, render_state_t* rs)
{
    memset(rs, 0, sizeof(*rs));
    rs->blend_enable = rsx_dsp_reg(rsx, M_BLEND_ENABLE) & 1;
    const u32 sf = rsx_dsp_reg(rsx, M_BLEND_SFACTOR);
    const u32 df = rsx_dsp_reg(rsx, M_BLEND_DFACTOR);
    const u32 eq = rsx_dsp_reg(rsx, M_BLEND_EQUATION);
    rs->sf_rgb = sf & 0xFFFF; rs->sf_a = sf >> 16;
    rs->df_rgb = df & 0xFFFF; rs->df_a = df >> 16;
    rs->eq_rgb = eq & 0xFFFF; rs->eq_a = eq >> 16;
    rs->depth_test  = rsx_dsp_reg(rsx, M_DEPTH_TEST_ENABLE) & 1;
    rs->depth_write = rsx_dsp_reg(rsx, M_DEPTH_WRITE) & 1;
    rs->depth_func  = rsx_dsp_reg(rsx, M_DEPTH_FUNC);
    rs->cull_enable = rsx_dsp_reg(rsx, M_CULL_FACE_ENABLE) & 1;
    rs->cull_face   = rsx_dsp_reg(rsx, M_CULL_FACE);
    rs->front_face  = rsx_dsp_reg(rsx, M_FRONT_FACE);
    /* s31 (scratch/s31_blue_emitter.md): honor the RAW register — 0 is a
     * legitimate game-written "write no color channels" (the character
     * shadow-mask depth-prime pass; treating it as ALL force-wrote the
     * pre-pass's garbage = the blue-character class, 5,635 px -> 0
     * validated in replay_blue_main.c). rsx_dispatch_init now seeds the
     * nv40 reset default (0x01010101), so never-written reads all-on. */
    rs->color_mask = rsx_dsp_reg(rsx, M_COLOR_MASK);
}

/* Called once per draw, right after the draw's own vertex fetch (BEFORE any
 * GPU-side vertex-buffer upload for it): detects a zeta-target pass
 * boundary and flushes the JUST-FINISHED pass's depth content into a
 * sampleable snapshot before the new pass's clear/writes would otherwise
 * make it unrecoverable. Only passes that actually wrote depth (depth_write
 * seen at least once) are snapshotted, to skip depth-test-only passes with
 * nothing new to capture. (s27 part 3, scratch/s26_fp_bisect.md) */
static void depth_pass_track(const rsx_dispatch* rsx)
{
    if (!honor_depth_rt())
        return;
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(rsx, &sf);
    render_state_t rs;
    decode_render_state(rsx, &rs);
    if (g.cur_zeta_valid &&
        (sf.zeta_location != g.cur_zeta_loc || sf.zeta_offset != g.cur_zeta_off)) {
        if (g.cur_zeta_had_write)
            depth_snapshot_flush(g.cur_zeta_loc, g.cur_zeta_off);
        g.cur_zeta_had_write = 0;
    }
    g.cur_zeta_loc = sf.zeta_location;
    g.cur_zeta_off = sf.zeta_offset;
    g.cur_zeta_valid = 1;
    if (rs.depth_write)
        g.cur_zeta_had_write = 1;
}

/* Populate a D3D12 PSO descriptor's blend/depth/raster/DSV fields from rs.
 * Depth is only enabled if the shared depth buffer exists. */
static void apply_render_state(D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd,
                               const render_state_t* rs)
{
    D3D12_RENDER_TARGET_BLEND_DESC* b = &pd->BlendState.RenderTarget[0];
    /* nv40_3d COLOR_MASK byte layout (Mesa/nouveau nv40_3d.xml.h, MIT/X11):
     * B=[0:7] G=[8:15] R=[16:23] A=[24:31]; any nonzero byte = channel on. */
    b->RenderTargetWriteMask =
        (((rs->color_mask       ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_BLUE  : 0) |
        (((rs->color_mask >>  8 ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_GREEN : 0) |
        (((rs->color_mask >> 16 ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_RED   : 0) |
        (((rs->color_mask >> 24 ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_ALPHA : 0);
    if (rs->blend_enable && g_b1_blend) {
        b->BlendEnable   = TRUE;
        b->SrcBlend      = gcm_blend_factor(rs->sf_rgb, 0);
        b->DestBlend     = gcm_blend_factor(rs->df_rgb, 0);
        b->BlendOp       = gcm_blend_op(rs->eq_rgb);
        b->SrcBlendAlpha = gcm_blend_factor(rs->sf_a, 1);
        b->DestBlendAlpha= gcm_blend_factor(rs->df_a, 1);
        b->BlendOpAlpha  = gcm_blend_op(rs->eq_a);
    }

    pd->RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    /* RSX front face: 0x900 = CW, 0x901 = CCW. D3D12 flips winding vs GL
     * because our viewport epilogue negates Y (window-y-down -> NDC-y-up),
     * so a CCW-front guest surface presents as CW-front here. Match RPCS3's
     * d3d convention: FrontCounterClockwise = (front_face == CW). */
    if (rs->cull_enable && rs->cull_face && g_b1_cull) {
        const u32 f = rs->cull_face;
        pd->RasterizerState.CullMode = (f == 0x0404) ? D3D12_CULL_MODE_FRONT
                                     : (f == 0x0405) ? D3D12_CULL_MODE_BACK
                                                     : D3D12_CULL_MODE_NONE; /* FRONT_AND_BACK */
    } else {
        pd->RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    }
    /* RSX front-face 0x900=CW, 0x901=CCW. Our viewport epilogue negates Y,
     * which mirrors every triangle and reverses its apparent winding, so the
     * front sense must be taken straight from the guest value (a CCW-front
     * guest triangle presents as the front we keep here). Empirically this
     * matches the capture's back-face-culled character geometry. */
    pd->RasterizerState.FrontCounterClockwise = (rs->front_face == 0x0901);

    if (g.depth && g_b1_depth) {
        pd->DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        pd->DepthStencilState.DepthEnable = rs->depth_test ? TRUE : FALSE;
        pd->DepthStencilState.DepthWriteMask =
            rs->depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pd->DepthStencilState.DepthFunc = gcm_cmp(rs->depth_func);
        pd->DepthStencilState.StencilEnable = FALSE;
    }
}

/* ---- sampler state (TEX_FILTER / TEX_ADDRESS / TEX_CONTROL0) ----------- */

/* gcm texture wrap -> D3D12 address mode */
static D3D12_TEXTURE_ADDRESS_MODE gcm_wrap(u32 w)
{
    switch (w & 0xF) {
    case 1: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;         /* WRAP (repeat) */
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;        /* CLAMP_TO_EDGE */
    case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;        /* CLAMP (to border, approx edge) */
    case 6: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    case 7:
    case 8: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

/* Build a D3D12 sampler desc from the texture's filter/wrap/control0 words.
 * Filter word: min = [16:18], mag = [24:26] (gcm: 1=NEAREST 2=LINEAR,
 * mip variants 3..6). Control0: min_lod = [19:30] max_lod = [7:18], both
 * 4.8 fixed point (RPCS3 decode_fxp<4,8>). */
static D3D12_SAMPLER_DESC decode_sampler(const rsx_dsp_texture* t)
{
    D3D12_SAMPLER_DESC sd = {0};
    const u32 minf = (t->filter >> 16) & 0x7;
    const u32 magf = (t->filter >> 24) & 0x7;

    /* min-filter mip mode: 1/2 = base level only (POINT mip), 3/4 = nearest
     * mip (POINT mip), 5/6 = linear mix between mips (LINEAR mip). Whether
     * the in-mip minification is point or linear: NEAREST_* (1,3,5) = point,
     * LINEAR_* (2,4,6) = linear. */
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

    /* 4.8 fixed-point LOD clamps from control0 */
    const u32 max_lod_fx = (t->control0 >> 7)  & 0xFFF;
    const u32 min_lod_fx = (t->control0 >> 19) & 0xFFF;
    sd.MinLOD = (float)min_lod_fx / 256.0f;
    sd.MaxLOD = mip_present ? (float)max_lod_fx / 256.0f : 0.0f;
    if (sd.MaxLOD < sd.MinLOD) sd.MaxLOD = sd.MinLOD;
    sd.MaxAnisotropy = 1;
    return sd;
}

/* A compact key so identical decoded samplers share one descriptor. */
static u32 sampler_key(const rsx_dsp_texture* t)
{
    const u32 minf = (t->filter >> 16) & 0x7;
    const u32 magf = (t->filter >> 24) & 0x7;
    const u32 wrap = t->wrap & 0xFFFFFF;
    const u32 lod  = (t->control0 >> 7) & 0x1FFFFF;  /* min+max LOD fields   */
    return (minf) | (magf << 3) | ((wrap & 0xFFF) << 6) | (lod << 18);
}

/* Find (or decode and cache) the SRV slot for a guest texture descriptor.
 * Returns SRV_WHITE when the texture cannot be decoded. Guest memory is
 * read at first use; later block re-applies over the same memory are not
 * tracked (one decode per distinct descriptor). */
static u32 texture_srv_slot(const rsx_dsp_texture* t)
{
    const u32 remap = t->remap & 0xFFFF;
    for (u32 i = 0; i < g.n_textures; i++) {
        const texcache_t* e = &g.textures[i];
        if (e->location == t->location && e->offset == t->offset &&
            e->format == t->format && e->width == t->width &&
            e->height == t->height && e->pitch == t->pitch && e->remap == remap)
            return e->tex ? SRV_TEXTURE_BASE + i : SRV_WHITE;
    }
    if (g.n_textures >= MAX_TEXTURES) {
        static int warned = 0;
        if (!warned) { printf("[gpu] texture cache full (%u), further textures white\n", MAX_TEXTURES); warned = 1; }
        return SRV_WHITE;
    }
    texcache_t* e = &g.textures[g.n_textures];
    e->location = t->location;
    e->offset = t->offset;
    e->format = t->format;
    e->width = t->width;
    e->height = t->height;
    e->pitch = t->pitch;
    e->remap = remap;
    e->tex = NULL;

    const u32 base_fmt = t->format & RSX_TEX_FMT_BASE_MASK & ~(u32)RSX_TEX_FMT_UNNORM;
    const int linear = (t->format & RSX_TEX_FMT_LINEAR) != 0;
    const u32 w = t->width, h = t->height;
    do {
        if (!w || !h || w > 4096 || h > 4096 || t->dimension != 2 || t->cubemap)
            break;

        /* B1: mip chain. The format field's mip-count (t->mipmaps) says how
         * many levels the guest packed after level 0; each level halves the
         * dimensions (min 1) and is stored consecutively. When there is only
         * one level this collapses to the stage-3 single-level path. */
        u32 n_mips = t->mipmaps ? t->mipmaps : 1;
        if (n_mips > 14) n_mips = 14;

        if (base_fmt == RSX_TEX_FMT_DXT1 || base_fmt == RSX_TEX_FMT_DXT23 ||
            base_fmt == RSX_TEX_FMT_DXT45) {
            /* S3TC blocks are byte-ordered identically on RSX and D3D12:
             * pass through as BC1/BC2/BC3 (no remap; the capture's DXT
             * textures all use the identity crossbar). Levels are packed
             * back to back; BC data is never swizzled. */
            const DXGI_FORMAT dxgi = base_fmt == RSX_TEX_FMT_DXT1 ? DXGI_FORMAT_BC1_UNORM
                                   : base_fmt == RSX_TEX_FMT_DXT23 ? DXGI_FORMAT_BC2_UNORM
                                                                   : DXGI_FORMAT_BC3_UNORM;
            const u32 block = base_fmt == RSX_TEX_FMT_DXT1 ? 8 : 16;
            const u8* src = guest_ptr(t->location, t->offset);
            if (!src)
                break;
            tex_level_t lv[14];
            u32 mw = w, mh = h, off = 0, n = 0;
            for (u32 m = 0; m < n_mips && mw >= 1 && mh >= 1; m++) {
                const u32 bw = (mw + 3) / 4, bh = (mh + 3) / 4;
                if ((u64)t->offset + off + (u64)bw * block * bh > ARENA_SIZE)
                    break;
                lv[n].w = (mw + 3) & ~3u;
                lv[n].h = (mh + 3) & ~3u;
                lv[n].data = src + off;
                lv[n].row_bytes = bw * block;
                lv[n].rows = bh;
                n++;
                off += bw * block * bh;
                if (mw == 1 && mh == 1) break;
                mw = mw > 1 ? mw >> 1 : 1;
                mh = mh > 1 ? mh >> 1 : 1;
            }
            e->tex = create_texture_mipped(dxgi, lv, n);
            break;
        }

        u32 texel_sz;
        switch (base_fmt) {
        case RSX_TEX_FMT_B8:       texel_sz = 1; break;
        case RSX_TEX_FMT_A4R4G4B4:
        case RSX_TEX_FMT_A1R5G5B5:
        case RSX_TEX_FMT_R5G6B5:
        case RSX_TEX_FMT_G8B8:     texel_sz = 2; break;
        case RSX_TEX_FMT_A8R8G8B8:
        case RSX_TEX_FMT_DEPTH24_D8: texel_sz = 4; break;
        default:                   texel_sz = 0; break;
        }
        if (!texel_sz)
            break;
        if (!linear && ((w & (w - 1)) || (h & (h - 1))))
            break;                              /* swizzled implies po2 */
        const u8* src = guest_ptr(t->location, t->offset);
        if (!src)
            break;

        /* s27 part 2 (scratch/s26_fp_bisect.md): the zero-lit-term hunt
         * found EVERY texture unit 2-15 for the blue-patch draws bound to
         * the SAME guest address, a 1x1 texture -- print its raw decoded
         * texel so we can tell whether the dummy/default texture the game
         * (or our replay) binds to unused units is itself black, which
         * would explain any effect reading it collapsing to zero. Additive,
         * env-gated, no effect unless RSX_TEXLOG_RAW is set. */
        if (getenv("RSX_TEXLOG_RAW")) {
            u8 t00[4];
            decode_texel(base_fmt, src, remap, t00);
            printf("[texlograw] off=0x%X fmt=0x%02X %ux%u texel0=(%u,%u,%u,%u)\n",
                   t->offset, t->format, w, h, t00[0], t00[1], t00[2], t00[3]);
        }

        /* Decode each mip level to RGBA8. Linear levels use the level-0 pitch
         * for the base and packed w*texel for reduced levels (the guest can
         * only supply a custom pitch for level 0); swizzled levels are Morton
         * ordered at that level's own dimensions. */
        u8* rgba[14] = {0};
        tex_level_t lv[14];
        u32 mw = w, mh = h, off = 0, n = 0;
        int oom = 0;
        for (u32 m = 0; m < n_mips && mw >= 1 && mh >= 1; m++) {
            const u32 mpitch = (m == 0 && linear && t->pitch) ? t->pitch : mw * texel_sz;
            if ((u64)t->offset + off + (u64)mpitch * mh > ARENA_SIZE)
                break;
            rgba[n] = malloc((size_t)mw * mh * 4);
            if (!rgba[n]) { oom = 1; break; }
            const u32 lw = log2_u32(mw), lh = log2_u32(mh);
            const u8* lsrc = src + off;
            for (u32 y = 0; y < mh; y++) {
                for (u32 x = 0; x < mw; x++) {
                    const u8* p = linear
                        ? lsrc + (size_t)y * mpitch + (size_t)x * texel_sz
                        : lsrc + (size_t)morton_index(x, y, lw, lh) * texel_sz;
                    decode_texel(base_fmt, p, remap, rgba[n] + ((size_t)y * mw + x) * 4);
                }
            }
            lv[n].w = mw; lv[n].h = mh;
            lv[n].data = rgba[n];
            lv[n].row_bytes = mw * 4;
            lv[n].rows = mh;
            n++;
            off += mpitch * mh;
            if (mw == 1 && mh == 1) break;
            mw = mw > 1 ? mw >> 1 : 1;
            mh = mh > 1 ? mh >> 1 : 1;
        }
        if (!oom && n)
            e->tex = create_texture_mipped(DXGI_FORMAT_R8G8B8A8_UNORM, lv, n);
        for (u32 m = 0; m < n; m++)
            free(rgba[m]);
    } while (0);

    if (e->tex)
        srv_write(SRV_TEXTURE_BASE + g.n_textures, e->tex);
    else
        printf("[gpu] tex fallback: off=0x%X fmt=0x%02X %ux%u pitch=%u %s dim=%u cube=%u mips=%u loc=%u\n",
               t->offset, t->format, w, h, t->pitch, linear ? "linear" : "swizzled",
               t->dimension, t->cubemap, t->mipmaps, t->location);
    if (getenv("YZ_B1_TEXLOG"))
        printf("[texlog] off=0x%X fmt=0x%02X %ux%u mips=%u filter minf=%u magf=%u %s\n",
               t->offset, t->format, w, h, t->mipmaps,
               (t->filter >> 16) & 7, (t->filter >> 24) & 7,
               e->tex ? "ok" : "FALLBACK");
    const u32 slot = e->tex ? SRV_TEXTURE_BASE + g.n_textures : SRV_WHITE;
    g.n_textures++;
    return slot;
}

/* ---------------------------------------------------------------------------
 * Stage 4: NV40 shader translation -> HLSL -> PSO cache
 *
 * At draw time the active transform (vertex) program is read out of the
 * dispatcher's VP instruction store (starting at VP_START_FROM_ID) and the
 * active fragment program out of guest memory (SHADER_PROGRAM offset, incl.
 * any inline-constant patches the game wrote there); both are decompiled to
 * HLSL, D3DCompiled, and cached as a PSO keyed on the combined ucode hash.
 * Draws whose pair fails any step fall back to the fixed stage-3 pipeline.
 * -----------------------------------------------------------------------*/

static const char* g_dump_dir;     /* --dump-shaders target (NULL = off)   */
static u32 g_xlat_fail, g_xlat_ok; /* distinct shader pairs                */

/* ---------------------------------------------------------------------------
 * RSX_FP_FORCE_STAGE (session s26, scratch/s26_fp_bisect.md): forced-output
 * pixel bisection for ONE named shader (fp_4792e42b9f86ad33, the s25g/s25e
 * black-character FP). Env-gated, default OFF, no effect on any other PSO
 * key. On a match, the generated ps_hlsl text is patched (post-decompile,
 * pre-D3DCompile) to capture a named intermediate value into a fresh
 * `dbg_stage` local right after the line that computes it, then the FP's
 * final `return h[0];` is replaced with `return dbg_stage;` so the rendered
 * image shows that one pipeline stage's value directly instead of the final
 * shaded color. Vector-valued stages (normals/half-vectors, range [-1,1])
 * are remapped *0.5+0.5 so a legitimate unit vector doesn't itself read as
 * "black"; scalar stages (dot/saturate light terms, already [0,1]) and the
 * raw texture sample are passed through unchanged. This is a text patch of
 * ONE cached shader's HLSL, not decompiler surgery -- least invasive surface
 * per the task's instruction, and it cannot perturb any other shader (the
 * key match is exact and this is the sole call site). */
#define FP_FORCE_TARGET_KEY 0x4792e42b9f86ad33ULL

typedef struct { const char* anchor; const char* capture; } fp_force_stage_t;

/* Anchors are verbatim substrings of the LIVE decompiler output, confirmed
 * MEASURED byte-identical to scratch/shd_dump2/fp_4792e42b9f86ad33.hlsl via
 * `diff` this session (scratch/s26_shaders/fp_4792e42b9f86ad33.hlsl) before
 * writing these strings, so a strstr() match is not a guess. */
static const fp_force_stage_t g_fp_force_stages[] = {
    /* 0: raw tex0 (diffuse) sample, unmodified colors */
    { "rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).xyzw).xy)); h[5].xyzw = _v.xyzw; }",
      "dbg_stage = h[5];" },
    /* 1: h[1] = normalize(tc2.xyz), the primary shading normal (s25 diag's
     *    "the intermediate register the diag doc identifies") */
    { "float4(normalize(((input.tc2).xyzw).xyz), 1.0)); h[1].xyz = _v.xyz; }",
      "dbg_stage = float4(h[1].xyz * 0.5 + 0.5, 1.0);" },
    /* 2: first fixed-direction fill-light term, saturate(dot(h1,-L1)) */
    { "dot(((h[1]).xyzw).xyz, (-((float4(-0.317166,0.812425,0.489256,1)).xyzw)).xyz)); _v = saturate(_v); h[4].z = _v.z; }",
      "dbg_stage = float4(h[4].zzz, 1.0);" },
    /* 3: second fixed-direction fill-light term */
    { "dot(((h[1]).xyzw).xyz, (-((float4(0.317166,-0.812425,-0.489256,1.649)).xyzw)).xyz)); _v = saturate(_v); h[5].w = _v.w; }",
      "dbg_stage = float4(h[5].www, 1.0);" },
    /* 4: h[2] = normalize(tc3.xyz), the dynamic per-vertex key-light dir
     *    (VP-computed, traced to a point-light direction relative to
     *    vp_c[56] -- see scratch/s26_fp_bisect.md) */
    { "float4(normalize(((input.tc3).xyzw).xyz), 1.0)); h[2].xyz = _v.xyz; }",
      "dbg_stage = float4(h[2].xyz * 0.5 + 0.5, 1.0);" },
    /* 5: key-light term, saturate(dot(h1,-h2)), captured BEFORE it is
     *    overwritten by its own log2() a few lines later */
    { "dot(((h[1]).xyzw).xyz, (-((h[2]).xyzw)).xyz)); _v = saturate(_v); h[4].w = _v.w; }",
      "dbg_stage = float4(h[4].www, 1.0);" },
    /* 6: half-vector h[5] = normalize(r[1]) (r[1] = h2 + a light-dir literal) */
    { "float4(normalize(((r[1]).xyzw).xyz), 1.0)); h[5].xyz = _v.xyz; }",
      "dbg_stage = float4(h[5].xyz * 0.5 + 0.5, 1.0);" },
    /* 7: specular-ish term, saturate(dot(h1,-halfvec)) */
    { "dot(((h[1]).xyzw).xyz, (-((h[5]).xyzw)).xyz)); _v = saturate(_v); h[5].x = _v.x; }",
      "dbg_stage = float4(h[5].xxx, 1.0);" },
    /* 8: first specular-power result, exp2(log2(ndotl)*(4,4,4,256)) */
    { "exp2(((h[4]).wwww).x)); h[7].z = _v.z; }",
      "dbg_stage = float4(h[7].zzz, 1.0);" },
    /* 9: second specular-power result (same pow-via-log2/exp2 idiom, other
     *    light) */
    { "exp2(((h[1]).wwww).x)); h[4].w = _v.w; }",
      "dbg_stage = float4(h[4].www, 1.0);" },
    /* 10: h[3], the diffuse accumulation term feeding the final combine */
    { "(((r[1]).xyzw) * ((h[4]).xxxx) + ((h[5]).xyzw)); h[3].xyz = _v.xyz; }",
      "dbg_stage = float4(h[3].xyz, 1.0);" },
    /* 11: h[4], the near-final accumulation (one add away from the return) */
    { "(((h[3]).xyzw) * ((h[5]).xyzw) + ((h[0]).xyzw)); h[4].xyz = _v.xyz; }",
      "dbg_stage = float4(h[4].xyz, 1.0);" },
    /* 12: input.tc0 (the diffuse UV itself) as a heatmap (r=frac(u),
     *      g=frac(v)), added after stage 0 showed the tex0 sample ITSELF
     *      already black at the character silhouette -- this checks whether
     *      the UV feeding that sample is degenerate. frac() folds the raw
     *      coordinate into canonical [0,1) the same way a WRAP sampler
     *      would resolve it, so this reads the effective sample position,
     *      not a signed value that a UNORM target would clip to black for
     *      any negative component (measured ATTR8 span for draw 1040 is
     *      x in [-2.51,3.49] -- a naive unclamped dump would read black for
     *      every negative-u vertex regardless of the real sampled texel).
     *      Anchor is the just-inserted dbg_stage declaration (always
     *      present, always before any other FP code runs). */
    { " float4 dbg_stage = (float4)0;",
      " dbg_stage = float4(frac(input.tc0.x), frac(input.tc0.y), 0.0, 1.0);" },
    /* 13: unconditional constant white -- a control, not a data probe. If
     * the character-silhouette region is a GEOMETRY HOLE (no fragment
     * rasterized there at all, background bleeding through) rather than a
     * drawn-but-wrong-colored fragment, every earlier stage would read
     * identically black regardless of what's forced, since forcing the FP's
     * *output* cannot paint a pixel the rasterizer never shades. This stage
     * settles that confound: if the silhouette is still black here, there
     * is no fragment there and the defect is upstream of the FP entirely
     * (geometry/culling/restart), not a wrong-color FP output. */
    { " float4 dbg_stage = (float4)0;",
      " dbg_stage = float4(1.0, 1.0, 1.0, 1.0);" },
};
#define FP_FORCE_NSTAGES (int)(sizeof(g_fp_force_stages) / sizeof(g_fp_force_stages[0]))

static int g_fp_force_stage = -1; /* RSX_FP_FORCE_STAGE, parsed once in main() */

static int fp_insert_after(char* buf, size_t cap, const char* anchor, const char* text)
{
    char* pos = strstr(buf, anchor);
    if (!pos) return 0;
    pos += strlen(anchor);
    const size_t tail = strlen(pos);
    const size_t ins = strlen(text);
    if (strlen(buf) + ins >= cap) return 0;
    memmove(pos + ins, pos, tail + 1); /* +1: NUL */
    memcpy(pos, text, ins);
    return 1;
}

static int fp_replace_first(char* buf, size_t cap, const char* needle, const char* repl)
{
    char* pos = strstr(buf, needle);
    if (!pos) return 0;
    const size_t nlen = strlen(needle), rlen = strlen(repl);
    const size_t tail = strlen(pos + nlen);
    if (rlen > nlen && strlen(buf) + (rlen - nlen) >= cap) return 0;
    memmove(pos + rlen, pos + nlen, tail + 1);
    memcpy(pos, repl, rlen);
    return 1;
}

/* Patches ps_hlsl in place for the target key only. Returns 1 if the patch
 * (declaration + capture + return-replace) all applied, 0 otherwise (any
 * failure leaves ps_hlsl untouched by convention of the caller re-decompiling
 * fresh each time this is invoked). */
static int fp_apply_force_stage(char* ps_hlsl, size_t cap, int stage)
{
    if (stage < 0 || stage >= FP_FORCE_NSTAGES) {
        printf("[fpforce] RSX_FP_FORCE_STAGE=%d out of range [0,%d)\n", stage, FP_FORCE_NSTAGES);
        return 0;
    }
    if (!fp_insert_after(ps_hlsl, cap,
            "float4 r[48]; float4 h[48];",
            " float4 dbg_stage = (float4)0;")) {
        printf("[fpforce] declaration anchor not found\n");
        return 0;
    }
    if (!fp_insert_after(ps_hlsl, cap, g_fp_force_stages[stage].anchor,
            g_fp_force_stages[stage].capture)) {
        printf("[fpforce] stage %d capture anchor not found\n", stage);
        return 0;
    }
    if (!fp_replace_first(ps_hlsl, cap, "return h[0];", "return dbg_stage;")) {
        printf("[fpforce] return-statement anchor not found\n");
        return 0;
    }
    printf("[fpforce] stage %d patch applied OK\n", stage);
    return 1;
}

/* ---------------------------------------------------------------------------
 * RSX_FP_FORCE_STAGE2 (session s27 part 1, scratch/s26_fp_bisect.md part 5's
 * "residual 2" follow-up): forced-output probe for ONE named shader
 * (fp_47f48eae1f650e68, the woman/zombie blue-patch FP), independent of and
 * additive to RSX_FP_FORCE_STAGE above (different target key, different env
 * var, same insert/replace mechanism, reuses fp_insert_after/
 * fp_replace_first). Two stages:
 *   0: tc7.w -- the raw interpolated w feeding the line-47 projective divide
 *      `rsx_tex[13].Sample(rsx_samp[13], input.tc7.xy / input.tc7.w)`, the
 *      part-5-named degenerate-w suspect.
 *   1: the actual line-95 lerp weight (h[0].w at the point it is consumed by
 *      `h[0] = h[0].w*(h[4]-h[0])+h[0]`) -- traced by static read of the
 *      live-dumped HLSL (scratch/s27_shaders/fp_47f48eae1f650e68.hlsl,
 *      confirmed byte-identical to the pre-existing
 *      scratch/s26r_shaders/ copy via `diff` this session) to be set by the
 *      single line `h[0].w = input.fog.xxxx.w;` and never touched again
 *      before line 95 -- NOT the log2/reflection chain part 5 guessed (that
 *      chain writes h[0].x via exp2, not h[0].w; see the s27 writeup).
 * Both stages encode the probed scalar as R=saturate(x), G=1 if |x|<1e-3
 * else 0 (a "near-zero" flag, since saturate() alone can't distinguish a
 * true 0 from any negative value), B=saturate(-x), so the rendered PNG is
 * directly readable: pure green = degenerate/near-zero, red-dominant =
 * healthy positive, blue-dominant = negative. */
#define FP_FORCE_TARGET_KEY2 0x47f48eae1f650e68ULL

typedef struct { const char* anchor; const char* expr; } fp_force_stage2_t;

static const fp_force_stage2_t g_fp_force_stages2[] = {
    /* 0: raw tc7.w, captured at the exact point it feeds the divide */
    { "rsx_tex[13].Sample(rsx_samp[13], ((input.tc7).xyzw).xy / ((input.tc7).xyzw).w)); r[1].xy = _v.xy; }",
      "input.tc7.w" },
    /* 1: the line-95 lerp weight, captured right after its last writer */
    { "{ float4 _v = (float4)((input.fog).xxxx); h[0].w = _v.w; }",
      "h[0].w" },
    /* 2: diagnostic-only diff check (s27 part 1): are tc7.w and the fog
     * interpolant (stage 1's weight source) literally the same VP output,
     * or merely correlated? Captured at the same point as stage 1 so
     * input.tc7.w is also in scope. */
    { "{ float4 _v = (float4)((input.fog).xxxx); h[0].w = _v.w; }",
      "(input.tc7.w - h[0].w)" },
    /* 3: sanity check (s27 part 1) -- capture input.fog.x DIRECTLY (bypass
     * h[0].w entirely) to rule out an h[]-array-aliasing artifact in stage
     * 1's own capture. If this disagrees with stage 1's reading, the bug is
     * in how this probe reads h[0].w, not in the shader's real data. */
    { "{ float4 _v = (float4)((input.fog).xxxx); h[0].w = _v.w; }",
      "input.fog.x" },
    /* 4: the "lit" term h[1] right after its final writer (line 91) -- with
     * the weight (stage 1) measured ~0 across the whole mesh, line 95's
     * lerp reduces to h[0]=h[0] (COL1/h[4] has no effect), so the final
     * color is h[1]*COL0 -- this is the next place to look for the actual
     * blue source. Direct color passthrough, no scalar encoding needed. */
    { "(((h[7]).xyzw) * ((h[6]).xxxx) + ((h[2]).xyzw)); h[1].xyz = _v.xyz; }",
      "float4(h[1].xyz, 1.0)" },
    /* 5: input.col0 direct passthrough (the other multiplicand at line 93) */
    { "{ float4 _v = (float4)((input.col0).xyzw); h[0].xyz = _v.xyz; }",
      "float4(input.col0.xyz, 1.0)" },
    /* 6: weight * 20 (s27 part 1 follow-up) -- stage 1 read "100% near-zero"
     * under the |x|<0.001 flag, but a SMALL nonzero weight is exactly what
     * `weight*COL1` (COL1=(0.84,0.89,1.0,1)) needs to read as a faint DARK
     * BLUE rather than pure black once h[1]=0 kills the COL0 term (stage 4)
     * -- this rescales by 20x before saturating so a weight in roughly
     * [0,0.05] becomes visible instead of clipping into the <0.001 flag. */
    { "{ float4 _v = (float4)((input.fog).xxxx); h[0].w = _v.w; }",
      "(h[0].w * 20.0)" },
    /* 7: weight * 100000 -- is it exactly 0.0 or merely extremely small? */
    { "{ float4 _v = (float4)((input.fog).xxxx); h[0].w = _v.w; }",
      "(h[0].w * 100000.0)" },
    /* s27 part 2 (coordinator follow-up): hunt the zero lit term. Line 91
     * is `h[1] = h[7]*h[6].xxxx + h[2]` -- bisect the two addends first. */
    /* 8: h[7] right after its final writer (line 89) */
    { "{ float4 _v = (float4)(((h[0]).xyzw) * ((h[3]).wwww)); h[7].xyz = _v.xyz; }",
      "float4(h[7].xyz, 1.0)" },
    /* 9: h[6].xyz right after its writer (line 50, raw rsx_tex[2] sample) */
    { "{ float4 _v = (float4)(rsx_tex[2].Sample(rsx_samp[2], ((input.tc0).xyzw).xy)); h[6].xyz = _v.xyz; }",
      "float4(h[6].xyz, 1.0)" },
    /* 10: h[2] right after its final writer (line 90) */
    { "{ float4 _v = (float4)(((h[2]).xyzw) * ((h[1]).xyzw)); h[2].xyz = _v.xyz; }",
      "float4(h[2].xyz, 1.0)" },
    /* 11: raw rsx_tex[0] (diffuse) sample, right after line 85 */
    { "{ float4 _v = (float4)(rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).xyzw).xy)); h[1].xyzw = _v.xyzw; }",
      "float4(h[1].xyz, 1.0)" },
    /* 12: h[3].w (the scalar multiplying h[0] into h[7] at line 89),
     * captured right after its line-58 writer */
    { "{ float4 _v = (float4)(min(((h[7]).zzzz), ((h[7]).xyzw))); h[3].w = _v.w; }",
      "h[3].w" },
    /* 13: h[0].x, the specular exp2() term (line 84), captured right after
     * its writer -- one factor feeding h[7] via lines 87-89 */
    { "{ float4 _v = (float4)(exp2(((h[2]).wwww).x)); h[0].x = _v.x; }",
      "h[0].x" },
    /* 14: h[2] right after line 78 (before line 83 modifies it further) --
     * bisects whether the accumulator is already zero here or only becomes
     * zero after line 83's additive term */
    { "{ float4 _v = (float4)(((h[6]).yyyy) * ((h[1]).xyzw) + ((h[7]).xyzw)); h[2].xyz = _v.xyz; }",
      "float4(h[2].xyz, 1.0)" },
    /* 15: h[1] right after line 74 (the "diffuse * atten" term feeding
     * line 78's h[6].y * h[1] product) */
    { "{ float4 _v = (float4)(((h[1]).xyzw) * ((h[2]).xxxx)); h[1].xyz = _v.xyz; }",
      "float4(h[1].xyz, 1.0)" },
    /* 16: h[6].w right after its last writer before line 80 (line 77) --
     * feeds h[0].xyz(80)=h6.w broadcast, the other additive term at 83 */
    { "{ float4 _v = (float4)(((h[0]).zzzz) / ((h[2]).zzzz).x); _v = saturate(_v); h[6].w = _v.w; }",
      "h[6].w" },
    /* 17: input.tc6 direct passthrough -- h[1]@74 traces back to
     * h[1].xyz(45) = input.tc6.xyz + h[3].xyz(41); checking the raw
     * interpolant itself before chasing the h[3] addend further */
    { "{ float4 _v = (float4)((input.tc6).xyzw); h[1].xyzw = _v.xyzw; }",
      "float4(input.tc6.xyz, 1.0)" },
    /* 18: h[3].xyz right after line 41 (the other addend at line 45) */
    { "{ float4 _v = (float4)(((h[1]).wwww) * ((h[6]).xyzw) + ((float4(0,0,0.117647,1)).xyzw)); h[3].xyz = _v.xyz; }",
      "float4(h[3].xyz, 1.0)" },
    /* s27 part 2 methodology fix: stages 14/18 (and originally 8/10) used a
     * direct float4 color passthrough, which clips NEGATIVE values to black
     * in the UNORM render target -- indistinguishable from a true zero.
     * Re-probe the .x component of each suspect with the signed
     * saturate/near-zero/negative encoding (stages 0-3's scheme) to tell
     * "exactly 0" from "negative and clipped". */
    /* 19: h[3].x @41 signed */
    { "{ float4 _v = (float4)(((h[1]).wwww) * ((h[6]).xyzw) + ((float4(0,0,0.117647,1)).xyzw)); h[3].xyz = _v.xyz; }",
      "h[3].x" },
    /* 20: h[1].x @74 signed */
    { "{ float4 _v = (float4)(((h[1]).xyzw) * ((h[2]).xxxx)); h[1].xyz = _v.xyz; }",
      "h[1].x" },
    /* 21: h[2].x @78 signed */
    { "{ float4 _v = (float4)(((h[6]).yyyy) * ((h[1]).xyzw) + ((h[7]).xyzw)); h[2].xyz = _v.xyz; }",
      "h[2].x" },
    /* 22: h[7].x @89 signed */
    { "{ float4 _v = (float4)(((h[0]).xyzw) * ((h[3]).wwww)); h[7].xyz = _v.xyz; }",
      "h[7].x" },
    /* s27 part 2: stages 19-21 read (0,0,0) on ALL THREE channels under the
     * signed R=saturate(x)/G=|x|<0.001-flag/B=saturate(-x) encoding --
     * mathematically impossible for any FINITE x (R and B can't both be 0
     * unless x==0, which forces G=1) -- this is the signature of NaN, since
     * saturate(NaN) and the NaN comparison both evaluate to 0 in HLSL.
     * Chasing the NaN source: line 22's rsqrt(-h[0].y) is a classic
     * normal-map Z-reconstruct that goes NaN if the packed XY is outside
     * the unit disk (1-x^2-y^2 < 0). */
    /* 23: h[0].y @19 (the rsqrt's -argument, i.e. x^2+y^2-ish term) signed */
    { "{ float4 _v = (float4)(dot(((h[0]).xwyw).xyz, ((h[0]).xwzw).xyz)); h[0].y = _v.y; }",
      "h[0].y" },
    /* 24: h[1].w @22 (the rsqrt RESULT itself) signed -- if THIS reads the
     * all-0 NaN signature, the rsqrt argument was negative */
    { "{ float4 _v = (float4)(rsqrt((-((h[0]).yyyy)).x)); h[1].w = _v.w; }",
      "h[1].w" },
    /* 25: raw rsx_tex[5] sample (the packed normal map), direct color */
    { "{ float4 _v = (float4)(rsx_tex[5].Sample(rsx_samp[5], ((input.tc0).xyzw).xy)); h[0].xy = _v.xy; }",
      "float4(h[0].xy, 0.0, 1.0)" },
    /* 26: h[0].xyz @28, the DIRECT input to line 29's normalize() -- the
     * most direct test of "normalize(~0 vector) = NaN". Signed .x probe
     * (this is the actual dependency; line 27's h[4] read is pre-init
     * zero and division by the healthy stage-24 rsqrt result is a
     * harmless dead branch, corrected trace from the first attempt). */
    { "{ float4 _v = (float4)(((h[1]).xyzw) + ((h[0]).xyzw)); h[0].xyz = _v.xyz; }",
      "h[0].x" },
    /* 27: h[4].x @29, the normalize() RESULT itself, signed */
    { "{ float4 _v = (float4)(float4(normalize(((h[0]).xyzw).xyz), 1.0)); h[4].xyz = _v.xyz; }",
      "h[4].x" },
    /* 28: h[0].x @25 signed -- h[0].w(26) traces to zero (never written
     * before line 26, so the *h[0].w term is a harmless no-op) meaning
     * h[0].xyz(26)=h[0].xyz(25) directly: h[0].xyz(25) = h[0].xxxx(18,
     * = tex5.x*2-1, and stage 25 already showed tex5.x is a CONSTANT 0
     * min=max=0, so this term is the constant -1) * h[5].xyzw(14=tc3) */
    { "{ float4 _v = (float4)(((h[0]).xxxx) * ((h[5]).xyzw)); h[0].xyz = _v.xyz; }",
      "h[0].x" },
    /* 29: input.tc3 direct passthrough (h[5]@14, feeds stage 28's product) */
    { "{ float4 _v = (float4)((input.tc3).xyzw); h[5].xyzw = _v.xyzw; }",
      "float4(input.tc3.xyz, 1.0)" },
    /* 30: input.tc3.w signed -- h[0].w(18) and h[0].xyz(25) both measured
     * healthy, so for h[0]@26 = h0.w(18)*h[1](24) + h0(25) to go NaN,
     * h[1](24) = h[1](23)*h[5].wwww(=tc3.w) must be the carrier; tc3.xyz
     * (stage 29) is healthy but .w was never separately checked */
    { "{ float4 _v = (float4)((input.tc3).xyzw); h[5].xyzw = _v.xyzw; }",
      "input.tc3.w" },
    /* s27 part 4 (coordinator ask 2): dump the ACTUAL MAGNITUDE of the
     * unpacked normal's x,y and the rsqrt radicand, not just sign -- the
     * signed encoding above can't distinguish "slightly negative" from
     * "very negative" and both look identical to the near-zero flag once
     * saturated. Captured right after line 18 (before line 19's dot
     * overwrites h[0].y with the cross term): h[0].x = unpacked X
     * (texX*2-1), h[0].w = unpacked Y (texY*2-1) -- see the s27p4 writeup
     * for the swizzle derivation (h0.y and h0.z become -1/1 constants at
     * line 18, not the unpacked Y, which lands in h0.w instead). Wide-range
     * direct-color encoding: v*0.25+0.5 maps [-2,2] to [0,1] (readable back
     * as (pixel/255-0.5)*4), instead of the standard encoding's implicit
     * [-1,1] clip. */
    /* 31: unpacked X magnitude (h0.x right after line 18) */
    { "{ float4 _v = (float4)(((h[0]).xxyy) * ((float4(2,0,-1,1)).xyyx) + ((float4(2,0,-1,1)).zzwz)); h[0].xyzw = _v.xyzw; }",
      "float4(h[0].x*0.25+0.5, 0.0, 0.0, 1.0)" },
    /* 32: unpacked Y magnitude (h0.w right after line 18) */
    { "{ float4 _v = (float4)(((h[0]).xxyy) * ((float4(2,0,-1,1)).xyyx) + ((float4(2,0,-1,1)).zzwz)); h[0].xyzw = _v.xyzw; }",
      "float4(h[0].w*0.25+0.5, 0.0, 0.0, 1.0)" },
    /* 33: the actual rsqrt radicand (1-x^2-y^2) magnitude, computed
     * DIRECTLY (independent of line 19's own dot-product formulation, as a
     * cross-check) -- positive means healthy (rsqrt of a positive number),
     * negative means x^2+y^2>1 and rsqrt(-negative)=rsqrt(positive-arg)
     * wait: line 22 computes rsqrt(-(h0.y)) where h0.y(19)=x^2+y^2-1, so
     * rsqrt argument = 1-x^2-y^2; if THIS captured value is negative,
     * x^2+y^2>1 and the FP's own rsqrt argument goes negative -> NaN. */
    { "{ float4 _v = (float4)(((h[0]).xxyy) * ((float4(2,0,-1,1)).xyyx) + ((float4(2,0,-1,1)).zzwz)); h[0].xyzw = _v.xyzw; }",
      "float4((1.0-h[0].x*h[0].x-h[0].w*h[0].w)*0.25+0.5, 0.0, 0.0, 1.0)" },
};
#define FP_FORCE_NSTAGES2 (int)(sizeof(g_fp_force_stages2) / sizeof(g_fp_force_stages2[0]))

static int g_fp_force_stage2 = -1; /* RSX_FP_FORCE_STAGE2, parsed once in main() */

static int fp_apply_force_stage2(char* ps_hlsl, size_t cap, int stage)
{
    if (stage < 0 || stage >= FP_FORCE_NSTAGES2) {
        printf("[fpforce2] RSX_FP_FORCE_STAGE2=%d out of range [0,%d)\n", stage, FP_FORCE_NSTAGES2);
        return 0;
    }
    if (!fp_insert_after(ps_hlsl, cap,
            "float4 r[48]; float4 h[48];",
            " float4 dbg_stage2 = (float4)0;")) {
        printf("[fpforce2] declaration anchor not found\n");
        return 0;
    }
    char capture[256];
    /* Stages whose expr is already a float4 color (starts with "float4(")
     * are a direct passthrough -- the scalar sign/near-zero encoding below
     * only applies to bare scalar expressions. */
    if (!strncmp(g_fp_force_stages2[stage].expr, "float4(", 7)) {
        snprintf(capture, sizeof(capture), " dbg_stage2 = %s;",
            g_fp_force_stages2[stage].expr);
    } else {
        snprintf(capture, sizeof(capture),
            " dbg_stage2 = float4(saturate(%s), (%s > -0.001 && %s < 0.001) ? 1.0 : 0.0, saturate(-(%s)), 1.0);",
            g_fp_force_stages2[stage].expr, g_fp_force_stages2[stage].expr,
            g_fp_force_stages2[stage].expr, g_fp_force_stages2[stage].expr);
    }
    if (!fp_insert_after(ps_hlsl, cap, g_fp_force_stages2[stage].anchor, capture)) {
        printf("[fpforce2] stage %d capture anchor not found\n", stage);
        return 0;
    }
    if (!fp_replace_first(ps_hlsl, cap, "return h[0];", "return dbg_stage2;")) {
        printf("[fpforce2] return-statement anchor not found\n");
        return 0;
    }
    printf("[fpforce2] stage %d patch applied OK\n", stage);
    return 1;
}

/* ---------------------------------------------------------------------------
 * RSX_FP_FORCE_STAGE3 (session s27 part 2, coordinator lead 2): bisect the
 * "blue" symptom's OTHER paired program, fp_ac8f022372b4de59 (draw 1370's
 * "opaque body pass", part 4's naming), a tiny 8-statement shader:
 *   h[0].xy = min(tex[14].Sample(tc0.xy/tc0.w), tex[15].Sample(tc1.xy/tc1.w))
 *   h[0].z  = saturate(tc2.x)^2
 *   h[0].w  = tex[0].Sample(tc2.zw).a
 *   return h[0]
 * i.e. the entire RGB output is (min(tex14,tex15).x, min(tex14,tex15).y,
 * saturate(tc2.x)^2) -- if the two projective light/shadow-map samples
 * (tex14/tex15) read low while the tc2.x^2 term reads relatively higher,
 * that alone is a self-contained, simple mechanism for a blue-dominant
 * output, independent of anything found in the fp_47f48eae1f650e68
 * investigation. Reuses fp_insert_after/fp_replace_first. */
#define FP_FORCE_TARGET_KEY3 0xac8f022372b4de59ULL

static const fp_force_stage2_t g_fp_force_stages3[] = {
    /* 0: raw tex14 sample (r[1].xy), direct color */
    { "{ float4 _v = (float4)(rsx_tex[14].Sample(rsx_samp[14], ((input.tc0).xyzw).xy / ((input.tc0).xyzw).w)); r[1].xy = _v.xy; }",
      "float4(r[1].xy, 0.0, 1.0)" },
    /* 1: raw tex15 sample (r[2].xy), direct color */
    { "{ float4 _v = (float4)(rsx_tex[15].Sample(rsx_samp[15], ((input.tc1).xyzw).xy / ((input.tc1).xyzw).w)); r[2].xy = _v.xy; }",
      "float4(r[2].xy, 0.0, 1.0)" },
    /* 2: h[0].xy = min(tex14,tex15), direct color -- the shader's own R,G */
    { "{ float4 _v = (float4)(min(((r[1]).xyzw), ((r[2]).xyzw))); h[0].xy = _v.xy; }",
      "float4(h[0].xy, 0.0, 1.0)" },
    /* 3: h[0].z = saturate(tc2.x)^2, direct color -- the shader's own B */
    { "{ float4 _v = (float4)(((r[1]).wwww) * ((r[1]).wwww)); h[0].z = _v.z; }",
      "float4(0.0, 0.0, h[0].z, 1.0)" },
    /* 4: tc0.w signed (tex14's projective divisor) */
    { "{ float4 _v = (float4)(rsx_tex[14].Sample(rsx_samp[14], ((input.tc0).xyzw).xy / ((input.tc0).xyzw).w)); r[1].xy = _v.xy; }",
      "input.tc0.w" },
    /* 5: tc1.w signed (tex15's projective divisor) */
    { "{ float4 _v = (float4)(rsx_tex[15].Sample(rsx_samp[15], ((input.tc1).xyzw).xy / ((input.tc1).xyzw).w)); r[2].xy = _v.xy; }",
      "input.tc1.w" },
    /* 6: tex0.a (h[0].w) direct, and 7: unmodified real output for baseline */
    { "{ float4 _v = (float4)((r[0]).xyzw); h[0].w = _v.w; }",
      "float4(0.0, 0.0, 0.0, h[0].w)" },
};
#define FP_FORCE_NSTAGES3 (int)(sizeof(g_fp_force_stages3) / sizeof(g_fp_force_stages3[0]))

static int g_fp_force_stage3 = -1; /* RSX_FP_FORCE_STAGE3, parsed once in main() */

static int fp_apply_force_stage3(char* ps_hlsl, size_t cap, int stage)
{
    if (stage < 0 || stage >= FP_FORCE_NSTAGES3) {
        printf("[fpforce3] RSX_FP_FORCE_STAGE3=%d out of range [0,%d)\n", stage, FP_FORCE_NSTAGES3);
        return 0;
    }
    if (!fp_insert_after(ps_hlsl, cap,
            "float4 r[48]; float4 h[48];",
            " float4 dbg_stage3 = (float4)0;")) {
        printf("[fpforce3] declaration anchor not found\n");
        return 0;
    }
    char capture[256];
    if (!strncmp(g_fp_force_stages3[stage].expr, "float4(", 7)) {
        snprintf(capture, sizeof(capture), " dbg_stage3 = %s;",
            g_fp_force_stages3[stage].expr);
    } else {
        snprintf(capture, sizeof(capture),
            " dbg_stage3 = float4(saturate(%s), (%s > -0.001 && %s < 0.001) ? 1.0 : 0.0, saturate(-(%s)), 1.0);",
            g_fp_force_stages3[stage].expr, g_fp_force_stages3[stage].expr,
            g_fp_force_stages3[stage].expr, g_fp_force_stages3[stage].expr);
    }
    if (!fp_insert_after(ps_hlsl, cap, g_fp_force_stages3[stage].anchor, capture)) {
        printf("[fpforce3] stage %d capture anchor not found\n", stage);
        return 0;
    }
    if (!fp_replace_first(ps_hlsl, cap, "return h[0];", "return dbg_stage3;")) {
        printf("[fpforce3] return-statement anchor not found\n");
        return 0;
    }
    printf("[fpforce3] stage %d patch applied OK\n", stage);
    return 1;
}

/* ---------------------------------------------------------------------------
 * RSX_FP_FORCE_STAGE4 (session s27 part 4): the overlay half of the
 * "zombie splotch" blue pair (draw 1350, part 4's pixel-bisected culprit --
 * paired with draw 1349's ac8f022372b4de59 base, same body+overlay pattern
 * as the woman's 47f48eae1f650e68/ac8f022372b4de59 pair from parts 1-2).
 * fp_d4ec2f11b39b1d38 shares the EXACT same structure as 47f48eae1f650e68:
 * a packed-normal-map + rsqrt Z-reconstruct feeding a shading normal, and a
 * final `h[0]=input.col1; ...; h[0]=h1.w*h0+h1` lerp gated by a fog-sourced
 * weight (line 85: h[1].w=input.fog.x). Reuses fp_insert_after/
 * fp_replace_first. */
#define FP_FORCE_TARGET_KEY4 0xd4ec2f11b39b1d38ULL

static const fp_force_stage2_t g_fp_force_stages4[] = {
    /* 0: h[1].x right after its final writer (line 101, the "lit*col0"
     * term feeding the final lerp) -- signed encoding to catch the
     * impossible-triple-zero NaN signature from part 2, not just a
     * naive black read */
    { "{ float4 _v = (float4)(((h[2]).xyzw) * ((h[1]).xyzw)); h[1].xyz = _v.xyz; }",
      "h[1].x" },
};
#define FP_FORCE_NSTAGES4 (int)(sizeof(g_fp_force_stages4) / sizeof(g_fp_force_stages4[0]))

static int g_fp_force_stage4 = -1; /* RSX_FP_FORCE_STAGE4, parsed once in main() */

static int fp_apply_force_stage4(char* ps_hlsl, size_t cap, int stage)
{
    if (stage < 0 || stage >= FP_FORCE_NSTAGES4) {
        printf("[fpforce4] RSX_FP_FORCE_STAGE4=%d out of range [0,%d)\n", stage, FP_FORCE_NSTAGES4);
        return 0;
    }
    if (!fp_insert_after(ps_hlsl, cap,
            "float4 r[48]; float4 h[48];",
            " float4 dbg_stage4 = (float4)0;")) {
        printf("[fpforce4] declaration anchor not found\n");
        return 0;
    }
    char capture[256];
    if (!strncmp(g_fp_force_stages4[stage].expr, "float4(", 7)) {
        snprintf(capture, sizeof(capture), " dbg_stage4 = %s;",
            g_fp_force_stages4[stage].expr);
    } else {
        snprintf(capture, sizeof(capture),
            " dbg_stage4 = float4(saturate(%s), (%s > -0.001 && %s < 0.001) ? 1.0 : 0.0, saturate(-(%s)), 1.0);",
            g_fp_force_stages4[stage].expr, g_fp_force_stages4[stage].expr,
            g_fp_force_stages4[stage].expr, g_fp_force_stages4[stage].expr);
    }
    if (!fp_insert_after(ps_hlsl, cap, g_fp_force_stages4[stage].anchor, capture)) {
        printf("[fpforce4] stage %d capture anchor not found\n", stage);
        return 0;
    }
    if (!fp_replace_first(ps_hlsl, cap, "return h[0];", "return dbg_stage4;")) {
        printf("[fpforce4] return-statement anchor not found\n");
        return 0;
    }
    printf("[fpforce4] stage %d patch applied OK\n", stage);
    return 1;
}

/* RSX_FP_FORCE_STAGE5 (session s29, scratch/s29_draw803_occluder.md): a
 * minimal shape probe for the "draw 803 cubemap-fallback occluder" PSO
 * (vp_key=04153642dbaa34c9, cited scratch/s26r_draw803diag.log:1984 -- this
 * IS the combined PSO cache key per get_translated_pso()'s out_key, not just
 * a VP hash, despite the field's printf label). Its own shading renders
 * solid/near-black against the harness's black clear color, so isolating
 * this one draw shows nothing -- this stage forces its output to
 * unconditional white so the draw's actual rasterized SCREEN FOOTPRINT
 * becomes visible, independent of its own (broken-cubemap) shading. Single
 * stage, no capture table needed -- reuses fp_replace_first() verbatim.
 * Env-gated (RSX_FP_FORCE_STAGE5=1), default off, touches only this one PSO
 * key's cached HLSL text. */
#define FP_FORCE_TARGET_KEY5 0x04153642dbaa34c9ULL
static int g_fp_force_stage5 = -1; /* RSX_FP_FORCE_STAGE5, parsed once in main() */

static int fp_apply_force_stage5(char* ps_hlsl, size_t cap)
{
    if (!fp_replace_first(ps_hlsl, cap, "return h[0];", "return float4(1.0,1.0,1.0,1.0);")) {
        printf("[fpforce5] return-statement anchor not found\n");
        return 0;
    }
    printf("[fpforce5] force-white patch applied OK\n");
    return 1;
}

/* RSX_FP_FORCE_STAGE6 (session s29b, scratch/s29_blue_remnants.md): the RSQ
 * fix's sibling check for DIVSQ. fp_d43fd20cbbad3342 (draw 1589, inside the
 * woman's own 1491-1589 span) computes a packed-normal-map normalize idiom:
 *   r[0].xyz = tex1.xyz*2-1; r[0].w = dot(r[0].xyz, r[0].xyz);
 *   r[0].xyz = r[0].xyz / sqrt(r[0].w);   <- OP_DIVSQ, rsx_fp_decompiler.c:316
 * RPCS3's oracle (Program/FragmentProgramDecompiler.cpp:966,1009-1013)
 * documents _builtin_divsq(a,b) as routing its divisor through
 * _builtin_sqrt(b)=sqrt(abs(b)) (same sign-ignoring quirk as the already-
 * fixed RSQ) AND forcing the result to exactly 0 when the numerator is 0
 * even if the denominator is also 0 (avoiding 0/0=NaN) -- our decompiler's
 * plain "(%s) / sqrt((%s).x)" has neither. This probe measures r[0].w (the
 * divisor, always >=0 for a real dot(v,v) unless v itself carries a NaN)
 * signed at its writer, and the post-divsq r[0].xyz as a direct color, to
 * see whether this specific site is live/degenerate for the still-blue
 * pixels. Reuses fp_insert_after/fp_replace_first. */
#define FP_FORCE_TARGET_KEY6 0xd43fd20cbbad3342ULL

static const fp_force_stage2_t g_fp_force_stages6[] = {
    /* 0: r[0].w (the dot-product divisor) signed, right after its writer */
    { "{ float4 _v = (float4)(dot(((r[0]).xyzw).xyz, ((r[0]).xyzw).xyz)); r[0].w = _v.w; }",
      "r[0].w" },
    /* 1: r[0].xyz right after the divsq line, direct color */
    { "{ float4 _v = (float4)(((r[0]).xyzw) / sqrt(((r[0]).wwww).x)); r[0].xyz = _v.xyz; }",
      "float4(r[0].xyz, 1.0)" },
};
#define FP_FORCE_NSTAGES6 (int)(sizeof(g_fp_force_stages6) / sizeof(g_fp_force_stages6[0]))

static int g_fp_force_stage6 = -1; /* RSX_FP_FORCE_STAGE6, parsed once in main() */

static int fp_apply_force_stage6(char* ps_hlsl, size_t cap, int stage)
{
    if (stage < 0 || stage >= FP_FORCE_NSTAGES6) {
        printf("[fpforce6] RSX_FP_FORCE_STAGE6=%d out of range [0,%d)\n", stage, FP_FORCE_NSTAGES6);
        return 0;
    }
    if (!fp_insert_after(ps_hlsl, cap,
            "float4 r[48]; float4 h[48];",
            " float4 dbg_stage6 = (float4)0;")) {
        printf("[fpforce6] declaration anchor not found\n");
        return 0;
    }
    char capture[256];
    if (!strncmp(g_fp_force_stages6[stage].expr, "float4(", 7)) {
        snprintf(capture, sizeof(capture), " dbg_stage6 = %s;",
            g_fp_force_stages6[stage].expr);
    } else {
        snprintf(capture, sizeof(capture),
            " dbg_stage6 = float4(saturate(%s), (%s > -0.001 && %s < 0.001) ? 1.0 : 0.0, saturate(-(%s)), 1.0);",
            g_fp_force_stages6[stage].expr, g_fp_force_stages6[stage].expr,
            g_fp_force_stages6[stage].expr, g_fp_force_stages6[stage].expr);
    }
    if (!fp_insert_after(ps_hlsl, cap, g_fp_force_stages6[stage].anchor, capture)) {
        printf("[fpforce6] stage %d capture anchor not found\n", stage);
        return 0;
    }
    if (!fp_replace_first(ps_hlsl, cap, "return h[0];", "return dbg_stage6;")) {
        printf("[fpforce6] return-statement anchor not found\n");
        return 0;
    }
    printf("[fpforce6] stage %d patch applied OK\n", stage);
    return 1;
}

static u64 fnv1a(const void* data, u32 n, u64 h)
{
    const u8* p = data;
    for (u32 i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void dump_text(const char* stem, u64 key, const char* ext, const void* data, u32 n)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s_%016llx.%s", g_dump_dir, stem,
             (unsigned long long)key, ext);
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
}

/* Compile the translated pair into a PSO (NULL on any failure). The captured
 * blend/depth/cull render state (B1) is folded into the descriptor here. */
static ID3D12PipelineState* build_translated_pso(const char* vs_hlsl, const char* ps_hlsl,
                                                 u64 key, const render_state_t* rs)
{
    ID3DBlob *vs = NULL, *ps = NULL, *err = NULL;
    if (FAILED(D3DCompile(vs_hlsl, strlen(vs_hlsl), "xvs", NULL, NULL, "main",
                          "vs_5_0", 0, 0, &vs, &err))) {
        printf("[xlat] VS %016llx D3DCompile failed:\n%s\n", (unsigned long long)key,
               err ? (char*)err->lpVtbl->GetBufferPointer(err) : "?");
        if (err) err->lpVtbl->Release(err);
        return NULL;
    }
    if (FAILED(D3DCompile(ps_hlsl, strlen(ps_hlsl), "xps", NULL, NULL, "main",
                          "ps_5_0", 0, 0, &ps, &err))) {
        printf("[xlat] PS %016llx D3DCompile failed:\n%s\n", (unsigned long long)key,
               err ? (char*)err->lpVtbl->GetBufferPointer(err) : "?");
        if (err) err->lpVtbl->Release(err);
        vs->lpVtbl->Release(vs);
        return NULL;
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
    pd.InputLayout.pInputElementDescs = il;
    pd.InputLayout.NumElements = 16;
    apply_render_state(&pd, rs);            /* B1: blend/depth/cull from capture */
    pd.SampleMask = 0xFFFFFFFFu;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    HRESULT hr = g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd,
                                                            &IID_ID3D12PipelineState,
                                                            (void**)&pso);
    vs->lpVtbl->Release(vs);
    ps->lpVtbl->Release(ps);
    if (FAILED(hr)) {
        printf("[xlat] PSO %016llx create failed 0x%08lX\n", (unsigned long long)key, hr);
        return NULL;
    }
    return pso;
}

/* Resolve (and cache) the PSO for the currently-bound VP+FP pair.
 * Returns NULL when either program can't be translated (use the fallback).
 * [skin] diag: *out_key gets the PSO cache key (vp key/hash for the log),
 * *out_skinned gets whether the active VP's decompiled HLSL contains the
 * literal "vp_c[467]" (the 4-bone linear-blend skinning scale constant
 * read, see scratch/s24_replay_fixes.md "The remaining defect"). Both
 * out-params are set on every path, including the early-return misses. */
static ID3D12PipelineState* get_translated_pso(const rsx_dispatch* rsx, u64* out_key, int* out_skinned)
{
    if (out_key) *out_key = 0;
    if (out_skinned) *out_skinned = 0;

    /* active transform program */
    const u32 start = rsx_dsp_vp_start(rsx);
    if (start >= RSX_DSP_VP_INSTR)
        return NULL;
    const u8* vp_uc = (const u8*)(rsx->vp + start * 4);
    const u32 vp_instrs = rsx_vp_program_size_instrs(vp_uc, (RSX_DSP_VP_INSTR - start) * 16);
    if (!vp_instrs)
        return NULL;

    /* active fragment program (guest memory, constants inline) */
    u32 fp_loc = 0;
    const u32 fp_off = rsx_dsp_fragment_program(rsx, &fp_loc);
    const u8* fp_uc = guest_ptr(fp_loc, fp_off);
    if (!fp_uc)
        return NULL;
    u32 fp_max = ARENA_SIZE - fp_off;
    if (fp_max > 0x10000)
        fp_max = 0x10000;
    const u32 fp_size = rsx_fp_program_size(fp_uc, fp_max);
    if (!fp_size)
        return NULL;

    /* Fragment output register mode (fp16 h0 vs fp32 r0) is driven by the
     * SHADER_CONTROL word; fold the deciding bit into the cache key so a
     * program reused under a different export mode gets its own PSO. */
    const u32 fp_ctrl = rsx_dsp_shader_control(rsx);

    u64 key = fnv1a(vp_uc, vp_instrs * 16, 1469598103934665603ull);
    key = fnv1a(fp_uc, fp_size, key);
    const u32 fp_ctrl_key = fp_ctrl & 0x40u;
    key = fnv1a(&fp_ctrl_key, sizeof(fp_ctrl_key), key);
    /* B1: the PSO bakes the render state, so it must be part of the cache key */
    render_state_t rs;
    decode_render_state(rsx, &rs);
    key = fnv1a(&rs, sizeof(rs), key);

    if (out_key) *out_key = key;

    for (u32 i = 0; i < g.n_psos; i++)
        if (g.psos[i].key == key) {
            if (out_skinned) *out_skinned = g.psos[i].skinned;
            return g.psos[i].pso;
        }
    if (g.n_psos >= MAX_PSOS) {
        printf("[xlat] PSO cache full\n");
        return NULL;
    }

    static char vs_hlsl[256 * 1024];
    static char ps_hlsl[256 * 1024];
    ID3D12PipelineState* pso = NULL;
    const int vi = rsx_vp_decompile(vp_uc, vp_instrs * 16, vs_hlsl, sizeof(vs_hlsl));
    const int fi = rsx_fp_decompile(fp_uc, fp_size, fp_ctrl, ps_hlsl, sizeof(ps_hlsl));
    if (fi > 0 && g_fp_force_stage >= 0 && key == FP_FORCE_TARGET_KEY)
        fp_apply_force_stage(ps_hlsl, sizeof(ps_hlsl), g_fp_force_stage);
    if (fi > 0 && g_fp_force_stage2 >= 0 && key == FP_FORCE_TARGET_KEY2)
        fp_apply_force_stage2(ps_hlsl, sizeof(ps_hlsl), g_fp_force_stage2);
    if (fi > 0 && g_fp_force_stage3 >= 0 && key == FP_FORCE_TARGET_KEY3)
        fp_apply_force_stage3(ps_hlsl, sizeof(ps_hlsl), g_fp_force_stage3);
    if (fi > 0 && g_fp_force_stage4 >= 0 && key == FP_FORCE_TARGET_KEY4)
        fp_apply_force_stage4(ps_hlsl, sizeof(ps_hlsl), g_fp_force_stage4);
    if (fi > 0 && g_fp_force_stage5 >= 0 && key == FP_FORCE_TARGET_KEY5)
        fp_apply_force_stage5(ps_hlsl, sizeof(ps_hlsl));
    if (fi > 0 && g_fp_force_stage6 >= 0 && key == FP_FORCE_TARGET_KEY6)
        fp_apply_force_stage6(ps_hlsl, sizeof(ps_hlsl), g_fp_force_stage6);
    if (vi > 0 && fi > 0)
        pso = build_translated_pso(vs_hlsl, ps_hlsl, key, &rs);
    else
        printf("[xlat] decompile failed (vp=%d fp=%d) key=%016llx\n", vi, fi,
               (unsigned long long)key);

    /* [skin] diag, CORRECTED (see scratch/s25_skin_diag.md): the original
     * "vp_c[467]" literal-string heuristic was a false-positive magnet —
     * cross-checking the shader corpus (scratch/shd_dump2/) showed 201
     * VPs read constant 467 for all sorts of unrelated per-object scale/
     * bias math, of which only 72 are real 4-bone skinning (matching the
     * prior session's count exactly). The reliable signature is the
     * INDEXED constant read itself: rsx_vp_decompile emits
     * "vp_c[(NNNu + a0.x) & 511u]" only for index_const=1 sources (the
     * a0-relative bone-matrix fetch); a plain "vp_c[467]" is emitted for
     * an ordinary unindexed constant read and proves nothing about
     * skinning. Every one of the 72 true indexed-read VPs also happens to
     * read vp_c[467] elsewhere (0 false negatives measured), so this is a
     * strict tightening, not a different population. */
    const int is_skinned = (vi > 0) && strstr(vs_hlsl, "& 511u") != NULL;
    if (out_skinned) *out_skinned = is_skinned;

    if (g_dump_dir) {
        dump_text("vp", key, "hlsl", vs_hlsl, (u32)strlen(vs_hlsl));
        dump_text("fp", key, "hlsl", ps_hlsl, (u32)strlen(ps_hlsl));
        /* raw ucode with the extensions the corpus validators expect */
        dump_text("vp", key, "vp", vp_uc, vp_instrs * 16);
        dump_text("fp", key, "fp", fp_uc, fp_size);
    }

    g.psos[g.n_psos].key = key;
    g.psos[g.n_psos].pso = pso;
    g.psos[g.n_psos].skinned = is_skinned;
    g.n_psos++;
    if (pso) {
        g_xlat_ok++;
        printf("[xlat] pair %016llx: vp %u instrs @slot %u + fp %u bytes -> PSO OK\n",
               (unsigned long long)key, vp_instrs, start, fp_size);
    } else {
        g_xlat_fail++;
    }
    return pso;
}

/* ---------------------------------------------------------------------------
 * Dispatcher sink: clears + draw accumulation
 * -----------------------------------------------------------------------*/

/* Wide vertex: all 16 RSX attributes as float4 (see VERT_STRIDE). */
typedef struct { float a[16][4]; } vtx_t;

/* RSX primitive ids (gcm public constants) */
#define PRIM_TRIANGLES      5
#define PRIM_TRIANGLE_STRIP 6
#define PRIM_TRIANGLE_FAN   7
#define PRIM_QUADS          8

typedef struct {
    const char* outdir;
    int   dump_surfaces;
    u32   frame_no;
    u32   clear_count, draw_count, drawn_verts, draws_xlat;
    u32   skipped_prims[16];

    /* Batches accumulated inside one begin/end. Vertex data is fetched at
     * END, not at the batch method: the capture applies the draw's memory
     * blocks between the batch and BEGIN_END(0) (RSX also latches the draw
     * until end), so fetching early reads stale guest memory. */
    struct { u32 first, count; } arr[256], idx[256];
    u32    n_arr, n_idx;

    /* fetched vertices */
    vtx_t* verts;
    u32    n_verts;
    u32    cap_verts;
    int    fetch_ok;

    /* Primitive-restart cut points (root cause, session s25c): a guest
     * index equal to the RSX restart sentinel (NV4097_SET_RESTART_INDEX,
     * confirmed measured = 0xFFFF for this capture's 16-bit index buffers)
     * ends the current TRIANGLE_STRIP/FAN run with NO connecting triangle.
     * fetch_batches() records the c->n_verts position at each restart
     * occurrence instead of pushing a phantom vertex; sink_end()'s
     * STRIP/FAN expansion must not bridge across a cut. */
    u32    cuts[520];
    u32    n_cuts;

    /* [skin] diag: the guest vertex index of the first vertex fetched this
     * batch (pre-attribute-conversion), so sink_end can independently
     * re-fetch ATTR7's raw bytes for the exact vertex that landed in
     * tri[0] (see fetch_one()). */
    u32    dbg_first_vert;
    int    dbg_first_vert_valid;

    /* s25c: parallel record of the resolved guest vertex index for every
     * pushed vertex this draw (whichever of array-sequential or
     * index-buffer-decoded "vert" fetch_one() was called with), so a
     * RSX_LOG_DRAW draw can name exactly which guest index produced an
     * anomalous (e.g. all-zero) position. Capped; only filled when logging
     * this draw (see fetch_one()). */
    u32    dbg_vert_index[8192];

    const rxs_stream* stream;
} sink_ctx;

static float be_f32(const u8* p)
{
    u32 v = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static float be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    u32 exp = (h >> 10) & 0x1F;
    u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0) {
        out = sign; /* denormals flushed */
    } else if (exp == 31) {
        out = sign | 0x7F800000 | (man << 13);
    } else {
        out = sign | ((exp + 112) << 23) | (man << 13);
    }
    float f;
    memcpy(&f, &out, 4);
    return f;
}

static int fetch_attr(const rsx_dispatch* rsx, u32 index, u32 base_offset,
                      u32 vert, float out[4])
{
    rsx_dsp_vertex_attr a;
    rsx_dsp_get_vertex_attr(rsx, index, &a);
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a.type || !a.size)
        return 0;

    const u32 addr = base_offset + a.offset + vert * a.stride;
    const u8* p = guest_ptr(a.location, addr);
    if (!p)
        return 0;

    for (u32 c = 0; c < a.size && c < 4; c++) {
        switch (a.type) {
        case RSX_VTX_TYPE_FLOAT:   out[c] = be_f32(p + c * 4); break;
        case RSX_VTX_TYPE_HALF:    out[c] = be_f16(p + c * 2); break;
        case RSX_VTX_TYPE_UNORM8:  out[c] = p[c] / 255.0f; break;
        case RSX_VTX_TYPE_UINT8:   out[c] = (float)p[c]; break;
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
            /* CELL_GCM_VERTEX_CMP: one BE 32-bit word packs three signed-
             * normalized fields X:[0..10] Y:[11..21] Z:[10..31] (11/11/10),
             * each normalized by its max positive magnitude (1023,1023,511).
             * Always 3 components regardless of a.size; commonly a normal. */
            const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                          ((u32)p[2] << 8) | (u32)p[3];
            s32 x = (s32)(w & 0x7FF);        if (x & 0x400) x -= 0x800;
            s32 y = (s32)((w >> 11) & 0x7FF); if (y & 0x400) y -= 0x800;
            s32 z = (s32)((w >> 22) & 0x3FF); if (z & 0x200) z -= 0x400;
            out[0] = (float)x / 1023.0f;
            out[1] = (float)y / 1023.0f;
            out[2] = (float)z / 511.0f;
            out[3] = 1.0f;
            return 1;                        /* consumed all components */
        }
        default:
            return 0; /* unhandled type */
        }
    }
    return 1;
}

static void sink_clear(void* user, const rsx_dispatch* rsx, u32 mask)
{
    sink_ctx* c = user;
    c->clear_count++;
    /* s26 fix (scratch/s26_fp_bisect.md): honor the game's OWN depth/stencil
     * clears at their per-pass cadence — a direct port of the live path's
     * already-correct sink_clear (rsx_live_draw.c:1130-1148). The old
     * discard-depth-clears + once-per-frame heuristic left draws testing
     * against stale depth from earlier passes (the black-character class;
     * A/B receipt: isolated draw 1040 renders perfectly). The frame-top
     * clear (see B1 below) remains as a first-clear safety net only.
     * Kill-switch RSX_NO_PASS_DEPTH_CLEAR restores the old behavior. */
    { static int nodc = -1;
      if (nodc < 0) nodc = getenv("RSX_NO_PASS_DEPTH_CLEAR") ? 1 : 0;
      if (!nodc && (mask & (RSX_CLEAR_DEPTH | RSX_CLEAR_STENCIL)) && g.depth) {
          /* s31 FIX 1: the game's depth clear applies to the CURRENTLY BOUND
           * zeta target's own memory, not to one global buffer. */
          u32 zslot = 0;
          if (honor_zeta_track()) {
              rsx_dsp_surface zsf;
              rsx_dsp_get_surface(rsx, &zsf);
              zslot = zdepth_get(zsf.zeta_location, zsf.zeta_offset, zsf.clip_w, zsf.clip_h);
              if (zslot) {
                  g.zdepths[zslot - 1].cleared = 1;
                  printf("[zetatrack] game depth clear -> zeta loc=%u off=0x%X\n",
                         zsf.zeta_location, zsf.zeta_offset);
              }
          }
          D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_handle(zslot);
          g.list->lpVtbl->ClearDepthStencilView(g.list, dsv,
              D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
          g.depth_cleared = 1;
      } }
    if (!(mask & (RSX_CLEAR_COLOR_R | RSX_CLEAR_COLOR_G | RSX_CLEAR_COLOR_B | RSX_CLEAR_COLOR_A)))
        return; /* no color bits: nothing further to do */

    const u32 argb = rsx_dsp_clear_color(rsx);
    float col[4] = {
        ((argb >> 16) & 0xFF) / 255.0f,
        ((argb >> 8) & 0xFF) / 255.0f,
        (argb & 0xFF) / 255.0f,
        ((argb >> 24) & 0xFF) / 255.0f,
    };
    const u32 si = current_surface(rsx);
    printf("[replay] clear #%u mask=0x%02X argb=0x%08X surface=0x%X\n",
           c->clear_count, mask, argb, g.surfaces[si].offset);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = surface_rtv(si);
    g.list->lpVtbl->ClearRenderTargetView(g.list, rtv, col, 0, NULL);
}

static void sink_begin(void* user, const rsx_dispatch* rsx, u32 prim)
{
    (void)rsx;
    (void)prim;
    sink_ctx* c = user;
    c->n_arr = c->n_idx = 0;
    c->n_verts = 0;
    c->fetch_ok = 1;
    c->dbg_first_vert_valid = 0;
    c->n_cuts = 0;
}

/* Forward decl: defined further down (env-parsed RSX_LOG_DRAW list); needed
 * here so fetch_one() can record the resolved guest index per vertex only
 * for draws actually being logged. */
static int log_draw_index(u32 draw_index);

static void push_vert(sink_ctx* c, const vtx_t* v, u32 vert_index)
{
    if (c->n_verts >= c->cap_verts) {
        c->cap_verts = c->cap_verts ? c->cap_verts * 2 : 4096;
        c->verts = realloc(c->verts, c->cap_verts * sizeof(vtx_t));
    }
    if (c->n_verts < 8192 && log_draw_index(c->draw_count))
        c->dbg_vert_index[c->n_verts] = vert_index;
    c->verts[c->n_verts++] = *v;
}

static void sink_draw_arrays(void* user, const rsx_dispatch* rsx, u32 first, u32 count)
{
    (void)rsx;
    sink_ctx* c = user;
    if (c->n_arr < 256) {
        c->arr[c->n_arr].first = first;
        c->arr[c->n_arr].count = count;
        c->n_arr++;
    }
}

static void sink_draw_index_array(void* user, const rsx_dispatch* rsx, u32 first, u32 count)
{
    (void)rsx;
    sink_ctx* c = user;
    if (c->n_idx < 256) {
        c->idx[c->n_idx].first = first;
        c->idx[c->n_idx].count = count;
        c->n_idx++;
    }
}

static void fetch_one(sink_ctx* c, const rsx_dispatch* rsx, u32 base, u32 vert)
{
    if (!c->dbg_first_vert_valid) {
        c->dbg_first_vert = vert;
        c->dbg_first_vert_valid = 1;
    }
    vtx_t v;
    for (u32 i = 0; i < 16; i++) {
        rsx_dsp_vertex_attr a;
        rsx_dsp_get_vertex_attr(rsx, i, &a);
        if (a.type && a.size && fetch_attr(rsx, i, base, vert, v.a[i]))
            continue;
        if (i == 0) {
            c->fetch_ok = 0;            /* no position stream: skip draw */
            return;
        }
        /* Disabled (or unfetchable) attribute: the VTX_ATTR_4F register
         * default. Attr 3 (diffuse) keeps the stage-2/3 white fallback when
         * the register block was never written, so untranslated draws still
         * modulate by white rather than black. */
        rsx_dsp_vertex_default(rsx, i, v.a[i]);
        if (i == 3 && v.a[3][0] == 0.0f && v.a[3][1] == 0.0f &&
            v.a[3][2] == 0.0f && v.a[3][3] == 1.0f) {
            v.a[3][0] = v.a[3][1] = v.a[3][2] = 1.0f;
        }
    }
    push_vert(c, &v, vert);
}

/* Fetch every accumulated batch's vertices (called at END, once the draw's
 * guest memory has been applied). */
static void fetch_batches(sink_ctx* c, const rsx_dispatch* rsx)
{
    const u32 base = rsx_dsp_vertex_data_base_offset(rsx);
    const u32 base_index = rsx_dsp_vertex_data_base_index(rsx);

    for (u32 r = 0; r < c->n_arr && c->fetch_ok; r++)
        for (u32 i = 0; i < c->arr[r].count && c->fetch_ok; i++)
            fetch_one(c, rsx, base, base_index + c->arr[r].first + i);

    if (!c->n_idx)
        return;
    rsx_dsp_index_array ia;
    rsx_dsp_get_index_array(rsx, &ia);
    /* Root cause (session s25c, oracle: RPCS3 Emu/RSX/RSXThread.cpp's
     * calculate_required_range() -- "if (value == restart) { continue; }" --
     * and Emu/RSX/rsx_methods.h restart_index_enabled()/restart_index()):
     * a raw index equal to the configured restart sentinel is a primitive-
     * restart marker, not a real vertex reference. Measured this session
     * (scratch/s25_skin_diag.md): every exploded-mesh draw on surf 0xE40000
     * has exactly one such index (raw 65535, i.e. 0xFFFF, matching a 16-bit
     * index buffer's default sentinel) landing mid-strip; fetching it as a
     * real vertex reads whatever guest memory sits at "vertex 65535" (here,
     * a region that happens to decode to position (0,0,0)), and the harness
     * previously stitched it into the strip like any other vertex --
     * fanning several long triangles out from that phantom point. */
    /* Kill switch for quick A/B (diagnosing a wall-fracturing regression
     * observed alongside the spike fix this session). */
    static int s_no_restart = -1;
    if (s_no_restart < 0)
        s_no_restart = getenv("RSX_NO_RESTART") ? 1 : 0;
    const int restart_en = !s_no_restart && rsx_dsp_restart_index_enabled(rsx, ia.is_u32);
    const u32 restart_val = rsx_dsp_restart_index(rsx);
    for (u32 r = 0; r < c->n_idx && c->fetch_ok; r++) {
        for (u32 i = 0; i < c->idx[r].count && c->fetch_ok; i++) {
            const u32 esz = ia.is_u32 ? 4 : 2;
            const u8* ip = guest_ptr(ia.location, ia.offset + (c->idx[r].first + i) * esz);
            if (!ip) {
                c->fetch_ok = 0;
                return;
            }
            const u32 index = ia.is_u32
                ? (((u32)ip[0] << 24) | ((u32)ip[1] << 16) | ((u32)ip[2] << 8) | ip[3])
                : (u32)((ip[0] << 8) | ip[1]);
            if (restart_en && index == restart_val) {
                if (c->n_cuts < 520)
                    c->cuts[c->n_cuts++] = c->n_verts;
                continue;
            }
            fetch_one(c, rsx, base, base_index + index);
        }
    }
}

/* [skin] attribution probe (coordinator follow-up, session s25b): RSX_SKIP_DRAW
 * = comma-separated list of draw indices (the same "[draw N]"/"[skin] draw N"
 * numbering already printed) to drop from sink_end() entirely — no vertex
 * buffer write, no PSO bind, no DrawInstanced — so the composite/surface
 * dumps can be pixel-compared against a baseline run to test "does removing
 * this ONE draw remove the spiky mesh". Parsed once; prints an armed banner
 * (LESSONS #21: a negative result needs a live probe, not a silent no-op). */
static int skip_draw_index(u32 draw_index)
{
    static int inited = 0;
    static u32 list[4096];
    static u32 n = 0;
    if (!inited) {
        inited = 1;
        const char* env = getenv("RSX_SKIP_DRAW");
        if (env && env[0]) {
            static char buf[65536];
            strncpy(buf, env, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            /* tokens are either a plain index or an "A-B" inclusive range,
             * so a whole surface's draw span can be dropped in one probe */
            char* tok = strtok(buf, ",");
            while (tok && n < 4096) {
                char* dash = strchr(tok, '-');
                if (dash) {
                    *dash = '\0';
                    u32 lo = (u32)strtoul(tok, NULL, 10);
                    u32 hi = (u32)strtoul(dash + 1, NULL, 10);
                    for (u32 v = lo; v <= hi && n < 4096; v++)
                        list[n++] = v;
                } else {
                    list[n++] = (u32)strtoul(tok, NULL, 10);
                }
                tok = strtok(NULL, ",");
            }
            printf("[replay] RSX_SKIP_DRAW ARMED: %u draw(s) will be dropped from sink_end"
                   " (first few:", n);
            for (u32 i = 0; i < n && i < 10; i++)
                printf(" %u", list[i]);
            printf("%s)\n", n > 10 ? " ..." : "");
        }
    }
    for (u32 i = 0; i < n; i++)
        if (list[i] == draw_index)
            return 1;
    return 0;
}

/* RSX_LOG_DRAW = comma/range list of draw indices (same numbering as
 * RSX_SKIP_DRAW) to force the full per-draw diagnostic block (state
 * dump + texture list) regardless of the is_skinned classification or the
 * draw_count<40 cap — for comparing one named draw's full state (e.g. a
 * bisection-identified culprit) against a healthy draw sharing the same VP. */
static int log_draw_index(u32 draw_index)
{
    static int inited = 0;
    static u32 list[4096];
    static u32 n = 0;
    if (!inited) {
        inited = 1;
        const char* env = getenv("RSX_LOG_DRAW");
        if (env && env[0]) {
            static char buf[65536];
            strncpy(buf, env, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            char* tok = strtok(buf, ",");
            while (tok && n < 4096) {
                char* dash = strchr(tok, '-');
                if (dash) {
                    *dash = '\0';
                    u32 lo = (u32)strtoul(tok, NULL, 10);
                    u32 hi = (u32)strtoul(dash + 1, NULL, 10);
                    for (u32 v = lo; v <= hi && n < 4096; v++)
                        list[n++] = v;
                } else {
                    list[n++] = (u32)strtoul(tok, NULL, 10);
                }
                tok = strtok(NULL, ",");
            }
            printf("[replay] RSX_LOG_DRAW ARMED: %u draw(s) will be force-logged\n", n);
        }
    }
    for (u32 i = 0; i < n; i++)
        if (list[i] == draw_index)
            return 1;
    return 0;
}

/* Expand the accumulated primitive to a triangle/line list and record a draw. */
static void sink_end(void* user, const rsx_dispatch* rsx)
{
    sink_ctx* c = user;
    const u32 prim = rsx->current_primitive;
    fetch_batches(c, rsx);
    if (!c->n_verts || !c->fetch_ok) {
        if (!c->fetch_ok && prim < 16)
            c->skipped_prims[prim]++;
        return;
    }

    /* s27 part 3: detect+flush a finished zeta pass. MUST run before this
     * draw's own vertex data is memcpy'd into the shared g.vb ring further
     * down this function (depth_snapshot_flush executes and drains the
     * command list, resetting g.vb_used/g.cb_used/g.srv_ring_used the same
     * way gpu_flush() does -- doing that AFTER this draw's own vertex
     * upload would orphan it, since the DrawInstanced call later reads the
     * now-reset g.vb_used as its buffer offset instead of where the data
     * actually landed). Safe here: nothing GPU-side for this draw has
     * happened yet, only CPU-side vertex fetch above. */
    depth_pass_track(rsx);

    /* RSX_LOG_DRAW extra: scan every fetched vertex's ATTR0 (position) for
     * this draw and print the bounding box + any outlier beyond a generous
     * scene-scale radius, and any NaN/Inf. Tests the "one bad vertex in a
     * long strip/fan scatters into a spike" hypothesis directly, independent
     * of the skinning-constant path — a strip shares vertices between
     * adjacent triangles, so a single corrupted position fans out into
     * several long degenerate triangles exactly like the observed spikes. */
    if (log_draw_index(c->draw_count)) {
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        u32 n_outlier = 0, n_nan = 0;
        int first_outlier = -1;
        for (u32 vi = 0; vi < c->n_verts; vi++) {
            const float* p = c->verts[vi].a[0];
            int bad = 0;
            for (int k = 0; k < 3; k++) {
                if (p[k] != p[k]) { bad = 1; n_nan++; } /* NaN */
                else {
                    if (p[k] < mn[k]) mn[k] = p[k];
                    if (p[k] > mx[k]) mx[k] = p[k];
                    if (p[k] > 500.0f || p[k] < -500.0f) bad = 1;
                }
            }
            if (bad) {
                n_outlier++;
                if (first_outlier < 0) {
                    first_outlier = (int)vi;
                    printf("       [posscan] first outlier vertex #%u pos=(%.3f %.3f %.3f)\n",
                           vi, p[0], p[1], p[2]);
                }
            }
        }
        printf("       [posscan] draw %u: %u verts, ATTR0 bbox min=(%.3f %.3f %.3f)"
               " max=(%.3f %.3f %.3f), %u outlier(s) (|coord|>500), %u NaN\n",
               c->draw_count, c->n_verts, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2],
               n_outlier, n_nan);
        /* Batch structure: does this draw accumulate MULTIPLE separate
         * DRAW_ARRAYS/DRAW_INDEX_ARRAY (first,count) ranges into one
         * BEGIN/END? If so, our PRIM_TRIANGLE_STRIP CPU expansion below
         * treats the CONCATENATION as a single continuous strip, which
         * would synthesize a bogus connecting triangle between the end of
         * range N and the start of range N+1 whenever real hardware treats
         * each accumulated range as an independent strip restart. */
        {
            rsx_dsp_index_array dbg_ia = {0};
            if (c->n_idx)
                rsx_dsp_get_index_array(rsx, &dbg_ia);
            printf("       [batches] draw %u: n_arr=%u n_idx=%u n_cuts(restart)=%u"
                   " idx_is_u32=%u restart_en=%u restart_val=%u\n",
                   c->draw_count, c->n_arr, c->n_idx, c->n_cuts, dbg_ia.is_u32,
                   c->n_idx ? rsx_dsp_restart_index_enabled(rsx, dbg_ia.is_u32) : 0,
                   rsx_dsp_restart_index(rsx));
        }
        for (u32 bi = 0; bi < c->n_arr; bi++)
            printf("         arr[%u] first=%u count=%u (ends at %u)\n",
                   bi, c->arr[bi].first, c->arr[bi].count, c->arr[bi].first + c->arr[bi].count);
        for (u32 bi = 0; bi < c->n_idx; bi++)
            printf("         idx[%u] first=%u count=%u (ends at %u)\n",
                   bi, c->idx[bi].first, c->idx[bi].count, c->idx[bi].first + c->idx[bi].count);
        for (u32 ci = 0; ci < c->n_cuts; ci++)
            printf("         cut[%u] at local vertex %u\n", ci, c->cuts[ci]);
        /* Position jump AT each batch boundary in fetch (concatenation)
         * order: arr[] batches are fetched first, then idx[] batches
         * (fetch_batches()). A genuine continuous strip has small
         * vertex-to-vertex distances throughout; a real restart point
         * (which our concatenation does NOT insert a break for) should
         * show as an anomalously large jump exactly at a batch boundary. */
        {
            u32 cum = 0;
            int is_first = 1;
            for (u32 bi = 0; bi < c->n_arr; bi++) {
                cum += c->arr[bi].count;
                if (!is_first || bi > 0) { /* boundary before this batch */ }
                is_first = 0;
                if (cum < c->n_verts && cum > 0) {
                    const float* a = c->verts[cum - 1].a[0];
                    const float* b = c->verts[cum].a[0];
                    float d = sqrtf((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) + (a[2]-b[2])*(a[2]-b[2]));
                    printf("         [boundary] after arr[%u] (local idx %u->%u): "
                           "pos (%.3f %.3f %.3f) -> (%.3f %.3f %.3f) dist=%.3f\n",
                           bi, cum - 1, cum, a[0], a[1], a[2], b[0], b[1], b[2], d);
                }
            }
            for (u32 bi = 0; bi < c->n_idx; bi++) {
                cum += c->idx[bi].count;
                if (cum < c->n_verts && cum > 0) {
                    const float* a = c->verts[cum - 1].a[0];
                    const float* b = c->verts[cum].a[0];
                    float d = sqrtf((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) + (a[2]-b[2])*(a[2]-b[2]));
                    printf("         [boundary] after idx[%u] (local idx %u->%u): "
                           "pos (%.3f %.3f %.3f) -> (%.3f %.3f %.3f) dist=%.3f\n",
                           bi, cum - 1, cum, a[0], a[1], a[2], b[0], b[1], b[2], d);
                }
            }
        }
        /* Full internal scan: the batch-boundary fix (256-index chunking,
         * likely just the DRAW_INDEX_ARRAY method's 8-bit count-field limit,
         * not a real restart) did not remove the spike on rebuild+rerun --
         * so look for the true largest consecutive-vertex jump ANYWHERE in
         * the fetched run, not just at the two chunk seams. */
        {
            float worst = -1.0f;
            u32 worst_i = 0;
            for (u32 i = 0; i + 1 < c->n_verts; i++) {
                const float* a = c->verts[i].a[0];
                const float* b = c->verts[i + 1].a[0];
                float d = sqrtf((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) + (a[2]-b[2])*(a[2]-b[2]));
                if (d > worst) { worst = d; worst_i = i; }
            }
            if (worst >= 0.0f) {
                const float* a = c->verts[worst_i].a[0];
                const float* b = c->verts[worst_i + 1].a[0];
                const int have_idx = worst_i + 1 < 8192;
                printf("       [maxjump] draw %u: largest consecutive-vertex jump at local %u->%u"
                       " (guest idx %u->%u):"
                       " pos (%.3f %.3f %.3f) -> (%.3f %.3f %.3f) dist=%.3f\n",
                       c->draw_count, worst_i, worst_i + 1,
                       have_idx ? c->dbg_vert_index[worst_i] : 0xFFFFFFFFu,
                       have_idx ? c->dbg_vert_index[worst_i + 1] : 0xFFFFFFFFu,
                       a[0], a[1], a[2], b[0], b[1], b[2], worst);
            }
        }
    }

    /* Segment boundaries for the adjacency-dependent primitives below
     * (STRIP/FAN): built from primitive-restart cut points recorded by
     * fetch_batches() (c->cuts), NOT from DRAW_ARRAYS/DRAW_INDEX_ARRAY
     * batch-call boundaries. An earlier version of this fix segmented on
     * batch-call boundaries instead (RSX/NV40 hardware does distinguish
     * disjoint primitives like TRIANGLES/QUADS from adjacency-dependent
     * STRIP/FAN -- see RPCS3's Emu/RSX/Common/BufferUtils.cpp
     * is_primitive_disjointed() -- and RPCS3's own draw_clause::append()
     * does insert a primitive_restart_barrier at contiguous same-primitive
     * batch seams); that version was built, rebuilt, and re-rendered this
     * session and did NOT remove the visible spikes, refuting batch-call
     * boundaries as the (or a sufficient) cut signal for THIS capture --
     * likely because the observed 3-batch-per-draw pattern (variable,256,
     * variable counts) is just DRAW_INDEX_ARRAY's 8-bit count-field
     * encoding limit (256 = max per method call), not a genuine RPCS3-side
     * FIFO-flattened multi-begin-end merge. The actual, measured root
     * (confirmed by rebuilding with this fix and re-rendering, see
     * scratch/s25_skin_diag.md): every exploded-mesh draw on surf 0xE40000
     * contains a raw index of 65535 (0xFFFF) buried INSIDE one of those
     * chunks -- the primitive-restart sentinel -- which fetch_batches()
     * now detects and cuts on instead of fetching a phantom vertex. */
    u32 seg_start[521], seg_count[521];
    u32 n_seg = 0;
    {
        u32 prev = 0;
        for (u32 ci = 0; ci < c->n_cuts && n_seg < 521; ci++) {
            const u32 cut = c->cuts[ci];
            seg_start[n_seg] = prev;
            seg_count[n_seg] = cut > prev ? cut - prev : 0;
            n_seg++;
            prev = cut;
        }
        if (n_seg < 521) {
            seg_start[n_seg] = prev;
            seg_count[n_seg] = c->n_verts > prev ? c->n_verts - prev : 0;
            n_seg++;
        }
    }

    /* CPU-expand to a triangle list */
    vtx_t* tri = NULL;
    u32 n_tri = 0;
    switch (prim) {
    case PRIM_TRIANGLES:
        tri = c->verts;
        n_tri = c->n_verts - c->n_verts % 3;
        break;
    case PRIM_TRIANGLE_STRIP: {
        if (c->n_verts < 3) return;
        u32 total = 0;
        for (u32 s = 0; s < n_seg; s++)
            if (seg_count[s] >= 3) total += (seg_count[s] - 2) * 3;
        if (!total) return;
        n_tri = total;
        tri = malloc(n_tri * sizeof(vtx_t));
        u32 w = 0;
        for (u32 s = 0; s < n_seg; s++) {
            const u32 base = seg_start[s], cnt = seg_count[s];
            if (cnt < 3) continue;
            /* Strip winding alternates every other triangle to keep face
             * orientation consistent (root cause, session s25d, coordinator
             * hypothesis -- confirmed live: YZ_B1_CULL=0 A/B made the
             * checkerboard vanish completely). Cull is very much NOT
             * always off (RasterizerState.CullMode is a real, enabled
             * D3D12 state whenever the guest's cull_enable/cull_face regs
             * say so, gated by g_b1_cull -- default ON), so the previous
             * "cull is off, keep it simple" comment was simply wrong,
             * predating this session. Local `i` already resets to 0 at the
             * start of every per-segment loop (a fresh C variable each `s`
             * iteration), so parity is automatically reset at every
             * primitive-restart cut with no extra bookkeeping needed --
             * the bug was purely "never flips odd triangles", not a stale
             * cross-segment parity counter. */
            for (u32 i = 0; i + 2 < cnt; i++) {
                if (i & 1) {
                    tri[w * 3 + 0] = c->verts[base + i + 1];
                    tri[w * 3 + 1] = c->verts[base + i];
                    tri[w * 3 + 2] = c->verts[base + i + 2];
                } else {
                    tri[w * 3 + 0] = c->verts[base + i];
                    tri[w * 3 + 1] = c->verts[base + i + 1];
                    tri[w * 3 + 2] = c->verts[base + i + 2];
                }
                w++;
            }
        }
        break;
    }
    case PRIM_TRIANGLE_FAN: {
        if (c->n_verts < 3) return;
        u32 total = 0;
        for (u32 s = 0; s < n_seg; s++)
            if (seg_count[s] >= 3) total += (seg_count[s] - 2) * 3;
        if (!total) return;
        n_tri = total;
        tri = malloc(n_tri * sizeof(vtx_t));
        u32 w = 0;
        for (u32 s = 0; s < n_seg; s++) {
            const u32 base = seg_start[s], cnt = seg_count[s];
            if (cnt < 3) continue;
            /* each segment fans from ITS OWN first vertex, not a global
             * one -- a restart resets the fan's center, same as the strip
             * case resets adjacency */
            for (u32 i = 1; i + 1 < cnt; i++) {
                tri[w * 3 + 0] = c->verts[base];
                tri[w * 3 + 1] = c->verts[base + i];
                tri[w * 3 + 2] = c->verts[base + i + 1];
                w++;
            }
        }
        break;
    }
    case PRIM_QUADS: {
        const u32 quads = c->n_verts / 4;
        if (!quads) return;
        n_tri = quads * 6;
        tri = malloc(n_tri * sizeof(vtx_t));
        for (u32 q = 0; q < quads; q++) {
            const vtx_t* v = &c->verts[q * 4];
            vtx_t* t = &tri[q * 6];
            t[0] = v[0]; t[1] = v[1]; t[2] = v[2];
            t[3] = v[2]; t[4] = v[3]; t[5] = v[0];
        }
        break;
    }
    default:
        if (prim < 16)
            c->skipped_prims[prim]++;
        return;
    }

    /* RSX_DEPTH_DUMP_PRE: raw depth readback right before this draw index
     * is processed (no GPU work for it recorded yet). See the definition
     * near depth_snapshot_flush() for the format/rationale. */
    {
        const char* ddp = ddump_lookup(g_ddump_pre, g_ddump_pre_n, c->draw_count);
        if (ddp) dump_depth_raw(ddp);
    }

    /* RSX_SKIP_DRAW attribution probe: drop this draw entirely (no VB write,
     * no PSO, no DrawInstanced) but still consume its draw_count slot so
     * every OTHER draw keeps the exact same index as the baseline run —
     * that's what makes the before/after pixel-compare valid. */
    if (skip_draw_index(c->draw_count)) {
        printf("[replay] SKIP draw %u (RSX_SKIP_DRAW)\n", c->draw_count);
        c->draw_count++;
        if (tri != c->verts) free(tri);
        return;
    }

    if ((u64)g.vb_used + (u64)n_tri * VERT_STRIDE > (u64)MAX_VERTS * VERT_STRIDE) {
        if ((u64)n_tri * VERT_STRIDE > (u64)MAX_VERTS * VERT_STRIDE) {
            /* A single batch larger than the whole buffer cannot be drawn
             * without growing MAX_VERTS; drop it and report. */
            printf("[replay] draw exceeds vertex buffer capacity, dropping"
                   " (%u verts > %u cap)\n", n_tri, MAX_VERTS);
            if (tri != c->verts) free(tri);
            return;
        }
        /* Recycle the buffer: flush what we have, then keep recording. */
        gpu_flush();
    }

    memcpy(g.vb_mapped + g.vb_used, tri, (size_t)n_tri * VERT_STRIDE);

    /* Stage 4: translate the active VP+FP pair; NULL -> fixed fallback. */
    u64 dbg_vp_key = 0;
    int dbg_is_skinned = 0;
    ID3D12PipelineState* xpso = get_translated_pso(rsx, &dbg_vp_key, &dbg_is_skinned);
    if (xpso && g.cb_used + CB_BLOCK_ALIGNED > CB_RING_BYTES) {
        printf("[xlat] constant ring full, draw falls back\n");
        xpso = NULL;
    }

    /* Fallback transform columns: uploaded constants c0..c3 as the MVP when
     * present, else the RSX viewport scale/translate inverse (stage 2). */
    float cols[16];
    int have_mvp = 0;
    for (u32 s = 0; s < 4; s++) {
        memcpy(&cols[s * 4], rsx_dsp_constant(rsx, s), 16);
        for (u32 k = 0; k < 4; k++)
            if (cols[s * 4 + k] != 0.0f)
                have_mvp = 1;
    }
    if (!have_mvp) {
        rsx_dsp_viewport vp;
        rsx_dsp_get_viewport(rsx, &vp);
        const float w = vp.w ? (float)vp.w : (float)g.width;
        const float h = vp.h ? (float)vp.h : (float)g.height;
        memset(cols, 0, sizeof(cols));
        cols[0]  = 2.0f / w;          /* c0.x */
        cols[5]  = -2.0f / h;         /* c1.y */
        cols[10] = 1.0f;              /* c2.z */
        cols[12] = -1.0f;             /* c3.x */
        cols[13] = 1.0f;              /* c3.y */
        cols[15] = 1.0f;              /* c3.w */
    }

    const u32 target = current_surface(rsx);

    /* [skin] diag (scratch/s24_replay_fixes.md "The remaining defect"):
     * every draw whose VP was detected as 4-bone linear-blend skinning
     * (get_translated_pso's vp_c[467] literal test) gets an UNCAPPED log
     * line naming: draw index, target surface address, VP key/hash,
     * vp_c[467].x (the a0.x bone-index scale const), vp_c[112/113/114].xyzw
     * (candidate bone-matrix rows at the a0.x==0 base), ATTR7's declared
     * format, and ATTR7's raw guest bytes + CPU-decoded value for the first
     * vertex actually fetched this draw (c->dbg_first_vert). Suspects from
     * the diagnostic plan: (a) bone rows 112.. never uploaded/wrong,
     * (b) vp_c[467].x wrong, (c) ATTR7 type/stride decoded wrong.
     *
     * Session s25c (coordinator bisection follow-up): also fires for any
     * draw named in RSX_LOG_DRAW, tagged "[state]" instead of "[skin]" when
     * the draw isn't classified skinned, and extended with primitive/vertex
     * count + c115 so a bisection-identified culprit draw's full state can
     * be dumped and compared against a healthy draw sharing the same VP,
     * regardless of the draw_count<40 cap or skin classification. */
    const int dbg_force_log = log_draw_index(c->draw_count);
    if (dbg_is_skinned || dbg_force_log) {
        union { u32 u; float f; } c467x, c112[4], c113[4], c114[4], c115[4];
        union { u32 u; float f; } c108[4], c109[4], c110[4], c111[4];
        c467x.u = rsx_dsp_constant(rsx, 467)[0];
        for (u32 k = 0; k < 4; k++) {
            c112[k].u = rsx_dsp_constant(rsx, 112)[k];
            c113[k].u = rsx_dsp_constant(rsx, 113)[k];
            c114[k].u = rsx_dsp_constant(rsx, 114)[k];
            c115[k].u = rsx_dsp_constant(rsx, 115)[k];
            /* c108-111: NOT part of the skinning suspect list, but this
             * specific draw's own VP source (scratch/shd_dump2/
             * vp_e2884931033b0e65.hlsl:46-96) shows the clip-space position
             * is dot(dot(v[0], c112..114), c108..111) -- i.e. object-space
             * position first goes through 112-114 (measured identity here,
             * consistent with the false-positive-VP finding earlier this
             * session) and THEN through 108-111 as a second, separate
             * matrix before reaching o[0]/SV_Position. 108-111 is the real
             * suspect for THIS draw, not 112-115. */
            c108[k].u = rsx_dsp_constant(rsx, 108)[k];
            c109[k].u = rsx_dsp_constant(rsx, 109)[k];
            c110[k].u = rsx_dsp_constant(rsx, 110)[k];
            c111[k].u = rsx_dsp_constant(rsx, 111)[k];
        }
        if (dbg_force_log)
            printf("       c108=(%.4f %.4f %.4f %.4f) c109=(%.4f %.4f %.4f %.4f)"
                   " c110=(%.4f %.4f %.4f %.4f) c111=(%.4f %.4f %.4f %.4f)\n",
                   c108[0].f, c108[1].f, c108[2].f, c108[3].f,
                   c109[0].f, c109[1].f, c109[2].f, c109[3].f,
                   c110[0].f, c110[1].f, c110[2].f, c110[3].f,
                   c111[0].f, c111[1].f, c111[2].f, c111[3].f);

        rsx_dsp_vertex_attr a7;
        rsx_dsp_get_vertex_attr(rsx, 7, &a7);
        static const char* type_names[8] = {
            "none", "SNORM16", "FLOAT", "HALF", "UNORM8", "SINT16", "CMP32", "UINT8"
        };
        const char* a7_type_name = a7.type < 8 ? type_names[a7.type] : "?";

        const u32 base_off = rsx_dsp_vertex_data_base_offset(rsx);
        const u32 addr7 = base_off + a7.offset +
            (c->dbg_first_vert_valid ? c->dbg_first_vert : 0) * a7.stride;
        const u8* p7 = guest_ptr(a7.location, addr7);
        char raw_hex[3 * 16 + 1] = {0};
        if (p7 && addr7 <= ARENA_SIZE - 16) {
            const u32 n_raw = 16; /* fixed safety-bound dump, independent of decoded type/size */
            for (u32 k = 0; k < n_raw; k++)
                snprintf(raw_hex + k * 3, 4, "%02X ", p7[k]);
        } else {
            snprintf(raw_hex, sizeof(raw_hex), "<unmapped/near-end loc=%u addr=0x%X>", a7.location, addr7);
        }

        const char* tag = dbg_is_skinned ? "[skin]" : "[state]";
        printf("%s draw %u prim=%u verts=%u surf=0x%X vp_key=%016llx"
               " c467.x=%.6f(0x%08X)"
               " c112=(%.4f %.4f %.4f %.4f) c113=(%.4f %.4f %.4f %.4f)"
               " c114=(%.4f %.4f %.4f %.4f) c115=(%.4f %.4f %.4f %.4f)\n",
               tag, c->draw_count, prim, n_tri, g.surfaces[target].offset,
               (unsigned long long)dbg_vp_key,
               c467x.f, c467x.u,
               c112[0].f, c112[1].f, c112[2].f, c112[3].f,
               c113[0].f, c113[1].f, c113[2].f, c113[3].f,
               c114[0].f, c114[1].f, c114[2].f, c114[3].f,
               c115[0].f, c115[1].f, c115[2].f, c115[3].f);
        printf("       ATTR7: type=%u(%s) size=%u stride=%u loc=%u offset=0x%X"
               " vert_idx=%u raw@0x%X=[ %s] decoded=(%.4f %.4f %.4f %.4f)\n",
               a7.type, a7_type_name, a7.size, a7.stride, a7.location, a7.offset,
               c->dbg_first_vert_valid ? c->dbg_first_vert : 0, addr7, raw_hex,
               tri[0].a[7][0], tri[0].a[7][1], tri[0].a[7][2], tri[0].a[7][3]);

        /* s26 part 5 (scratch/s26_fp_bisect.md): vp_47f48eae1f650e68's own
         * ucode (independently decoded, scratch/s26_vp_decode.py) shows
         * o[1]/o[2] (COL0/COL1, which the paired FP feeds directly into the
         * final pixel color) are NOT sourced from the bone-blend accumulator
         * at all -- their last writers before the output MOVs are plain,
         * non-indexed constant reads vp_c[467].yyyy and vp_c[62].xyzx. Dump
         * both in full (all 4 components) for any RSX_LOG_DRAW-named draw so
         * the runtime value feeding the blue color can be read directly,
         * cross-checked against the .rrc's own CONSTANT_LOAD record for
         * these two slots. Also dumps c58 (residual-3's blit dest-rect
         * constant, vp_f9177145503f1f42, draw 1624) in the same line so one
         * RSX_LOG_DRAW run covers both asks. */
        if (dbg_force_log) {
            union { u32 u; float f; } c467[4], c62[4], c58[4], c63[4];
            for (u32 k = 0; k < 4; k++) {
                c467[k].u = rsx_dsp_constant(rsx, 467)[k];
                c62[k].u = rsx_dsp_constant(rsx, 62)[k];
                c58[k].u = rsx_dsp_constant(rsx, 58)[k];
                c63[k].u = rsx_dsp_constant(rsx, 63)[k];
            }
            /* s27 part 1 (scratch/s26_fp_bisect.md): the last writer to
             * r[3].w before o[5].x=r[3].w (FOGC, the source of the FP's
             * line-95 lerp weight, see scratch/s26p5_vp_47f48eae.txt instr
             * [82]) is `MAD r[0].wwww * vp_c[63].zzzz + vp_c[63].wwww ->
             * r[3].w` -- a classic linear fog scale+bias idiom. Dump c63 to
             * sanity-check it's a plausible scale/bias pair, not garbage. */
            printf("       [s27] c63(fog scale/bias)=(%.6f %.6f %.6f %.6f)\n",
                   c63[0].f, c63[1].f, c63[2].f, c63[3].f);
            printf("       [s26p5] c467=(%.6f %.6f %.6f %.6f) c62=(%.6f %.6f %.6f %.6f)"
                   " c58=(%.6f %.6f %.6f %.6f)\n",
                   c467[0].f, c467[1].f, c467[2].f, c467[3].f,
                   c62[0].f, c62[1].f, c62[2].f, c62[3].f,
                   c58[0].f, c58[1].f, c58[2].f, c58[3].f);
        }

        /* s25f follow-up (scratch/s25_skin_diag.md): the character-class
         * black-shading diagnosis pins EVERY lighting term of the affected
         * FP family on the vertex NORMAL (ATTR2 -> VP o[9] -> FP h[1] =
         * normalize(tc2.xyz)); a degenerate ATTR2 collapses all shading to
         * black. Dump ATTR2 exactly like ATTR7 above: declared format, the
         * first fetched vertex's raw guest bytes, and the CPU-decoded value
         * the VS actually received — discriminates raw-data-zero (capture
         * gap) from wrong-decode (VTXFMT offset/stride) from healthy. */
        rsx_dsp_vertex_attr a2;
        rsx_dsp_get_vertex_attr(rsx, 2, &a2);
        const char* a2_type_name = a2.type < 8 ? type_names[a2.type] : "?";
        const u32 addr2 = base_off + a2.offset +
            (c->dbg_first_vert_valid ? c->dbg_first_vert : 0) * a2.stride;
        const u8* p2 = guest_ptr(a2.location, addr2);
        char raw2_hex[3 * 16 + 1] = {0};
        if (p2 && addr2 <= ARENA_SIZE - 16) {
            for (u32 k = 0; k < 16; k++)
                snprintf(raw2_hex + k * 3, 4, "%02X ", p2[k]);
        } else {
            snprintf(raw2_hex, sizeof(raw2_hex), "<unmapped/near-end loc=%u addr=0x%X>", a2.location, addr2);
        }
        printf("       ATTR2: type=%u(%s) size=%u stride=%u loc=%u offset=0x%X"
               " vert_idx=%u raw@0x%X=[ %s] decoded=(%.4f %.4f %.4f %.4f)\n",
               a2.type, a2_type_name, a2.size, a2.stride, a2.location, a2.offset,
               c->dbg_first_vert_valid ? c->dbg_first_vert : 0, addr2, raw2_hex,
               tri[0].a[2][0], tri[0].a[2][1], tri[0].a[2][2], tri[0].a[2][3]);

        /* s26 follow-up (scratch/s26_fp_bisect.md): forced-output pixel
         * bisection on this draw's FP (fp_4792e42b9f86ad33) found the
         * collapse ALREADY present at the very first FP stage (the raw tex0
         * sample) and traced it to input.tc0 itself reading (0,0) at the
         * black triangles -- i.e. upstream of the FP, in TEXCOORD0 (VP o[7]
         * = vp_c[60] + ATTR8.xy). Dump ATTR8's declared format once plus its
         * PER-VERTEX decoded spread across this whole draw (not just vertex
         * 0, since this draw batches disjoint sub-meshes via primitive
         * restart -- vertex 0 may not belong to the affected sub-mesh) to
         * test whether ATTR8 genuinely varies (some vertices zero, others
         * not) within one draw call sharing one vertex format. */
        rsx_dsp_vertex_attr a8;
        rsx_dsp_get_vertex_attr(rsx, 8, &a8);
        const char* a8_type_name = a8.type < 8 ? type_names[a8.type] : "?";
        float a8min[2] = {  1e30f,  1e30f };
        float a8max[2] = { -1e30f, -1e30f };
        u32 a8_nzero = 0;
        for (u32 vi = 0; vi < c->n_verts; vi++) {
            const float x = c->verts[vi].a[8][0], y = c->verts[vi].a[8][1];
            if (x < a8min[0]) a8min[0] = x;
            if (y < a8min[1]) a8min[1] = y;
            if (x > a8max[0]) a8max[0] = x;
            if (y > a8max[1]) a8max[1] = y;
            if (x == 0.0f && y == 0.0f) a8_nzero++;
        }
        printf("       ATTR8: type=%u(%s) size=%u stride=%u loc=%u offset=0x%X"
               " -- per-draw span over %u verts: xy_min=(%.4f %.4f) xy_max=(%.4f %.4f)"
               " zero_xy_count=%u/%u\n",
               a8.type, a8_type_name, a8.size, a8.stride, a8.location, a8.offset,
               c->n_verts, a8min[0], a8min[1], a8max[0], a8max[1], a8_nzero, c->n_verts);

        /* s26 coordinator follow-up (scratch/s26_fp_bisect.md "per-vertex
         * clip-space depth dump"): the stage-13 control proved the missing
         * fragments never reach the pixel shader, and YZ_B1_DEPTH=0 changed
         * the image drastically, pointing at the depth/position pipeline.
         * vp_4792e42b9f86ad33's clip position o[0]=r[0] is built in exactly
         * two DP4 stages (hand-decoded against the raw ucode this session,
         * scratch/s26_vp_decode.py output, both UNINDEXED plain constant
         * reads, no a0-relative addressing anywhere in this VP):
         *   r7.xyz = ( dot(v0,c112), dot(v0,c113), dot(v0,c114) )
         *   r4 = (r7.xyz, 0)              -- r4.w is never written before
         *                                    this point (array zero-init)
         *   clip.xyzw = ( dot(r4,c108), dot(r4,c109), dot(r4,c110), dot(r4,c111) )
         * Replicated here on the CPU (same constants already read for the
         * c108-111 printf above) for a sample of vertices in EVERY
         * primitive-restart SEGMENT of this draw (c->cuts[]) -- s26 already
         * found draw 1040 batches disjoint sub-meshes across cuts (a
         * correctly-shaded prop and the black region), so a per-segment
         * split is what can actually discriminate them, unlike the
         * whole-draw ATTR8 span above. Gated to RSX_VP_CLIP_DUMP=1 (default
         * off) AND this exact VP (dbg_vp_key match) since the two-DP4
         * formula is specific to this VP's own instruction stream. */
        if (getenv("RSX_VP_CLIP_DUMP") && dbg_vp_key == FP_FORCE_TARGET_KEY) {
            rsx_dsp_viewport cvp;
            rsx_dsp_get_viewport(rsx, &cvp);
            printf("       [clipdump] draw %u: vp scale=(%.4f %.4f %.4f)"
                   " translate=(%.4f %.4f %.4f) n_segs=%u n_verts=%u\n",
                   c->draw_count, cvp.scale[0], cvp.scale[1], cvp.scale[2],
                   cvp.translate[0], cvp.translate[1], cvp.translate[2],
                   c->n_cuts + 1, c->n_verts);

            u32 seg_start = 0;
            for (u32 s = 0; s <= c->n_cuts; s++) {
                const u32 seg_end = (s < c->n_cuts) ? c->cuts[s] : c->n_verts; /* exclusive */
                if (seg_end <= seg_start) { seg_start = seg_end; continue; }
                const u32 samples[3] = { seg_start, (seg_start + seg_end - 1) / 2, seg_end - 1 };
                float ndc_min = 1e30f, ndc_max = -1e30f, w_min = 1e30f, w_max = -1e30f;
                int any_behind = 0, any_nan = 0;
                char sampletxt[300] = {0};
                int stpos = 0;
                for (u32 si = 0; si < 3; si++) {
                    const u32 vi = samples[si];
                    if (vi >= c->n_verts) continue;
                    const float* p0 = c->verts[vi].a[0]; /* ATTR0, object-space position */
                    const float v0w = p0[3] != 0.0f ? p0[3] : 1.0f;
                    const float r7x = p0[0]*c112[0].f + p0[1]*c112[1].f + p0[2]*c112[2].f + v0w*c112[3].f;
                    const float r7y = p0[0]*c113[0].f + p0[1]*c113[1].f + p0[2]*c113[2].f + v0w*c113[3].f;
                    const float r7z = p0[0]*c114[0].f + p0[1]*c114[1].f + p0[2]*c114[2].f + v0w*c114[3].f;
                    /* CORRECTED this session: r4.w is NOT 0 here. Ucode
                     * instruction [2] (scratch/s26_vp_decode.txt) sets
                     * r[4].w = dot(v[0], vp_c[115]) BEFORE r[4].xyz is even
                     * written (instruction [11]) -- an earlier hand-trace
                     * missed this and assumed r4.w=0, which silently dropped
                     * the c108-111 rows' .w column entirely and produced
                     * nonsensical clip_x/clip_w ratios (~[-2,-20], nowhere
                     * near a plausible NDC range) on the first run of this
                     * dump. c115 is measured identity-like (0,0,0,1) for
                     * these draws, so r4.w = dot(v0,c115) = v0.w = ATTR0's
                     * own w (typically 1.0), NOT 0. */
                    const float r4w = p0[0]*c115[0].f + p0[1]*c115[1].f + p0[2]*c115[2].f + v0w*c115[3].f;
                    const float clip_x = r7x*c108[0].f + r7y*c108[1].f + r7z*c108[2].f + r4w*c108[3].f;
                    const float clip_y = r7x*c109[0].f + r7y*c109[1].f + r7z*c109[2].f + r4w*c109[3].f;
                    const float clip_z = r7x*c110[0].f + r7y*c110[1].f + r7z*c110[2].f + r4w*c110[3].f;
                    const float clip_w = r7x*c111[0].f + r7y*c111[1].f + r7z*c111[2].f + r4w*c111[3].f;
                    const float ndc_z = (clip_w != 0.0f)
                        ? (clip_z * cvp.scale[2] + clip_w * cvp.translate[2]) / clip_w
                        : 0.0f;
                    if (clip_w <= 0.0f) any_behind = 1;
                    if (clip_w != clip_w || ndc_z != ndc_z) any_nan = 1;
                    if (ndc_z < ndc_min) ndc_min = ndc_z;
                    if (ndc_z > ndc_max) ndc_max = ndc_z;
                    if (clip_w < w_min) w_min = clip_w;
                    if (clip_w > w_max) w_max = clip_w;
                    /* RSX window-space xy directly from clip/w * viewport
                     * scale + translate (same semantics as the xf[] upload
                     * a few hundred lines up) -- lets a segment's screen
                     * footprint be read off directly, not guessed from
                     * vertex-index order. */
                    const float scr_x = (clip_w != 0.0f) ? (clip_x/clip_w)*cvp.scale[0] + cvp.translate[0] : 0.0f;
                    const float scr_y = (clip_w != 0.0f) ? (clip_y/clip_w)*cvp.scale[1] + cvp.translate[1] : 0.0f;
                    stpos += snprintf(sampletxt + stpos, (size_t)(sizeof(sampletxt) - stpos),
                        "[v%u scr=(%.1f,%.1f) clip=(%.3f %.3f %.6f %.6f) ndc_z=%.9f] ",
                        vi, scr_x, scr_y, clip_x, clip_y, clip_z, clip_w, ndc_z);
                }
                printf("         seg %u: verts[%u,%u) n=%u ndc_z_range=[%.9f,%.9f]"
                       " clip_w_range=[%.4f,%.4f]%s%s -- %s\n",
                       s, seg_start, seg_end, seg_end - seg_start, ndc_min, ndc_max,
                       w_min, w_max, any_behind ? " BEHIND-EYE(w<=0)" : "",
                       any_nan ? " NAN" : "", sampletxt);
                seg_start = seg_end;
            }
        }
    }

    /* Resolve the fragment texture units into a 16-slot table (fallback
     * only uses t0). A previously-rendered surface at the same (location,
     * offset) wins over guest memory: the capture's memory blocks hold
     * stale (pre-frame) contents for render targets. */
    u32 slots[SRV_TABLE_SIZE];
    u32 smp_slots[SMP_TABLE_SIZE];      /* B1: per-unit sampler cache slots  */
    u32 surf_used[SRV_TABLE_SIZE];
    u32 n_surf_used = 0;
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++)
        slots[u] = SRV_WHITE;
    for (u32 u = 0; u < SMP_TABLE_SIZE; u++)
        smp_slots[u] = SMP_DEFAULT;
    rsx_dsp_texture t0;
    rsx_dsp_get_texture(rsx, 0, &t0);
    const u32 n_units = xpso ? SRV_TABLE_SIZE : 1;
    for (u32 u = 0; u < n_units; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(rsx, u, &t);
        if (!t.enabled)
            continue;
        /* B1: honor this unit's captured filter/wrap/LOD */
        if (g_b1_samp)
            smp_slots[u] = sampler_slot(&t, sampler_key(&t));
        int sampled_surface = -1;
        for (u32 i = 0; i < g.n_surfaces; i++) {
            if (g.surfaces[i].location == t.location &&
                g.surfaces[i].offset == t.offset && i != target) {
                sampled_surface = (int)i;
                break;
            }
        }
        /* s27 part 3: a depth/zeta target rendered earlier this frame and
         * later sampled as a texture (the tex15-class shadow-map read) --
         * check AFTER color surfaces (a color-RTV match should always win
         * if both somehow existed) but BEFORE falling to raw guest memory,
         * which is stale/zeroed for an address that's never actually
         * written back into the capture's own memory blocks. */
        int sampled_depth = -1;
        if (sampled_surface < 0 && honor_depth_rt()) {
            for (u32 i = 0; i < g.n_depth_surfaces; i++) {
                if (g.depth_surfaces[i].location == t.location &&
                    g.depth_surfaces[i].offset == t.offset && g.depth_surfaces[i].tex) {
                    sampled_depth = (int)i;
                    break;
                }
            }
        }
        if (sampled_surface >= 0) {
            /* s29: this texture unit's GAME-DECLARED size (t.width/height)
             * vs the physical canvas every surface_get() texture is
             * allocated at (g.width/g.height) -- see honor_surf_crop()'s
             * comment for the mechanism. Only crop when declared is
             * strictly smaller (the mismatch case); an exact match (e.g.
             * a surface already at full canvas size) takes the unmodified
             * raw-bind path, identical to pre-fix behavior. */
            ID3D12Resource* cropped = NULL;
            /* s31 FIX 2: with logical-size surfaces the declared-vs-physical
             * mismatch this crop existed for is gone whenever the texture
             * unit declares the surface's own size; compare against the
             * surface's real dims, not the display canvas. */
            const u32 ss_w = g.surfaces[sampled_surface].w ? g.surfaces[sampled_surface].w : g.width;
            const u32 ss_h = g.surfaces[sampled_surface].h ? g.surfaces[sampled_surface].h : g.height;
            if (honor_surf_crop() && t.width && t.height &&
                (t.width != ss_w || t.height != ss_h) &&
                t.width <= ss_w && t.height <= ss_h)
                cropped = surface_crop_flush((u32)sampled_surface, t.width, t.height);
            if (cropped) {
                slots[u] = SRV_CROP_BASE + (u32)sampled_surface;
            } else {
                slots[u] = SRV_SURFACE_BASE + sampled_surface;
                u32 seen = 0;
                for (u32 k = 0; k < n_surf_used; k++)
                    if (surf_used[k] == (u32)sampled_surface)
                        seen = 1;
                if (!seen && n_surf_used < SRV_TABLE_SIZE)
                    surf_used[n_surf_used++] = (u32)sampled_surface;
            }
        } else if (sampled_depth >= 0) {
            slots[u] = SRV_DEPTH_BASE + sampled_depth;
        } else {
            slots[u] = texture_srv_slot(&t);
        }
        const int dbg_target =
            (target < g.n_surfaces && g.surfaces[target].offset == 0x3C0000);
        if ((c->draw_count < 40 || dbg_target || dbg_force_log) && t.enabled)
            printf("          tex%u <- %s 0x%X (%ux%u fmt=0x%02X)%s\n", u,
                   sampled_surface >= 0 ? "surface" : sampled_depth >= 0 ? "depth-rt" : "guest",
                   t.offset, t.width, t.height, t.format & 0xFF,
                   slots[u] == SRV_WHITE ? " [WHITE-FALLBACK]" : "");
    }

    if (c->draw_count < 40 || dbg_force_log ||
        (target < g.n_surfaces && g.surfaces[target].offset == 0x3C0000)) {
        const float* p0 = tri[0].a[0];
        rsx_dsp_surface sf;
        rsx_dsp_get_surface(rsx, &sf);
        render_state_t drs;
        decode_render_state(rsx, &drs);
        printf("[draw %2u] prim=%u verts=%u surf=0x%X %s v0=(%.2f %.2f %.2f)"
               " cull(en=%u face=0x%X front=0x%X) blend=%u depth(t=%u w=%u f=0x%X)"
               " cmask_raw=0x%08X cmask_eff=0x%08X\n",
               c->draw_count, prim, n_tri, sf.color_offset[0],
               xpso ? "XLAT" : (have_mvp ? "fb-mvp" : "fb-vp"),
               p0[0], p0[1], p0[2],
               drs.cull_enable, drs.cull_face, drs.front_face,
               drs.blend_enable, drs.depth_test, drs.depth_write, drs.depth_func,
               rsx_dsp_reg(rsx, M_COLOR_MASK), drs.color_mask);
        /* s27 part 3 (scratch/s26_fp_bisect.md, coordinator's depth-RT fix):
         * is any draw's ZETA target ever the tex15-class address (0x2910000)
         * or does a depth-only pass (RT_ENABLE with no color, depth write
         * on) exist at all before draw 1370? Diagnostic-only, gated on the
         * same RSX_LOG_DRAW range so a wide scan (e.g. RSX_LOG_DRAW=0-1370)
         * answers the capture-scope question directly. */
        if (getenv("RSX_LOG_ZETA"))
            printf("          [s27zeta] zeta_offset=0x%X zeta_loc=%u zeta_pitch=%u"
                   " color_target=0x%X depth(t=%u w=%u)\n",
                   sf.zeta_offset, sf.zeta_location, sf.zeta_pitch,
                   sf.color_target, drs.depth_test, drs.depth_write);
    }

    /* transition every sampled surface RT -> SRV around the draw */
    D3D12_RESOURCE_BARRIER bar = {0};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    for (u32 k = 0; k < n_surf_used; k++) {
        bar.Transition.pResource = g.surfaces[surf_used[k]].tex;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &bar);
    }

    /* B1: bind the shared depth target with the color RT so depth-test/write
     * state has somewhere to act; clear it once per frame to the far plane. */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = surface_rtv(target);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    int have_dsv = 0;
    if (g.depth) {
        /* s31 FIX 1: bind THIS draw's zeta target's own depth buffer (slot 0
         * = legacy shared fallback under RSX_NO_ZETA_TRACK / create-fail). */
        u32 zslot = 0;
        if (honor_zeta_track()) {
            rsx_dsp_surface zsf;
            rsx_dsp_get_surface(rsx, &zsf);
            zslot = zdepth_get(zsf.zeta_location, zsf.zeta_offset, zsf.clip_w, zsf.clip_h);
        }
        dsv = dsv_handle(zslot);
        have_dsv = 1;
        if (zslot) {
            zdepth_t* z = &g.zdepths[zslot - 1];
            if (!z->cleared) {   /* per-frame first-use safety net (mirrors
                                  * the legacy g.depth_cleared latch) */
                g.list->lpVtbl->ClearDepthStencilView(
                    g.list, dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                    1.0f, 0, 0, NULL);
                z->cleared = 1;
            }
            g_zdump_res = z->tex;
        } else {
            if (!g.depth_cleared) {
                g.list->lpVtbl->ClearDepthStencilView(
                    g.list, dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                    1.0f, 0, 0, NULL);
                g.depth_cleared = 1;
            }
            g_zdump_res = g.depth;
        }
    }
    g.list->lpVtbl->OMSetRenderTargets(g.list, 1, &rtv, FALSE, have_dsv ? &dsv : NULL);
    ID3D12DescriptorHeap* heaps[] = {g.srv_heap, g.smp_heap};
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE table = srv_table(slots);
    const D3D12_GPU_DESCRIPTOR_HANDLE smp_tbl = sampler_table(smp_slots);

    D3D12_VIEWPORT dvp = {0, 0, (float)g.width, (float)g.height, 0.0f, 1.0f};
    if (xpso) {
        /* per-draw constant block: 512 vec4 constants + the RSX viewport
         * transform pre-mapped to D3D clip space (see fill in RSXDrawCommands
         * semantics: ndc = clip.xyz*scale + clip.w*offset, y negated because
         * RSX window y points down while D3D NDC y points up) */
        rsx_dsp_surface sf;
        rsx_dsp_viewport vp;
        rsx_dsp_get_surface(rsx, &sf);
        rsx_dsp_get_viewport(rsx, &vp);
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
        u8* dst = g.cb_mapped + g.cb_used;
        memcpy(dst, rsx->constants, RSX_DSP_NUM_CONSTANTS * 16);
        memcpy(dst + 512 * 16, xf, sizeof(xf));

        if (c->draw_count < 40 || dbg_force_log)
            printf("          vp=(%u,%u %ux%u) scale=(%.2f %.2f %.4f)"
                   " trans=(%.2f %.2f %.4f) clip=%ux%u\n",
                   vp.x, vp.y, vp.w, vp.h, vp.scale[0], vp.scale[1], vp.scale[2],
                   vp.translate[0], vp.translate[1], vp.translate[2],
                   sf.clip_w, sf.clip_h);

        /* s27 part 1 (scratch/s26_fp_bisect.md residual 3, x=820 scanout
         * band): the harness never reads NV4097_SET_SCISSOR_HORIZONTAL/
         * VERTICAL (0x8C0/0x8C4) at all -- every register write lands in
         * rsx->regs[] unconditionally (rsx_dispatch.c:220) regardless of
         * whether any consumer decodes it, so the capture's real scissor
         * value is sitting right here, just unread. Decode it the same way
         * rsx_commands.c's (unused-by-this-path) NV4097_SET_SCISSOR_*
         * handler does: x=lo16,w=hi16 / y=lo16,h=hi16. Diagnostic-only
         * print, gated on the same RSX_LOG_DRAW/dbg_force_log condition as
         * the vp= line above -- no behavior change here, see
         * RSX_HONOR_SCISSOR below for the actual fix. */
        if (dbg_force_log) {
            const u32 sc_h_reg = rsx_dsp_reg(rsx, 0x000008C0u); /* NV4097_SET_SCISSOR_HORIZONTAL */
            const u32 sc_v_reg = rsx_dsp_reg(rsx, 0x000008C4u); /* NV4097_SET_SCISSOR_VERTICAL */
            printf("          [s27scissor] raw_h=0x%08X raw_v=0x%08X"
                   " decoded=(x=%u y=%u w=%u h=%u) surf_clip=%ux%u\n",
                   sc_h_reg, sc_v_reg,
                   sc_h_reg & 0xFFFFu, sc_v_reg & 0xFFFFu,
                   sc_h_reg >> 16, sc_v_reg >> 16,
                   sf.clip_w, sf.clip_h);
        }

        g.list->lpVtbl->SetPipelineState(g.list, xpso);
        g.list->lpVtbl->SetGraphicsRootSignature(g.list, g.rootsig_x);
        g.list->lpVtbl->SetGraphicsRootConstantBufferView(
            g.list, 0, g.cb->lpVtbl->GetGPUVirtualAddress(g.cb) + g.cb_used);
        g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 1, table);
        g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 2, smp_tbl); /* B1 samplers */
        g.cb_used += CB_BLOCK_ALIGNED;
        dvp.Width = W;
        dvp.Height = H;
        c->draws_xlat++;
    } else {
        g.list->lpVtbl->SetPipelineState(g.list, t0.enabled ? g.pso_tex : g.pso_tri);
        g.list->lpVtbl->SetGraphicsRootSignature(g.list, g.rootsig);
        g.list->lpVtbl->SetGraphicsRoot32BitConstants(g.list, 0, 16, cols, 0);
        g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 1, table);
    }
    g.list->lpVtbl->RSSetViewports(g.list, 1, &dvp);
    /* s31 FIX 2: the boot-time/flush-reopen scissor is display-canvas-sized;
     * a logical surface TALLER than the canvas (this capture: 1024x768 and
     * 1024x1024 passes on a 1280x720 display) would have its bottom rows
     * scissored away. Default scissor = the current target's full extent
     * (the guest's own scissor decodes to full-surface for every draw this
     * harness has measured; general per-draw guest scissor plumbing remains
     * the known separate gap, s27-4). */
    {
        D3D12_RECT dsc = {0, 0,
            (LONG)(target < g.n_surfaces && g.surfaces[target].w ? g.surfaces[target].w : g.width),
            (LONG)(target < g.n_surfaces && g.surfaces[target].h ? g.surfaces[target].h : g.height)};
        g.list->lpVtbl->RSSetScissorRects(g.list, 1, &dsc);
    }

    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation = g.vb->lpVtbl->GetGPUVirtualAddress(g.vb) + g.vb_used;
    vbv.StrideInBytes = VERT_STRIDE;
    vbv.SizeInBytes = n_tri * VERT_STRIDE;
    g.list->lpVtbl->IASetVertexBuffers(g.list, 0, 1, &vbv);
    g.list->lpVtbl->IASetPrimitiveTopology(g.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->lpVtbl->DrawInstanced(g.list, n_tri, 1, 0, 0);

    for (u32 k = 0; k < n_surf_used; k++) {
        bar.Transition.pResource = g.surfaces[surf_used[k]].tex;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &bar);
    }

    g.vb_used += n_tri * VERT_STRIDE;
    if (target < g.n_surfaces)
        g.surfaces[target].draw_hits++;
    /* RSX_DEPTH_DUMP_POST: raw depth readback right after this draw's
     * DrawInstanced was recorded -- c->draw_count is still THIS draw's
     * index here (incremented right below). */
    {
        const char* ddp = ddump_lookup(g_ddump_post, g_ddump_post_n, c->draw_count);
        if (ddp) dump_depth_raw(ddp);
    }
    c->draw_count++;
    c->drawn_verts += n_tri;
    if (tri != c->verts)
        free(tri);
}

static void sink_flip(void* user, const rsx_dispatch* rsx, u32 arg)
{
    sink_ctx* c = user;
    const rxs_stream* s = c->stream;
    const u32 buf = arg & 7;
    const u32 scan_offset = buf < s->disp_count ? s->disp[buf].offset : 0;

    /* s27 part 3: flush whatever zeta pass was still active at frame end --
     * without this, a shadow/depth pass that happens to be the LAST one
     * before the flip (no later draw ever changes the zeta target again)
     * would never hit depth_pass_track()'s boundary-detection and its
     * snapshot would be silently skipped. */
    if (honor_depth_rt() && g.cur_zeta_valid && g.cur_zeta_had_write) {
        depth_snapshot_flush(g.cur_zeta_loc, g.cur_zeta_off);
        g.cur_zeta_had_write = 0;
    }
    (void)rsx;

    printf("[replay] flip(buffer %u @0x%X) -> frame %u"
           " (%u clears, %u draws [%u translated], %u verts, %u surfaces, %u flushes)\n",
           buf, scan_offset, c->frame_no,
           c->clear_count, c->draw_count, c->draws_xlat, c->drawn_verts,
           g.n_surfaces, g_flush_count);
    printf("[replay]   textures decoded: %u / %u cap\n", g.n_textures, MAX_TEXTURES);
    for (u32 i = 0; i < g.n_surfaces; i++)
        printf("[replay]   surf 0x%07X loc=%u draws=%u%s\n",
               g.surfaces[i].offset, g.surfaces[i].location,
               g.surfaces[i].draw_hits,
               g.surfaces[i].offset == scan_offset ? "  <- SCANOUT" : "");

    char path[MAX_PATH];
    const u32 si = surface_get(RSX_LOCATION_LOCAL, scan_offset, g.width, g.height);
    snprintf(path, sizeof(path), "%s\\frame_%03u.ppm", c->outdir, c->frame_no);
    gpu_readback_surface(si, path);

    if (c->dump_surfaces) {
        for (u32 i = 0; i < g.n_surfaces; i++) {
            if (i == si)
                continue;
            snprintf(path, sizeof(path), "%s\\frame_%03u_s%07X.ppm",
                     c->outdir, c->frame_no, g.surfaces[i].offset);
            gpu_readback_surface(i, path);
        }
    }
    c->frame_no++;
    g.depth_cleared = 0;    /* B1: re-clear depth for the next frame */
    for (u32 i = 0; i < g.n_zdepths; i++)   /* s31: same latch, per target */
        g.zdepths[i].cleared = 0;
}

/* ---------------------------------------------------------------------------
 * Coverage report
 * -----------------------------------------------------------------------*/

static void coverage_report(const rsx_dispatch* rsx, const char* names_path)
{
    /* load names sidecar: "MMMMM count NAME" per line */
    char* names[RSX_DSP_NUM_REGS] = {0};
    static char namebuf[512 * 1024];
    size_t used = 0;
    FILE* f = fopen(names_path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            unsigned m, cnt;
            char nm[128];
            if (sscanf(line, "%x %u %127s", &m, &cnt, nm) == 3 && (m >> 2) < RSX_DSP_NUM_REGS) {
                size_t len = strlen(nm) + 1;
                if (used + len < sizeof(namebuf)) {
                    memcpy(namebuf + used, nm, len);
                    names[m >> 2] = namebuf + used;
                    used += len;
                }
            }
        }
        fclose(f);
    }

    u32 n_seen = 0, n_exec = 0, n_state = 0, n_todo = 0;
    u64 w_total = 0, w_handled = 0;
    printf("\n== method coverage ==\n");
    printf("   count  method  class  name\n");
    for (u32 i = 0; i < RSX_DSP_NUM_REGS; i++) {
        if (!rsx->seen[i])
            continue;
        n_seen++;
        w_total += rsx->seen[i];
        const char* cls = "TODO ";
        if (rsx->klass[i] == RSX_DSP_CLASS_EXEC) { cls = "EXEC "; n_exec++; w_handled += rsx->seen[i]; }
        else if (rsx->klass[i] == RSX_DSP_CLASS_STATE) { cls = "STATE"; n_state++; w_handled += rsx->seen[i]; }
        else n_todo++;
        printf("%8u  0x%05X %s  %s\n", rsx->seen[i], i << 2, cls,
               names[i] ? names[i] : "?");
    }
    printf("== %u distinct methods seen: %u EXEC + %u STATE handled, %u TODO"
           " (%llu/%llu writes consumed)\n",
           n_seen, n_exec, n_state, n_todo,
           (unsigned long long)w_handled, (unsigned long long)w_total);
}

/* ---------------------------------------------------------------------------
 * main
 * -----------------------------------------------------------------------*/

int main(int argc, char** argv)
{
    const char* path = NULL;
    const char* outdir = ".";
    int use_hw = 0;
    int dump_surfaces = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hw")) use_hw = 1;
        else if (!strcmp(argv[i], "--surfaces")) dump_surfaces = 1;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "--dump-shaders") && i + 1 < argc) g_dump_dir = argv[++i];
        else path = argv[i];
    }
    if (!path) {
        printf("usage: %s <stream.rxs> [--hw] [--surfaces] [--out dir]"
               " [--dump-shaders dir]\n", argv[0]);
        return 2;
    }
    b1_read_env();
    printf("[b1] state: blend=%d depth=%d cull=%d samp=%d\n",
           g_b1_blend, g_b1_depth, g_b1_cull, g_b1_samp);

    {
        const char* e = getenv("RSX_FP_FORCE_STAGE");
        if (e && e[0]) {
            g_fp_force_stage = atoi(e);
            printf("[fpforce] RSX_FP_FORCE_STAGE ARMED: stage=%d target_key=%016llx"
                   " (only fp_%016llx's PSO is affected)\n",
                   g_fp_force_stage, (unsigned long long)FP_FORCE_TARGET_KEY,
                   (unsigned long long)FP_FORCE_TARGET_KEY);
        }
    }
    {
        const char* e = getenv("RSX_FP_FORCE_STAGE2");
        if (e && e[0]) {
            g_fp_force_stage2 = atoi(e);
            printf("[fpforce2] RSX_FP_FORCE_STAGE2 ARMED: stage=%d target_key=%016llx"
                   " (only fp_%016llx's PSO is affected)\n",
                   g_fp_force_stage2, (unsigned long long)FP_FORCE_TARGET_KEY2,
                   (unsigned long long)FP_FORCE_TARGET_KEY2);
        }
    }
    {
        const char* e = getenv("RSX_FP_FORCE_STAGE3");
        if (e && e[0]) {
            g_fp_force_stage3 = atoi(e);
            printf("[fpforce3] RSX_FP_FORCE_STAGE3 ARMED: stage=%d target_key=%016llx"
                   " (only fp_%016llx's PSO is affected)\n",
                   g_fp_force_stage3, (unsigned long long)FP_FORCE_TARGET_KEY3,
                   (unsigned long long)FP_FORCE_TARGET_KEY3);
        }
    }
    {
        const char* e = getenv("RSX_FP_FORCE_STAGE4");
        if (e && e[0]) {
            g_fp_force_stage4 = atoi(e);
            printf("[fpforce4] RSX_FP_FORCE_STAGE4 ARMED: stage=%d target_key=%016llx"
                   " (only fp_%016llx's PSO is affected)\n",
                   g_fp_force_stage4, (unsigned long long)FP_FORCE_TARGET_KEY4,
                   (unsigned long long)FP_FORCE_TARGET_KEY4);
        }
    }
    {
        const char* e = getenv("RSX_FP_FORCE_STAGE5");
        if (e && e[0]) {
            g_fp_force_stage5 = atoi(e);
            printf("[fpforce5] RSX_FP_FORCE_STAGE5 ARMED: force-white target_key=%016llx"
                   " (only fp_%016llx's PSO / draw 803's PSO is affected)\n",
                   (unsigned long long)FP_FORCE_TARGET_KEY5,
                   (unsigned long long)FP_FORCE_TARGET_KEY5);
        }
    }
    {
        const char* e = getenv("RSX_DEPTH_DUMP_PRE");
        if (e && e[0]) {
            parse_ddump_list(e, g_ddump_pre, &g_ddump_pre_n, 8);
            printf("[ddump] RSX_DEPTH_DUMP_PRE ARMED: %d target(s)", g_ddump_pre_n);
            for (int i = 0; i < g_ddump_pre_n; i++)
                printf(" [%d]=%s", g_ddump_pre[i].idx, g_ddump_pre[i].path);
            printf("\n");
        }
    }
    {
        const char* e = getenv("RSX_DEPTH_DUMP_POST");
        if (e && e[0]) {
            parse_ddump_list(e, g_ddump_post, &g_ddump_post_n, 8);
            printf("[ddump] RSX_DEPTH_DUMP_POST ARMED: %d target(s)", g_ddump_post_n);
            for (int i = 0; i < g_ddump_post_n; i++)
                printf(" [%d]=%s", g_ddump_post[i].idx, g_ddump_post[i].path);
            printf("\n");
        }
    }
    {
        const char* e = getenv("RSX_FP_FORCE_STAGE6");
        if (e && e[0]) {
            g_fp_force_stage6 = atoi(e);
            printf("[fpforce6] RSX_FP_FORCE_STAGE6 ARMED: stage=%d target_key=%016llx"
                   " (only fp_%016llx's PSO / draw 1589's PSO is affected)\n",
                   g_fp_force_stage6, (unsigned long long)FP_FORCE_TARGET_KEY6,
                   (unsigned long long)FP_FORCE_TARGET_KEY6);
        }
    }

    rxs_stream s = {0};
    if (rxs_load(path, &s))
        return 1;
    printf("[replay] %u records, %u blocks, display %ux%u\n",
           s.n_records, s.n_blocks, s.disp_w, s.disp_h);

    g_arena[0] = VirtualAlloc(NULL, ARENA_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_arena[1] = VirtualAlloc(NULL, ARENA_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_arena[0] || !g_arena[1]) {
        printf("arena alloc failed\n");
        return 1;
    }

    const u32 width = s.disp_w ? s.disp_w : 1280;
    const u32 height = s.disp_h ? s.disp_h : 720;
    if (gpu_init(width, height, use_hw))
        return 1;

    static rsx_dispatch rsx;
    sink_ctx ctx = {0};
    ctx.outdir = outdir;
    ctx.dump_surfaces = dump_surfaces;
    ctx.stream = &s;
    rsx_dispatch_sink sink = {0};
    sink.user = &ctx;
    sink.clear = sink_clear;
    sink.begin = sink_begin;
    sink.end = sink_end;
    sink.draw_arrays = sink_draw_arrays;
    sink.draw_index_array = sink_draw_index_array;
    sink.flip = sink_flip;
    rsx_dispatch_init(&rsx, &sink);
    rsx_dispatch_seed_registers(&rsx, s.regs, s.reg_words);
    rsx_dispatch_seed_transform_program(&rsx, s.vp, s.vp_words);

    for (u32 i = 0; i < s.n_records; i++) {
        const u32 a = s.records[i * 2];
        const u32 b = s.records[i * 2 + 1];
        if (a & 0x80000000u)
            apply_block(&s, b);
        else
            rsx_dispatch_method(&rsx, a, b);
    }

    if (ctx.frame_no == 0) {
        printf("[replay] no flip in stream; dumping the current color target\n");
        char fpath[MAX_PATH];
        snprintf(fpath, sizeof(fpath), "%s\\frame_000.ppm", outdir);
        gpu_readback_surface(current_surface(&rsx), fpath);
    }

    for (u32 p = 0; p < 16; p++)
        if (ctx.skipped_prims[p])
            printf("[replay] skipped %u draws of primitive %u\n", ctx.skipped_prims[p], p);

    printf("[xlat] %u shader pairs translated, %u failed -> fallback\n",
           g_xlat_ok, g_xlat_fail);

    char names_path[MAX_PATH];
    snprintf(names_path, sizeof(names_path), "%s.names.txt", path);
    coverage_report(&rsx, names_path);
    return 0;
}
