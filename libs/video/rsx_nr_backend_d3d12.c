/*
 * ps3recomp - native-render D3D12 execution sink. See
 * rsx_nr_backend_d3d12.h.
 *
 * Offline execution model: every GPU op records into a fresh command list
 * and is executed-and-waited immediately (retire -> mirror session ->
 * record -> execute -> signal -> wait). That keeps mirror-staging and
 * upload-ring hazard machinery exercised while making offline validation
 * deterministic; frame-batched submission is an integration-time
 * optimization of this same structure.
 */
#include "rsx_nr_backend_d3d12.h"

#include <string.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsx_gpu_mirror_d3d12.h"
#include "rsx_nr_resources.h"
#include "rsx_vertex_pull.h"

#define NRB_MAX_RTS      32
#define NRB_UPLOAD_BYTES (4u << 20)
#define NRB_VS_TEXT      (256 * 1024)
#define NRB_PSO_CAP      1024

typedef struct nrb_rt {
    ID3D12Resource* tex;
    ID3D12Resource* depth;
    u32 space, offset, w, h, fmt;
    u32 rtv_slot, dsv_slot;
    int live;
} nrb_rt;

struct rsx_nr_d3d12 {
    ID3D12Device* dev;
    ID3D12CommandQueue* queue;
    ID3D12CommandAllocator* alloc;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence* fence;
    HANDLE fence_event;
    u64 fence_value;
    int list_open;

    ID3D12DescriptorHeap* rtv_heap;
    ID3D12DescriptorHeap* dsv_heap;
    u32 rtv_size, dsv_size, rtv_used, dsv_used;

    ID3D12RootSignature* rootsig;

    ID3D12Resource* upload;          /* fence-gated per-exec bump ring     */
    u8* upload_mapped;
    u32 upload_used;

    ID3D12Resource* readback;
    u32 readback_size;

    rsx_guest_pages pages;
    rsx_gpu_mirror* mirror;
    rsx_gpu_mirror_d3d12* mirror_be;
    rsx_gpu_mirror_range range_local, range_main;
    u32 local_size, main_size;

    rsx_nr_pso_cache psos;
    nrb_rt rts[NRB_MAX_RTS];

    const u8* (*guest_ptr)(void* user, u32 space, u32 offset, u32 min_bytes);
    u8* (*writable_ptr)(void* user, u32 space, u32 offset, u32 min_bytes);
    void* guest_user;

    /* restart-draw index conversion scratch (offline model grows on
     * demand; a live integration preallocates its high-water size) */
    u32* idx_scratch;
    u32 idx_scratch_cap;

    rsx_nr_d3d12_stats stats;
    char vs_text[NRB_VS_TEXT];
    char pull_globals[48 * 1024];
    char pull_loads[8 * 1024];
};

/* ---- device plumbing --------------------------------------------------- */

static void nrb_wait_idle(rsx_nr_d3d12* b)
{
    const u64 v = ++b->fence_value;
    b->queue->lpVtbl->Signal(b->queue, b->fence, v);
    if (b->fence->lpVtbl->GetCompletedValue(b->fence) < v) {
        b->fence->lpVtbl->SetEventOnCompletion(b->fence, v, b->fence_event);
        WaitForSingleObject(b->fence_event, 10000);
    }
}

static int nrb_open_list(rsx_nr_d3d12* b)
{
    if (b->list_open)
        return 0;
    if (FAILED(b->alloc->lpVtbl->Reset(b->alloc)) ||
        FAILED(b->list->lpVtbl->Reset(b->list, b->alloc, NULL)))
        return -1;
    b->list_open = 1;
    b->upload_used = 0;              /* previous exec was waited on        */
    return 0;
}

static void nrb_exec_wait(rsx_nr_d3d12* b)
{
    if (!b->list_open)
        return;
    b->list->lpVtbl->Close(b->list);
    ID3D12CommandList* lists[1] = { (ID3D12CommandList*)b->list };
    b->queue->lpVtbl->ExecuteCommandLists(b->queue, 1, lists);
    nrb_wait_idle(b);
    b->list_open = 0;
}

/* fence-gated upload-ring slice (the exec-and-wait model retires the whole
 * ring before reuse; the guard still refuses oversubscription loudly) */
static u8* nrb_upload_alloc(rsx_nr_d3d12* b, u32 size, u64* gpu_va)
{
    const u32 aligned = (size + 255u) & ~255u;
    if (b->upload_used + aligned > NRB_UPLOAD_BYTES)
        return NULL;
    u8* p = b->upload_mapped + b->upload_used;
    *gpu_va = b->upload->lpVtbl->GetGPUVirtualAddress(b->upload) +
              b->upload_used;
    b->upload_used += aligned;
    return p;
}

static ID3D12Resource* nrb_make_buffer(ID3D12Device* dev, u64 size,
                                       D3D12_HEAP_TYPE heap,
                                       D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* res = NULL;
    if (FAILED(dev->lpVtbl->CreateCommittedResource(
            dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL,
            &IID_ID3D12Resource, (void**)&res)))
        return NULL;
    return res;
}

/* ---- render targets ---------------------------------------------------- */

static nrb_rt* nrb_get_rt(rsx_nr_d3d12* b, u32 space, u32 offset, u32 fmt,
                          u32 w, u32 h, int create)
{
    for (u32 i = 0; i < NRB_MAX_RTS; i++) {
        nrb_rt* rt = &b->rts[i];
        if (rt->live && rt->space == space && rt->offset == offset &&
            rt->fmt == fmt && rt->w == w && rt->h == h)
            return rt;
    }
    if (!create)
        return NULL;
    nrb_rt* rt = NULL;
    for (u32 i = 0; i < NRB_MAX_RTS; i++) {
        if (!b->rts[i].live) {
            rt = &b->rts[i];
            break;
        }
    }
    if (!rt || b->rtv_used >= 64 || b->dsv_used >= 64)
        return NULL;

    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(b->dev->lpVtbl->CreateCommittedResource(
            b->dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource,
            (void**)&rt->tex)))
        return NULL;

    D3D12_RESOURCE_DESC dd = rd;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE dcv = {0};
    dcv.Format = dd.Format;
    dcv.DepthStencil.Depth = 1.0f;
    if (FAILED(b->dev->lpVtbl->CreateCommittedResource(
            b->dev, &hp, D3D12_HEAP_FLAG_NONE, &dd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv, &IID_ID3D12Resource,
            (void**)&rt->depth))) {
        rt->tex->lpVtbl->Release(rt->tex);
        rt->tex = NULL;
        return NULL;
    }

    rt->space = space;
    rt->offset = offset;
    rt->fmt = fmt;
    rt->w = w;
    rt->h = h;
    rt->rtv_slot = b->rtv_used++;
    rt->dsv_slot = b->dsv_used++;
    rt->live = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    b->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(b->rtv_heap, &rtv);
    rtv.ptr += (SIZE_T)rt->rtv_slot * b->rtv_size;
    b->dev->lpVtbl->CreateRenderTargetView(b->dev, rt->tex, NULL, rtv);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    b->dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(b->dsv_heap, &dsv);
    dsv.ptr += (SIZE_T)rt->dsv_slot * b->dsv_size;
    b->dev->lpVtbl->CreateDepthStencilView(b->dev, rt->depth, NULL, dsv);

    b->stats.rt_builds++;
    return rt;
}

static void nrb_rt_handles(rsx_nr_d3d12* b, const nrb_rt* rt,
                           D3D12_CPU_DESCRIPTOR_HANDLE* rtv,
                           D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
{
    b->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(b->rtv_heap, rtv);
    rtv->ptr += (SIZE_T)rt->rtv_slot * b->rtv_size;
    b->dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(b->dsv_heap, dsv);
    dsv->ptr += (SIZE_T)rt->dsv_slot * b->dsv_size;
}

/* BGRA8-class gcm surface color formats the sink can host directly:
 * 4/5 = X8R8G8B8 (Z/O alpha-ignore variants), 8 = A8R8G8B8. */
static int nrb_color_format_ok(u32 fmt)
{
    return fmt == 4 || fmt == 5 || fmt == 8;
}

static nrb_rt* nrb_rt_from_state(rsx_nr_d3d12* b, const rsx_nir_pipeline* st,
                                 int create)
{
    const rsx_nir_surface* s = &st->surface;
    if (!nrb_color_format_ok(s->color_format))
        return NULL;
    u32 w = s->clip_w ? s->clip_w : 1280;
    u32 h = s->clip_h ? s->clip_h : 720;
    return nrb_get_rt(b, s->color_location[0], s->color_offset[0],
                      s->color_format, w, h, create);
}

/* ---- mirror session helper --------------------------------------------- */

static void nrb_mirror_sync(rsx_nr_d3d12* b)
{
    rsx_gpu_mirror_d3d12_retire(
        b->mirror_be, b->fence->lpVtbl->GetCompletedValue(b->fence));
    if (rsx_gpu_mirror_d3d12_begin(b->mirror_be, b->list) == 0) {
        rsx_gpu_mirror_sync(b->mirror, 0);
        rsx_gpu_mirror_d3d12_end(b->mirror_be, b->fence_value + 1);
    }
}

/* ---- exec ops ---------------------------------------------------------- */

static int nrb_clear(void* user, const rsx_nir_pipeline* st,
                     const rsx_nir_clear* c)
{
    rsx_nr_d3d12* b = user;
    const u32 color_bits = c->mask & 0xF0u;
    if (color_bits && color_bits != 0xF0u) {
        b->stats.unsupported_clears++;   /* partial-channel clear          */
        return -1;
    }
    nrb_rt* rt = nrb_rt_from_state(b, st, 1);
    if (!rt) {
        b->stats.unsupported_clears++;
        return -1;
    }
    if (nrb_open_list(b))
        return -1;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv;
    nrb_rt_handles(b, rt, &rtv, &dsv);
    /* CLEAR_SURFACE respects the scissor box (cellGcmSetClearSurface:
     * the cleared area is within the
     * scissor box). Clear only the folded scissor when it is a proper
     * sub-rect; zero/unset scissor means the full target. */
    D3D12_RECT sc_rect;
    const D3D12_RECT* rects = NULL;
    UINT nrects = 0;
    if (st->scissor.w && st->scissor.h &&
        (st->scissor.x || st->scissor.y || st->scissor.w < rt->w ||
         st->scissor.h < rt->h)) {
        sc_rect.left = (LONG)st->scissor.x;
        sc_rect.top = (LONG)st->scissor.y;
        sc_rect.right = (LONG)(st->scissor.x + st->scissor.w);
        sc_rect.bottom = (LONG)(st->scissor.y + st->scissor.h);
        if (sc_rect.right > (LONG)rt->w)
            sc_rect.right = (LONG)rt->w;
        if (sc_rect.bottom > (LONG)rt->h)
            sc_rect.bottom = (LONG)rt->h;
        rects = &sc_rect;
        nrects = 1;
    }
    if (color_bits) {
        float col[4];
        col[0] = (float)((c->color_value >> 16) & 0xFF) / 255.0f;
        col[1] = (float)((c->color_value >> 8) & 0xFF) / 255.0f;
        col[2] = (float)(c->color_value & 0xFF) / 255.0f;
        col[3] = (float)((c->color_value >> 24) & 0xFF) / 255.0f;
        b->list->lpVtbl->ClearRenderTargetView(b->list, rtv, col, nrects,
                                               rects);
    }
    D3D12_CLEAR_FLAGS df = 0;
    if (c->mask & 0x01u)
        df |= D3D12_CLEAR_FLAG_DEPTH;
    if (c->mask & 0x02u)
        df |= D3D12_CLEAR_FLAG_STENCIL;
    if (df)
        b->list->lpVtbl->ClearDepthStencilView(
            b->list, dsv, df, (float)c->depth_value / 16777215.0f,
            (UINT8)c->stencil_value, nrects, rects);
    nrb_exec_wait(b);
    b->stats.clears++;
    return 0;
}

static D3D12_COMPARISON_FUNC nrb_depth_func(u32 gl)
{
    switch (gl) {
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

static int nrb_topology(u32 prim, D3D12_PRIMITIVE_TOPOLOGY* topo,
                        D3D12_PRIMITIVE_TOPOLOGY_TYPE* type)
{
    switch (prim) {
    case 1: *topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; return 0;
    case 2: *topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; return 0;
    case 4: *topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; return 0;
    case 5: *topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; return 0;
    case 6: *topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; return 0;
    default: return -1;              /* loop/fan/quads: CPU path territory */
    }
}

static const char NRB_PS_SOLID[] =
    "float4 main() : SV_Target { return float4(1.0, 0.0, 1.0, 1.0); }\n";

static ID3DBlob* nrb_compile(rsx_nr_d3d12* b, const char* text, size_t len,
                             const char* target)
{
    ID3DBlob* blob = NULL;
    ID3DBlob* err = NULL;
    if (FAILED(D3DCompile(text, len, "nrb", NULL, NULL, "main", target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &blob, &err))) {
        b->stats.compile_failures++;
        if (err) {
            static int logged = 0;
            if (logged < 2) {
                logged++;
                fprintf(stderr, "[nr-d3d12] %s compile failed:\n%.*s\n",
                        target, (int)(err->lpVtbl->GetBufferSize(err) > 1024
                                          ? 1024
                                          : err->lpVtbl->GetBufferSize(err)),
                        (const char*)err->lpVtbl->GetBufferPointer(err));
            }
            err->lpVtbl->Release(err);
        }
        return NULL;
    }
    if (err)
        err->lpVtbl->Release(err);
    return blob;
}

static ID3D12PipelineState* nrb_get_pso(rsx_nr_d3d12* b,
                                        const rsx_nir_pipeline* st,
                                        const u32* vp_words, u32 vp_word_count,
                                        const rsx_vertex_pull_plan* plan,
                                        D3D12_PRIMITIVE_TOPOLOGY_TYPE tt,
                                        int strip_cut)
{
    /* PSO identity: VP content + pull signature + topology class + strip
     * cut + the depth/raster state words that shape the PSO */
    u64 key = rsx_nir_hash_words(vp_words, vp_word_count);
    key = rsx_nr_hash_fold(key ^ rsx_vertex_pull_signature(plan), &tt,
                           sizeof(tt));
    key = rsx_nr_hash_fold(key, &strip_cut, sizeof(strip_cut));
    key = rsx_nr_hash_fold(key, &st->depth_stencil,
                           sizeof(st->depth_stencil));
    u64 cached = 0;
    if (rsx_nr_pso_lookup(&b->psos, key, &cached)) {
        b->stats.pso_hits++;
        return (ID3D12PipelineState*)(uintptr_t)cached;
    }

    int gl = rsx_vertex_pull_emit_globals(plan, b->pull_globals,
                                          sizeof(b->pull_globals));
    int ld = rsx_vertex_pull_emit_loads(plan, "yz_sysvid", b->pull_loads,
                                        sizeof(b->pull_loads));
    if (gl < 0 || ld < 0)
        return NULL;

    int n;
    if (vp_word_count) {
        n = rsx_vertex_pull_decompile(plan, (const u8*)vp_words,
                                      vp_word_count * 4, 0, b->vs_text,
                                      sizeof(b->vs_text));
        if (n < 0)
            return NULL;
    } else {
        /* clip-space passthrough of ATTR0 (offline pixel tests) */
        n = snprintf(b->vs_text, sizeof(b->vs_text),
                     "%s\n"
                     "void main(uint yz_sysvid : SV_VertexID,\n"
                     "          out float4 yz_pos : SV_Position) {\n"
                     "    float4 v[16];\n"
                     "    [unroll] for (uint i = 0u; i < 16u; i++)\n"
                     "        v[i] = float4(0.0, 0.0, 0.0, 1.0);\n"
                     "%s"
                     "    yz_pos = v[0];\n"
                     "}\n",
                     b->pull_globals, b->pull_loads);
        if (n <= 0 || n >= (int)sizeof(b->vs_text))
            return NULL;
    }

    ID3DBlob* vs = nrb_compile(b, b->vs_text, strlen(b->vs_text), "vs_5_0");
    if (!vs)
        return NULL;
    ID3DBlob* ps = nrb_compile(b, NRB_PS_SOLID, sizeof(NRB_PS_SOLID) - 1,
                               "ps_5_0");
    if (!ps) {
        vs->lpVtbl->Release(vs);
        return NULL;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = b->rootsig;
    pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
    pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
    pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0x0F;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable =
        st->depth_stencil.depth_test_enable ? TRUE : FALSE;
    pd.DepthStencilState.DepthWriteMask =
        st->depth_stencil.depth_write_enable ? D3D12_DEPTH_WRITE_MASK_ALL
                                             : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc =
        nrb_depth_func(st->depth_stencil.depth_func);
    pd.InputLayout.NumElements = 0;  /* vertex pulling: no IA              */
    pd.IBStripCutValue = strip_cut
        ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
        : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pd.PrimitiveTopologyType = tt;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.SampleDesc.Count = 1;

    ID3D12PipelineState* pso = NULL;
    HRESULT hr = b->dev->lpVtbl->CreateGraphicsPipelineState(
        b->dev, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    vs->lpVtbl->Release(vs);
    ps->lpVtbl->Release(ps);
    if (FAILED(hr)) {
        b->stats.compile_failures++;
        return NULL;
    }
    rsx_nr_pso_insert(&b->psos, key, (u64)(uintptr_t)pso);
    b->stats.pso_builds++;
    return pso;
}

/* Read one batch of the guest index array [first, first+count) as u32
 * values, translating the restart sentinel: strips keep it as the D3D12
 * cut value 0xFFFFFFFF; list topologies drop it. Returns the converted
 * count into b->idx_scratch, or ~0u when the span is unreadable. */
static u32 nrb_read_indices(rsx_nr_d3d12* b, const rsx_nir_pipeline* st,
                            u32 first, u32 count, int strips)
{
    const u32 esize = st->index_binding.is_u32 ? 4u : 2u;
    const u8* src = b->guest_ptr(b->guest_user, st->index_binding.location,
                                 st->index_binding.offset + first * esize,
                                 count * esize);
    if (!src)
        return ~0u;
    if (count > b->idx_scratch_cap) {
        u32 ncap = b->idx_scratch_cap ? b->idx_scratch_cap : 4096;
        while (ncap < count)
            ncap *= 2;
        u32* nbuf = realloc(b->idx_scratch, (size_t)ncap * 4);
        if (!nbuf)
            return ~0u;
        b->idx_scratch = nbuf;
        b->idx_scratch_cap = ncap;
    }
    /* The restart comparison is always evaluated against the FULL 32-bit
     * restart register, with 16-bit indices zero-extended
     * (cellGcmSetRestartIndex). A register
     * value above 0xFFFF therefore never matches a 16-bit index — which
     * is exactly what the width gate in restart_enable already encodes. */
    const u32 restart = st->index_binding.restart_index;
    const int have_restart = st->index_binding.restart_enable;
    u32 n = 0;
    for (u32 i = 0; i < count; i++) {
        u32 v;
        if (esize == 4)
            v = ((u32)src[i * 4] << 24) | ((u32)src[i * 4 + 1] << 16) |
                ((u32)src[i * 4 + 2] << 8) | src[i * 4 + 3];
        else
            v = ((u32)src[i * 2] << 8) | src[i * 2 + 1];
        if (have_restart && v == restart) {
            if (strips)
                b->idx_scratch[n++] = 0xFFFFFFFFu;
            /* list topologies: the cut carries no geometry; drop it */
            continue;
        }
        b->idx_scratch[n++] = v;
    }
    return n;
}

static int nrb_draw(void* user, const rsx_nir_pipeline* st,
                    const u32* vp_words, u32 vp_word_count,
                    const rsx_nir_draw* d, const u32* batches)
{
    rsx_nr_d3d12* b = user;

    D3D12_PRIMITIVE_TOPOLOGY topo;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE tt;
    if (nrb_topology(d->primitive, &topo, &tt) != 0) {
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_topology++;
        return -1;
    }
    const int strips = d->primitive == 4 || d->primitive == 6;
    const int use_cut_ib = d->indexed && st->index_binding.restart_enable;
    nrb_rt* rt = nrb_rt_from_state(b, st, 1);
    if (!rt) {
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_rt++;
        return -1;
    }

    /* pull plan from folded state */
    rsx_vertex_layout_plan layout;
    u32 input_mask = st->vertex_program.attrib_input_mask;
    if (!input_mask)
        input_mask = 1;              /* passthrough mode pulls ATTR0       */
    rsx_vertex_layout_plan_init(&layout, input_mask);

    rsx_dsp_vertex_attr attrs[RSX_NIR_NUM_VERTEX_ATTR];
    float defaults[RSX_NIR_NUM_VERTEX_ATTR][4];
    for (u32 i = 0; i < RSX_NIR_NUM_VERTEX_ATTR; i++) {
        const rsx_nir_vertex_attr* a = &st->vertex_bindings.attr[i];
        attrs[i].type = a->type;
        attrs[i].size = a->size;
        attrs[i].stride = a->stride;
        attrs[i].frequency = a->frequency;
        attrs[i].offset = a->offset;
        attrs[i].location = a->location;
        memcpy(defaults[i], a->def, sizeof(defaults[i]));
    }
    rsx_vertex_pull_plan plan;
    if (!rsx_vertex_pull_plan_init_decoded(
            &plan, attrs, defaults, st->vertex_bindings.base_offset,
            st->vertex_bindings.freq_divider_op, &layout,
            RSX_PULL_TYPES_ALL)) {
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_plan++;
        return -1;
    }

    ID3D12PipelineState* pso = nrb_get_pso(b, st, vp_words, vp_word_count,
                                           &plan, tt, use_cut_ib && strips);
    if (!pso) {
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_pso++;
        return -1;
    }
    b->stats.approx_fp_draws++;      /* solid PS stands in for the FP      */

    if (nrb_open_list(b))
        return -1;
    nrb_mirror_sync(b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv;
    nrb_rt_handles(b, rt, &rtv, &dsv);
    b->list->lpVtbl->OMSetRenderTargets(b->list, 1, &rtv, FALSE, &dsv);

    D3D12_VIEWPORT vp = {0};
    vp.Width = (float)(st->viewport.w ? st->viewport.w : rt->w);
    vp.Height = (float)(st->viewport.h ? st->viewport.h : rt->h);
    vp.TopLeftX = (float)st->viewport.x;
    vp.TopLeftY = (float)st->viewport.y;
    vp.MaxDepth = 1.0f;
    b->list->lpVtbl->RSSetViewports(b->list, 1, &vp);
    D3D12_RECT sc;
    sc.left = (LONG)st->scissor.x;
    sc.top = (LONG)st->scissor.y;
    sc.right = (LONG)(st->scissor.w ? st->scissor.x + st->scissor.w : rt->w);
    sc.bottom = (LONG)(st->scissor.h ? st->scissor.y + st->scissor.h : rt->h);
    b->list->lpVtbl->RSSetScissorRects(b->list, 1, &sc);

    b->list->lpVtbl->SetGraphicsRootSignature(b->list, b->rootsig);
    b->list->lpVtbl->SetPipelineState(b->list, pso);
    b->list->lpVtbl->IASetPrimitiveTopology(b->list, topo);

    /* b0: transform constants */
    u64 const_va = 0;
    u8* cp = nrb_upload_alloc(b, sizeof(st->constants), &const_va);
    if (!cp) {
        nrb_exec_wait(b);
        b->stats.unsupported_draws++;
        return -1;
    }
    memcpy(cp, st->constants, sizeof(st->constants));
    b->list->lpVtbl->SetGraphicsRootConstantBufferView(b->list, 0, const_va);

    /* t20/t21: mirror buffers */
    ID3D12Resource* rl =
        (ID3D12Resource*)rsx_gpu_mirror_d3d12_buffer(b->mirror_be, 0);
    ID3D12Resource* rm =
        (ID3D12Resource*)rsx_gpu_mirror_d3d12_buffer(b->mirror_be, 1);
    if (rl)
        b->list->lpVtbl->SetGraphicsRootShaderResourceView(
            b->list, 2, rl->lpVtbl->GetGPUVirtualAddress(rl));
    if (rm)
        b->list->lpVtbl->SetGraphicsRootShaderResourceView(
            b->list, 3, rm->lpVtbl->GetGPUVirtualAddress(rm));

    /* Restart draws go through a host-built u32 index buffer with the
     * D3D12 strip-cut sentinel (strips) or cuts dropped (lists); the
     * shader then runs the ARRAYS source with first = 0, so SV_VertexID
     * IS the fetched index and base_index still applies in-shader —
     * exactly the pull module's documented host-index integration. All
     * other draws use the in-shader guest index fetch. */
    const u32 source = (d->indexed && !use_cut_ib)
                           ? (st->index_binding.is_u32
                                  ? RSX_PULL_SOURCE_INDEX_U32
                                  : RSX_PULL_SOURCE_INDEX_U16)
                           : RSX_PULL_SOURCE_ARRAYS;
    int failed = 0;

    for (u32 bi = 0; bi < d->batch_count; bi++) {
        const u32 first = batches[bi * 2];
        const u32 count = batches[bi * 2 + 1];
        u32 draw_count = count;
        u32 pc_first = first;

        if (use_cut_ib) {
            u32 n = nrb_read_indices(b, st, first, count, strips);
            if (n == ~0u) {
                b->stats.unsup_draw_index++;
                failed = 1;
                break;
            }
            if (!n) {
                b->stats.draw_batches++;
                continue;            /* batch was cuts only               */
            }
            u64 ib_va = 0;
            u8* ip = nrb_upload_alloc(b, n * 4, &ib_va);
            if (!ip) {
                failed = 1;
                break;
            }
            memcpy(ip, b->idx_scratch, (size_t)n * 4);
            D3D12_INDEX_BUFFER_VIEW ibv;
            ibv.BufferLocation = ib_va;
            ibv.SizeInBytes = n * 4;
            ibv.Format = DXGI_FORMAT_R32_UINT;
            b->list->lpVtbl->IASetIndexBuffer(b->list, &ibv);
            draw_count = n;
            pc_first = 0;
        }

        rsx_vertex_pull_constants pc;
        rsx_vertex_pull_fill_constants(
            &plan, st->vertex_bindings.base_index, pc_first, source,
            st->index_binding.offset, st->index_binding.location,
            rsx_gpu_mirror_d3d12_buffer_size(b->mirror_be, 0),
            rsx_gpu_mirror_d3d12_buffer_size(b->mirror_be, 1), &pc);
        u64 pull_va = 0;
        u8* pp = nrb_upload_alloc(b, sizeof(pc), &pull_va);
        if (!pp) {
            failed = 1;
            break;
        }
        memcpy(pp, &pc, sizeof(pc));
        b->list->lpVtbl->SetGraphicsRootConstantBufferView(b->list, 1,
                                                           pull_va);
        if (use_cut_ib)
            b->list->lpVtbl->DrawIndexedInstanced(b->list, draw_count, 1, 0,
                                                  0, 0);
        else
            b->list->lpVtbl->DrawInstanced(b->list, draw_count, 1, 0, 0);
        b->stats.draw_batches++;
    }

    nrb_exec_wait(b);
    if (failed) {
        b->stats.unsupported_draws++;
        return -1;
    }
    if (use_cut_ib)
        b->stats.restart_draws++;
    b->stats.draws++;
    return 0;
}

static int nrb_transfer(void* user, const rsx_nir_pipeline* st,
                        const rsx_nir_transfer* t, const u32* words)
{
    rsx_nr_d3d12* b = user;
    (void)st;
    if (!b->writable_ptr) {
        b->stats.unsupported_transfers++;
        return -1;
    }
    switch (t->kind) {
    case RSX_NIR_XFER_BUFFER: {
        /* NV0039 format bytes are byte-address increments {1,2,4}, not
         * pixel formats (cellGcmSetTransferDataFormat): values > 1
         * subsample bytes.
         * Only the tightly-packed case (0/unset or 1) is a straight
         * copy; refuse subsampling loudly. */
        if (t->src_format > 1 || t->dst_format > 1) {
            b->stats.unsupported_transfers++;
            return -1;
        }
        const u32 total_in = t->src_pitch * (t->line_count ? t->line_count - 1
                                                           : 0) +
                             t->line_length;
        const u8* src =
            b->guest_ptr(b->guest_user, t->src_location, t->src_offset,
                         total_in);
        u8* dst = b->writable_ptr(b->guest_user, t->dst_location,
                                  t->dst_offset,
                                  t->dst_pitch * (t->line_count
                                                      ? t->line_count - 1
                                                      : 0) +
                                      t->line_length);
        if (!src || !dst || !t->line_length) {
            b->stats.unsupported_transfers++;
            return -1;
        }
        for (u32 l = 0; l < t->line_count; l++)
            memmove(dst + (size_t)l * t->dst_pitch,
                    src + (size_t)l * t->src_pitch, t->line_length);
        rsx_guest_pages_note_write(
            &b->pages, t->dst_location, t->dst_offset,
            t->dst_pitch * (t->line_count ? t->line_count - 1 : 0) +
                t->line_length);
        break;
    }
    case RSX_NIR_XFER_INLINE: {
        /* raw word copy to dst + x*4 + y*pitch, the consumer's own
         * semantics (import_overrides.cpp NV308A window) */
        u8* dst = b->writable_ptr(b->guest_user, t->dst_location,
                                  t->dst_offset,
                                  t->point_y * t->dst_pitch +
                                      (t->point_x + t->word_count) * 4);
        if (!dst || !words) {
            b->stats.unsupported_transfers++;
            return -1;
        }
        u8* row = dst + (size_t)t->point_y * t->dst_pitch;
        for (u32 i = 0; i < t->word_count; i++)
            memcpy(row + (size_t)(t->point_x + i) * 4, &words[i], 4);
        rsx_guest_pages_note_write(&b->pages, t->dst_location,
                                   t->dst_offset +
                                       t->point_y * t->dst_pitch +
                                       t->point_x * 4,
                                   t->word_count * 4);
        break;
    }
    case RSX_NIR_XFER_SCALED: {
        /* 1:1 raw copy only (ds_dx/dt_dy == 1.0 in 16.16); real scaling
         * and format conversion stay on the fallback path for now */
        if (t->ds_dx != 0x00100000u || t->dt_dy != 0x00100000u ||
            t->src_format != t->dst_format) {
            b->stats.unsupported_transfers++;
            return -1;
        }
        const u32 bpp = 4;
        const u8* src = b->guest_ptr(b->guest_user, t->src_location,
                                     t->src_offset,
                                     t->in_h * t->src_pitch);
        u8* dst = b->writable_ptr(b->guest_user, t->dst_location,
                                  t->dst_offset,
                                  (t->out_y + t->out_h) * t->dst_pitch);
        if (!src || !dst || !t->out_w || !t->out_h) {
            b->stats.unsupported_transfers++;
            return -1;
        }
        for (u32 y = 0; y < t->out_h && y < t->in_h; y++)
            memmove(dst + (size_t)(t->out_y + y) * t->dst_pitch +
                        (size_t)t->out_x * bpp,
                    src + (size_t)y * t->src_pitch,
                    (size_t)(t->out_w < t->in_w ? t->out_w : t->in_w) * bpp);
        rsx_guest_pages_note_write(&b->pages, t->dst_location, t->dst_offset,
                                   (t->out_y + t->out_h) * t->dst_pitch);
        break;
    }
    default:
        b->stats.unsupported_transfers++;
        return -1;
    }
    b->stats.transfers++;
    return 0;
}

static int nrb_present(void* user, u32 buffer)
{
    rsx_nr_d3d12* b = user;
    (void)buffer;
    nrb_exec_wait(b);                /* offscreen: complete the frame      */
    b->stats.presents++;
    return 0;
}

static void nrb_flush(void* user)
{
    nrb_exec_wait((rsx_nr_d3d12*)user);
}

void rsx_nr_d3d12_get_exec_ops(rsx_nr_d3d12* b, rsx_nr_exec_ops* out)
{
    out->user = b;
    out->clear = nrb_clear;
    out->draw = nrb_draw;
    out->transfer = nrb_transfer;
    out->present = nrb_present;
    out->flush = nrb_flush;
}

/* ---- lifecycle --------------------------------------------------------- */

static const u8* nrb_mirror_guest(void* user, u32 space, u32 offset,
                                  u32 min_bytes)
{
    rsx_nr_d3d12* b = user;
    return b->guest_ptr(b->guest_user, space, offset, min_bytes);
}

rsx_nr_d3d12* rsx_nr_d3d12_create(void* device, u32 local_size, u32 main_size,
                                  const u8* (*guest_ptr)(void*, u32, u32, u32),
                                  u8* (*writable_ptr)(void*, u32, u32, u32),
                                  void* user)
{
    if (!guest_ptr)
        return NULL;
    rsx_nr_d3d12* b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->guest_ptr = guest_ptr;
    b->writable_ptr = writable_ptr;
    b->guest_user = user;
    b->local_size = local_size;
    b->main_size = main_size;

    if (device) {
        b->dev = (ID3D12Device*)device;
        b->dev->lpVtbl->AddRef(b->dev);
    } else {
        IDXGIFactory4* factory = NULL;
        IDXGIAdapter* adapter = NULL;
        if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4,
                                      (void**)&factory)))
            goto fail;
        HRESULT hr = factory->lpVtbl->EnumWarpAdapter(
            factory, &IID_IDXGIAdapter, (void**)&adapter);
        if (SUCCEEDED(hr))
            hr = D3D12CreateDevice((IUnknown*)adapter,
                                   D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                                   (void**)&b->dev);
        if (adapter)
            adapter->lpVtbl->Release(adapter);
        factory->lpVtbl->Release(factory);
        if (FAILED(hr) || !b->dev)
            goto fail;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {0};
    if (FAILED(b->dev->lpVtbl->CreateCommandQueue(
            b->dev, &qd, &IID_ID3D12CommandQueue, (void**)&b->queue)) ||
        FAILED(b->dev->lpVtbl->CreateCommandAllocator(
            b->dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void**)&b->alloc)) ||
        FAILED(b->dev->lpVtbl->CreateCommandList(
            b->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, b->alloc, NULL,
            &IID_ID3D12GraphicsCommandList, (void**)&b->list)) ||
        FAILED(b->dev->lpVtbl->CreateFence(b->dev, 0, D3D12_FENCE_FLAG_NONE,
                                           &IID_ID3D12Fence,
                                           (void**)&b->fence)))
        goto fail;
    b->list->lpVtbl->Close(b->list);
    b->fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!b->fence_event)
        goto fail;

    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 64;
    if (FAILED(b->dev->lpVtbl->CreateDescriptorHeap(
            b->dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&b->rtv_heap)))
        goto fail;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(b->dev->lpVtbl->CreateDescriptorHeap(
            b->dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&b->dsv_heap)))
        goto fail;
    b->rtv_size = b->dev->lpVtbl->GetDescriptorHandleIncrementSize(
        b->dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    b->dsv_size = b->dev->lpVtbl->GetDescriptorHandleIncrementSize(
        b->dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    /* root signature: b0 constants, b1 pull cbuffer, t20/t21 raw SRVs */
    {
        D3D12_ROOT_PARAMETER params[4];
        memset(params, 0, sizeof(params));
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor.ShaderRegister = 1;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor.ShaderRegister = 20;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[3].Descriptor.ShaderRegister = 21;
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rsd = {0};
        rsd.NumParameters = 4;
        rsd.pParameters = params;
        ID3DBlob* sig = NULL;
        ID3DBlob* err = NULL;
        if (FAILED(D3D12SerializeRootSignature(&rsd,
                                               D3D_ROOT_SIGNATURE_VERSION_1,
                                               &sig, &err))) {
            if (err)
                err->lpVtbl->Release(err);
            goto fail;
        }
        HRESULT hr = b->dev->lpVtbl->CreateRootSignature(
            b->dev, 0, sig->lpVtbl->GetBufferPointer(sig),
            sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature,
            (void**)&b->rootsig);
        sig->lpVtbl->Release(sig);
        if (err)
            err->lpVtbl->Release(err);
        if (FAILED(hr))
            goto fail;
    }

    b->upload = nrb_make_buffer(b->dev, NRB_UPLOAD_BYTES,
                                D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!b->upload)
        goto fail;
    D3D12_RANGE none = {0, 0};
    if (FAILED(b->upload->lpVtbl->Map(b->upload, 0, &none,
                                      (void**)&b->upload_mapped)))
        goto fail;

    if (rsx_guest_pages_init(&b->pages, local_size, main_size))
        goto fail;
    b->mirror_be = rsx_gpu_mirror_d3d12_create(b->dev, local_size, main_size,
                                               4u << 20);
    if (!b->mirror_be)
        goto fail;
    rsx_gpu_mirror_d3d12_set_guest(b->mirror_be, nrb_mirror_guest, b);
    rsx_gpu_mirror_ops mops;
    rsx_gpu_mirror_d3d12_get_ops(b->mirror_be, &mops);
    b->mirror = rsx_gpu_mirror_create(&b->pages, &mops);
    if (!b->mirror)
        goto fail;
    if (local_size)
        b->range_local = rsx_gpu_mirror_register(b->mirror, 0, 0, local_size);
    if (main_size)
        b->range_main = rsx_gpu_mirror_register(b->mirror, 1, 0, main_size);

    if (rsx_nr_pso_cache_init(&b->psos, NRB_PSO_CAP))
        goto fail;
    return b;

fail:
    rsx_nr_d3d12_destroy(b);
    return NULL;
}

void rsx_nr_d3d12_destroy(rsx_nr_d3d12* b)
{
    if (!b)
        return;
    if (b->queue && b->fence)
        nrb_wait_idle(b);
    for (u32 i = 0; i < b->psos.cap && b->psos.keys; i++) {
        if (b->psos.keys[i]) {
            ID3D12PipelineState* p =
                (ID3D12PipelineState*)(uintptr_t)b->psos.values[i];
            if (p)
                p->lpVtbl->Release(p);
        }
    }
    rsx_nr_pso_cache_destroy(&b->psos);
    for (u32 i = 0; i < NRB_MAX_RTS; i++) {
        if (b->rts[i].tex)
            b->rts[i].tex->lpVtbl->Release(b->rts[i].tex);
        if (b->rts[i].depth)
            b->rts[i].depth->lpVtbl->Release(b->rts[i].depth);
    }
    if (b->mirror)
        rsx_gpu_mirror_destroy(b->mirror);
    if (b->mirror_be)
        rsx_gpu_mirror_d3d12_destroy(b->mirror_be);
    if (b->pages.space[0].page_gen || b->pages.space[1].page_gen)
        rsx_guest_pages_destroy(&b->pages);
    free(b->idx_scratch);
    if (b->readback)
        b->readback->lpVtbl->Release(b->readback);
    if (b->upload)
        b->upload->lpVtbl->Release(b->upload);
    if (b->rootsig)
        b->rootsig->lpVtbl->Release(b->rootsig);
    if (b->rtv_heap)
        b->rtv_heap->lpVtbl->Release(b->rtv_heap);
    if (b->dsv_heap)
        b->dsv_heap->lpVtbl->Release(b->dsv_heap);
    if (b->fence_event)
        CloseHandle(b->fence_event);
    if (b->fence)
        b->fence->lpVtbl->Release(b->fence);
    if (b->list)
        b->list->lpVtbl->Release(b->list);
    if (b->alloc)
        b->alloc->lpVtbl->Release(b->alloc);
    if (b->queue)
        b->queue->lpVtbl->Release(b->queue);
    if (b->dev)
        b->dev->lpVtbl->Release(b->dev);
    free(b);
}

rsx_guest_pages* rsx_nr_d3d12_pages(rsx_nr_d3d12* b)
{
    return &b->pages;
}

int rsx_nr_d3d12_read_rt(rsx_nr_d3d12* b, u32 space, u32 offset,
                         u32 w, u32 h, u8* out)
{
    nrb_rt* rt = NULL;
    for (u32 i = 0; i < NRB_MAX_RTS; i++) {
        if (b->rts[i].live && b->rts[i].space == space &&
            b->rts[i].offset == offset) {
            rt = &b->rts[i];
            break;
        }
    }
    if (!rt || w != rt->w || h != rt->h)
        return -1;

    const u32 row = (w * 4 + 255u) & ~255u;
    const u32 need = row * h;
    if (!b->readback || b->readback_size < need) {
        if (b->readback)
            b->readback->lpVtbl->Release(b->readback);
        b->readback = nrb_make_buffer(b->dev, need,
                                      D3D12_HEAP_TYPE_READBACK,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
        if (!b->readback)
            return -1;
        b->readback_size = need;
    }

    if (nrb_open_list(b))
        return -1;
    D3D12_RESOURCE_BARRIER bar = {0};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = rt->tex;
    bar.Transition.Subresource = 0;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b->list->lpVtbl->ResourceBarrier(b->list, 1, &bar);

    D3D12_TEXTURE_COPY_LOCATION srcl = {0};
    srcl.pResource = rt->tex;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dstl = {0};
    dstl.pResource = b->readback;
    dstl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    dstl.PlacedFootprint.Footprint.Width = w;
    dstl.PlacedFootprint.Footprint.Height = h;
    dstl.PlacedFootprint.Footprint.Depth = 1;
    dstl.PlacedFootprint.Footprint.RowPitch = row;
    b->list->lpVtbl->CopyTextureRegion(b->list, &dstl, 0, 0, 0, &srcl, NULL);

    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b->list->lpVtbl->ResourceBarrier(b->list, 1, &bar);
    nrb_exec_wait(b);

    u8* mapped = NULL;
    D3D12_RANGE rr = {0, need};
    if (FAILED(b->readback->lpVtbl->Map(b->readback, 0, &rr,
                                        (void**)&mapped)))
        return -1;
    for (u32 y = 0; y < h; y++)
        memcpy(out + (size_t)y * w * 4, mapped + (size_t)y * row, w * 4);
    D3D12_RANGE nw = {0, 0};
    b->readback->lpVtbl->Unmap(b->readback, 0, &nw);
    return 0;
}

void rsx_nr_d3d12_get_stats(const rsx_nr_d3d12* b, rsx_nr_d3d12_stats* out)
{
    *out = b->stats;
}

#else /* !_WIN32 */

rsx_nr_d3d12* rsx_nr_d3d12_create(void* device, u32 local_size, u32 main_size,
                                  const u8* (*guest_ptr)(void*, u32, u32, u32),
                                  u8* (*writable_ptr)(void*, u32, u32, u32),
                                  void* user)
{
    (void)device; (void)local_size; (void)main_size;
    (void)guest_ptr; (void)writable_ptr; (void)user;
    return 0;
}
void rsx_nr_d3d12_destroy(rsx_nr_d3d12* b) { (void)b; }
rsx_guest_pages* rsx_nr_d3d12_pages(rsx_nr_d3d12* b) { (void)b; return 0; }
void rsx_nr_d3d12_get_exec_ops(rsx_nr_d3d12* b, rsx_nr_exec_ops* out)
{
    (void)b;
    if (out)
        memset(out, 0, sizeof(*out));
}
int rsx_nr_d3d12_read_rt(rsx_nr_d3d12* b, u32 space, u32 offset,
                         u32 w, u32 h, u8* out)
{
    (void)b; (void)space; (void)offset; (void)w; (void)h; (void)out;
    return -1;
}
void rsx_nr_d3d12_get_stats(const rsx_nr_d3d12* b, rsx_nr_d3d12_stats* out)
{
    (void)b;
    if (out)
        memset(out, 0, sizeof(*out));
}

#endif /* _WIN32 */
