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
#include "rsx_fp_decompiler.h"
#include "rsx_nr_resources.h"
#include "rsx_vertex_pull.h"

#define NRB_MAX_RTS      32
#define NRB_UPLOAD_BYTES (32u << 20)
#define NRB_VS_TEXT      (256 * 1024)
#define NRB_PS_TEXT      (256 * 1024)
#define NRB_PSO_CAP      1024
#define NRB_TEX_CAP      2048
#define NRB_TEX_SNAP_WORDS (256u * 1024u)
#define NRB_TEX_UNITS    RSX_NIR_NUM_TEXTURES

typedef struct nrb_rt {
    ID3D12Resource* tex;
    ID3D12Resource* depth;
    u32 space, offset, w, h, fmt;
    u32 rtv_slot, dsv_slot;
    D3D12_RESOURCE_STATES color_state;
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
    ID3D12DescriptorHeap* texture_cpu_heap;
    ID3D12DescriptorHeap* texture_gpu_heap;
    ID3D12DescriptorHeap* sampler_gpu_heap;
    u32 texture_desc_size, sampler_desc_size;

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
    rsx_nr_res_cache textures;
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
    char ps_text[NRB_PS_TEXT];
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
    rt->color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
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

static void nrb_rt_transition(rsx_nr_d3d12* b, nrb_rt* rt,
                              D3D12_RESOURCE_STATES state);

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
    nrb_rt_transition(b, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
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

typedef struct nrb_fp_info {
    const u8* bytes;
    u32 size;
    u32 texture_mask;
    u32 unsupported;
    u64 structural_hash;
    rsx_fp_constant_block constants;
} nrb_fp_info;

/* Validate the exact subset translated by rsx_fp_decompiler before a draw
 * can become native-owned.  The decompiler deliberately emits comments for
 * unknown instructions so offline shader-corpus work can continue; the live
 * execution sink is stricter and must never turn those comments into a
 * visually plausible but incorrect draw. */
static int nrb_fp_opcode_supported(u32 opcode)
{
    switch (opcode) {
    case 0x00: /* NOP */
    case 0x01: /* MOV */
    case 0x02: /* MUL */
    case 0x03: /* ADD */
    case 0x04: /* MAD */
    case 0x05: /* DP3 */
    case 0x06: /* DP4 */
    case 0x08: /* MIN */
    case 0x09: /* MAX */
    case 0x0A: /* SLT */
    case 0x0B: /* SGE */
    case 0x0C: /* SLE */
    case 0x0D: /* SGT */
    case 0x0E: /* SNE */
    case 0x0F: /* SEQ */
    case 0x10: /* FRC */
    case 0x11: /* FLR */
    case 0x12: /* KIL */
    case 0x17: /* TEX */
    case 0x18: /* TXP */
    case 0x1A: /* RCP */
    case 0x1B: /* RSQ */
    case 0x1C: /* EX2 */
    case 0x1D: /* LG2 */
    case 0x1F: /* LRP */
    case 0x22: /* COS */
    case 0x23: /* SIN */
    case 0x26: /* POW */
    case 0x38: /* DP2 */
    case 0x39: /* NRM */
    case 0x3A: /* DIV */
    case 0x3B: /* DIVSQ */
    case 0x3D: /* FENCT */
    case 0x3E: /* FENCB */
        return 1;
    default:
        return 0;
    }
}

/* ---- persistent fragment textures ------------------------------------ */

#define NRB_TEX_B8          0x81u
#define NRB_TEX_A1R5G5B5    0x82u
#define NRB_TEX_A4R4G4B4    0x83u
#define NRB_TEX_R5G6B5      0x84u
#define NRB_TEX_A8R8G8B8    0x85u
#define NRB_TEX_DXT1        0x86u
#define NRB_TEX_DXT23       0x87u
#define NRB_TEX_DXT45       0x88u
#define NRB_TEX_G8B8        0x8Bu
#define NRB_TEX_DEPTH24_D8  0x90u
#define NRB_TEX_LINEAR      0x20u
#define NRB_TEX_UNNORM      0x40u
#define NRB_TEX_BASE_MASK   0x9Fu

typedef struct nrb_tex_level {
    u32 w, h;
    const u8* data;
    u32 row_bytes, rows;
} nrb_tex_level;

static u32 nrb_log2_u32(u32 v)
{
    u32 n = 0;
    while (v > 1u) {
        v >>= 1;
        n++;
    }
    return n;
}

static u32 nrb_morton_index(u32 x, u32 y, u32 lw, u32 lh)
{
    u32 index = 0, shift = 0;
    while (lw || lh) {
        if (lw) {
            index |= (x & 1u) << shift++;
            x >>= 1;
            lw--;
        }
        if (lh) {
            index |= (y & 1u) << shift++;
            y >>= 1;
            lh--;
        }
    }
    return index;
}

static u8 nrb_remap_comp(const u8 source[4], u32 remap, u32 component)
{
    const u32 op = (remap >> (8u + component * 2u)) & 3u;
    const u32 sel = (remap >> (component * 2u)) & 3u;
    if (op == 0u)
        return 0;
    if (op == 1u)
        return 255;
    return source[sel];
}

static void nrb_decode_texel(u32 format, const u8* p, u32 remap, u8 out[4])
{
    u8 source[4];                 /* A,R,G,B in RSX component order      */
    switch (format) {
    case NRB_TEX_B8:
        source[0] = 255;
        source[1] = source[2] = source[3] = p[0];
        break;
    case NRB_TEX_A4R4G4B4: {
        const u16 value = (u16)(((u16)p[0] << 8) | p[1]);
        source[0] = (u8)(((value >> 12) & 0xFu) * 17u);
        source[1] = (u8)(((value >> 8) & 0xFu) * 17u);
        source[2] = (u8)(((value >> 4) & 0xFu) * 17u);
        source[3] = (u8)((value & 0xFu) * 17u);
        break;
    }
    case NRB_TEX_A1R5G5B5: {
        const u16 value = (u16)(((u16)p[0] << 8) | p[1]);
        source[0] = (value & 0x8000u) ? 255 : 0;
        source[1] = (u8)(((value >> 10) & 0x1Fu) * 255u / 31u);
        source[2] = (u8)(((value >> 5) & 0x1Fu) * 255u / 31u);
        source[3] = (u8)((value & 0x1Fu) * 255u / 31u);
        break;
    }
    case NRB_TEX_R5G6B5: {
        const u16 value = (u16)(((u16)p[0] << 8) | p[1]);
        source[0] = 255;
        source[1] = (u8)(((value >> 11) & 0x1Fu) * 255u / 31u);
        source[2] = (u8)(((value >> 5) & 0x3Fu) * 255u / 63u);
        source[3] = (u8)((value & 0x1Fu) * 255u / 31u);
        break;
    }
    case NRB_TEX_G8B8:
        source[0] = 255;
        source[1] = source[2] = p[0];
        source[3] = p[1];
        break;
    case NRB_TEX_DEPTH24_D8:
        source[0] = 255;
        source[1] = source[2] = source[3] = p[0];
        break;
    default:                       /* A8R8G8B8 guest bytes                */
        source[0] = p[0]; source[1] = p[1];
        source[2] = p[2]; source[3] = p[3];
        break;
    }
    out[0] = nrb_remap_comp(source, remap, 1);
    out[1] = nrb_remap_comp(source, remap, 2);
    out[2] = nrb_remap_comp(source, remap, 3);
    out[3] = nrb_remap_comp(source, remap, 0);
}

static u32 nrb_texture_mips(const rsx_nir_texture* texture, int block)
{
    u32 count = texture->mipmaps ? texture->mipmaps : 1u;
    if (count > 14u)
        count = 14u;
    if (texture->cubemap && block) {
        count = 1u;
        for (u32 d = (texture->width < texture->height
                          ? texture->width : texture->height) / 4u;
             d > 1u; d >>= 1)
            count++;
        if (texture->mipmaps && count > texture->mipmaps)
            count = texture->mipmaps;
    } else {
        u32 physical = 1u;
        for (u32 d = texture->width > texture->height
                         ? texture->width : texture->height;
             d > 1u; d >>= 1)
            physical++;
        if (count > physical)
            count = physical;
    }
    return count;
}

static u32 nrb_texture_span(const rsx_nir_texture* texture)
{
    const u32 format = texture->format & NRB_TEX_BASE_MASK & ~NRB_TEX_UNNORM;
    const int linear = (texture->format & NRB_TEX_LINEAR) != 0;
    u32 texel = 0, block = 0;
    switch (format) {
    case NRB_TEX_DXT1: block = 8; break;
    case NRB_TEX_DXT23:
    case NRB_TEX_DXT45: block = 16; break;
    case NRB_TEX_B8: texel = 1; break;
    case NRB_TEX_A1R5G5B5:
    case NRB_TEX_A4R4G4B4:
    case NRB_TEX_R5G6B5:
    case NRB_TEX_G8B8: texel = 2; break;
    case NRB_TEX_A8R8G8B8:
    case NRB_TEX_DEPTH24_D8: texel = 4; break;
    default: return 0;
    }
    if (!texture->width || !texture->height || texture->width > 4096u ||
        texture->height > 4096u || texture->dimension != 2u)
        return 0;
    const u32 mips = nrb_texture_mips(texture, block != 0);
    u32 mw = texture->width, mh = texture->height;
    u64 span = 0;
    for (u32 mip = 0; mip < mips; mip++) {
        if (block)
            span += (u64)((mw + 3u) / 4u) * block * ((mh + 3u) / 4u);
        else {
            const u32 pitch = mip == 0u && linear && texture->pitch
                ? texture->pitch : mw * texel;
            span += (u64)pitch * mh;
        }
        if (mw == 1u && mh == 1u)
            break;
        mw = mw > 1u ? mw >> 1 : 1u;
        mh = mh > 1u ? mh >> 1 : 1u;
    }
    if (texture->cubemap)
        span *= 6u;
    return span && span <= 0xFFFFFFFFull ? (u32)span : 0;
}

static u8* nrb_texture_upload_slice(rsx_nr_d3d12* b, u32 size, u64* offset)
{
    u32 start = (b->upload_used + 511u) & ~511u;
    if ((u64)start + size > NRB_UPLOAD_BYTES) {
        nrb_exec_wait(b);
        if (nrb_open_list(b))
            return NULL;
        start = 0;
    }
    if ((u64)start + size > NRB_UPLOAD_BYTES)
        return NULL;
    *offset = start;
    b->upload_used = start + size;
    return b->upload_mapped + start;
}

static ID3D12Resource* nrb_create_texture_levels(
    rsx_nr_d3d12* b, DXGI_FORMAT format, const nrb_tex_level* levels,
    u32 mip_count, int cube)
{
    if (!mip_count || nrb_open_list(b))
        return NULL;
    D3D12_HEAP_PROPERTIES heap = {0};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {0};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = levels[0].w;
    desc.Height = levels[0].h;
    desc.DepthOrArraySize = (u16)(cube ? 6u : 1u);
    desc.MipLevels = (u16)mip_count;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    ID3D12Resource* resource = NULL;
    if (FAILED(b->dev->lpVtbl->CreateCommittedResource(
            b->dev, &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
            (void**)&resource)))
        return NULL;

    const u32 faces = cube ? 6u : 1u;
    for (u32 face = 0; face < faces; face++) {
        for (u32 mip = 0; mip < mip_count; mip++) {
            const nrb_tex_level* level = &levels[face * mip_count + mip];
            const u32 pitch = (level->row_bytes + 255u) & ~255u;
            const u64 bytes64 = (u64)pitch * level->rows;
            if (bytes64 > 0xFFFFFFFFull) {
                resource->lpVtbl->Release(resource);
                return NULL;
            }
            u64 upload_offset = 0;
            u8* upload = nrb_texture_upload_slice(
                b, (u32)bytes64, &upload_offset);
            if (!upload) {
                resource->lpVtbl->Release(resource);
                return NULL;
            }
            for (u32 row = 0; row < level->rows; row++)
                memcpy(upload + (size_t)row * pitch,
                       level->data + (size_t)row * level->row_bytes,
                       level->row_bytes);
            D3D12_TEXTURE_COPY_LOCATION source = {0}, destination = {0};
            source.pResource = b->upload;
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint.Offset = upload_offset;
            source.PlacedFootprint.Footprint.Format = format;
            source.PlacedFootprint.Footprint.Width = level->w;
            source.PlacedFootprint.Footprint.Height = level->h;
            source.PlacedFootprint.Footprint.Depth = 1;
            source.PlacedFootprint.Footprint.RowPitch = pitch;
            destination.pResource = resource;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = face * mip_count + mip;
            b->list->lpVtbl->CopyTextureRegion(
                b->list, &destination, 0, 0, 0, &source, NULL);
        }
    }
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b->list->lpVtbl->ResourceBarrier(b->list, 1, &barrier);
    return resource;
}

static ID3D12Resource* nrb_decode_guest_texture(
    rsx_nr_d3d12* b, const rsx_nir_texture* texture)
{
    const u32 format = texture->format & NRB_TEX_BASE_MASK & ~NRB_TEX_UNNORM;
    const int linear = (texture->format & NRB_TEX_LINEAR) != 0;
    const u32 span = nrb_texture_span(texture);
    const u8* source = span ? b->guest_ptr(
        b->guest_user, texture->location, texture->offset, span) : NULL;
    if (!source)
        return NULL;
    u32 mip_count = nrb_texture_mips(
        texture, format == NRB_TEX_DXT1 || format == NRB_TEX_DXT23 ||
                     format == NRB_TEX_DXT45);

    if (format == NRB_TEX_DXT1 || format == NRB_TEX_DXT23 ||
        format == NRB_TEX_DXT45) {
        const DXGI_FORMAT dxgi = format == NRB_TEX_DXT1
            ? DXGI_FORMAT_BC1_UNORM : format == NRB_TEX_DXT23
            ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
        const u32 block = format == NRB_TEX_DXT1 ? 8u : 16u;
        nrb_tex_level levels[6 * 14];
        const u32 faces = texture->cubemap ? 6u : 1u;
        const u32 face_span = texture->cubemap ? span / 6u : span;
        for (u32 face = 0; face < faces; face++) {
            u32 mw = texture->width, mh = texture->height, offset = 0;
            for (u32 mip = 0; mip < mip_count; mip++) {
                const u32 bw = (mw + 3u) / 4u, bh = (mh + 3u) / 4u;
                nrb_tex_level* level = &levels[face * mip_count + mip];
                level->w = mw;
                level->h = mh;
                level->data = source + (size_t)face * face_span + offset;
                level->row_bytes = bw * block;
                level->rows = bh;
                offset += bw * block * bh;
                mw = mw > 1u ? mw >> 1 : 1u;
                mh = mh > 1u ? mh >> 1 : 1u;
            }
        }
        return nrb_create_texture_levels(
            b, dxgi, levels, mip_count, texture->cubemap != 0);
    }

    u32 texel = 0;
    switch (format) {
    case NRB_TEX_B8: texel = 1; break;
    case NRB_TEX_A1R5G5B5:
    case NRB_TEX_A4R4G4B4:
    case NRB_TEX_R5G6B5:
    case NRB_TEX_G8B8: texel = 2; break;
    case NRB_TEX_A8R8G8B8:
    case NRB_TEX_DEPTH24_D8: texel = 4; break;
    default: return NULL;
    }
    if (!linear && ((texture->width & (texture->width - 1u)) ||
                    (texture->height & (texture->height - 1u))))
        return NULL;

    const u32 faces = texture->cubemap ? 6u : 1u;
    const u32 face_span = texture->cubemap ? span / 6u : span;
    nrb_tex_level levels[6 * 14];
    u8* rgba[6 * 14] = {0};
    u32 built = 0;
    for (u32 face = 0; face < faces; face++) {
        u32 mw = texture->width, mh = texture->height, offset = 0;
        for (u32 mip = 0; mip < mip_count; mip++) {
            const u32 pitch = mip == 0u && linear && texture->pitch
                ? texture->pitch : mw * texel;
            rgba[built] = malloc((size_t)mw * mh * 4u);
            if (!rgba[built])
                goto fail_rgba;
            const u8* level_source =
                source + (size_t)face * face_span + offset;
            const u32 lw = nrb_log2_u32(mw), lh = nrb_log2_u32(mh);
            for (u32 y = 0; y < mh; y++) {
                for (u32 x = 0; x < mw; x++) {
                    const u8* pixel = linear
                        ? level_source + (size_t)y * pitch + (size_t)x * texel
                        : level_source +
                            (size_t)nrb_morton_index(x, y, lw, lh) * texel;
                    nrb_decode_texel(
                        format, pixel, texture->remap & 0xFFFFu,
                        rgba[built] + ((size_t)y * mw + x) * 4u);
                }
            }
            levels[built].w = mw;
            levels[built].h = mh;
            levels[built].data = rgba[built];
            levels[built].row_bytes = mw * 4u;
            levels[built].rows = mh;
            built++;
            offset += pitch * mh;
            mw = mw > 1u ? mw >> 1 : 1u;
            mh = mh > 1u ? mh >> 1 : 1u;
        }
    }
    {
        ID3D12Resource* resource = nrb_create_texture_levels(
            b, DXGI_FORMAT_R8G8B8A8_UNORM, levels, mip_count,
            texture->cubemap != 0);
        for (u32 i = 0; i < built; i++)
            free(rgba[i]);
        return resource;
    }

fail_rgba:
    for (u32 i = 0; i < built; i++)
        free(rgba[i]);
    return NULL;
}

static D3D12_CPU_DESCRIPTOR_HANDLE nrb_texture_cpu_handle(
    rsx_nr_d3d12* b, u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    b->texture_cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
        b->texture_cpu_heap, &handle);
    handle.ptr += (SIZE_T)slot * b->texture_desc_size;
    return handle;
}

static D3D12_CPU_DESCRIPTOR_HANDLE nrb_texture_gpu_cpu_handle(
    rsx_nr_d3d12* b, u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    b->texture_gpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
        b->texture_gpu_heap, &handle);
    handle.ptr += (SIZE_T)slot * b->texture_desc_size;
    return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE nrb_texture_gpu_handle(
    rsx_nr_d3d12* b)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle;
    b->texture_gpu_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
        b->texture_gpu_heap, &handle);
    return handle;
}

static D3D12_CPU_DESCRIPTOR_HANDLE nrb_sampler_cpu_handle(
    rsx_nr_d3d12* b, u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    b->sampler_gpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
        b->sampler_gpu_heap, &handle);
    handle.ptr += (SIZE_T)slot * b->sampler_desc_size;
    return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE nrb_sampler_gpu_handle(
    rsx_nr_d3d12* b)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle;
    b->sampler_gpu_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
        b->sampler_gpu_heap, &handle);
    return handle;
}

static u32 nrb_component_mapping(u32 remap)
{
    static const u32 sel2d3d[4] = { 3, 0, 1, 2 };
    static const u32 out2comp[4] = { 1, 2, 3, 0 };
    u32 mapping = 1u << 12;
    for (u32 out = 0; out < 4; out++) {
        const u32 comp = out2comp[out];
        const u32 op = (remap >> (8u + comp * 2u)) & 3u;
        const u32 sel = (remap >> (comp * 2u)) & 3u;
        const u32 value = op == 0u ? 4u : op == 1u ? 5u : sel2d3d[sel];
        mapping |= value << (out * 3u);
    }
    return mapping;
}

static void nrb_write_texture_srv(rsx_nr_d3d12* b, u32 slot,
                                  const rsx_nir_texture* texture,
                                  ID3D12Resource* resource)
{
    const u32 format = texture->format & NRB_TEX_BASE_MASK &
                       ~NRB_TEX_UNNORM;
    const int compressed = format == NRB_TEX_DXT1 ||
                           format == NRB_TEX_DXT23 ||
                           format == NRB_TEX_DXT45;
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
    desc.Format = compressed
        ? (format == NRB_TEX_DXT1 ? DXGI_FORMAT_BC1_UNORM
           : format == NRB_TEX_DXT23 ? DXGI_FORMAT_BC2_UNORM
                                     : DXGI_FORMAT_BC3_UNORM)
        : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Shader4ComponentMapping =
        compressed && (texture->remap & 0xFFFFu) != 0xAAE4u
        ? nrb_component_mapping(texture->remap & 0xFFFFu)
        : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (texture->cubemap) {
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        desc.TextureCube.MostDetailedMip = 0;
        desc.TextureCube.MipLevels = nrb_texture_mips(texture, compressed);
        desc.TextureCube.ResourceMinLODClamp = 0.0f;
    } else {
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MostDetailedMip = 0;
        desc.Texture2D.MipLevels = nrb_texture_mips(texture, compressed);
        desc.Texture2D.ResourceMinLODClamp = 0.0f;
        desc.Texture2D.PlaneSlice = 0;
    }
    b->dev->lpVtbl->CreateShaderResourceView(
        b->dev, resource, &desc, nrb_texture_cpu_handle(b, slot));
}

static u64 nrb_texture_key(const rsx_nir_texture* texture)
{
    const u32 fields[] = {
        texture->format, texture->dimension, texture->cubemap,
        texture->mipmaps, texture->width, texture->height,
        texture->pitch, texture->depth, texture->remap & 0xFFFFu
    };
    return rsx_nr_hash_fold(0, fields, sizeof(fields));
}

static int nrb_resolve_guest_texture(rsx_nr_d3d12* b,
                                     const rsx_nir_texture* texture,
                                     u32* slot_out)
{
    const u32 span = nrb_texture_span(texture);
    if (!span)
        return -1;
    rsx_nr_res_key key = {0};
    key.kind = 1;
    key.space = texture->location;
    key.offset = texture->offset;
    key.size = span;
    key.fmt = nrb_texture_key(texture);
    rsx_nr_res* entry = rsx_nr_res_lookup(&b->textures, &key);
    if (entry && rsx_nr_res_current(&b->textures, entry)) {
        b->stats.texture_hits++;
        *slot_out = (u32)(entry - b->textures.slots);
        return 0;
    }

    ID3D12Resource* resource = nrb_decode_guest_texture(b, texture);
    if (!resource)
        return -1;
    if (entry) {
        ID3D12Resource* old = (ID3D12Resource*)(uintptr_t)entry->backend_id;
        entry->backend_id = (u64)(uintptr_t)resource;
        rsx_nr_res_revalidate(&b->textures, entry);
        if (old)
            old->lpVtbl->Release(old);
        b->stats.texture_refreshes++;
    } else {
        entry = rsx_nr_res_insert(
            &b->textures, &key, (u64)(uintptr_t)resource);
        if (!entry) {
            resource->lpVtbl->Release(resource);
            return -1;
        }
        b->stats.texture_builds++;
    }
    *slot_out = (u32)(entry - b->textures.slots);
    nrb_write_texture_srv(b, *slot_out, texture, resource);
    return 0;
}

static D3D12_TEXTURE_ADDRESS_MODE nrb_wrap(u32 wrap)
{
    switch (wrap & 0xFu) {
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

static D3D12_SAMPLER_DESC nrb_sampler(const rsx_nir_texture* texture)
{
    D3D12_SAMPLER_DESC desc = {0};
    const u32 minf = (texture->filter >> 16) & 7u;
    const u32 magf = (texture->filter >> 24) & 7u;
    const D3D12_FILTER_TYPE min_type =
        minf == 2u || minf == 4u || minf == 6u
        ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    const D3D12_FILTER_TYPE mag_type = magf == 2u
        ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    const D3D12_FILTER_TYPE mip_type = minf == 5u || minf == 6u
        ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    desc.Filter = D3D12_ENCODE_BASIC_FILTER(
        min_type, mag_type, mip_type, D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    desc.AddressU = nrb_wrap(texture->wrap);
    desc.AddressV = nrb_wrap(texture->wrap >> 8);
    desc.AddressW = nrb_wrap(texture->wrap >> 16);
    desc.MinLOD = (float)((texture->control0 >> 19) & 0xFFFu) / 256.0f;
    desc.MaxLOD = minf >= 3u
        ? (float)((texture->control0 >> 7) & 0xFFFu) / 256.0f : 0.0f;
    if (desc.MaxLOD < desc.MinLOD)
        desc.MaxLOD = desc.MinLOD;
    desc.MaxAnisotropy = 1;
    desc.BorderColor[0] =
        (float)((texture->border_color >> 16) & 0xFFu) / 255.0f;
    desc.BorderColor[1] =
        (float)((texture->border_color >> 8) & 0xFFu) / 255.0f;
    desc.BorderColor[2] =
        (float)(texture->border_color & 0xFFu) / 255.0f;
    desc.BorderColor[3] =
        (float)((texture->border_color >> 24) & 0xFFu) / 255.0f;
    return desc;
}

static nrb_rt* nrb_texture_rt_alias(rsx_nr_d3d12* b,
                                    const rsx_nir_texture* texture,
                                    const nrb_rt* draw_rt, u32 unit)
{
    const u32 format = texture->format & NRB_TEX_BASE_MASK &
                       ~NRB_TEX_UNNORM;
    if (texture->cubemap || texture->dimension != 2u ||
        texture->mipmaps > 1u || format != NRB_TEX_A8R8G8B8)
        return NULL;
    for (u32 i = 0; i < NRB_MAX_RTS; i++) {
        nrb_rt* rt = &b->rts[i];
        if (!rt->live || rt->space != texture->location ||
            rt->offset != texture->offset || rt->w != texture->width ||
            rt->h != texture->height)
            continue;
        if (rt == draw_rt)
            return rt;              /* input/output alias: must refuse    */
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping =
            (texture->remap & 0xFFFFu) == 0xAAE4u
            ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
            : nrb_component_mapping(texture->remap & 0xFFFFu);
        desc.Texture2D.MipLevels = 1;
        b->dev->lpVtbl->CreateShaderResourceView(
            b->dev, rt->tex, &desc,
            nrb_texture_cpu_handle(b, NRB_TEX_CAP + 1u + unit));
        return rt;
    }
    return NULL;
}

static int nrb_prepare_textures(rsx_nr_d3d12* b,
                                const rsx_nir_pipeline* st,
                                u32 texture_mask, nrb_rt* draw_rt,
                                nrb_rt* aliases[NRB_TEX_UNITS],
                                u32* cube_mask_out)
{
    const u32 null_slot = NRB_TEX_CAP;
    u32 cube_mask = 0;
    for (u32 unit = 0; unit < NRB_TEX_UNITS; unit++) {
        const rsx_nir_texture* texture = &st->textures[unit];
        u32 source_slot = null_slot;
        aliases[unit] = NULL;
        if (texture_mask & (1u << unit)) {
            if (!texture->enabled)
                return -1;
            if (texture->cubemap)
                cube_mask |= 1u << unit;
            aliases[unit] = nrb_texture_rt_alias(
                b, texture, draw_rt, unit);
            if (aliases[unit] == draw_rt)
                return -1;
            if (aliases[unit]) {
                nrb_rt_transition(b, aliases[unit],
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                source_slot = NRB_TEX_CAP + 1u + unit;
                b->stats.rt_alias_binds++;
            } else if (nrb_resolve_guest_texture(
                           b, texture, &source_slot) != 0) {
                return -1;
            }
        }
        b->dev->lpVtbl->CopyDescriptorsSimple(
            b->dev, 1, nrb_texture_gpu_cpu_handle(b, unit),
            nrb_texture_cpu_handle(b, source_slot),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SAMPLER_DESC sampler = nrb_sampler(texture);
        b->dev->lpVtbl->CreateSampler(
            b->dev, &sampler, nrb_sampler_cpu_handle(b, unit));
    }
    *cube_mask_out = cube_mask;
    return 0;
}

static void nrb_restore_texture_aliases(
    rsx_nr_d3d12* b, nrb_rt* aliases[NRB_TEX_UNITS])
{
    for (u32 unit = 0; unit < NRB_TEX_UNITS; unit++) {
        if (aliases[unit])
            nrb_rt_transition(b, aliases[unit],
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
}

static void nrb_release_texture(void* user, u64 backend_id)
{
    (void)user;
    ID3D12Resource* resource = (ID3D12Resource*)(uintptr_t)backend_id;
    if (resource)
        resource->lpVtbl->Release(resource);
}

static void nrb_rt_transition(rsx_nr_d3d12* b, nrb_rt* rt,
                              D3D12_RESOURCE_STATES state)
{
    if (!rt || rt->color_state == state)
        return;
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rt->tex;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = rt->color_state;
    barrier.Transition.StateAfter = state;
    b->list->lpVtbl->ResourceBarrier(b->list, 1, &barrier);
    rt->color_state = state;
}

static int nrb_resolve_fp(rsx_nr_d3d12* b, const rsx_nir_pipeline* st,
                          nrb_fp_info* out)
{
    memset(out, 0, sizeof(*out));
    const rsx_nir_fragment_program* fp = &st->fragment_program;
    const u32 space_size = fp->location ? b->main_size : b->local_size;
    if (fp->location > 1 || fp->offset >= space_size)
        return -1;
    u32 max_bytes = space_size - fp->offset;
    if (max_bytes > 0x10000u)
        max_bytes = 0x10000u;
    const u8* first = b->guest_ptr(
        b->guest_user, fp->location, fp->offset,
        max_bytes < 16u ? max_bytes : 16u);
    if (!first || max_bytes < 16u)
        return -1;
    const u32 size = rsx_fp_program_size(first, max_bytes);
    if (!size)
        return -1;
    const u8* bytes = b->guest_ptr(
        b->guest_user, fp->location, fp->offset, size);
    if (!bytes)
        return -1;

    u32 off = 0;
    while (off + 16u <= size) {
        const u32 w0 = rsx_fp_read_word(bytes + off);
        const u32 w1 = rsx_fp_read_word(bytes + off + 4u);
        const u32 w2 = rsx_fp_read_word(bytes + off + 8u);
        const u32 w3 = rsx_fp_read_word(bytes + off + 12u);
        const u32 opcode = (w0 >> 24) & 0x3Fu;
        if (!nrb_fp_opcode_supported(opcode) || (w2 & 0x80000000u))
            out->unsupported++;
        if (opcode == 0x17u || opcode == 0x18u)
            out->texture_mask |= 1u << ((w0 >> 17) & 0xFu);
        off += 16u;
        if ((w1 & 3u) == 2u || (w2 & 3u) == 2u || (w3 & 3u) == 2u)
            off += 16u;
        if (w0 & 1u)
            break;
    }
    if (off > size || rsx_fp_collect_constants(bytes, size, &out->constants) < 0)
        return -1;
    out->structural_hash = rsx_fp_structural_hash(
        bytes, size, 1469598103934665603ull);
    if (!out->structural_hash)
        return -1;
    out->bytes = bytes;
    out->size = size;
    return 0;
}

static D3D12_BLEND nrb_blend_factor(u32 f, int alpha)
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
    case 0x8001:
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
    case 0x8002:
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default: return D3D12_BLEND_ONE;
    }
}

static D3D12_BLEND_OP nrb_blend_op(u32 op)
{
    switch (op) {
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
    }
}

static D3D12_STENCIL_OP nrb_stencil_op(u32 op)
{
    switch (op) {
    case 0x0000: return D3D12_STENCIL_OP_ZERO;
    case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D12_STENCIL_OP_INVERT;
    case 0x8507: return D3D12_STENCIL_OP_INCR;
    case 0x8508: return D3D12_STENCIL_OP_DECR;
    default: return D3D12_STENCIL_OP_KEEP;
    }
}

static void nrb_apply_render_state(
    D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, const rsx_nir_pipeline* st)
{
    const rsx_nir_blend* bl = &st->blend;
    const rsx_nir_raster* ra = &st->raster;
    const rsx_nir_depth_stencil* ds = &st->depth_stencil;
    D3D12_RENDER_TARGET_BLEND_DESC* rt = &pd->BlendState.RenderTarget[0];
    rt->RenderTargetWriteMask =
        ((ra->color_mask & 0x000000FFu) ? D3D12_COLOR_WRITE_ENABLE_BLUE : 0) |
        ((ra->color_mask & 0x0000FF00u) ? D3D12_COLOR_WRITE_ENABLE_GREEN : 0) |
        ((ra->color_mask & 0x00FF0000u) ? D3D12_COLOR_WRITE_ENABLE_RED : 0) |
        ((ra->color_mask & 0xFF000000u) ? D3D12_COLOR_WRITE_ENABLE_ALPHA : 0);
    if (bl->blend_enable) {
        rt->BlendEnable = TRUE;
        rt->SrcBlend = nrb_blend_factor(bl->sfactor & 0xFFFFu, 0);
        rt->DestBlend = nrb_blend_factor(bl->dfactor & 0xFFFFu, 0);
        rt->BlendOp = nrb_blend_op(bl->equation & 0xFFFFu);
        rt->SrcBlendAlpha = nrb_blend_factor(bl->sfactor >> 16, 1);
        rt->DestBlendAlpha = nrb_blend_factor(bl->dfactor >> 16, 1);
        rt->BlendOpAlpha = nrb_blend_op(bl->equation >> 16);
    }
    pd->RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd->RasterizerState.CullMode = !ra->cull_face_enable
        ? D3D12_CULL_MODE_NONE
        : ra->cull_face == 0x0404u ? D3D12_CULL_MODE_FRONT
        : ra->cull_face == 0x0405u ? D3D12_CULL_MODE_BACK
                                    : D3D12_CULL_MODE_NONE;
    pd->RasterizerState.FrontCounterClockwise =
        ra->front_face == 0x0901u ? TRUE : FALSE;
    pd->RasterizerState.DepthClipEnable = TRUE;
    pd->DepthStencilState.DepthEnable = ds->depth_test_enable ? TRUE : FALSE;
    pd->DepthStencilState.DepthWriteMask = ds->depth_write_enable
        ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd->DepthStencilState.DepthFunc = nrb_depth_func(ds->depth_func);
    if (ds->stencil_test_enable) {
        pd->DepthStencilState.StencilEnable = TRUE;
        pd->DepthStencilState.StencilReadMask = (UINT8)ds->stencil_mask;
        pd->DepthStencilState.StencilWriteMask = (UINT8)ds->stencil_mask;
        pd->DepthStencilState.FrontFace.StencilFunc =
            nrb_depth_func(ds->stencil_func);
        pd->DepthStencilState.FrontFace.StencilFailOp =
            nrb_stencil_op(ds->stencil_op_fail);
        pd->DepthStencilState.FrontFace.StencilDepthFailOp =
            nrb_stencil_op(ds->stencil_op_zfail);
        pd->DepthStencilState.FrontFace.StencilPassOp =
            nrb_stencil_op(ds->stencil_op_zpass);
        pd->DepthStencilState.BackFace = pd->DepthStencilState.FrontFace;
    }
}

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
                                        const nrb_fp_info* fp,
                                        const rsx_vertex_pull_plan* plan,
                                        D3D12_PRIMITIVE_TOPOLOGY_TYPE tt,
                                        int strip_cut, u32 cube_mask)
{
    /* Structural shader identity deliberately excludes inline FP constants
     * and alpha-ref: both are uploaded through b1.  Everything that shapes
     * the compiled source or PSO remains in the key. */
    u64 key = rsx_nir_hash_words(vp_words, vp_word_count);
    key = rsx_nr_hash_fold(key ^ rsx_vertex_pull_signature(plan), &tt,
                           sizeof(tt));
    key = rsx_nr_hash_fold(key, &strip_cut, sizeof(strip_cut));
    key = rsx_nr_hash_fold(key, &fp->structural_hash,
                           sizeof(fp->structural_hash));
    key = rsx_nr_hash_fold(key, &cube_mask, sizeof(cube_mask));
    const u32 fp_ctrl = st->fragment_program.control & 0x40u;
    key = rsx_nr_hash_fold(key, &fp_ctrl, sizeof(fp_ctrl));
    const u32 alpha_enable = st->blend.alpha_test_enable ? 1u : 0u;
    const u32 alpha_func = alpha_enable ? st->blend.alpha_func : 0u;
    key = rsx_nr_hash_fold(key, &alpha_enable, sizeof(alpha_enable));
    key = rsx_nr_hash_fold(key, &alpha_func, sizeof(alpha_func));
    key = rsx_nr_hash_fold(key, &st->surface.color_format,
                           sizeof(st->surface.color_format));
    key = rsx_nr_hash_fold(key, &st->raster, sizeof(st->raster));
    key = rsx_nr_hash_fold(key, &st->depth_stencil,
                           sizeof(st->depth_stencil));
    rsx_nir_blend pso_blend = st->blend;
    /* These are draw data in buffered/dynamic state, not PSO identity. */
    pso_blend.alpha_ref = 0;
    pso_blend.blend_color = 0;
    key = rsx_nr_hash_fold(key, &pso_blend, sizeof(pso_blend));
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
        /* Clip-space passthrough of ATTR0 (offline pixel tests), with the
         * complete varying interface expected by a real fragment program. */
        n = snprintf(b->vs_text, sizeof(b->vs_text),
                     "%s\n"
                     "struct VSOutput {\n"
                     " float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
                     " float4 fog:FOG;\n"
                     " float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
                     " float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
                     "};\n"
                     "VSOutput main(uint yz_sysvid : SV_VertexID) {\n"
                     "    float4 v[16];\n"
                     "    [unroll] for (uint i = 0u; i < 16u; i++)\n"
                     "        v[i] = float4(0.0, 0.0, 0.0, 1.0);\n"
                     "%s"
                     "    VSOutput o; o.pos=v[0]; o.col0=float4(1,0,1,1);\n"
                     "    o.col1=0; o.fog=0; o.t0=0; o.t1=0; o.t2=0; o.t3=0;\n"
                     "    o.t4=0; o.t5=0; o.t6=0; o.t7=0; return o;\n"
                     "}\n",
                     b->pull_globals, b->pull_loads);
        if (n <= 0 || n >= (int)sizeof(b->vs_text))
            return NULL;
    }

    ID3DBlob* vs = nrb_compile(b, b->vs_text, strlen(b->vs_text), "vs_5_0");
    if (!vs)
        return NULL;
    u32 constant_count = 0;
    int fi = rsx_fp_decompile_buffered_ex(
        fp->bytes, fp->size, st->fragment_program.control, cube_mask,
        b->ps_text, sizeof(b->ps_text), &constant_count);
    if (fi <= 0 || constant_count != fp->constants.count ||
        (alpha_enable && rsx_fp_apply_alpha_test_buffered(
            b->ps_text, sizeof(b->ps_text), alpha_func) < 0)) {
        vs->lpVtbl->Release(vs);
        return NULL;
    }
    ID3DBlob* ps = nrb_compile(
        b, b->ps_text, strlen(b->ps_text), "ps_5_0");
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
    pd.SampleMask = 0xFFFFFFFFu;
    nrb_apply_render_state(&pd, st);
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

    nrb_fp_info fp;
    if (nrb_resolve_fp(b, st, &fp) != 0 || fp.unsupported) {
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_fp++;
        return -1;
    }
    u32 cube_mask = 0;
    for (u32 unit = 0; unit < NRB_TEX_UNITS; unit++) {
        if (!(fp.texture_mask & (1u << unit)))
            continue;
        if (!st->textures[unit].enabled) {
            b->stats.unsupported_draws++;
            b->stats.unsup_draw_texture++;
            b->stats.texture_failures++;
            return -1;
        }
        if (st->textures[unit].cubemap)
            cube_mask |= 1u << unit;
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

    ID3D12PipelineState* pso = nrb_get_pso(
        b, st, vp_words, vp_word_count, &fp, &plan, tt,
        use_cut_ib && strips, cube_mask);
    if (!pso) {
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_pso++;
        return -1;
    }
    if (nrb_open_list(b))
        return -1;
    nrb_mirror_sync(b);

    nrb_rt* texture_aliases[NRB_TEX_UNITS] = {0};
    u32 resolved_cube_mask = 0;
    if (nrb_prepare_textures(b, st, fp.texture_mask, rt,
                             texture_aliases, &resolved_cube_mask) != 0 ||
        resolved_cube_mask != cube_mask) {
        nrb_restore_texture_aliases(b, texture_aliases);
        nrb_exec_wait(b);
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_texture++;
        b->stats.texture_failures++;
        return -1;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv;
    nrb_rt_transition(b, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
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

    ID3D12DescriptorHeap* descriptor_heaps[2] = {
        b->texture_gpu_heap, b->sampler_gpu_heap
    };
    b->list->lpVtbl->SetDescriptorHeaps(b->list, 2, descriptor_heaps);
    b->list->lpVtbl->SetGraphicsRootSignature(b->list, b->rootsig);
    b->list->lpVtbl->SetGraphicsRootDescriptorTable(
        b->list, 5, nrb_texture_gpu_handle(b));
    b->list->lpVtbl->SetGraphicsRootDescriptorTable(
        b->list, 6, nrb_sampler_gpu_handle(b));
    b->list->lpVtbl->SetPipelineState(b->list, pso);
    b->list->lpVtbl->IASetPrimitiveTopology(b->list, topo);

    /* b0: transform constants + the RSX viewport mapping consumed by the
     * generated VP epilogue. */
    u64 const_va = 0;
    const u32 vp_cb_bytes = sizeof(st->constants) + 8u * sizeof(float);
    u8* cp = nrb_upload_alloc(b, vp_cb_bytes, &const_va);
    if (!cp) {
        nrb_restore_texture_aliases(b, texture_aliases);
        nrb_exec_wait(b);
        b->stats.unsupported_draws++;
        return -1;
    }
    memcpy(cp, st->constants, sizeof(st->constants));
    float* xf = (float*)(cp + sizeof(st->constants));
    const float w = (float)(st->surface.clip_w ? st->surface.clip_w : rt->w);
    const float h = (float)(st->surface.clip_h ? st->surface.clip_h : rt->h);
    xf[0] = 1.0f; xf[1] = 1.0f; xf[2] = 1.0f; xf[3] = 0.0f;
    xf[4] = 0.0f; xf[5] = 0.0f; xf[6] = 0.0f; xf[7] = 0.0f;
    if (st->viewport.scale[0] != 0.0f ||
        st->viewport.translate[0] != 0.0f) {
        xf[0] = st->viewport.scale[0] / (w * 0.5f);
        xf[1] = -(st->viewport.scale[1] / (h * 0.5f));
        xf[2] = st->viewport.scale[2];
        xf[4] = (st->viewport.translate[0] - w * 0.5f) / (w * 0.5f);
        xf[5] = -((st->viewport.translate[1] - h * 0.5f) / (h * 0.5f));
        xf[6] = st->viewport.translate[2];
    }
    b->list->lpVtbl->SetGraphicsRootConstantBufferView(b->list, 0, const_va);

    /* b1 (pixel): exact inline CONST words plus dynamic alpha reference.
     * A one-slot placeholder preserves the HLSL layout for no-CONST FPs. */
    const u32 fp_slots = fp.constants.count ? fp.constants.count : 1u;
    const u32 fp_cb_bytes = (fp_slots + 1u) * 16u;
    u64 fp_va = 0;
    u8* fp_cp = nrb_upload_alloc(b, fp_cb_bytes, &fp_va);
    if (!fp_cp) {
        nrb_restore_texture_aliases(b, texture_aliases);
        nrb_exec_wait(b);
        b->stats.unsupported_draws++;
        return -1;
    }
    memset(fp_cp, 0, fp_cb_bytes);
    if (fp.constants.count)
        memcpy(fp_cp, fp.constants.values, fp.constants.count * 16u);
    const float alpha_ref = rsx_fp_alpha_ref(
        st->blend.alpha_ref, st->surface.color_format);
    memcpy(fp_cp + fp_slots * 16u, &alpha_ref, sizeof(alpha_ref));
    b->list->lpVtbl->SetGraphicsRootConstantBufferView(b->list, 4, fp_va);

    float blend_factor[4];
    blend_factor[0] = (float)((st->blend.blend_color >> 16) & 0xFFu) / 255.0f;
    blend_factor[1] = (float)((st->blend.blend_color >> 8) & 0xFFu) / 255.0f;
    blend_factor[2] = (float)(st->blend.blend_color & 0xFFu) / 255.0f;
    blend_factor[3] = (float)((st->blend.blend_color >> 24) & 0xFFu) / 255.0f;
    b->list->lpVtbl->OMSetBlendFactor(b->list, blend_factor);
    b->list->lpVtbl->OMSetStencilRef(
        b->list, st->depth_stencil.stencil_ref & 0xFFu);

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

    nrb_restore_texture_aliases(b, texture_aliases);
    nrb_exec_wait(b);
    if (failed) {
        b->stats.unsupported_draws++;
        return -1;
    }
    if (use_cut_ib)
        b->stats.restart_draws++;
    b->stats.real_fp_draws++;
    if (fp.texture_mask)
        b->stats.texture_draws++;
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
    rsx_nr_res_next_frame(&b->textures);
    if ((b->textures.frame & 127u) == 0u)
        rsx_nr_res_sweep(&b->textures, 600u, nrb_release_texture, b);
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

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = NRB_TEX_CAP + 1u + NRB_TEX_UNITS;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(b->dev->lpVtbl->CreateDescriptorHeap(
            b->dev, &hd, &IID_ID3D12DescriptorHeap,
            (void**)&b->texture_cpu_heap)))
        goto fail;
    hd.NumDescriptors = NRB_TEX_UNITS;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(b->dev->lpVtbl->CreateDescriptorHeap(
            b->dev, &hd, &IID_ID3D12DescriptorHeap,
            (void**)&b->texture_gpu_heap)))
        goto fail;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    if (FAILED(b->dev->lpVtbl->CreateDescriptorHeap(
            b->dev, &hd, &IID_ID3D12DescriptorHeap,
            (void**)&b->sampler_gpu_heap)))
        goto fail;
    b->texture_desc_size =
        b->dev->lpVtbl->GetDescriptorHandleIncrementSize(
            b->dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    b->sampler_desc_size =
        b->dev->lpVtbl->GetDescriptorHandleIncrementSize(
            b->dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC null_desc = {0};
        null_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        null_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        null_desc.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        null_desc.Texture2D.MipLevels = 1;
        b->dev->lpVtbl->CreateShaderResourceView(
            b->dev, NULL, &null_desc,
            nrb_texture_cpu_handle(b, NRB_TEX_CAP));
    }

    /* root signature: b0 vertex constants, b1 vertex-pull constants,
     * t20/t21 mirror buffers, pixel b1 for buffered FP constants, and
     * exact fragment SRV/sampler descriptor tables t0..t15/s0..s15. */
    {
        D3D12_DESCRIPTOR_RANGE ranges[2];
        memset(ranges, 0, sizeof(ranges));
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = NRB_TEX_UNITS;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        ranges[1].NumDescriptors = NRB_TEX_UNITS;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER params[7];
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
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[4].Descriptor.ShaderRegister = 1;
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[5].DescriptorTable.NumDescriptorRanges = 1;
        params[5].DescriptorTable.pDescriptorRanges = &ranges[0];
        params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[6].DescriptorTable.NumDescriptorRanges = 1;
        params[6].DescriptorTable.pDescriptorRanges = &ranges[1];
        params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {0};
        rsd.NumParameters = 7;
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
    if (rsx_nr_res_cache_init(&b->textures, NRB_TEX_CAP,
                              NRB_TEX_SNAP_WORDS, &b->pages, NULL))
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
    if (b->textures.slots) {
        rsx_nr_res_sweep(&b->textures, 0, nrb_release_texture, b);
        rsx_nr_res_cache_destroy(&b->textures);
    }
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
    if (b->texture_cpu_heap)
        b->texture_cpu_heap->lpVtbl->Release(b->texture_cpu_heap);
    if (b->texture_gpu_heap)
        b->texture_gpu_heap->lpVtbl->Release(b->texture_gpu_heap);
    if (b->sampler_gpu_heap)
        b->sampler_gpu_heap->lpVtbl->Release(b->sampler_gpu_heap);
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
    nrb_rt_transition(b, rt, D3D12_RESOURCE_STATE_COPY_SOURCE);

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

    nrb_rt_transition(b, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
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
