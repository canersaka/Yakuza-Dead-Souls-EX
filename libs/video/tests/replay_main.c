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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define MAX_VERTS   (256 * 1024)
#define VERT_STRIDE 28          /* float3 pos + float4 color */

/* The capture renders through many offscreen surfaces (render-to-texture
 * passes) before one final composite draw into the scanout buffer. Model
 * each distinct (location, color0 offset) as its own render target; the
 * flip picks the display buffer's surface for readback. */
#define MAX_SURFACES 64

typedef struct {
    u32             location, offset;
    ID3D12Resource* tex;
} surface_t;

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

    ID3D12RootSignature*       rootsig;
    ID3D12PipelineState*       pso_tri;
    ID3D12PipelineState*       pso_line;

    ID3D12Resource*            vb;
    u8*                        vb_mapped;
    u32                        vb_used;    /* bytes */
} gpu_t;

static gpu_t g;

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

    /* root signature: 16 root constants = 4x4 transform at b0 */
    D3D12_ROOT_PARAMETER rp = {0};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 16;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rsd = {0};
    rsd.NumParameters = 1;
    rsd.pParameters = &rp;
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

    /* pass-through vertex-color shaders; transform columns arrive at b0 */
    static const char vs_src[] =
        "cbuffer cb : register(b0) { float4 c0, c1, c2, c3; };\n"
        "struct I { float3 p : POSITION; float4 c : COLOR; };\n"
        "struct O { float4 p : SV_POSITION; float4 c : COLOR; };\n"
        "O main(I i) {\n"
        "  O o;\n"
        "  float4 p = float4(i.p, 1.0);\n"
        "  o.p = c0 * p.x + c1 * p.y + c2 * p.z + c3 * p.w;\n"
        "  o.c = i.c;\n"
        "  return o;\n"
        "}\n";
    static const char ps_src[] =
        "struct I { float4 p : SV_POSITION; float4 c : COLOR; };\n"
        "float4 main(I i) : SV_TARGET { return i.c; }\n";
    ID3DBlob *vs = NULL, *ps = NULL;
    if (FAILED(D3DCompile(vs_src, sizeof(vs_src) - 1, "vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vs, &err))) {
        printf("vs: %s\n", err ? (char*)err->lpVtbl->GetBufferPointer(err) : "?");
        return -1;
    }
    if (FAILED(D3DCompile(ps_src, sizeof(ps_src) - 1, "ps", NULL, NULL, "main", "ps_5_0", 0, 0, &ps, &err))) {
        printf("ps: %s\n", err ? (char*)err->lpVtbl->GetBufferPointer(err) : "?");
        return -1;
    }

    D3D12_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = g.rootsig;
    pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
    pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
    pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    pd.InputLayout.pInputElementDescs = il;
    pd.InputLayout.NumElements = 2;
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
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd, &IID_ID3D12PipelineState,
                                               (void**)&g.pso_line);
    vs->lpVtbl->Release(vs);
    ps->lpVtbl->Release(ps);

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
}

/* ---------------------------------------------------------------------------
 * Dispatcher sink: clears + draw accumulation
 * -----------------------------------------------------------------------*/

typedef struct { float x, y, z, r, g2, b, a; } vtx_t;

/* RSX primitive ids (gcm public constants) */
#define PRIM_TRIANGLES      5
#define PRIM_TRIANGLE_STRIP 6
#define PRIM_TRIANGLE_FAN   7
#define PRIM_QUADS          8

typedef struct {
    const char* outdir;
    int   dump_surfaces;
    u32   frame_no;
    u32   clear_count, draw_count, drawn_verts;
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
        default:
            return 0; /* cmp32 etc: unhandled */
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
}

static void push_vert(sink_ctx* c, const float pos[4], const float col[4])
{
    if (c->n_verts >= c->cap_verts) {
        c->cap_verts = c->cap_verts ? c->cap_verts * 2 : 4096;
        c->verts = realloc(c->verts, c->cap_verts * sizeof(vtx_t));
    }
    vtx_t* v = &c->verts[c->n_verts++];
    v->x = pos[0];
    v->y = pos[1];
    v->z = pos[2];
    v->r = col[0];
    v->g2 = col[1];
    v->b = col[2];
    v->a = col[3];
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
    float pos[4], col[4];
    if (!fetch_attr(rsx, 0, base, vert, pos)) {
        c->fetch_ok = 0;
        return;
    }
    if (!fetch_attr(rsx, 3, base, vert, col)) {
        col[0] = col[1] = col[2] = col[3] = 1.0f; /* no diffuse: white */
    }
    push_vert(c, pos, col);
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
            fetch_one(c, rsx, base, base_index + index);
        }
    }
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

    /* CPU-expand to a triangle list */
    vtx_t* tri = NULL;
    u32 n_tri = 0;
    switch (prim) {
    case PRIM_TRIANGLES:
        tri = c->verts;
        n_tri = c->n_verts - c->n_verts % 3;
        break;
    case PRIM_TRIANGLE_STRIP:
        if (c->n_verts < 3) return;
        n_tri = (c->n_verts - 2) * 3;
        tri = malloc(n_tri * sizeof(vtx_t));
        for (u32 i = 0; i + 2 < c->n_verts; i++) {
            /* strip winding alternates; cull is off, keep it simple */
            tri[i * 3 + 0] = c->verts[i];
            tri[i * 3 + 1] = c->verts[i + 1];
            tri[i * 3 + 2] = c->verts[i + 2];
        }
        break;
    case PRIM_TRIANGLE_FAN:
        if (c->n_verts < 3) return;
        n_tri = (c->n_verts - 2) * 3;
        tri = malloc(n_tri * sizeof(vtx_t));
        for (u32 i = 1; i + 1 < c->n_verts; i++) {
            tri[(i - 1) * 3 + 0] = c->verts[0];
            tri[(i - 1) * 3 + 1] = c->verts[i];
            tri[(i - 1) * 3 + 2] = c->verts[i + 1];
        }
        break;
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

    if (g.vb_used + n_tri * VERT_STRIDE > MAX_VERTS * VERT_STRIDE) {
        printf("[replay] vertex buffer full, dropping draw (%u verts)\n", n_tri);
        if (tri != c->verts) free(tri);
        return;
    }

    memcpy(g.vb_mapped + g.vb_used, tri, (size_t)n_tri * VERT_STRIDE);

    /* Transform selection: if the game uploaded transform constants c0..c3
     * this frame we use them as the 4 columns (matches how RSX vertex
     * programs consume the MVP); otherwise map window coords -> NDC using
     * the RSX viewport scale/translate inverse. */
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

    if (c->draw_count < 40) {
        const vtx_t* v0 = &tri[0];
        const float cx = cols[0] * v0->x + cols[4] * v0->y + cols[8] * v0->z + cols[12];
        const float cy = cols[1] * v0->x + cols[5] * v0->y + cols[9] * v0->z + cols[13];
        const float cz = cols[2] * v0->x + cols[6] * v0->y + cols[10] * v0->z + cols[14];
        const float cw = cols[3] * v0->x + cols[7] * v0->y + cols[11] * v0->z + cols[15];
        rsx_dsp_surface sf;
        rsx_dsp_get_surface(rsx, &sf);
        printf("[draw %2u] prim=%u verts=%u surf=0x%X mvp=%d v0=(%.2f %.2f %.2f)"
               " clip=(%.2f %.2f %.2f %.2f)\n",
               c->draw_count, prim, n_tri, sf.color_offset[0], have_mvp,
               v0->x, v0->y, v0->z, cx, cy, cz, cw);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = surface_rtv(current_surface(rsx));
    g.list->lpVtbl->OMSetRenderTargets(g.list, 1, &rtv, FALSE, NULL);
    g.list->lpVtbl->SetPipelineState(g.list, g.pso_tri);
    g.list->lpVtbl->SetGraphicsRootSignature(g.list, g.rootsig);
    g.list->lpVtbl->SetGraphicsRoot32BitConstants(g.list, 0, 16, cols, 0);
    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation = g.vb->lpVtbl->GetGPUVirtualAddress(g.vb) + g.vb_used;
    vbv.StrideInBytes = VERT_STRIDE;
    vbv.SizeInBytes = n_tri * VERT_STRIDE;
    g.list->lpVtbl->IASetVertexBuffers(g.list, 0, 1, &vbv);
    g.list->lpVtbl->IASetPrimitiveTopology(g.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->lpVtbl->DrawInstanced(g.list, n_tri, 1, 0, 0);

    g.vb_used += n_tri * VERT_STRIDE;
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
           " (%u clears, %u draws, %u verts, %u surfaces)\n",
           buf, scan_offset, c->frame_no,
           c->clear_count, c->draw_count, c->drawn_verts, g.n_surfaces);

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
        else path = argv[i];
    }
    if (!path) {
        printf("usage: %s <stream.rxs> [--hw] [--surfaces] [--out dir]\n", argv[0]);
        return 2;
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

    char names_path[MAX_PATH];
    snprintf(names_path, sizeof(names_path), "%s.names.txt", path);
    coverage_report(&rsx, names_path);
    return 0;
}
