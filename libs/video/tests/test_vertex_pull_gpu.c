/*
 * ps3recomp - WARP execution parity test for GPU vertex pulling
 *
 * End-to-end offline validation of the whole pulling chain with no game
 * boot: pseudo-random guest arenas are published through rsx_guest_pages,
 * mirrored to real D3D12 buffers by rsx_gpu_mirror + the D3D12 backend,
 * and a compute-shader wrapper around the *production* generated fetch
 * functions (rsx_vertex_pull_emit_globals/_loads, exactly what the pull
 * vertex shader will inline) decodes attributes on a WARP device.  Results
 * are compared against the production CPU path (rsx_vertex_fetch_one) per
 * lane: bit-exact for FLOAT/HALF/SINT16/UINT8 (both-NaN treated equal),
 * <= 1 ULP for the divide-based UNORM8/SNORM16/CMP32 conversions.
 *
 * Also covered: frequency modulo/divide, 20-bit base-index wrap, main vs
 * local locations, unaligned offsets/strides, in-shader big-endian u16/u32
 * index pulling, out-of-bounds fetch falling back to the RSX default value
 * (CPU parity), dirty-page re-upload feeding the GPU fresh data, an
 * unchanged sync uploading zero bytes, and vs_5_0 compilation of a full
 * pull-variant vertex program.
 *
 * Exits 2 ("skip") when no WARP D3D12 device exists on the machine.
 */
#ifndef _WIN32
#error WARP vertex-pull parity test is Windows-only (D3D12)
#endif

/* NOTE: rsx_vertex_formats.h must stay out of this TU — its DXGI_FORMAT_*
 * macros collide with the real dxgiformat.h enum included below. */
#include "../rsx_guest_pages.h"
#include "../rsx_gpu_mirror.h"
#include "../rsx_gpu_mirror_d3d12.h"
#include "../rsx_vertex_pull.h"
#include "../rsx_vp_decompiler.h"

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

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#define M_VTXBUF_OFFSET    0x1680u
#define M_VERTEX_DATA_BASE 0x1738u
#define M_VTXFMT           0x1740u
#define M_FREQUENCY_DIV    0x1FC0u

#define LOCAL_SIZE (2u * RSX_GUEST_BLOCK_SIZE)   /* 128 KiB */
#define MAIN_SIZE  (1u * RSX_GUEST_BLOCK_SIZE)   /*  64 KiB */
#define BASE_OFFSET 0x40u
#define MAX_REFS 128u

static int failures = 0;

#define CHECK(condition, label) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); \
        failures++; \
    } \
} while (0)

/* ---- guest arenas ------------------------------------------------------ */

static u8 g_local[LOCAL_SIZE];
static u8 g_main[MAIN_SIZE];

static const u8* arena_ptr(void* user, u32 space, u32 offset, u32 min_bytes)
{
    (void)user;
    if (space == 0)
        return ((u64)offset + min_bytes <= LOCAL_SIZE) ? g_local + offset
                                                       : NULL;
    if (space == 1)
        return ((u64)offset + min_bytes <= MAIN_SIZE) ? g_main + offset
                                                      : NULL;
    return NULL;
}

static u32 xorshift(u32* s)
{
    u32 x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x ? x : 0x9E3779B9u;
    return *s;
}

static void fill_arenas(void)
{
    u32 rng = 0x1234ABCDu;
    for (u32 i = 0; i < LOCAL_SIZE; i++)
        g_local[i] = (u8)xorshift(&rng);
    for (u32 i = 0; i < MAIN_SIZE; i++)
        g_main[i] = (u8)xorshift(&rng);
}

/* ---- D3D12 harness ----------------------------------------------------- */

typedef struct gpu {
    ID3D12Device* dev;
    ID3D12CommandQueue* queue;
    ID3D12CommandAllocator* alloc;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence* fence;
    HANDLE fence_event;
    u64 fence_value;
} gpu;

static gpu g;

static int gpu_init(void)
{
    IDXGIFactory4* factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory)))
        return -1;
    IDXGIAdapter* adapter = NULL;
    HRESULT hr = factory->lpVtbl->EnumWarpAdapter(
        factory, &IID_IDXGIAdapter, (void**)&adapter);
    if (FAILED(hr)) {
        factory->lpVtbl->Release(factory);
        return -1;
    }
    hr = D3D12CreateDevice((IUnknown*)adapter, D3D_FEATURE_LEVEL_11_0,
                           &IID_ID3D12Device, (void**)&g.dev);
    adapter->lpVtbl->Release(adapter);
    factory->lpVtbl->Release(factory);
    if (FAILED(hr))
        return -1;
    D3D12_COMMAND_QUEUE_DESC qd = {0};
    if (FAILED(g.dev->lpVtbl->CreateCommandQueue(
            g.dev, &qd, &IID_ID3D12CommandQueue, (void**)&g.queue)))
        return -1;
    if (FAILED(g.dev->lpVtbl->CreateCommandAllocator(
            g.dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void**)&g.alloc)))
        return -1;
    if (FAILED(g.dev->lpVtbl->CreateCommandList(
            g.dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, NULL,
            &IID_ID3D12GraphicsCommandList, (void**)&g.list)))
        return -1;
    if (FAILED(g.dev->lpVtbl->CreateFence(
            g.dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
            (void**)&g.fence)))
        return -1;
    g.fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    return 0;
}

static void gpu_exec_and_wait(void)
{
    g.list->lpVtbl->Close(g.list);
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    const u64 value = ++g.fence_value;
    g.queue->lpVtbl->Signal(g.queue, g.fence, value);
    if (g.fence->lpVtbl->GetCompletedValue(g.fence) < value) {
        g.fence->lpVtbl->SetEventOnCompletion(g.fence, value, g.fence_event);
        WaitForSingleObject(g.fence_event, 15000);
    }
    g.alloc->lpVtbl->Reset(g.alloc);
    g.list->lpVtbl->Reset(g.list, g.alloc, NULL);
}

static ID3D12Resource* make_buffer(u64 size, D3D12_HEAP_TYPE heap,
                                   D3D12_RESOURCE_STATES state,
                                   D3D12_RESOURCE_FLAGS flags)
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
    rd.Flags = flags;
    ID3D12Resource* res = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL,
            &IID_ID3D12Resource, (void**)&res)))
        return NULL;
    return res;
}

/* ---- shared mirror stack ----------------------------------------------- */

static rsx_guest_pages g_tracker;
static rsx_gpu_mirror* g_mirror;
static rsx_gpu_mirror_d3d12* g_backend;

/* Run one mirror sync session on the shared list.  Returns bytes. */
static u32 mirror_session(void)
{
    rsx_gpu_mirror_d3d12_retire(
        g_backend, g.fence->lpVtbl->GetCompletedValue(g.fence));
    rsx_gpu_mirror_d3d12_begin(g_backend, g.list);
    const u32 bytes = rsx_gpu_mirror_sync(g_mirror, 0);
    rsx_gpu_mirror_d3d12_end(g_backend, g.fence_value + 1u);
    gpu_exec_and_wait();
    return bytes;
}

/* ---- dispatch seeding helpers ------------------------------------------ */

static void set_attr(rsx_dispatch* rsx, u32 attr, u32 type, u32 size,
                     u32 stride, u32 frequency, u32 location, u32 offset)
{
    rsx->regs[(M_VTXFMT + attr * 4) >> 2] =
        type | (size << 4) | (stride << 8) | (frequency << 16);
    rsx->regs[(M_VTXBUF_OFFSET + attr * 4) >> 2] =
        (location << 31) | offset;
}

/* ---- comparison -------------------------------------------------------- */

static u32 f_bits(float f)
{
    u32 b;
    memcpy(&b, &f, 4);
    return b;
}

static int is_nan_bits(u32 b)
{
    return (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu);
}

static u64 ulp_distance(u32 a, u32 b)
{
    const s64 ka = (a & 0x80000000u) ? -(s64)(a & 0x7FFFFFFFu) : (s64)a;
    const s64 kb = (b & 0x80000000u) ? -(s64)(b & 0x7FFFFFFFu) : (s64)b;
    const s64 d = ka - kb;
    return (u64)(d < 0 ? -d : d);
}

static u32 type_tolerance_ulp(u32 type)
{
    switch (type) {
    case RSX_VTX_TYPE_UNORM8:
    case RSX_VTX_TYPE_SNORM16:
    case RSX_VTX_TYPE_CMP32:
        return 1u;   /* float divide: correctly-rounded on WARP, allow 1 */
    default:
        return 0u;   /* bit-exact conversions */
    }
}

/* ---- one parity case ---------------------------------------------------- */

typedef struct pull_case {
    const char* name;
    u32 mask;
    /* CPU side */
    rsx_vertex_ref refs[MAX_REFS];
    u32 count;
    /* GPU side */
    u32 use_vid_buffer;      /* 1: sysvid comes from vids[] (host-index) */
    u32 vids[MAX_REFS];
    u32 source;              /* RSX_PULL_SOURCE_* */
    u32 first;
    u32 base_index;
    u32 index_offset;
    u32 index_location;
} pull_case;

static char g_cs_text[256 * 1024];

static void run_case(const rsx_dispatch* rsx, const pull_case* pc)
{
    rsx_vertex_layout_plan layout;
    rsx_vertex_layout_plan_init(&layout, pc->mask);

    rsx_vertex_pull_plan plan;
    if (!rsx_vertex_pull_plan_init(&plan, rsx, &layout,
                                   RSX_PULL_TYPES_ALL)) {
        CHECK(0, pc->name);
        return;
    }

    /* CPU oracle: the production compact fetch. */
    rsx_vertex_fetch_plan cpu_plan;
    rsx_vertex_fetch_plan_init(&cpu_plan, rsx, &layout, arena_ptr, NULL);
    rsx_vertex_fetch_plan_prepare(&cpu_plan, pc->refs, pc->count);
    static float cpu_out[MAX_REFS][16][4];
    for (u32 i = 0; i < pc->count; i++) {
        if (!rsx_vertex_fetch_one(&cpu_plan, &pc->refs[i],
                                  (u8*)cpu_out[i])) {
            CHECK(0, "CPU oracle fetch failed");
            return;
        }
    }

    /* Compute-shader wrapper around the generated pull code. */
    static char globals[48 * 1024], loads[4 * 1024];
    if (rsx_vertex_pull_emit_globals(&plan, globals, sizeof(globals)) < 0 ||
        rsx_vertex_pull_emit_loads(&plan, "yz_sysvid", loads,
                                   sizeof(loads)) < 0) {
        CHECK(0, "codegen failed");
        return;
    }
    int n = snprintf(g_cs_text, sizeof(g_cs_text),
        "%s"
        "cbuffer YzTest : register(b2) { uint4 yz_test; };\n"
        "StructuredBuffer<uint> yz_test_vids : register(t23);\n"
        "RWStructuredBuffer<float4> yz_test_out : register(u0);\n"
        "[numthreads(64,1,1)]\n"
        "void main(uint3 yz_tid : SV_DispatchThreadID) {\n"
        "    if (yz_tid.x >= yz_test.x) return;\n"
        "    uint yz_sysvid = (yz_test.y != 0u)\n"
        "        ? yz_test_vids[yz_tid.x] : yz_tid.x;\n"
        "    float4 v[16];\n"
        "    [unroll] for (int _k=0;_k<16;_k++) v[_k]=float4(0,0,0,1);\n"
        "%s",
        globals, loads);
    for (u32 slot = 0; slot < layout.count; slot++) {
        n += snprintf(g_cs_text + n, sizeof(g_cs_text) - n,
                      "    yz_test_out[yz_tid.x * %uu + %uu] = v[%u];\n",
                      layout.count, slot, layout.attrs[slot]);
    }
    n += snprintf(g_cs_text + n, sizeof(g_cs_text) - n, "}\n");

    ID3DBlob* cs = NULL;
    ID3DBlob* err = NULL;
    if (FAILED(D3DCompile(g_cs_text, (SIZE_T)n, pc->name, NULL, NULL,
                          "main", "cs_5_0", 0, 0, &cs, &err))) {
        fprintf(stderr, "CS compile failed (%s): %s\n", pc->name,
                err ? (const char*)err->lpVtbl->GetBufferPointer(err)
                    : "?");
        if (err) err->lpVtbl->Release(err);
        CHECK(0, "CS compile");
        return;
    }
    if (err) err->lpVtbl->Release(err);

    /* Root signature: b1 (pull), b2 (test), t20/t21 (guest), t23 (vids),
     * u0 (out) — all root descriptors, no heaps. */
    D3D12_ROOT_PARAMETER params[6];
    memset(params, 0, sizeof(params));
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 1;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 2;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor.ShaderRegister = 20;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 21;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[4].Descriptor.ShaderRegister = 23;
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[5].Descriptor.ShaderRegister = 0;
    D3D12_ROOT_SIGNATURE_DESC rsd = {6, params, 0, NULL,
                                     D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* sig = NULL;
    if (FAILED(D3D12SerializeRootSignature(
            &rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, NULL))) {
        cs->lpVtbl->Release(cs);
        CHECK(0, "root signature serialize");
        return;
    }
    ID3D12RootSignature* rootsig = NULL;
    HRESULT hr = g.dev->lpVtbl->CreateRootSignature(
        g.dev, 0, sig->lpVtbl->GetBufferPointer(sig),
        sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature,
        (void**)&rootsig);
    sig->lpVtbl->Release(sig);
    if (FAILED(hr)) {
        cs->lpVtbl->Release(cs);
        CHECK(0, "root signature create");
        return;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = rootsig;
    pd.CS.pShaderBytecode = cs->lpVtbl->GetBufferPointer(cs);
    pd.CS.BytecodeLength = cs->lpVtbl->GetBufferSize(cs);
    ID3D12PipelineState* pso = NULL;
    hr = g.dev->lpVtbl->CreateComputePipelineState(
        g.dev, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    cs->lpVtbl->Release(cs);
    if (FAILED(hr)) {
        rootsig->lpVtbl->Release(rootsig);
        CHECK(0, "compute PSO create");
        return;
    }

    /* Constants + vids (upload heap) and output (default + readback). */
    const u32 out_bytes = pc->count * layout.count * 16u;
    ID3D12Resource* cbuf = make_buffer(
        4096, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* vids = make_buffer(
        MAX_REFS * 4u, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* outb = make_buffer(
        out_bytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12Resource* readback = make_buffer(
        out_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_NONE);
    if (!cbuf || !vids || !outb || !readback) {
        CHECK(0, "buffer allocation");
        return;
    }

    rsx_vertex_pull_constants consts;
    rsx_vertex_pull_fill_constants(
        &plan, pc->base_index, pc->first, pc->source, pc->index_offset,
        pc->index_location, LOCAL_SIZE, MAIN_SIZE, &consts);
    u8* mapped = NULL;
    D3D12_RANGE none = {0, 0};
    cbuf->lpVtbl->Map(cbuf, 0, &none, (void**)&mapped);
    memcpy(mapped, &consts, sizeof(consts));
    u32 test_consts[4] = {pc->count, pc->use_vid_buffer, 0, 0};
    memcpy(mapped + 1024, test_consts, sizeof(test_consts));
    cbuf->lpVtbl->Unmap(cbuf, 0, NULL);
    vids->lpVtbl->Map(vids, 0, &none, (void**)&mapped);
    memcpy(mapped, pc->vids, MAX_REFS * 4u);
    vids->lpVtbl->Unmap(vids, 0, NULL);

    g.list->lpVtbl->SetPipelineState(g.list, pso);
    g.list->lpVtbl->SetComputeRootSignature(g.list, rootsig);
    g.list->lpVtbl->SetComputeRootConstantBufferView(
        g.list, 0, cbuf->lpVtbl->GetGPUVirtualAddress(cbuf));
    g.list->lpVtbl->SetComputeRootConstantBufferView(
        g.list, 1, cbuf->lpVtbl->GetGPUVirtualAddress(cbuf) + 1024);
    ID3D12Resource* mem0 =
        (ID3D12Resource*)rsx_gpu_mirror_d3d12_buffer(g_backend, 0);
    ID3D12Resource* mem1 =
        (ID3D12Resource*)rsx_gpu_mirror_d3d12_buffer(g_backend, 1);
    g.list->lpVtbl->SetComputeRootShaderResourceView(
        g.list, 2, mem0->lpVtbl->GetGPUVirtualAddress(mem0));
    g.list->lpVtbl->SetComputeRootShaderResourceView(
        g.list, 3, mem1->lpVtbl->GetGPUVirtualAddress(mem1));
    g.list->lpVtbl->SetComputeRootShaderResourceView(
        g.list, 4, vids->lpVtbl->GetGPUVirtualAddress(vids));
    g.list->lpVtbl->SetComputeRootUnorderedAccessView(
        g.list, 5, outb->lpVtbl->GetGPUVirtualAddress(outb));
    g.list->lpVtbl->Dispatch(g.list, (pc->count + 63u) / 64u, 1, 1);

    D3D12_RESOURCE_BARRIER bar = {0};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = outb;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &bar);
    g.list->lpVtbl->CopyResource(g.list, readback, outb);
    gpu_exec_and_wait();

    float* gpu_out = NULL;
    D3D12_RANGE read_all = {0, out_bytes};
    readback->lpVtbl->Map(readback, 0, &read_all, (void**)&gpu_out);

    u32 mismatches = 0;
    u64 max_ulp = 0;
    for (u32 i = 0; i < pc->count; i++) {
        for (u32 slot = 0; slot < layout.count; slot++) {
            const u32 attr = layout.attrs[slot];
            const u32 tol = plan.attr[attr].pulled
                ? type_tolerance_ulp(plan.attr[attr].desc.type) : 0u;
            for (u32 lane = 0; lane < 4; lane++) {
                const u32 want = f_bits(cpu_out[i][slot][lane]);
                const u32 got = f_bits(
                    gpu_out[(i * layout.count + slot) * 4u + lane]);
                if (want == got)
                    continue;
                if (is_nan_bits(want) && is_nan_bits(got))
                    continue;   /* NaN payload identity not contractual */
                const u64 d = ulp_distance(want, got);
                if (d > max_ulp)
                    max_ulp = d;
                if (d <= tol)
                    continue;
                if (mismatches < 8)
                    fprintf(stderr,
                            "  %s: ref %u attr %u lane %u: "
                            "cpu %08X gpu %08X (%llu ulp)\n",
                            pc->name, i, attr, lane, want, got,
                            (unsigned long long)d);
                mismatches++;
            }
        }
    }
    readback->lpVtbl->Unmap(readback, 0, NULL);
    printf("  case %-24s refs=%3u slots=%u max_ulp=%llu -> %s\n",
           pc->name, pc->count, layout.count,
           (unsigned long long)max_ulp, mismatches ? "MISMATCH" : "match");
    CHECK(mismatches == 0, pc->name);

    readback->lpVtbl->Release(readback);
    outb->lpVtbl->Release(outb);
    vids->lpVtbl->Release(vids);
    cbuf->lpVtbl->Release(cbuf);
    pso->lpVtbl->Release(pso);
    rootsig->lpVtbl->Release(rootsig);
}

/* ---- the attribute universe used by the cases -------------------------- */

static void seed_formats(rsx_dispatch* rsx)
{
    rsx_dispatch_init(rsx, NULL);
    rsx->regs[M_VERTEX_DATA_BASE >> 2] = BASE_OFFSET;
    /*                attr type                  sz stride freq loc offset  */
    set_attr(rsx, 0, RSX_VTX_TYPE_FLOAT,   3, 32, 1, 0, 0x0100);
    set_attr(rsx, 1, RSX_VTX_TYPE_HALF,    4,  8, 1, 1, 0x2000);
    set_attr(rsx, 2, RSX_VTX_TYPE_UNORM8,  4,  4, 1, 0, 0x2200);
    set_attr(rsx, 3, RSX_VTX_TYPE_SNORM16, 2,  6, 1, 0, 0x2602);
    set_attr(rsx, 4, RSX_VTX_TYPE_SINT16,  4,  8, 1, 0, 0x2C10);
    set_attr(rsx, 5, RSX_VTX_TYPE_UINT8,   3,  3, 1, 0, 0x3421);
    set_attr(rsx, 6, RSX_VTX_TYPE_CMP32,   1,  4, 1, 0, 0x3730);
    set_attr(rsx, 7, RSX_VTX_TYPE_FLOAT,   1,  4, 1, 1, 0x0400);
}

static void case_all_formats(void)
{
    rsx_dispatch rsx;
    seed_formats(&rsx);
    pull_case pc;
    memset(&pc, 0, sizeof(pc));
    pc.name = "all-formats-arrays";
    pc.mask = 0xFFu;
    pc.count = 64;
    pc.first = 5;
    pc.source = RSX_PULL_SOURCE_ARRAYS;
    for (u32 i = 0; i < pc.count; i++) {
        pc.refs[i].vertex_id = pc.first + i;
        pc.refs[i].base_index = 0;
    }
    run_case(&rsx, &pc);
}

static void case_frequency_and_base(void)
{
    rsx_dispatch rsx;
    seed_formats(&rsx);
    /* Rewire frequencies: attr2 modulo 3, attr4 divide 4, attr5 divide 7,
     * attr0/1 keep freq 1 and take the wrapping base index. */
    set_attr(&rsx, 2, RSX_VTX_TYPE_UNORM8,  4, 4, 3, 0, 0x2200);
    set_attr(&rsx, 4, RSX_VTX_TYPE_SINT16,  4, 8, 4, 0, 0x2C10);
    set_attr(&rsx, 5, RSX_VTX_TYPE_UINT8,   3, 3, 7, 0, 0x3421);
    rsx.regs[M_FREQUENCY_DIV >> 2] = 1u << 2;   /* only attr2 is modulo */

    pull_case pc;
    memset(&pc, 0, sizeof(pc));
    pc.name = "freq-divider-base-wrap";
    pc.mask = 0xFFu;
    pc.count = 48;
    pc.use_vid_buffer = 1;          /* host-built raw-index mode */
    pc.source = RSX_PULL_SOURCE_ARRAYS;
    pc.first = 0;
    pc.base_index = 0xFFFFEu;       /* wraps in the 20-bit domain */
    u32 rng = 77u;
    for (u32 i = 0; i < pc.count; i++) {
        /* Elements land in 2..49 after the wrap: (vid + 0xFFFFE) & 0xFFFFF */
        const u32 vid = 4u + (xorshift(&rng) % 48u);
        pc.vids[i] = vid;
        pc.refs[i].vertex_id = vid;
        pc.refs[i].base_index = pc.base_index;
    }
    run_case(&rsx, &pc);
}

static void write_be16(u8* p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void write_be32(u8* p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

static void case_index_pulling(void)
{
    rsx_dispatch rsx;
    seed_formats(&rsx);

    /* Big-endian guest index arrays: u16 in main at 0x4000 (with two
     * leading entries the draw skips via first=2), u32 in local at 0x8000. */
    u32 rng = 991u;
    u16 idx16[40];
    u32 idx32[32];
    for (u32 i = 0; i < 40; i++) {
        idx16[i] = (u16)(xorshift(&rng) % 60u);
        write_be16(g_main + 0x4000 + i * 2u, idx16[i]);
    }
    for (u32 i = 0; i < 32; i++) {
        idx32[i] = xorshift(&rng) % 60u;
        write_be32(g_local + 0x8000 + i * 4u, idx32[i]);
    }
    rsx_guest_pages_note_write(&g_tracker, 1, 0x4000, 80);
    rsx_guest_pages_note_write(&g_tracker, 0, 0x8000, 128);
    mirror_session();

    pull_case pc;
    memset(&pc, 0, sizeof(pc));
    pc.name = "index-pull-u16-main";
    pc.mask = 0xFFu;
    pc.count = 38;
    pc.first = 2;
    pc.base_index = 6;
    pc.source = RSX_PULL_SOURCE_INDEX_U16;
    pc.index_offset = 0x4000;
    pc.index_location = 1;
    for (u32 i = 0; i < pc.count; i++) {
        pc.refs[i].vertex_id = idx16[pc.first + i];
        pc.refs[i].base_index = pc.base_index;
    }
    run_case(&rsx, &pc);

    memset(&pc, 0, sizeof(pc));
    pc.name = "index-pull-u32-local";
    pc.mask = 0xFFu;
    pc.count = 32;
    pc.first = 0;
    pc.base_index = 0;
    pc.source = RSX_PULL_SOURCE_INDEX_U32;
    pc.index_offset = 0x8000;
    pc.index_location = 0;
    for (u32 i = 0; i < pc.count; i++) {
        pc.refs[i].vertex_id = idx32[i];
        pc.refs[i].base_index = 0;
    }
    run_case(&rsx, &pc);
}

static void case_oob_default(void)
{
    rsx_dispatch rsx;
    seed_formats(&rsx);
    /* attr1 near the end of main memory: elements 0..7 fit, later ones run
     * off the end -> RSX default, on both paths. */
    set_attr(&rsx, 1, RSX_VTX_TYPE_HALF, 4, 8, 1, 1, MAIN_SIZE - 0x80u);
    pull_case pc;
    memset(&pc, 0, sizeof(pc));
    pc.name = "oob-default-fallback";
    pc.mask = 0x03u;   /* attr0 valid + attr1 crossing the end */
    pc.count = 24;
    pc.first = 0;
    pc.source = RSX_PULL_SOURCE_ARRAYS;
    for (u32 i = 0; i < pc.count; i++) {
        pc.refs[i].vertex_id = i;
        pc.refs[i].base_index = 0;
    }
    run_case(&rsx, &pc);
}

static void case_dirty_update(void)
{
    /* Overwrite part of attr0's array, publish, sync: exactly one page
     * must upload, and the GPU must decode the fresh bytes. */
    u32 rng = 0xD1547u;
    for (u32 i = 0; i < 64; i++)
        g_local[0x100u + BASE_OFFSET + 10u * 32u + i] = (u8)xorshift(&rng);
    rsx_guest_pages_note_write(&g_tracker, 0, 0x100u + BASE_OFFSET + 320u,
                               64);
    const u32 uploaded = mirror_session();
    CHECK(uploaded == RSX_GUEST_PAGE_SIZE,
          "dirty write re-uploads exactly one page");

    rsx_dispatch rsx;
    seed_formats(&rsx);
    pull_case pc;
    memset(&pc, 0, sizeof(pc));
    pc.name = "post-dirty-refetch";
    pc.mask = 0x01u;
    pc.count = 32;
    pc.first = 0;
    pc.source = RSX_PULL_SOURCE_ARRAYS;
    for (u32 i = 0; i < pc.count; i++) {
        pc.refs[i].vertex_id = i;
        pc.refs[i].base_index = 0;
    }
    run_case(&rsx, &pc);

    CHECK(mirror_session() == 0, "unchanged guest memory syncs zero bytes");
}

static void case_vs_compile(void)
{
    rsx_dispatch rsx;
    seed_formats(&rsx);
    rsx_vertex_layout_plan layout;
    rsx_vertex_layout_plan_init(&layout, 0xFFu);
    rsx_vertex_pull_plan plan;
    rsx_vertex_pull_plan_init(&plan, &rsx, &layout, RSX_PULL_TYPES_ALL);

    /* MOV o0, v0; END (same hand assembly as test_vertex_pull.c). */
    const u32 words[4] = {0x401F8000u, 0x0040000Du, 0x81000000u,
                          0x0001FF81u};
    u8 ucode[16];
    for (u32 w = 0; w < 4; w++) {
        ucode[w * 4 + 0] = (u8)words[w];
        ucode[w * 4 + 1] = (u8)(words[w] >> 8);
        ucode[w * 4 + 2] = (u8)(words[w] >> 16);
        ucode[w * 4 + 3] = (u8)(words[w] >> 24);
    }
    static char hlsl[192 * 1024];
    const int instrs = rsx_vertex_pull_decompile(
        &plan, ucode, sizeof(ucode), 0u, hlsl, sizeof(hlsl));
    CHECK(instrs == 1, "pull decompile");
    ID3DBlob* vs = NULL;
    ID3DBlob* err = NULL;
    const HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "pull_vs", NULL,
                                  NULL, "main", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr))
        fprintf(stderr, "pull VS compile: %s\n",
                err ? (const char*)err->lpVtbl->GetBufferPointer(err)
                    : "?");
    CHECK(SUCCEEDED(hr), "generated pull vertex shader compiles (vs_5_0)");
    if (vs) vs->lpVtbl->Release(vs);
    if (err) err->lpVtbl->Release(err);
}

int main(void)
{
    fill_arenas();
    if (gpu_init() != 0) {
        fprintf(stderr, "test_vertex_pull_gpu: SKIP (no WARP device)\n");
        return 2;
    }
    if (rsx_guest_pages_init(&g_tracker, LOCAL_SIZE, MAIN_SIZE) != 0) {
        fprintf(stderr, "tracker init failed\n");
        return 1;
    }
    g_backend = rsx_gpu_mirror_d3d12_create(g.dev, LOCAL_SIZE, MAIN_SIZE,
                                            3u * 256u * 1024u);
    if (!g_backend) {
        fprintf(stderr, "backend create failed\n");
        return 1;
    }
    rsx_gpu_mirror_d3d12_set_guest(g_backend, arena_ptr, NULL);
    rsx_gpu_mirror_ops ops;
    rsx_gpu_mirror_d3d12_get_ops(g_backend, &ops);
    g_mirror = rsx_gpu_mirror_create(&g_tracker, &ops);
    if (!g_mirror) {
        fprintf(stderr, "mirror create failed\n");
        return 1;
    }

    /* Register both arenas and bring the mirror current. */
    rsx_gpu_mirror_range r0 =
        rsx_gpu_mirror_register(g_mirror, 0, 0, LOCAL_SIZE);
    rsx_gpu_mirror_range r1 =
        rsx_gpu_mirror_register(g_mirror, 1, 0, MAIN_SIZE);
    CHECK(r0 && r1, "arena registration");
    const u32 first_sync = mirror_session();
    CHECK(first_sync == LOCAL_SIZE + MAIN_SIZE,
          "first sync uploads the registered arenas");
    CHECK(rsx_gpu_mirror_range_current(g_mirror, r0) &&
              rsx_gpu_mirror_range_current(g_mirror, r1),
          "mirror current after first sync");

    case_all_formats();
    case_frequency_and_base();
    case_index_pulling();
    case_oob_default();
    case_dirty_update();
    case_vs_compile();

    rsx_gpu_mirror_destroy(g_mirror);
    rsx_gpu_mirror_d3d12_destroy(g_backend);
    rsx_guest_pages_destroy(&g_tracker);
    if (failures) {
        fprintf(stderr, "test_vertex_pull_gpu: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_vertex_pull_gpu: ALL PASS\n");
    return 0;
}
