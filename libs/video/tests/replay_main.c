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
#define SRV_HEAP_SLOTS   (SRV_TEXTURE_BASE + MAX_TEXTURES)

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
    u32             draw_hits;   /* draws that targeted this surface this run */
} surface_t;

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
    ID3D12Resource*            depth;
    int                        depth_cleared;   /* per-frame clear latch      */

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
static D3D12_SAMPLER_DESC decode_sampler(const rsx_dsp_texture* t);
static D3D12_CPU_DESCRIPTOR_HANDLE smp_cpu(u32 slot);

static int gpu_init(u32 width, u32 height, int use_hw)
{
    HRESULT hr;
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

    /* readback buffer */
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (u64)g.rb_pitch * height;
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
        dhd.NumDescriptors = 1;
        if (FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &dhd, &IID_ID3D12DescriptorHeap, (void**)&g.dsv_heap)))
            return -1;
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

/* Find or create the render target for a (location, color offset) pair. */
static u32 surface_get(u32 location, u32 offset)
{
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].location == location && g.surfaces[i].offset == offset)
            return i;
    if (g.n_surfaces >= MAX_SURFACES) {
        printf("[gpu] surface cache full; reusing surface 0\n");
        return 0;
    }

    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = g.width;
    rd.Height = g.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = rd.Format;
    surface_t* s = &g.surfaces[g.n_surfaces];
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
    g.dev->lpVtbl->CreateRenderTargetView(g.dev, s->tex, NULL, surface_rtv(g.n_surfaces));
    srv_write(SRV_SURFACE_BASE + g.n_surfaces, s->tex);
    return g.n_surfaces++;
}

/* Surface currently selected as color target 0 by the register state. */
static u32 current_surface(const rsx_dispatch* rsx)
{
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(rsx, &sf);
    return surface_get(sf.color_location[0], sf.color_offset[0]);
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
    dst.PlacedFootprint.Footprint.Width = g.width;
    dst.PlacedFootprint.Footprint.Height = g.height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = g.rb_pitch;
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    g.list->lpVtbl->Close(g.list);
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    gpu_wait();

    u8* px = NULL;
    D3D12_RANGE rr = {0, (size_t)g.rb_pitch * g.height};
    g.readback->lpVtbl->Map(g.readback, 0, &rr, (void**)&px);
    write_ppm(path, px, g.rb_pitch, g.width, g.height);
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
    /* RSX/nv40 default (register never written) is all channels enabled;
     * the capture confirms this (no COLOR_MASK write before the earliest
     * draws still show correct channel output). */
    const u32 cm = rsx_dsp_reg(rsx, M_COLOR_MASK);
    rs->color_mask = cm ? cm : 0xFFFFFFFFu;
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
    if (!(mask & (RSX_CLEAR_COLOR_R | RSX_CLEAR_COLOR_G | RSX_CLEAR_COLOR_B | RSX_CLEAR_COLOR_A)))
        return; /* depth/stencil-only clear; no depth buffer yet */

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
        if (sampled_surface >= 0) {
            slots[u] = SRV_SURFACE_BASE + sampled_surface;
            u32 seen = 0;
            for (u32 k = 0; k < n_surf_used; k++)
                if (surf_used[k] == (u32)sampled_surface)
                    seen = 1;
            if (!seen && n_surf_used < SRV_TABLE_SIZE)
                surf_used[n_surf_used++] = (u32)sampled_surface;
        } else {
            slots[u] = texture_srv_slot(&t);
        }
        const int dbg_target =
            (target < g.n_surfaces && g.surfaces[target].offset == 0x3C0000);
        if ((c->draw_count < 40 || dbg_target || dbg_force_log) && t.enabled)
            printf("          tex%u <- %s 0x%X (%ux%u fmt=0x%02X)%s\n", u,
                   sampled_surface >= 0 ? "surface" : "guest", t.offset,
                   t.width, t.height, t.format & 0xFF,
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
               " cull(en=%u face=0x%X front=0x%X) blend=%u depth(t=%u w=%u f=0x%X)\n",
               c->draw_count, prim, n_tri, sf.color_offset[0],
               xpso ? "XLAT" : (have_mvp ? "fb-mvp" : "fb-vp"),
               p0[0], p0[1], p0[2],
               drs.cull_enable, drs.cull_face, drs.front_face,
               drs.blend_enable, drs.depth_test, drs.depth_write, drs.depth_func);
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
        g.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.dsv_heap, &dsv);
        have_dsv = 1;
        if (!g.depth_cleared) {
            g.list->lpVtbl->ClearDepthStencilView(
                g.list, dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                1.0f, 0, 0, NULL);
            g.depth_cleared = 1;
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
    c->draw_count++;
    c->drawn_verts += n_tri;
    if (tri != c->verts)
        free(tri);
}

static void sink_flip(void* user, const rsx_dispatch* rsx, u32 arg)
{
    (void)rsx;
    sink_ctx* c = user;
    const rxs_stream* s = c->stream;
    const u32 buf = arg & 7;
    const u32 scan_offset = buf < s->disp_count ? s->disp[buf].offset : 0;

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
    const u32 si = surface_get(RSX_LOCATION_LOCAL, scan_offset);
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
