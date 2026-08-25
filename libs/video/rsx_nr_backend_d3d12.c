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
#include <limits.h>
#include <stddef.h>

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
#include "rsx_vp_decompiler.h"

/* Offline-only device-removal oracle. It must be enabled before device
 * creation, remains completely absent unless the narrow environment flag is
 * set, and emits only after an actual failure. */
static int g_nrb_dred_enabled;

static void nrb_enable_device_oracle(void)
{
    if (!getenv("YZ_NR_D3D12_DRED"))
        return;
    ID3D12DeviceRemovedExtendedDataSettings* settings = NULL;
    if (SUCCEEDED(D3D12GetDebugInterface(
            &IID_ID3D12DeviceRemovedExtendedDataSettings,
            (void**)&settings)) && settings) {
        settings->lpVtbl->SetAutoBreadcrumbsEnablement(
            settings, D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->lpVtbl->SetPageFaultEnablement(
            settings, D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->lpVtbl->Release(settings);
        g_nrb_dred_enabled = 1;
    }
    if (getenv("YZ_NR_D3D12_DEBUG")) {
        ID3D12Debug* debug = NULL;
        if (SUCCEEDED(D3D12GetDebugInterface(
                &IID_ID3D12Debug, (void**)&debug)) && debug) {
            debug->lpVtbl->EnableDebugLayer(debug);
            debug->lpVtbl->Release(debug);
        }
    }
}

/* The union of the archived orphanage/Hana/Frontier/gun captures contains
 * 33 exact color-target identities (32 guest addresses).  Keep every native
 * target alive by identity: evicting an offscreen producer would discard GPU
 * content that a later pass can sample without republishing guest bytes.
 * The live descriptor heap already reserved 64 RTV slots, so use that
 * bounded capacity rather than failing on the 33rd encountered target. */
#define NRB_MAX_RTS      64u
#define NRB_MAX_DEPTHS   32u
#define NRB_UPLOAD_BYTES (32u << 20)
#define NRB_VS_TEXT      (256 * 1024)
#define NRB_PS_TEXT      (256 * 1024)
/* A complete live boot carries menu/movie shader pairs into world rendering.
 * The extended Hana->Frontier route proved 7,391 distinct persisted PSOs and
 * filled the former 8,192-slot table at its intentional 75% load ceiling;
 * 185,501 later requests then rebuilt/reloaded instead of staying resident.
 * Keep the same bounded load rule with enough headroom for the measured route.
 * The same capture filled the 2,048-slot texture table at exactly 1,536 live
 * identities and then refused a valid DXT5 gun-transition dependency. These
 * are storage limits, not unsupported RSX semantics; enlarging the fixed
 * tables preserves every cache key, invalidation, and resource-lifetime rule. */
#define NRB_PSO_CAP      16384
#define NRB_TEX_CAP      8192
#define NRB_TEX_SNAP_WORDS (1024u * 1024u)
#define NRB_TEX_UNITS    RSX_NIR_NUM_TEXTURES
#define NRB_VTEX_UNITS   RSX_NIR_NUM_VERTEX_TEXTURES
#define NRB_DRAW_TABLES  128u       /* D3D12 sampler heap limit / 16      */
#define NRB_SRV_TABLE_STRIDE (NRB_TEX_UNITS + NRB_VTEX_UNITS)
#define NRB_SAMPLER_TABLE_STRIDE NRB_TEX_UNITS
#define NRB_SHADER_DESCRIPTORS (NRB_DRAW_TABLES * NRB_SRV_TABLE_STRIDE)
#define NRB_SHADER_SAMPLERS (NRB_DRAW_TABLES * NRB_SAMPLER_TABLE_STRIDE)
#define NRB_MAX_RETIRED_TEXTURES NRB_SHADER_DESCRIPTORS
#define NRB_MAX_DRAW_BATCHES 4096u
#define NRB_MAX_REQUIRED_SPANS (RSX_NIR_NUM_VERTEX_ATTR * 2u + 2u)
#define NRB_HANA_INPUT_SAMPLES 16u
#define NRB_HANA_INPUT_SPANS   8u
#define NRB_HANA_DEPTH_UNITS   2u
#define NRB_HANA_DEPTH_GRID    16u
#define NRB_HANA_DEPTH_POINTS  \
    (NRB_HANA_DEPTH_GRID * NRB_HANA_DEPTH_GRID)
#define NRB_HANA_DEPTH_POINT_STRIDE 512u
#define NRB_HANA_DEPTH_SAMPLE_BYTES \
    (NRB_HANA_DEPTH_UNITS * NRB_HANA_DEPTH_POINTS * \
     NRB_HANA_DEPTH_POINT_STRIDE)
#define NRB_HANA_DEPTH_READBACK_BYTES \
    (NRB_HANA_INPUT_SAMPLES * NRB_HANA_DEPTH_SAMPLE_BYTES)
#define NRB_HANA_CONDITION_KEYS 512u

typedef struct nrb_prepared_batch {
    u64 index_va;
    u64 pull_va;
    u32 draw_count;
    u32 skip;
} nrb_prepared_batch;

typedef struct nrb_required_span {
    u32 space, offset, size;
} nrb_required_span;

typedef struct nrb_hana_input_vtex {
    rsx_nir_texture texture;
    u64 source_hash;
    u64 uploaded_hash;
    u64 space_epoch;
    u32 span;
    u32 cache_slot;
    u32 cache_current;
    u32 resolution;
    u32 first_page_gen;
    u32 last_page_gen;
} nrb_hana_input_vtex;

typedef struct nrb_hana_input_depth {
    u64 resource_identity;
    u64 sample_identity;
    u64 write_generation;
    u64 resolve_generation;
    u64 command_generation;
    u64 recording_fence;
    u64 completed_fence;
    u64 content_hash;
    u32 space;
    u32 offset;
    u32 resource_state;
    u32 sample_state;
    u32 srv_format;
    u32 texture_format;
    u32 texture_wrap;
    u32 texture_remap;
    u32 texture_filter;
    u32 texture_control;
    u32 texture_border;
    u32 sampler_filter;
    u32 sampler_address_u;
    u32 sampler_address_v;
    u32 sampler_address_w;
    u32 sampler_comparison;
    u32 zero_count;
    u32 one_count;
    u32 external;
    u32 sample_valid;
    u32 copy_recorded;
} nrb_hana_input_depth;

typedef struct nrb_hana_input_sample {
    u64 match;
    u64 vp_hash;
    u64 fp_hash;
    u64 constants_hash;
    u64 required_hash;
    u32 vp_start;
    u32 vp_words;
    u32 vp_branch_bits;
    u32 render_condition_enabled;
    u32 render_condition_dma;
    u32 render_condition_offset;
    u32 bound_vtex_mask;
    u32 used_vtex_mask;
    u32 required_count;
    u32 index_location;
    u32 index_offset;
    u32 base_index;
    u32 batch_count;
    u32 total_count;
    nrb_required_span required[NRB_HANA_INPUT_SPANS];
    u64 required_span_hash[NRB_HANA_INPUT_SPANS];
    u64 required_space_epoch[NRB_HANA_INPUT_SPANS];
    u32 required_first_page_gen[NRB_HANA_INPUT_SPANS];
    u32 required_last_page_gen[NRB_HANA_INPUT_SPANS];
    nrb_hana_input_vtex vtex[NRB_VTEX_UNITS];
    nrb_hana_input_depth depth[NRB_HANA_DEPTH_UNITS];
} nrb_hana_input_sample;

/* Fixed-memory census of conditional-render draw families.  It is armed by
 * the existing narrow Hana input oracle only, so ordinary production draws
 * do not pay a key construction or lookup cost.  The key deliberately uses
 * producer-visible identities rather than host pointers: this makes a live
 * skipped family comparable with captured NIR and the report stream. */
typedef struct nrb_hana_condition_key {
    u32 dma_report;
    u32 report_offset;
    u32 observed_value;
    u32 fp_location;
    u32 fp_offset;
    u32 fp_control;
    u32 vp_start;
    u32 vp_branch_bits;
    u32 color_location;
    u32 color_offset;
    u32 color_format;
    u32 depth_location;
    u32 depth_offset;
    u32 depth_format;
    u32 depth_test;
    u32 depth_write;
    u32 blend_enable;
    u32 alpha_test;
    u64 attempts;
    u64 skipped;
} nrb_hana_condition_key;

typedef struct nrb_rt {
    ID3D12Resource* tex;
    u32 space, offset, w, h, fmt;
    /* Strict-native mode may legitimately retain more than one RSX surface
     * identity at one display address (for example the title and world use
     * different logical color formats).  Presentation must choose the
     * identity most recently written, not the oldest table slot. */
    u64 last_write_serial;
    u64 color_clear_writes;
    u64 draw_writes;
    u64 present_count;
    DXGI_FORMAT dxgi;
    u32 rtv_slot;
    D3D12_RESOURCE_STATES color_state;
    int live;
    int external;
} nrb_rt;

typedef struct nrb_depth {
    ID3D12Resource* tex;
    ID3D12Resource* sample_tex;
    u32 space, offset, w, h, fmt;
    u32 dsv_slot;
    DXGI_FORMAT resource_dxgi, dsv_dxgi, srv_dxgi, sample_srv_dxgi;
    D3D12_RESOURCE_STATES state;
    D3D12_RESOURCE_STATES sample_state;
    u64 write_generation;
    u64 resolve_generation;
    int live;
    int external;
    int sample_valid;
} nrb_depth;

typedef struct nrb_descriptor_table_key {
    u64 resource[NRB_SRV_TABLE_STRIDE];
    u64 view[NRB_SRV_TABLE_STRIDE];
    D3D12_SAMPLER_DESC sampler[NRB_TEX_UNITS];
    u32 texture_mask;
    u32 vtex_mask;
} nrb_descriptor_table_key;

typedef struct nrb_display {
    u32 location, offset, width, height;
    int valid;
} nrb_display;

struct rsx_nr_d3d12 {
    ID3D12Device* dev;
    ID3D12CommandQueue* queue;
    ID3D12CommandAllocator* alloc;
    ID3D12GraphicsCommandList* list;
    ID3D12GraphicsCommandList1* list1;
    ID3D12GraphicsCommandList* owned_list;
    ID3D12GraphicsCommandList1* owned_list1;
    ID3D12GraphicsCommandList1* shared_list1;
    ID3D12Fence* fence;
    HANDLE fence_event;
    u64 fence_value;
    int list_open;
    int depth_bounds_supported;
    int shared_timeline;
    int timeline_leased;
    int timeline_fault;
    rsx_nr_d3d12_timeline_acquire_fn timeline_acquire;
    rsx_nr_d3d12_timeline_release_fn timeline_release;
    rsx_nr_d3d12_timeline_flush_fn timeline_flush;
    void* timeline_user;
    ID3D12GraphicsCommandList* shared_list;
    u64 shared_generation;
    u64 shared_recording_fence;
    u64 shared_completed_fence;

    ID3D12DescriptorHeap* rtv_heap;
    ID3D12DescriptorHeap* dsv_heap;
    u32 rtv_size, dsv_size, rtv_used, dsv_used;
    ID3D12DescriptorHeap* texture_cpu_heap;
    ID3D12DescriptorHeap* texture_gpu_heap;
    ID3D12DescriptorHeap* sampler_gpu_heap;
    ID3D12DescriptorHeap* depth_snapshot_heap;
    u32 texture_desc_size, sampler_desc_size;
    u32 descriptor_tables_used;
    nrb_descriptor_table_key descriptor_table_keys[NRB_DRAW_TABLES];

    ID3D12RootSignature* rootsig;
    ID3D12RootSignature* depth_snapshot_rootsig;
    ID3D12PipelineState* depth_snapshot_pso;

    ID3D12Resource* upload;          /* fence-gated per-exec bump ring     */
    u8* upload_mapped;
    u32 upload_used;

    ID3D12Resource* readback;
    u32 readback_size;

    rsx_guest_pages pages;
    rsx_gpu_mirror* mirror;
    rsx_gpu_mirror_d3d12* mirror_be;
    rsx_gpu_mirror_range* resident_page[RSX_GUEST_NUM_SPACES];
    u32 resident_page_count[RSX_GUEST_NUM_SPACES];
    u64* watched_host_page_bits[RSX_GUEST_NUM_SPACES];
    u32 watched_host_page_count[RSX_GUEST_NUM_SPACES];
    rsx_nr_d3d12_watch_page_fn watch_page;
    void* watch_page_user;
    rsx_nr_d3d12_borrow_color_fn borrow_color;
    rsx_nr_d3d12_borrow_depth_fn borrow_depth;
    rsx_nr_d3d12_resolve_depth_sample_fn resolve_depth_sample;
    void* broker_user;
    rsx_nr_d3d12_publish_write_fn publish_write;
    void* publish_write_user;
    rsx_nr_d3d12_render_condition_fn render_condition_read;
    void* render_condition_user;
    rsx_nr_d3d12_compile_shader_fn compile_shader;
    rsx_nr_d3d12_pso_load_fn pso_load;
    rsx_nr_d3d12_pso_store_fn pso_store;
    rsx_nr_d3d12_pso_free_fn pso_free;
    void* content_cache_user;
    u32 local_size, main_size;

    rsx_nr_pso_cache psos;
    rsx_nr_res_cache textures;
    ID3D12Resource* retired_textures[NRB_MAX_RETIRED_TEXTURES];
    u32 retired_texture_count;
    nrb_rt rts[NRB_MAX_RTS];
    nrb_depth depths[NRB_MAX_DEPTHS];
    nrb_rt* last_rt;
    u64 rt_write_serial;
    nrb_display displays[8];
    rsx_nr_d3d12_present_fn present_cb;
    void* present_user;
    int rgba_targets;
    int scanout_provenance;
    int stall_aggregate;
    int submit_attribution;
    u64 submit_retired_draws;
    u64 submit_retired_batches;
    u32 coherent_vp_options;
    int force_draw_input_refresh;
    int force_draw_input_allocated;
    u32 force_draw_input_epoch;
    u32* force_draw_input_page_epoch[RSX_GUEST_NUM_SPACES];

    /* Default-off, bounded live-input oracle for the exact Hana shadow
     * consumer. It samples only the first four matches and one of each later
     * 8192, keeps a fixed rolling table, and emits once at shutdown. The
     * ordinary production path performs one cached flag test and nothing
     * else. Per-texture uploaded hashes are populated only while armed. */
    int hana_input_oracle;
    int hana_input_dumped;
    u64 hana_input_matches;
    u32 hana_input_writes;
    nrb_hana_input_sample hana_input[NRB_HANA_INPUT_SAMPLES];
    nrb_hana_condition_key hana_condition[NRB_HANA_CONDITION_KEYS];
    u32 hana_condition_count;
    u32 hana_condition_replace;
    u64 hana_condition_total;
    u64 hana_condition_overflow;
    u64 hana_vtex_uploaded_hash[NRB_TEX_CAP];
    u8 hana_vtex_resolution[NRB_TEX_CAP]; /* 1 hit, 2 build, 3 refresh */
    ID3D12Resource* hana_depth_readback;
    u32 hana_depth_copy_failures;

    const u8* (*guest_ptr)(void* user, u32 space, u32 offset, u32 min_bytes);
    u8* (*writable_ptr)(void* user, u32 space, u32 offset, u32 min_bytes);
    void* guest_user;

    /* restart-draw index conversion scratch (offline model grows on
     * demand; a live integration preallocates its high-water size) */
    u32* idx_scratch;
    u32 idx_scratch_cap;
    /* Every fallible batch read/expansion and upload reservation completes
     * before the first Draw* is recorded. A refused draw is therefore safe
     * to hand to the ordered legacy fallback without a partial render. */
    nrb_prepared_batch prepared_batches[NRB_MAX_DRAW_BATCHES];

    rsx_nr_d3d12_stats stats;
    char vs_text[NRB_VS_TEXT];
    char ps_text[NRB_PS_TEXT];
    char pull_globals[48 * 1024];
    char pull_loads[8 * 1024];
};

static void nrb_dump_device_oracle(ID3D12Device* dev, const char* where,
                                   HRESULT trigger)
{
    if (!g_nrb_dred_enabled || !dev)
        return;
    fprintf(stderr, "[nrb-dred] %s trigger=0x%08lX removed=0x%08lX\n",
            where, (unsigned long)trigger,
            (unsigned long)dev->lpVtbl->GetDeviceRemovedReason(dev));
    ID3D12DeviceRemovedExtendedData1* dred = NULL;
    HRESULT hr = dev->lpVtbl->QueryInterface(
        dev, &IID_ID3D12DeviceRemovedExtendedData1, (void**)&dred);
    if (FAILED(hr) || !dred) {
        fprintf(stderr, "[nrb-dred] query=0x%08lX\n", (unsigned long)hr);
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {0};
    hr = dred->lpVtbl->GetAutoBreadcrumbsOutput1(dred, &breadcrumbs);
    fprintf(stderr, "[nrb-dred] breadcrumbs=0x%08lX\n",
            (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        const D3D12_AUTO_BREADCRUMB_NODE1* node =
            breadcrumbs.pHeadAutoBreadcrumbNode;
        for (u32 n = 0; node && n < 12u; ++n, node = node->pNext) {
            const u32 last = node->pLastBreadcrumbValue
                ? *node->pLastBreadcrumbValue : 0u;
            fprintf(stderr,
                    "[nrb-dred] node=%u count=%u last=%u list=%s queue=%s\n",
                    n, node->BreadcrumbCount, last,
                    node->pCommandListDebugNameA
                        ? node->pCommandListDebugNameA : "<unnamed>",
                    node->pCommandQueueDebugNameA
                        ? node->pCommandQueueDebugNameA : "<unnamed>");
            if (!node->pCommandHistory || !node->BreadcrumbCount)
                continue;
            const u32 begin = last > 6u ? last - 6u : 0u;
            u32 end = last + 6u;
            if (end >= node->BreadcrumbCount)
                end = node->BreadcrumbCount - 1u;
            for (u32 i = begin; i <= end; ++i)
                fprintf(stderr, "[nrb-dred] op[%u]%s=%u\n", i,
                        i == last ? "*" : "",
                        (unsigned)node->pCommandHistory[i]);
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {0};
    hr = dred->lpVtbl->GetPageFaultAllocationOutput1(dred, &fault);
    fprintf(stderr, "[nrb-dred] page-fault=0x%08lX va=0x%016llX\n",
            (unsigned long)hr, (unsigned long long)fault.PageFaultVA);
    dred->lpVtbl->Release(dred);
    fflush(stderr);
}

/* ---- device plumbing --------------------------------------------------- */

static u64 nrb_stall_now(const rsx_nr_d3d12* b)
{
    LARGE_INTEGER now;
    if (!b->stall_aggregate || !QueryPerformanceCounter(&now))
        return 0u;
    return (u64)now.QuadPart;
}

static void nrb_stall_finish(const rsx_nr_d3d12* b, u64 start,
                             u64* count, u64* ticks)
{
    LARGE_INTEGER now;
    if (!start || !b->stall_aggregate ||
        !QueryPerformanceCounter(&now) || (u64)now.QuadPart < start)
        return;
    (*count)++;
    *ticks += (u64)now.QuadPart - start;
}

static u64 nrb_submit_now(const rsx_nr_d3d12* b)
{
    LARGE_INTEGER now;
    if (!b->submit_attribution || !QueryPerformanceCounter(&now))
        return 0u;
    return (u64)now.QuadPart;
}

static void nrb_submit_finish(
    rsx_nr_d3d12* b, rsx_nr_d3d12_submit_cause cause, u64 start,
    u32 descriptor_tables, u32 upload_bytes, u64 readback_bytes)
{
    LARGE_INTEGER now;
    if (!b->submit_attribution || cause >= RSX_NR_D3D12_SUBMIT_CAUSE_COUNT ||
        !start || !QueryPerformanceCounter(&now) ||
        (u64)now.QuadPart < start)
        return;
    rsx_nr_d3d12_submit_cause_stats* const out =
        &b->stats.submit_cause[cause];
    out->submissions++;
    out->cpu_wait_ticks += (u64)now.QuadPart - start;
    out->draws += b->stats.draws - b->submit_retired_draws;
    out->draw_batches += b->stats.draw_batches - b->submit_retired_batches;
    out->descriptor_tables += descriptor_tables;
    out->upload_bytes += upload_bytes;
    out->readback_bytes += readback_bytes;
    b->submit_retired_draws = b->stats.draws;
    b->submit_retired_batches = b->stats.draw_batches;
}

static void nrb_submit_transfer_finish(rsx_nr_d3d12* b, u64 start,
                                       int upload, u64 bytes)
{
    LARGE_INTEGER now;
    if (!b->submit_attribution || !start ||
        !QueryPerformanceCounter(&now) || (u64)now.QuadPart < start)
        return;
    if (upload) {
        b->stats.submit_transfer_upload_count++;
        b->stats.submit_transfer_upload_ticks +=
            (u64)now.QuadPart - start;
        b->stats.submit_transfer_upload_bytes += bytes;
    } else {
        b->stats.submit_transfer_readback_count++;
        b->stats.submit_transfer_readback_ticks +=
            (u64)now.QuadPart - start;
        b->stats.submit_transfer_readback_bytes += bytes;
    }
}

static int nrb_wait_idle(rsx_nr_d3d12* b)
{
    const u64 v = ++b->fence_value;
    if (FAILED(b->queue->lpVtbl->Signal(b->queue, b->fence, v)))
        return -1;
    u64 completed = b->fence->lpVtbl->GetCompletedValue(b->fence);
    /* D3D12 returns UINT64_MAX when the device was removed. It must never be
     * mistaken for a fence value newer than the requested submission. */
    if (completed == UINT64_MAX)
        return -1;
    if (completed < v) {
        if (FAILED(b->fence->lpVtbl->SetEventOnCompletion(
                b->fence, v, b->fence_event)) ||
            WaitForSingleObject(b->fence_event, 60000) != WAIT_OBJECT_0)
            return -1;
    }
    completed = b->fence->lpVtbl->GetCompletedValue(b->fence);
    return completed != UINT64_MAX && completed >= v ? 0 : -1;
}

static void nrb_release_timeline_lease(rsx_nr_d3d12* b)
{
    if (b->timeline_leased) {
        b->timeline_release(b->timeline_user);
        b->timeline_leased = 0;
    }
}

static void nrb_release_retired_textures(rsx_nr_d3d12* b)
{
    for (u32 i = 0; i < b->retired_texture_count; ++i)
        b->retired_textures[i]->lpVtbl->Release(b->retired_textures[i]);
    b->retired_texture_count = 0;
}

static int nrb_acquire_shared_list(rsx_nr_d3d12* b)
{
    if (!b->shared_timeline)
        return 0;
    if (b->timeline_fault || b->timeline_leased)
        return b->timeline_fault ? -1 : 0;

    void* opaque_list = NULL;
    u64 generation = 0, recording_fence = 0, completed_fence = 0;
    if (b->timeline_acquire(
            b->timeline_user, &opaque_list, &generation,
            &recording_fence, &completed_fence) != 0)
        return -1; /* host movie ownership is a transient safe fallback */
    b->timeline_leased = 1;
    if (!opaque_list || !recording_fence) {
        nrb_release_timeline_lease(b);
        b->timeline_fault = 1;
        return -1;
    }
    b->stats.shared_timeline_acquires++;

    ID3D12GraphicsCommandList* const list =
        (ID3D12GraphicsCommandList*)opaque_list;
    if (list != b->shared_list) {
        ID3D12GraphicsCommandList1* list1 = NULL;
        if (SUCCEEDED(list->lpVtbl->QueryInterface(
                list, &IID_ID3D12GraphicsCommandList1, (void**)&list1))) {
            if (b->shared_list1)
                b->shared_list1->lpVtbl->Release(b->shared_list1);
            b->shared_list1 = list1;
        } else {
            if (b->shared_list1) {
                b->shared_list1->lpVtbl->Release(b->shared_list1);
                b->shared_list1 = NULL;
            }
            if (b->depth_bounds_supported) {
                nrb_release_timeline_lease(b);
                b->timeline_fault = 1;
                return -1;
            }
        }
        b->shared_list = list;
    }

    if (b->shared_generation && generation != b->shared_generation) {
        /* A generation may change only after the host synchronously retired
         * the fence promised for the old borrowed list.  Refuse a broker
         * which resets an allocator while native uploads are still live. */
        if (completed_fence < b->shared_recording_fence) {
            nrb_release_timeline_lease(b);
            b->timeline_fault = 1;
            return -1;
        }
        nrb_release_retired_textures(b);
        b->upload_used = 0;
        b->descriptor_tables_used = 0;
        b->list_open = 0;
        b->stats.shared_timeline_generations++;
    }
    b->shared_generation = generation;
    b->shared_recording_fence = recording_fence;
    b->shared_completed_fence = completed_fence;
    b->list = list;
    b->list1 = b->shared_list1;
    return 0;
}

static int nrb_open_list(rsx_nr_d3d12* b)
{
    if (b->shared_timeline) {
        if (nrb_acquire_shared_list(b))
            return -1;
        if (!b->list_open)
            b->list_open = 1;
        return 0;
    }
    if (b->list_open)
        return 0;
    if (FAILED(b->alloc->lpVtbl->Reset(b->alloc)) ||
        FAILED(b->list->lpVtbl->Reset(b->list, b->alloc, NULL)))
        return -1;
    b->list_open = 1;
    b->upload_used = 0;              /* previous exec was waited on        */
    b->descriptor_tables_used = 0;   /* shader-visible tables fence-retired */
    return 0;
}

static int nrb_exec_wait(rsx_nr_d3d12* b,
                         rsx_nr_d3d12_submit_cause cause,
                         u64 readback_bytes)
{
    if (!b->list_open)
        return 0;
    const u32 attributed_descriptors = b->descriptor_tables_used;
    const u32 attributed_upload = b->upload_used;
    const u64 attribution_start = nrb_submit_now(b);
    const u64 stall_start = nrb_stall_now(b);
    int result = 0;
    if (b->shared_timeline) {
        nrb_release_timeline_lease(b);
        if (b->timeline_flush(b->timeline_user) != 0) {
            b->timeline_fault = 1;
            result = -1;
            goto done;
        }
        b->list_open = 0;
        nrb_release_retired_textures(b);
        b->upload_used = 0;
        b->descriptor_tables_used = 0;
        b->stats.queue_submissions++;
        b->stats.shared_timeline_forced_submissions++;
        goto done;
    }
    b->list->lpVtbl->Close(b->list);
    ID3D12CommandList* lists[1] = { (ID3D12CommandList*)b->list };
    b->queue->lpVtbl->ExecuteCommandLists(b->queue, 1, lists);
    if (nrb_wait_idle(b)) {
        nrb_dump_device_oracle(
            b->dev, "command submission",
            b->dev->lpVtbl->GetDeviceRemovedReason(b->dev));
        b->list_open = 0;
        result = -1;
        goto done;
    }
    b->list_open = 0;
    nrb_release_retired_textures(b);
    b->stats.queue_submissions++;
done:
    nrb_submit_finish(b, cause, attribution_start, attributed_descriptors,
                      attributed_upload, readback_bytes);
    nrb_stall_finish(b, stall_start,
                     &b->stats.stall_fence_drain_count,
                     &b->stats.stall_fence_drain_ticks);
    return result;
}

static int nrb_ensure_descriptor_capacity(rsx_nr_d3d12* b)
{
    if (b->descriptor_tables_used >= NRB_DRAW_TABLES) {
        if (nrb_exec_wait(b, RSX_NR_D3D12_SUBMIT_DESCRIPTOR_RECYCLE, 0u) ||
            nrb_open_list(b))
            return -1;
    }
    return b->retired_texture_count + NRB_TEX_UNITS <=
        NRB_MAX_RETIRED_TEXTURES ? 0 : -1;
}

static void nrb_retire_texture(rsx_nr_d3d12* b, ID3D12Resource* resource)
{
    if (!resource)
        return;
    if (b->list_open &&
        b->retired_texture_count < NRB_MAX_RETIRED_TEXTURES) {
        b->retired_textures[b->retired_texture_count++] = resource;
        return;
    }
    resource->lpVtbl->Release(resource);
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

static u64 nrb_upload_aligned_size(u64 size)
{
    return (size + 255u) & ~255ull;
}

static void nrb_note_draw_upload_failure(rsx_nr_d3d12* b, u32 stage,
                                         u64 budget, u32 request,
                                         u32 batches)
{
    switch (stage) {
    case 1u: b->stats.unsup_upload_index++; break;
    case 2u: b->stats.unsup_upload_pull++; break;
    case 3u: b->stats.unsup_upload_vp++; break;
    case 4u: b->stats.unsup_upload_fp++; break;
    default: break;
    }
    if (!b->stats.first_upload_stage) {
        b->stats.first_upload_used = b->upload_used;
        b->stats.first_upload_budget = budget;
        b->stats.first_upload_request = request;
        b->stats.first_upload_batches = batches;
        b->stats.first_upload_stage = stage;
    }
}

/* A transactionally admitted section may contain thousands of draws.  The
 * upload arena is fence-gated, so retire only the already-recorded ordered
 * prefix before starting a draw that cannot fit.  At this point the current
 * draw has recorded no render, descriptor, or target side effects. */
static int nrb_ensure_draw_upload_capacity(rsx_nr_d3d12* b, u64 budget)
{
    if (budget > NRB_UPLOAD_BYTES)
        return -1;
    if ((u64)b->upload_used + budget <= NRB_UPLOAD_BYTES)
        return 0;
    if (nrb_exec_wait(b, RSX_NR_D3D12_SUBMIT_UPLOAD_ROLLOVER, 0u) ||
        nrb_open_list(b))
        return -1;
    b->stats.upload_rollovers++;
    return (u64)b->upload_used + budget <= NRB_UPLOAD_BYTES ? 0 : -1;
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

static DXGI_FORMAT nrb_color_dxgi(const rsx_nr_d3d12* b, u32 fmt)
{
    switch (fmt) {
    case 3: return DXGI_FORMAT_B5G6R5_UNORM;
    case 13: return DXGI_FORMAT_R32_FLOAT;
    case 4:
    case 5:
    case 8: return b->rgba_targets ? DXGI_FORMAT_R8G8B8A8_UNORM
                                   : DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

static int nrb_color_rtv_dxgi_ok(DXGI_FORMAT fmt)
{
    switch (fmt) {
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return 1;
    default:
        return 0;
    }
}

static nrb_rt* nrb_get_rt(rsx_nr_d3d12* b, u32 space, u32 offset, u32 fmt,
                          u32 w, u32 h, int create, int broker_create)
{
    const DXGI_FORMAT logical_dxgi = nrb_color_dxgi(b, fmt);
    DXGI_FORMAT color_dxgi = logical_dxgi;
    if (logical_dxgi == DXGI_FORMAT_UNKNOWN)
        return NULL;

    ID3D12Resource* borrowed = NULL;
    if (create && b->borrow_color) {
        u32 borrowed_format = 0;
        u32 borrowed_width = 0;
        u32 borrowed_height = 0;
        if (b->borrow_color(
                b->broker_user, space, offset, w, h, broker_create,
                (void**)&borrowed, &borrowed_format,
                &borrowed_width, &borrowed_height) != 0 || !borrowed ||
            !borrowed_width || !borrowed_height ||
            !nrb_color_rtv_dxgi_ok((DXGI_FORMAT)borrowed_format)) {
            if (borrowed)
                borrowed->lpVtbl->Release(borrowed);
            return NULL;
        }
        /* The live surface registry stores every logical Cell GCM color
         * format in a canonical RGBA8 resource.  The guest format remains
         * part of the render-target identity (and governs guest-memory
         * interpretation), but D3D12 PSO/RTV compatibility must follow the
         * actual shared resource.  Verify the broker's opaque format rather
         * than silently trusting a stale registry entry. */
        D3D12_RESOURCE_DESC borrowed_desc;
        borrowed->lpVtbl->GetDesc(borrowed, &borrowed_desc);
        if ((u32)borrowed_desc.Format != borrowed_format ||
            borrowed_desc.Width != borrowed_width ||
            borrowed_desc.Height != borrowed_height) {
            borrowed->lpVtbl->Release(borrowed);
            return NULL;
        }
        color_dxgi = (DXGI_FORMAT)borrowed_format;
        w = borrowed_width;
        h = borrowed_height;
    }

    for (u32 i = 0; i < NRB_MAX_RTS; i++) {
        nrb_rt* rt = &b->rts[i];
        const int exact_private_identity =
            rt->fmt == fmt && rt->w == w && rt->h == h;
        const int exact_live_identity =
            borrowed && rt->external;
        if (rt->live && rt->space == space && rt->offset == offset &&
            (exact_private_identity || exact_live_identity)) {
            if (borrowed) {
                if (borrowed == rt->tex) {
                    if (color_dxgi != rt->dxgi) {
                        borrowed->lpVtbl->Release(borrowed);
                        return NULL;
                    }
                    borrowed->lpVtbl->Release(borrowed);
                } else {
                    /* The live registry replaced the D3D resource while the
                     * guest identity stayed constant. Retire every native
                     * reference to the old generation before rebinding its
                     * stable descriptor slot. */
                    if (nrb_exec_wait(
                            b, RSX_NR_D3D12_SUBMIT_RESOURCE_REFRESH, 0u)) {
                        borrowed->lpVtbl->Release(borrowed);
                        return NULL;
                    }
                    if (rt->tex)
                        rt->tex->lpVtbl->Release(rt->tex);
                    rt->tex = borrowed;
                    rt->external = 1;
                    rt->dxgi = color_dxgi;
                    rt->color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
                    b->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
                        b->rtv_heap, &rtv);
                    rtv.ptr += (SIZE_T)rt->rtv_slot * b->rtv_size;
                    b->dev->lpVtbl->CreateRenderTargetView(
                        b->dev, rt->tex, NULL, rtv);
                    b->stats.rt_refreshes++;
                }
            }
            if (rt->external) {
                /* The established registry owns color identity by guest
                 * address. Keep one native state tracker for that resource,
                 * even when later texture/target declarations use a
                 * different logical format or view size. */
                rt->fmt = fmt;
                rt->w = w;
                rt->h = h;
            }
            return rt;
        }
    }
    if (!create) {
        if (borrowed)
            borrowed->lpVtbl->Release(borrowed);
        return NULL;
    }
    nrb_rt* rt = NULL;
    for (u32 i = 0; i < NRB_MAX_RTS; i++) {
        if (!b->rts[i].live) {
            rt = &b->rts[i];
            break;
        }
    }
    if (!rt || b->rtv_used >= NRB_MAX_RTS) {
        if (borrowed)
            borrowed->lpVtbl->Release(borrowed);
        return NULL;
    }

    if (borrowed) {
        rt->tex = borrowed;
        rt->external = 1;
    } else {
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = color_dxgi;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(b->dev->lpVtbl->CreateCommittedResource(
                b->dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
                &IID_ID3D12Resource, (void**)&rt->tex)))
            return NULL;
    }

    rt->space = space;
    rt->offset = offset;
    rt->fmt = fmt;
    rt->dxgi = color_dxgi;
    rt->w = w;
    rt->h = h;
    rt->rtv_slot = b->rtv_used++;
    rt->color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rt->live = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    b->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(b->rtv_heap, &rtv);
    rtv.ptr += (SIZE_T)rt->rtv_slot * b->rtv_size;
    b->dev->lpVtbl->CreateRenderTargetView(b->dev, rt->tex, NULL, rtv);

    b->stats.rt_builds++;
    return rt;
}

static D3D12_CPU_DESCRIPTOR_HANDLE nrb_rt_handle(rsx_nr_d3d12* b,
                                                 const nrb_rt* rt)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    b->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(b->rtv_heap,
                                                            &rtv);
    rtv.ptr += (SIZE_T)rt->rtv_slot * b->rtv_size;
    return rtv;
}

static void nrb_note_rt_write(rsx_nr_d3d12* b, nrb_rt* rt)
{
    /* Zero means "allocated/preflighted but never written".  A practical
     * process cannot wrap this counter, but preserve the invariant anyway. */
    if (++b->rt_write_serial == 0u)
        b->rt_write_serial = 1u;
    rt->last_write_serial = b->rt_write_serial;
    b->last_rt = rt;
}

static nrb_rt* nrb_latest_rt(rsx_nr_d3d12* b, u32 space, u32 offset,
                             u32 w, u32 h, int rgba_only)
{
    nrb_rt* selected = NULL;
    for (u32 i = 0; i < NRB_MAX_RTS; ++i) {
        nrb_rt* const candidate = &b->rts[i];
        if (!candidate->live || !candidate->last_write_serial ||
            candidate->space != space ||
            candidate->offset != offset || (w && candidate->w != w) ||
            (h && candidate->h != h) ||
            (rgba_only &&
             candidate->dxgi != DXGI_FORMAT_B8G8R8A8_UNORM &&
             candidate->dxgi != DXGI_FORMAT_R8G8B8A8_UNORM))
            continue;
        if (!selected || candidate->last_write_serial >
                             selected->last_write_serial)
            selected = candidate;
    }
    return selected;
}

static nrb_rt* nrb_display_rt(rsx_nr_d3d12* b, u32 buffer)
{
    if (buffer >= 8u || !b->displays[buffer].valid)
        return NULL;
    const nrb_display* const display = &b->displays[buffer];
    return nrb_latest_rt(b, display->location, display->offset,
                         display->width, display->height, 0);
}

/* Capture-observed pitch-linear color formats with an exact D3D12 RTV.
 * 3 = R5G6B5; 4/5 = X8R8G8B8; 8 = A8R8G8B8; 13 = F_X32. */
static int nrb_color_format_ok(const rsx_nr_d3d12* b, u32 fmt)
{
    return nrb_color_dxgi(b, fmt) != DXGI_FORMAT_UNKNOWN;
}

static nrb_rt* nrb_rt_from_state(rsx_nr_d3d12* b, const rsx_nir_pipeline* st,
                                 int create)
{
    const rsx_nir_surface* s = &st->surface;
    if (!nrb_color_format_ok(b, s->color_format))
        return NULL;
    u32 w = s->clip_w ? s->clip_w : 1280;
    u32 h = s->clip_h ? s->clip_h : 720;
    return nrb_get_rt(b, s->color_location[0], s->color_offset[0],
                      s->color_format, w, h, create, create);
}

static int nrb_depth_formats(u32 fmt, DXGI_FORMAT* resource,
                             DXGI_FORMAT* dsv, DXGI_FORMAT* srv)
{
    if (fmt == 1u) {
        *resource = DXGI_FORMAT_R16_TYPELESS;
        *dsv = DXGI_FORMAT_D16_UNORM;
        *srv = DXGI_FORMAT_R16_UNORM;
        return 0;
    }
    if (fmt == 2u) {
        /* Match the established live renderer's physical depth resource.
         * The guest contract remains logical D24S8, but world/shadow passes
         * have always rasterized through D32S8 and sampled its R32 snapshot.
         * Keeping the private full-native lane on D24 changed the producer
         * values before the otherwise-identical snapshot conversion. */
        *resource = DXGI_FORMAT_R32G8X24_TYPELESS;
        *dsv = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        *srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        return 0;
    }
    return -1;
}

static DXGI_FORMAT nrb_depth_dsv_dxgi(const rsx_nr_d3d12* b, u32 fmt)
{
    if (b->borrow_depth && fmt == 2u)
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    DXGI_FORMAT resource, dsv, srv;
    return nrb_depth_formats(fmt, &resource, &dsv, &srv) == 0
        ? dsv : DXGI_FORMAT_UNKNOWN;
}

static nrb_depth* nrb_get_depth(rsx_nr_d3d12* b, u32 space, u32 offset,
                                 u32 fmt, u32 w, u32 h, int create,
                                 int broker_create)
{
    DXGI_FORMAT resource_dxgi, dsv_dxgi, srv_dxgi;
    if (nrb_depth_formats(fmt, &resource_dxgi, &dsv_dxgi, &srv_dxgi) != 0)
        return NULL;

    ID3D12Resource* borrowed = NULL;
    ID3D12Resource* borrowed_sample = NULL;
    DXGI_FORMAT sample_srv_dxgi = DXGI_FORMAT_UNKNOWN;
    if (create && b->borrow_depth) {
        u32 rf = 0, df = 0, sf = 0, ssf = 0;
        int publication_required = 0;
        if (b->borrow_depth(
                b->broker_user, space, offset, fmt, w, h, broker_create,
                (void**)&borrowed, &rf, &df, &sf,
                (void**)&borrowed_sample, &ssf,
                &publication_required) != 0 || !borrowed) {
            if (borrowed)
                borrowed->lpVtbl->Release(borrowed);
            if (borrowed_sample)
                borrowed_sample->lpVtbl->Release(borrowed_sample);
            return NULL;
        }
        (void)publication_required; /* live wrapper completes this handoff */
        resource_dxgi = (DXGI_FORMAT)rf;
        dsv_dxgi = (DXGI_FORMAT)df;
        srv_dxgi = (DXGI_FORMAT)sf;
        sample_srv_dxgi = (DXGI_FORMAT)ssf;
        if (borrowed_sample &&
            sample_srv_dxgi != DXGI_FORMAT_R32_FLOAT &&
            sample_srv_dxgi != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS) {
            borrowed->lpVtbl->Release(borrowed);
            borrowed_sample->lpVtbl->Release(borrowed_sample);
            return NULL;
        }
    } else if (create && !broker_create) {
        /* A sampled-depth lookup must never invent a native depth target.
         * Without a broker there is no established GPU result to import. */
        return NULL;
    }

    for (u32 i = 0; i < NRB_MAX_DEPTHS; i++) {
        nrb_depth* depth = &b->depths[i];
        if (depth->live && depth->space == space && depth->offset == offset &&
            depth->fmt == fmt && depth->w == w && depth->h == h) {
            if (borrowed) {
                const int raw_changed = borrowed != depth->tex;
                const int sample_changed = borrowed_sample != depth->sample_tex;
                if (raw_changed || sample_changed) {
                    if (nrb_exec_wait(
                            b, RSX_NR_D3D12_SUBMIT_RESOURCE_REFRESH, 0u)) {
                        borrowed->lpVtbl->Release(borrowed);
                        if (borrowed_sample)
                            borrowed_sample->lpVtbl->Release(borrowed_sample);
                        return NULL;
                    }
                }
                if (raw_changed) {
                    if (depth->tex) depth->tex->lpVtbl->Release(depth->tex);
                    depth->tex = borrowed;
                } else {
                    borrowed->lpVtbl->Release(borrowed);
                }
                if (sample_changed) {
                    if (depth->sample_tex)
                        depth->sample_tex->lpVtbl->Release(depth->sample_tex);
                    depth->sample_tex = borrowed_sample;
                } else if (borrowed_sample) {
                    borrowed_sample->lpVtbl->Release(borrowed_sample);
                }
                if (raw_changed || sample_changed) {
                    depth->external = 1;
                    depth->resource_dxgi = resource_dxgi;
                    depth->dsv_dxgi = dsv_dxgi;
                    depth->srv_dxgi = srv_dxgi;
                    depth->sample_srv_dxgi = sample_srv_dxgi;
                    depth->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {0};
                    dsv.Format = dsv_dxgi;
                    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                    D3D12_CPU_DESCRIPTOR_HANDLE handle;
                    b->dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
                        b->dsv_heap, &handle);
                    handle.ptr += (SIZE_T)depth->dsv_slot * b->dsv_size;
                    b->dev->lpVtbl->CreateDepthStencilView(
                        b->dev, depth->tex, &dsv, handle);
                    b->stats.depth_refreshes++;
                }
            }
            return depth;
        }
    }
    if (!create) {
        if (borrowed)
            borrowed->lpVtbl->Release(borrowed);
        if (borrowed_sample)
            borrowed_sample->lpVtbl->Release(borrowed_sample);
        return NULL;
    }
    nrb_depth* depth = NULL;
    for (u32 i = 0; i < NRB_MAX_DEPTHS; i++) {
        if (!b->depths[i].live) {
            depth = &b->depths[i];
            break;
        }
    }
    if (!depth || b->dsv_used >= NRB_MAX_DEPTHS) {
        if (borrowed)
            borrowed->lpVtbl->Release(borrowed);
        if (borrowed_sample)
            borrowed_sample->lpVtbl->Release(borrowed_sample);
        return NULL;
    }

    if (borrowed) {
        depth->tex = borrowed;
        depth->sample_tex = borrowed_sample;
        depth->external = 1;
    } else {
        D3D12_HEAP_PROPERTIES heap = {0};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {0};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = resource_dxgi;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear = {0};
        clear.Format = dsv_dxgi;
        clear.DepthStencil.Depth = 1.0f;
        if (FAILED(b->dev->lpVtbl->CreateCommittedResource(
                b->dev, &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                &IID_ID3D12Resource, (void**)&depth->tex)))
            return NULL;
    }

    depth->space = space;
    depth->offset = offset;
    depth->fmt = fmt;
    depth->w = w;
    depth->h = h;
    depth->resource_dxgi = resource_dxgi;
    depth->dsv_dxgi = dsv_dxgi;
    depth->srv_dxgi = srv_dxgi;
    depth->sample_srv_dxgi = sample_srv_dxgi;
    depth->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    depth->dsv_slot = b->dsv_used++;
    depth->live = 1;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {0};
    dsv.Format = dsv_dxgi;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    b->dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(b->dsv_heap,
                                                            &handle);
    handle.ptr += (SIZE_T)depth->dsv_slot * b->dsv_size;
    b->dev->lpVtbl->CreateDepthStencilView(
        b->dev, depth->tex, &dsv, handle);
    b->stats.depth_builds++;
    return depth;
}

static nrb_depth* nrb_depth_from_state(rsx_nr_d3d12* b,
                                      const rsx_nir_pipeline* st,
                                      int create)
{
    const rsx_nir_surface* surface = &st->surface;
    const u32 w = surface->clip_w ? surface->clip_w : 1280u;
    const u32 h = surface->clip_h ? surface->clip_h : 720u;
    return nrb_get_depth(b, surface->zeta_location, surface->zeta_offset,
                         surface->depth_format, w, h, create, 1);
}

static void nrb_depth_transition(rsx_nr_d3d12* b, nrb_depth* depth,
                                 D3D12_RESOURCE_STATES state);

static D3D12_CPU_DESCRIPTOR_HANDLE nrb_depth_handle(
    rsx_nr_d3d12* b, const nrb_depth* depth)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    b->dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(b->dsv_heap,
                                                            &handle);
    handle.ptr += (SIZE_T)depth->dsv_slot * b->dsv_size;
    return handle;
}

static D3D12_CPU_DESCRIPTOR_HANDLE nrb_depth_snapshot_cpu_handle(
    rsx_nr_d3d12* b, const nrb_depth* depth, u32 destination)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    b->depth_snapshot_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
        b->depth_snapshot_heap, &handle);
    handle.ptr += (SIZE_T)(depth->dsv_slot * 2u + destination) *
                  b->texture_desc_size;
    return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE nrb_depth_snapshot_gpu_handle(
    rsx_nr_d3d12* b, const nrb_depth* depth, u32 destination)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle;
    b->depth_snapshot_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
        b->depth_snapshot_heap, &handle);
    handle.ptr += (UINT64)(depth->dsv_slot * 2u + destination) *
                  b->texture_desc_size;
    return handle;
}

/* Strict full-native frames own private logical-D24S8 resources backed by the
 * same D32S8 physical format as the established renderer. The RSX sampling
 * contract consumes a single-channel R32_FLOAT snapshot. Keep that
 * representation conversion entirely inside the native backend: one compute
 * dispatch after a depth-writing generation, then reuse the snapshot until
 * another clear/draw invalidates it. */
static int nrb_make_depth_snapshot_pipeline(rsx_nr_d3d12* b)
{
    static const char source[] =
        "Texture2D<float> depth_source : register(t0);\n"
        "RWTexture2D<float> depth_destination : register(u0);\n"
        "[numthreads(8, 8, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) {\n"
        "    uint width, height;\n"
        "    depth_destination.GetDimensions(width, height);\n"
        "    if (id.x < width && id.y < height)\n"
        "        depth_destination[id.xy] = "
        "depth_source.Load(int3(id.xy, 0));\n"
        "}\n";
    ID3DBlob* shader = NULL;
    ID3DBlob* errors = NULL;
    HRESULT hr = D3DCompile(
        source, sizeof(source) - 1u, "native_depth_snapshot", NULL, NULL,
        "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &shader, &errors);
    if (errors)
        errors->lpVtbl->Release(errors);
    if (FAILED(hr) || !shader)
        return -1;

    D3D12_DESCRIPTOR_RANGE ranges[2] = {0};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER parameters[2] = {0};
    for (u32 i = 0; i < 2u; ++i) {
        parameters[i].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable.NumDescriptorRanges = 1;
        parameters[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC description = {0};
    description.NumParameters = 2;
    description.pParameters = parameters;
    ID3DBlob* serialized = NULL;
    ID3DBlob* signature_errors = NULL;
    hr = D3D12SerializeRootSignature(
        &description, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &signature_errors);
    if (signature_errors)
        signature_errors->lpVtbl->Release(signature_errors);
    if (FAILED(hr) || !serialized) {
        shader->lpVtbl->Release(shader);
        return -1;
    }
    hr = b->dev->lpVtbl->CreateRootSignature(
        b->dev, 0, serialized->lpVtbl->GetBufferPointer(serialized),
        serialized->lpVtbl->GetBufferSize(serialized),
        &IID_ID3D12RootSignature, (void**)&b->depth_snapshot_rootsig);
    serialized->lpVtbl->Release(serialized);
    if (FAILED(hr)) {
        shader->lpVtbl->Release(shader);
        return -1;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {0};
    pipeline.pRootSignature = b->depth_snapshot_rootsig;
    pipeline.CS.pShaderBytecode = shader->lpVtbl->GetBufferPointer(shader);
    pipeline.CS.BytecodeLength = shader->lpVtbl->GetBufferSize(shader);
    hr = b->dev->lpVtbl->CreateComputePipelineState(
        b->dev, &pipeline, &IID_ID3D12PipelineState,
        (void**)&b->depth_snapshot_pso);
    shader->lpVtbl->Release(shader);
    return SUCCEEDED(hr) ? 0 : -1;
}

static int nrb_resolve_private_depth_sample(
    rsx_nr_d3d12* b, nrb_depth* depth)
{
    if (!b || !depth || depth->external || !b->depth_snapshot_pso ||
        !b->depth_snapshot_rootsig || !b->depth_snapshot_heap)
        return -1;
    if (depth->sample_valid)
        return 0;
    if (!depth->sample_tex) {
        D3D12_HEAP_PROPERTIES heap = {0};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {0};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = depth->w;
        desc.Height = depth->h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(b->dev->lpVtbl->CreateCommittedResource(
                b->dev, &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
                &IID_ID3D12Resource, (void**)&depth->sample_tex)))
            return -1;
        depth->sample_srv_dxgi = DXGI_FORMAT_R32_FLOAT;
        depth->sample_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {0};
        srv.Format = depth->srv_dxgi;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        b->dev->lpVtbl->CreateShaderResourceView(
            b->dev, depth->tex, &srv,
            nrb_depth_snapshot_cpu_handle(b, depth, 0u));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {0};
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        b->dev->lpVtbl->CreateUnorderedAccessView(
            b->dev, depth->sample_tex, NULL, &uav,
            nrb_depth_snapshot_cpu_handle(b, depth, 1u));
        b->stats.depth_snapshot_builds++;
    }
    if (nrb_open_list(b))
        return -1;
    nrb_depth_transition(
        b, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (depth->sample_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER barrier = {0};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = depth->sample_tex;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = depth->sample_state;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b->list->lpVtbl->ResourceBarrier(b->list, 1, &barrier);
        depth->sample_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    ID3D12DescriptorHeap* heaps[1] = { b->depth_snapshot_heap };
    b->list->lpVtbl->SetDescriptorHeaps(b->list, 1, heaps);
    b->list->lpVtbl->SetPipelineState(b->list, b->depth_snapshot_pso);
    b->list->lpVtbl->SetComputeRootSignature(
        b->list, b->depth_snapshot_rootsig);
    b->list->lpVtbl->SetComputeRootDescriptorTable(
        b->list, 0, nrb_depth_snapshot_gpu_handle(b, depth, 0u));
    b->list->lpVtbl->SetComputeRootDescriptorTable(
        b->list, 1, nrb_depth_snapshot_gpu_handle(b, depth, 1u));
    b->list->lpVtbl->Dispatch(
        b->list, (depth->w + 7u) / 8u, (depth->h + 7u) / 8u, 1u);

    D3D12_RESOURCE_BARRIER barriers[2] = {0};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = depth->sample_tex;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = depth->sample_tex;
    barriers[1].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b->list->lpVtbl->ResourceBarrier(b->list, 2, barriers);
    depth->sample_state = barriers[1].Transition.StateAfter;
    nrb_depth_transition(b, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    depth->sample_valid = 1;
    depth->resolve_generation = depth->write_generation;
    b->stats.depth_snapshot_resolves++;
    return 0;
}

/* ---- mirror session helper --------------------------------------------- */

/* Arm the embedder's sparse exact-write route for every 4 KiB host page
 * touched by a persistent guest resource. The page tracker itself remains
 * 1 KiB granular, but the VM-write hook rejects at host-page granularity.
 * A fixed bitset makes registration idempotent without allocation or a scan
 * on later draws. This is shared by vertex/index mirror spans and decoded
 * texture resources; previously only the former armed live notifications. */
static int nrb_watch_guest_span(rsx_nr_d3d12* b, u32 space,
                                u32 offset, u32 size)
{
    if (!size || space >= RSX_GUEST_NUM_SPACES ||
        (u64)offset + size > b->pages.space[space].size)
        return -1;
    if (!b->watch_page)
        return 0;
    const u32 first = offset >> 12;
    const u32 last = (u32)(((u64)offset + size - 1u) >> 12);
    if (last >= b->watched_host_page_count[space])
        return -1;
    for (u32 page = first; page <= last; ++page) {
        const u64 mask = 1ull << (page & 63u);
        if (b->watched_host_page_bits[space][page >> 6] & mask)
            continue;
        const u32 page_offset = page << 12;
        u32 page_size = 4096u;
        if ((u64)page_offset + page_size > b->pages.space[space].size)
            page_size = b->pages.space[space].size - page_offset;
        if (!b->guest_ptr(
                b->guest_user, space, page_offset, page_size) ||
            b->watch_page(
                b->watch_page_user, space, page_offset) != 0)
            return -1;
        b->watched_host_page_bits[space][page >> 6] |= mask;
        b->stats.watched_guest_pages[space]++;
    }
    return 0;
}

/* Permanently arm each exact mirror subpage a native shader can read.
 * Registrations are deliberately subpage-sized and deduplicated in a fixed
 * array: the first use pays a bounded loop, subsequent draws only test
 * existing handles. The host write router coalesces these to sparse 4 KiB
 * watched pages; generation tracking below remains 1 KiB granular. */
static int nrb_require_span(rsx_nr_d3d12* b, u32 space, u32 offset, u32 size)
{
    if (!size || space >= RSX_GUEST_NUM_SPACES ||
        (u64)offset + size > b->pages.space[space].size)
        return -1;
    const u32 first = offset >> RSX_GUEST_PAGE_SHIFT;
    const u32 last = (u32)(((u64)offset + size - 1u) >>
                           RSX_GUEST_PAGE_SHIFT);
    if (last >= b->resident_page_count[space])
        return -1;
    for (u32 page = first; page <= last; ++page) {
        if (b->resident_page[space][page])
            continue;
        const u32 page_offset = page << RSX_GUEST_PAGE_SHIFT;
        u32 page_size = RSX_GUEST_PAGE_SIZE;
        if ((u64)page_offset + page_size > b->pages.space[space].size)
            page_size = b->pages.space[space].size - page_offset;
        /* Refuse holes in a sparse IO map before publishing the watch. */
        if (!b->guest_ptr(b->guest_user, space, page_offset, page_size))
            return -1;
        const rsx_gpu_mirror_range range = rsx_gpu_mirror_register(
            b->mirror, space, page_offset, page_size);
        if (!range)
            return -1;
        if (nrb_watch_guest_span(b, space, page_offset, page_size) != 0) {
            rsx_gpu_mirror_unregister(b->mirror, range);
            b->stats.residency_failures++;
            return -1;
        }
        b->resident_page[space][page] = range;
        b->stats.resident_pages[space]++;
    }
    return 0;
}

static int nrb_span_current(const rsx_nr_d3d12* b, u32 space, u32 offset,
                            u32 size)
{
    if (!size || space >= RSX_GUEST_NUM_SPACES ||
        (u64)offset + size > b->pages.space[space].size)
        return 0;
    const u32 first = offset >> RSX_GUEST_PAGE_SHIFT;
    const u32 last = (u32)(((u64)offset + size - 1u) >>
                           RSX_GUEST_PAGE_SHIFT);
    for (u32 page = first; page <= last; ++page) {
        const rsx_gpu_mirror_range range =
            b->resident_page[space][page];
        if (!range || !rsx_gpu_mirror_range_current(b->mirror, range))
            return 0;
    }
    return 1;
}

static int nrb_add_required_span(nrb_required_span* spans, u32* count,
                                 u32 space, u64 offset, u64 size)
{
    if (!size || offset > UINT32_MAX || size > UINT32_MAX ||
        offset + size > 0x100000000ull ||
        *count >= NRB_MAX_REQUIRED_SPANS)
        return -1;
    spans[*count].space = space;
    spans[*count].offset = (u32)offset;
    spans[*count].size = (u32)size;
    (*count)++;
    return 0;
}

static int nrb_mirror_sync(rsx_nr_d3d12* b)
{
    const u64 completed = b->shared_timeline
        ? b->shared_completed_fence
        : b->fence->lpVtbl->GetCompletedValue(b->fence);
    const u64 recording = b->shared_timeline
        ? b->shared_recording_fence : b->fence_value + 1u;
    rsx_gpu_mirror_d3d12_retire(b->mirror_be, completed);
    if (rsx_gpu_mirror_d3d12_begin_fenced(
            b->mirror_be, b->list, recording) != 0)
        return -1;
    rsx_gpu_mirror_sync(b->mirror, 0);
    rsx_gpu_mirror_d3d12_end(b->mirror_be, recording);
    return 0;
}

static int nrb_required_spans_current(
    const rsx_nr_d3d12* b, const nrb_required_span* required, u32 count)
{
    for (u32 i = 0; i < count; ++i)
        if (!nrb_span_current(b, required[i].space, required[i].offset,
                              required[i].size))
            return 0;
    return 1;
}

/* Repair only shader-visible bytes when a producer is continuously updating
 * another part of the same coarse generation page.  The ordinary mirror sync
 * remains the fast persistent path.  An exact patch is ordered after that
 * page copy and before the draw on the same command list, but deliberately
 * does not acknowledge the page generation: a later draw that consumes a
 * different span must independently prove or upload its own bytes. */
static int nrb_patch_stale_required_spans(
    rsx_nr_d3d12* b, const nrb_required_span* required, u32 count)
{
    const u64 completed = b->shared_timeline
        ? b->shared_completed_fence
        : b->fence->lpVtbl->GetCompletedValue(b->fence);
    const u64 recording = b->shared_timeline
        ? b->shared_recording_fence : b->fence_value + 1u;
    rsx_gpu_mirror_d3d12_retire(b->mirror_be, completed);
    if (rsx_gpu_mirror_d3d12_begin_fenced(
            b->mirror_be, b->list, recording) != 0)
        return -1;

    int result = 0;
    for (u32 i = 0; i < count; ++i) {
        const nrb_required_span* const span = &required[i];
        if (nrb_span_current(b, span->space, span->offset, span->size))
            continue;
        const u8* const src = b->guest_ptr(
            b->guest_user, span->space, span->offset, span->size);
        if (!src) {
            result = -1;
            break;
        }
        result = rsx_gpu_mirror_d3d12_patch_exact(
            b->mirror_be, span->space, span->offset, src, span->size);
        if (result != 0) {
            b->stats.mirror_exact_patch_retries++;
            break;
        }
        b->stats.mirror_exact_patches++;
        b->stats.mirror_exact_patch_bytes += span->size;
    }
    rsx_gpu_mirror_d3d12_end(b->mirror_be, recording);
    return result;
}

/* A strict-native refusal is a bounded development failure. Preserve the
 * first exact residency dependency in fixed stats so a live-only failure can
 * be corrected from one run without per-draw logging or a profiler. */
static void nrb_note_first_residency_failure(
    rsx_nr_d3d12* b, u32 stage, u32 result,
    const nrb_required_span* required, u32 count)
{
    if (b->stats.first_residency_failure_stage)
        return;
    b->stats.first_residency_failure_stage = stage;
    b->stats.first_residency_failure_result = result;
    b->stats.first_residency_required_count = count;
    if (!required || !count)
        return;
    const nrb_required_span* selected = &required[0];
    for (u32 i = 0; i < count; ++i) {
        if (!nrb_span_current(b, required[i].space, required[i].offset,
                              required[i].size)) {
            selected = &required[i];
            break;
        }
    }
    b->stats.first_residency_space = selected->space;
    b->stats.first_residency_offset = selected->offset;
    b->stats.first_residency_size = selected->size;
    if (!selected->size || selected->space >= RSX_GUEST_NUM_SPACES)
        return;
    const u32 first = selected->offset >> RSX_GUEST_PAGE_SHIFT;
    const u32 last = (u32)(((u64)selected->offset + selected->size - 1u) >>
                           RSX_GUEST_PAGE_SHIFT);
    b->stats.first_residency_first_page = first;
    b->stats.first_residency_last_page = last;
    b->stats.first_residency_first_gen =
        rsx_guest_pages_page_gen(&b->pages, selected->space, first);
    b->stats.first_residency_last_gen =
        rsx_guest_pages_page_gen(&b->pages, selected->space, last);
}

/* Establish one immutable GPU-visible point for every byte this draw pulls.
 * A write published while a page is copied leaves its generation stale; in
 * that case append a repair copy before recording the draw. If the bounded
 * staging slice fills, submit only the already-owned ordered prefix, retire
 * it, and continue on a fresh slice. No draw is recorded until this returns
 * success, so a residency retry can neither duplicate nor partially render
 * the current action. */
static int nrb_stabilize_required_spans(
    rsx_nr_d3d12* b, const nrb_required_span* required, u32 count)
{
    if (!count)
        return 0;
    for (u32 attempt = 0; attempt < 8u; ++attempt) {
        if (nrb_open_list(b) || nrb_mirror_sync(b) != 0)
            return -1;
        if (nrb_required_spans_current(b, required, count))
            return 0;
        b->stats.mirror_resyncs++;

        /* A busy dynamic arena can republish one 1 KiB tracking page for the
         * next draw forever even though this draw's exact 52-byte (or similar)
         * span is stable.  Snapshot and patch only every still-stale required
         * span; successful patches are later commands in this same list, so
         * they supersede the coarse page copy without weakening cache state. */
        if (nrb_patch_stale_required_spans(b, required, count) == 0)
            return 0;

        /* One append retry repairs an ordinary generation race without a
         * submission. If it is still stale, the fixed slice is either full
         * or the producer is actively republishing the page; retire the
         * ordered prefix before retrying with fresh bounded storage. */
        if (attempt & 1u) {
            if (nrb_exec_wait(
                    b, RSX_NR_D3D12_SUBMIT_RESIDENCY_RETRY, 0u))
                return -1;
            b->stats.mirror_rollovers++;
        }
    }
    nrb_note_first_residency_failure(b, 3u, 0u, required, count);
    return -1;
}

static void nrb_rt_transition(rsx_nr_d3d12* b, nrb_rt* rt,
                              D3D12_RESOURCE_STATES state);
static void nrb_depth_transition(rsx_nr_d3d12* b, nrb_depth* depth,
                                 D3D12_RESOURCE_STATES state);

/* ---- exec ops ---------------------------------------------------------- */

static int nrb_clear(void* user, const rsx_nir_pipeline* st,
                     const rsx_nir_clear* c)
{
    rsx_nr_d3d12* b = user;
    /* CLEAR_SURFACE with no color/depth/stencil bits is an ordered no-op.
     * Do not require a valid target merely to consume it. */
    if (!(c->mask & 0xF3u)) {
        b->stats.clears++;
        return 0;
    }
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
    D3D12_CLEAR_FLAGS depth_flags = 0;
    if (c->mask & 0x01u)
        depth_flags |= D3D12_CLEAR_FLAG_DEPTH;
    if (c->mask & 0x02u)
        depth_flags |= D3D12_CLEAR_FLAG_STENCIL;
    nrb_depth* depth = NULL;
    if (depth_flags) {
        depth = nrb_depth_from_state(b, st, 1);
        if (!depth || ((depth_flags & D3D12_CLEAR_FLAG_STENCIL) &&
                       depth->fmt != 2u)) {
            b->stats.unsupported_clears++;
            return -1;
        }
    }
    if (nrb_open_list(b))
        return -1;
    nrb_rt_transition(b, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = nrb_rt_handle(b, rt);
    if (depth)
        nrb_depth_transition(b, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
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
    if (depth_flags) {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = nrb_depth_handle(b, depth);
        b->list->lpVtbl->ClearDepthStencilView(
            b->list, dsv, depth_flags,
            (float)c->depth_value / 16777215.0f,
            (UINT8)c->stencil_value, nrects, rects);
        depth->sample_valid = 0;
        depth->write_generation++;
    }
    /* A depth/stencil-only clear does not modify the color resource.  Do not
     * let a newly allocated, still-black color alias become the most recent
     * scanout merely because it shared the pipeline's surface declaration. */
    if (color_bits && b->scanout_provenance)
        rt->color_clear_writes++;
    if (color_bits)
        nrb_note_rt_write(b, rt);
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
    case 3: *topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; return 0;
    case 7:                            /* triangle fan -> host triangle IB */
    case 8:                            /* quads                            */
    case 9:                            /* quad strip                       */
    case 10:                           /* polygon -> fan                   */
            *topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; return 0;
    default: return -1;
    }
}

static int nrb_needs_expansion(u32 primitive)
{
    return primitive == 3u || primitive == 6u || primitive == 7u ||
           primitive == 8u ||
           primitive == 9u || primitive == 10u;
}

typedef struct nrb_fp_info {
    const u8* bytes;
    u32 size;
    u32 texture_mask;
    u32 unsupported;
    u32 first_unsupported_offset;
    u32 first_unsupported_opcode;
    u32 first_unsupported_reason;
    u64 structural_hash;
    rsx_fp_constant_block constants;
} nrb_fp_info;

/* Complete worst-case bump-arena footprint of one draw.  Constants are per
 * action, pull constants are per emitted D3D draw, and host-expanded indices
 * conservatively reserve three output indices per source element. */
static u64 nrb_draw_upload_budget(const rsx_nir_pipeline* st,
                                  const nrb_fp_info* fp,
                                  const rsx_nir_draw* draw,
                                  const u32* batches)
{
    const int expand = nrb_needs_expansion(draw->primitive);
    const int host_indices = expand ||
        (draw->indexed && st->index_binding.restart_enable);
    const int combine_strip = draw->primitive == 6u;
    const u32 output_batches = combine_strip ? 1u : draw->batch_count;
    u64 bytes = nrb_upload_aligned_size(
        sizeof(st->constants) + 12u * sizeof(float));
    const u32 fp_slots = fp->constants.count ? fp->constants.count : 1u;
    bytes += nrb_upload_aligned_size((u64)(fp_slots + 1u) * 16u);
    bytes += (u64)output_batches *
             nrb_upload_aligned_size(sizeof(rsx_vertex_pull_constants));

    if (host_indices) {
        if (combine_strip) {
            u64 source_count = 0;
            for (u32 bi = 0; bi < draw->batch_count; ++bi)
                source_count += batches[bi * 2u + 1u];
            if (source_count > 0x55555555u)
                return UINT64_MAX;
            bytes += nrb_upload_aligned_size(
                (source_count * 3u + 2u) * sizeof(u32));
        } else {
            for (u32 bi = 0; bi < draw->batch_count; ++bi) {
                const u64 count = batches[bi * 2u + 1u];
                const u64 worst = expand ? count * 3u + 2u : count;
                bytes += nrb_upload_aligned_size(worst * sizeof(u32));
            }
        }
    }
    return bytes;
}

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
    case 0x45: /* RET: predicated top-level early return */
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
#define NRB_VTEX_W16Z16Y16X16_FLOAT 0x9Au
#define NRB_VTEX_W32Z32Y32X32_FLOAT 0x9Bu
#define NRB_VTEX_X32_FLOAT          0x9Cu
#define NRB_VTEX_Y16X16_FLOAT       0x9Fu
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
        if (nrb_exec_wait(b, RSX_NR_D3D12_SUBMIT_UPLOAD_ROLLOVER, 0u) ||
            nrb_open_list(b))
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
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
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

    if (format == NRB_TEX_DEPTH24_D8) {
        if (texture->cubemap)
            return NULL;
        nrb_tex_level levels[14];
        float* depth[14] = {0};
        u32 built = 0, mw = texture->width, mh = texture->height, offset = 0;
        for (u32 mip = 0; mip < mip_count; mip++) {
            const u32 pitch = mip == 0u && linear && texture->pitch
                ? texture->pitch : mw * 4u;
            depth[built] = malloc((size_t)mw * mh * sizeof(float));
            if (!depth[built])
                goto fail_depth;
            const u8* level_source = source + offset;
            const u32 lw = nrb_log2_u32(mw), lh = nrb_log2_u32(mh);
            for (u32 y = 0; y < mh; y++) {
                for (u32 x = 0; x < mw; x++) {
                    const u8* pixel = linear
                        ? level_source + (size_t)y * pitch + (size_t)x * 4u
                        : level_source +
                            (size_t)nrb_morton_index(x, y, lw, lh) * 4u;
                    const u32 z24 = ((u32)pixel[0] << 16) |
                                    ((u32)pixel[1] << 8) | pixel[2];
                    depth[built][(size_t)y * mw + x] =
                        (float)z24 / 16777215.0f;
                }
            }
            levels[built].w = mw;
            levels[built].h = mh;
            levels[built].data = (const u8*)depth[built];
            levels[built].row_bytes = mw * sizeof(float);
            levels[built].rows = mh;
            built++;
            offset += pitch * mh;
            mw = mw > 1u ? mw >> 1 : 1u;
            mh = mh > 1u ? mh >> 1 : 1u;
        }
        {
            ID3D12Resource* resource = nrb_create_texture_levels(
                b, DXGI_FORMAT_R32_FLOAT, levels, mip_count, 0);
            for (u32 i = 0; i < built; i++)
                free(depth[i]);
            return resource;
        }
fail_depth:
        for (u32 i = 0; i < built; i++)
            free(depth[i]);
        return NULL;
    }

    u32 texel = 0;
    switch (format) {
    case NRB_TEX_B8: texel = 1; break;
    case NRB_TEX_A1R5G5B5:
    case NRB_TEX_A4R4G4B4:
    case NRB_TEX_R5G6B5:
    case NRB_TEX_G8B8: texel = 2; break;
    case NRB_TEX_A8R8G8B8: texel = 4; break;
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

typedef struct nrb_vertex_texture_format {
    DXGI_FORMAT dxgi;
    u32 bytes_per_texel;
    u32 component_bytes;
} nrb_vertex_texture_format;

static int nrb_vertex_texture_format_of(
    const rsx_nir_texture* texture, nrb_vertex_texture_format* out)
{
    if (!texture || texture->dimension != 2u || texture->cubemap)
        return -1;
    nrb_vertex_texture_format format = {DXGI_FORMAT_UNKNOWN, 0, 0};
    switch (texture->format & NRB_TEX_BASE_MASK) {
    case NRB_VTEX_W16Z16Y16X16_FLOAT:
        format.dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT;
        format.bytes_per_texel = 8u;
        format.component_bytes = 2u;
        break;
    case NRB_VTEX_W32Z32Y32X32_FLOAT:
        format.dxgi = DXGI_FORMAT_R32G32B32A32_FLOAT;
        format.bytes_per_texel = 16u;
        format.component_bytes = 4u;
        break;
    case NRB_VTEX_X32_FLOAT:
        format.dxgi = DXGI_FORMAT_R32_FLOAT;
        format.bytes_per_texel = 4u;
        format.component_bytes = 4u;
        break;
    case NRB_VTEX_Y16X16_FLOAT:
        format.dxgi = DXGI_FORMAT_R16G16_FLOAT;
        format.bytes_per_texel = 4u;
        format.component_bytes = 2u;
        break;
    default:
        return -1;
    }
    if (out)
        *out = format;
    return 0;
}

static u32 nrb_vertex_texture_span(
    const rsx_nir_texture* texture, nrb_vertex_texture_format* format_out,
    u32* pitch_out)
{
    nrb_vertex_texture_format format;
    if (nrb_vertex_texture_format_of(texture, &format) != 0 ||
        !texture->width || !texture->height || texture->width > 4096u ||
        texture->height > 4096u || texture->mipmaps > 1u)
        return 0;
    const u64 row_bytes = (u64)texture->width * format.bytes_per_texel;
    const u64 pitch = texture->pitch ? texture->pitch : row_bytes;
    const u64 span = pitch * texture->height;
    if (row_bytes > UINT32_MAX || pitch < row_bytes ||
        pitch > UINT32_MAX || !span || span > UINT32_MAX)
        return 0;
    if (format_out)
        *format_out = format;
    if (pitch_out)
        *pitch_out = (u32)pitch;
    return (u32)span;
}

static u64 nrb_hana_hash(const void* data, size_t size)
{
    const u8* const bytes = data;
    u64 hash = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

static u64 nrb_hana_fp_hash(const void* data, size_t size)
{
    static const u32 tag = 0x31435046u; /* producer-contract "FPC1" */
    const u8* const bytes = data;
    u64 hash = 1469598103934665603ull;
    const u8* const tag_bytes = (const u8*)&tag;
    for (size_t i = 0; i < sizeof(tag); ++i) {
        hash ^= tag_bytes[i];
        hash *= 1099511628211ull;
    }
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static ID3D12Resource* nrb_decode_guest_vertex_texture(
    rsx_nr_d3d12* b, const rsx_nir_texture* texture)
{
    nrb_vertex_texture_format format;
    u32 pitch = 0;
    const u32 span = nrb_vertex_texture_span(texture, &format, &pitch);
    const u8* source = span ? b->guest_ptr(
        b->guest_user, texture->location, texture->offset, span) : NULL;
    if (!source)
        return NULL;
    const u32 row_bytes = texture->width * format.bytes_per_texel;
    const u64 staging_bytes = (u64)row_bytes * texture->height;
    if (staging_bytes > SIZE_MAX)
        return NULL;
    u8* staging = malloc((size_t)staging_bytes);
    if (!staging)
        return NULL;
    for (u32 y = 0; y < texture->height; ++y) {
        const u8* source_row = source + (size_t)y * pitch;
        u8* destination_row = staging + (size_t)y * row_bytes;
        const u32 components = row_bytes / format.component_bytes;
        for (u32 component = 0; component < components; ++component) {
            const u8* s = source_row +
                (size_t)component * format.component_bytes;
            u8* d = destination_row +
                (size_t)component * format.component_bytes;
            for (u32 byte = 0; byte < format.component_bytes; ++byte)
                d[byte] = s[format.component_bytes - 1u - byte];
        }
    }
    const nrb_tex_level level = {
        texture->width, texture->height, staging, row_bytes, texture->height
    };
    ID3D12Resource* resource = nrb_create_texture_levels(
        b, format.dxgi, &level, 1u, 0);
    free(staging);
    return resource;
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
    rsx_nr_d3d12* b, u32 slot)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle;
    b->texture_gpu_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
        b->texture_gpu_heap, &handle);
    handle.ptr += (UINT64)slot * b->texture_desc_size;
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
    rsx_nr_d3d12* b, u32 slot)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle;
    b->sampler_gpu_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
        b->sampler_gpu_heap, &handle);
    handle.ptr += (UINT64)slot * b->sampler_desc_size;
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
    const int depth24 = format == NRB_TEX_DEPTH24_D8;
    desc.Format = depth24 ? DXGI_FORMAT_R32_FLOAT : compressed
        ? (format == NRB_TEX_DXT1 ? DXGI_FORMAT_BC1_UNORM
           : format == NRB_TEX_DXT23 ? DXGI_FORMAT_BC2_UNORM
                                     : DXGI_FORMAT_BC3_UNORM)
        : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Shader4ComponentMapping = depth24
        ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
              D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1)
        :
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
    if (!span || nrb_watch_guest_span(
            b, texture->location, texture->offset, span) != 0)
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
        nrb_retire_texture(b, old);
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

static void nrb_write_vertex_texture_srv(
    rsx_nr_d3d12* b, u32 slot, const rsx_nir_texture* texture,
    ID3D12Resource* resource)
{
    nrb_vertex_texture_format format;
    if (nrb_vertex_texture_format_of(texture, &format) != 0)
        return;
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
    desc.Format = format.dxgi;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MostDetailedMip = 0;
    desc.Texture2D.MipLevels = 1;
    desc.Texture2D.ResourceMinLODClamp = 0.0f;
    desc.Texture2D.PlaneSlice = 0;
    b->dev->lpVtbl->CreateShaderResourceView(
        b->dev, resource, &desc, nrb_texture_cpu_handle(b, slot));
}

static int nrb_resolve_guest_vertex_texture(
    rsx_nr_d3d12* b, const rsx_nir_texture* texture, u32* slot_out)
{
    const u32 span = nrb_vertex_texture_span(texture, NULL, NULL);
    if (!span || nrb_watch_guest_span(
            b, texture->location, texture->offset, span) != 0)
        return -1;
    rsx_nr_res_key key = {0};
    key.kind = 2;                    /* distinct from fragment textures   */
    key.space = texture->location;
    key.offset = texture->offset;
    key.size = span;
    key.fmt = nrb_texture_key(texture);
    rsx_nr_res* entry = rsx_nr_res_lookup(&b->textures, &key);
    if (entry && rsx_nr_res_current(&b->textures, entry)) {
        b->stats.texture_hits++;
        *slot_out = (u32)(entry - b->textures.slots);
        if (b->hana_input_oracle)
            b->hana_vtex_resolution[*slot_out] = 1u;
        return 0;
    }

    ID3D12Resource* resource = nrb_decode_guest_vertex_texture(b, texture);
    if (!resource)
        return -1;
    if (entry) {
        ID3D12Resource* old = (ID3D12Resource*)(uintptr_t)entry->backend_id;
        entry->backend_id = (u64)(uintptr_t)resource;
        rsx_nr_res_revalidate(&b->textures, entry);
        nrb_retire_texture(b, old);
        b->stats.texture_refreshes++;
        if (b->hana_input_oracle)
            b->hana_vtex_resolution[entry - b->textures.slots] = 3u;
    } else {
        entry = rsx_nr_res_insert(
            &b->textures, &key, (u64)(uintptr_t)resource);
        if (!entry) {
            resource->lpVtbl->Release(resource);
            return -1;
        }
        b->stats.texture_builds++;
        if (b->hana_input_oracle)
            b->hana_vtex_resolution[entry - b->textures.slots] = 2u;
    }
    *slot_out = (u32)(entry - b->textures.slots);
    if (b->hana_input_oracle) {
        const u8* const source = b->guest_ptr(
            b->guest_user, texture->location, texture->offset, span);
        b->hana_vtex_uploaded_hash[*slot_out] = source
            ? nrb_hana_hash(source, span) : 0u;
    }
    nrb_write_vertex_texture_srv(b, *slot_out, texture, resource);
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
    /* Match the established renderer's sampler contract exactly.  It
     * zero-initializes D3D12_SAMPLER_DESC and never copies the guest border
     * register.  Honoring Yakuza's opaque-white value here made fullscreen
     * BORDER-sampled post effects feather white in from every image edge;
     * it also changed out-of-range character-shadow depth comparisons. */
    return desc;
}

static void nrb_note_first_texture_failure(
    rsx_nr_d3d12* b, u32 stage, u32 unit, int result, u32 texture_mask,
    const rsx_nir_texture* texture)
{
    if (!b || b->stats.first_texture_failure_stage)
        return;
    b->stats.first_texture_failure_stage = stage;
    b->stats.first_texture_failure_unit = unit;
    b->stats.first_texture_failure_result = result;
    b->stats.first_texture_failure_mask = texture_mask;
    b->stats.first_texture_cache_count = b->textures.count;
    b->stats.first_texture_cache_table_full = b->textures.stats.table_full;
    b->stats.first_texture_cache_arena_exhausted =
        b->textures.stats.arena_exhausted;
    if (!texture)
        return;
    b->stats.first_texture_failure_location = texture->location;
    b->stats.first_texture_failure_offset = texture->offset;
    b->stats.first_texture_failure_format = texture->format;
    b->stats.first_texture_failure_width = texture->width;
    b->stats.first_texture_failure_height = texture->height;
    b->stats.first_texture_failure_pitch = texture->pitch;
    b->stats.first_texture_failure_mipmaps = texture->mipmaps;
    b->stats.first_texture_failure_cubemap = texture->cubemap;
}

static nrb_rt* nrb_texture_rt_alias(rsx_nr_d3d12* b,
                                    const rsx_nir_texture* texture,
                                    const nrb_rt* draw_rt, u32 unit)
{
    const u32 format = texture->format & NRB_TEX_BASE_MASK &
                       ~NRB_TEX_UNNORM;
    const int live_identity = b->borrow_color != NULL;
    if (texture->cubemap || texture->dimension != 2u ||
        (!live_identity && (texture->mipmaps > 1u ||
         (format != NRB_TEX_A8R8G8B8 && format != NRB_TEX_R5G6B5))))
        return NULL;
    nrb_rt* rt = NULL;
    for (u32 i = 0; i < NRB_MAX_RTS; i++) {
        nrb_rt* const candidate = &b->rts[i];
        if (!candidate->live || candidate->space != texture->location ||
            candidate->offset != texture->offset)
            continue;
        /* Live RSX surfaces are persistent GPU memory identities. The title
         * routinely samples one through a differently declared texture view;
         * the established renderer resolves such aliases by address, not by
         * repeating the producer's dimensions. Offline/private resources keep
         * the stricter byte-layout equivalence rule. */
        if (!live_identity &&
            (candidate->w != texture->width ||
             candidate->h != texture->height))
            continue;
        if (!live_identity &&
            ((format == NRB_TEX_R5G6B5 && candidate->fmt != 3u) ||
            (format == NRB_TEX_A8R8G8B8 &&
             candidate->dxgi != nrb_color_dxgi(b, 8u))))
            continue;
        /* Private strict-native rendering can retain multiple logical RSX
         * surfaces at one guest address.  Sampling that address must observe
         * the most recent GPU writer, just like scanout does; table order is
         * allocation history and is not an ownership rule.  The previous
         * first-match lookup could therefore rebind a stale, still-black
         * format sibling after a newer pass had populated the address. */
        if (!rt || candidate->last_write_serial > rt->last_write_serial)
            rt = candidate;
    }
    if (!rt && live_identity) {
        /* A complete producer pass may have stayed wholly established. Its
         * first native appearance is then a sampled color dependency, not a
         * native target. Import the existing address identity lookup-only;
         * the broker is forbidden from creating or resizing it. */
        const u32 surface_format =
            format == NRB_TEX_R5G6B5 ? 3u : 8u;
        rt = nrb_get_rt(
            b, texture->location, texture->offset, surface_format,
            texture->width, texture->height, 1, 0);
    }
    if (!rt)
        return NULL;
    if (rt == draw_rt)
        return rt;                  /* input/output alias: must refuse    */
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
    desc.Format = rt->dxgi;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    /* The live renderer's SRV_SURFACE table exposes rendered targets in
     * their native component order; texture remap applies when decoding
     * guest texels, not when rebinding that already-rendered GPU image.
     * Reapplying it here turned post-process RGB into forced/alpha
     * channels (dark scene with an exaggerated bright border). */
    desc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    b->dev->lpVtbl->CreateShaderResourceView(
        b->dev, rt->tex, &desc,
        nrb_texture_cpu_handle(b, NRB_TEX_CAP + 1u + unit));
    return rt;
}

static nrb_depth* nrb_texture_depth_alias(
    rsx_nr_d3d12* b, const rsx_nir_texture* texture,
    const nrb_depth* draw_depth, u32 unit)
{
    (void)unit;
    const u32 format = texture->format & NRB_TEX_BASE_MASK &
                       ~NRB_TEX_UNNORM;
    if (format != NRB_TEX_DEPTH24_D8 || texture->cubemap ||
        texture->dimension != 2u || texture->mipmaps > 1u)
        return NULL;
    for (u32 i = 0; i < NRB_MAX_DEPTHS; i++) {
        nrb_depth* depth = &b->depths[i];
        if (!depth->live || depth->space != texture->location ||
            depth->offset != texture->offset || depth->w != texture->width ||
            depth->h != texture->height)
            continue;
        if (depth == draw_depth)
            return depth;           /* active DSV/SRV alias: refuse       */
        return depth;
    }
    /* The producing pass may have remained wholly on the established
     * renderer. Import only an already-existing exact depth identity; the
     * lookup-only broker call is forbidden from creating/clearing a target,
     * so unsupported content still follows the proven guest fallback without
     * changing later producer state. */
    if (b->borrow_depth)
        return nrb_get_depth(
            b, texture->location, texture->offset, 2u,
            texture->width, texture->height, 1, 0);
    return NULL;
}

int rsx_nr_d3d12_validate_depth_sample_alias(
    rsx_nr_d3d12* b, const rsx_nir_texture* texture)
{
    if (!b || !texture)
        return -1;
    nrb_depth* const depth = nrb_texture_depth_alias(b, texture, NULL, 0u);
    if (!depth)
        return -1;
    if (!depth->external)
        return 0;
    return depth->sample_tex && b->resolve_depth_sample ? 0 : -1;
}

static void nrb_write_depth_sample_srv(rsx_nr_d3d12* b,
                                       ID3D12Resource* resource,
                                       DXGI_FORMAT format, u32 unit)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
    desc.Format = format;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping =
        D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    desc.Texture2D.MipLevels = 1;
    b->dev->lpVtbl->CreateShaderResourceView(
        b->dev, resource, &desc,
        nrb_texture_cpu_handle(b, NRB_TEX_CAP + 1u + unit));
}

static int nrb_prepare_textures(rsx_nr_d3d12* b,
                                const rsx_nir_pipeline* st,
                                u32 texture_mask, u32 vtex_mask,
                                nrb_rt* draw_rt,
                                nrb_depth* draw_depth,
                                nrb_rt* aliases[NRB_TEX_UNITS],
                                nrb_depth* depth_aliases[NRB_TEX_UNITS],
                                nrb_rt* vtex_aliases[NRB_VTEX_UNITS],
                                nrb_depth* vtex_depth_aliases[NRB_VTEX_UNITS],
                                u32* cube_mask_out, u32* table_index_out)
{
    const u32 null_slot = NRB_TEX_CAP;
    u32 cube_mask = 0;
    u32 source_slots[NRB_TEX_UNITS];
    u32 vtex_source_slots[NRB_VTEX_UNITS];
    D3D12_SAMPLER_DESC samplers[NRB_TEX_UNITS];
    nrb_descriptor_table_key table_key;
    memset(&table_key, 0, sizeof(table_key));
    table_key.texture_mask = texture_mask;
    table_key.vtex_mask = vtex_mask;
    for (u32 unit = 0; unit < NRB_TEX_UNITS; unit++) {
        const rsx_nir_texture* texture = &st->textures[unit];
        u32 source_slot = null_slot;
        u64 alias_resource = 0;
        aliases[unit] = NULL;
        depth_aliases[unit] = NULL;
        if (texture_mask & (1u << unit)) {
            if (!texture->enabled) {
                nrb_note_first_texture_failure(
                    b, 1u, unit, -1, texture_mask, texture);
                return -1;
            }
            if (texture->cubemap)
                cube_mask |= 1u << unit;
            aliases[unit] = nrb_texture_rt_alias(
                b, texture, draw_rt, unit);
            if (aliases[unit] == draw_rt) {
                nrb_note_first_texture_failure(
                    b, 2u, unit, -1, texture_mask, texture);
                return -1;
            }
            if (aliases[unit]) {
                nrb_rt_transition(b, aliases[unit],
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                source_slot = NRB_TEX_CAP + 1u + unit;
                alias_resource = (u64)(uintptr_t)aliases[unit]->tex;
                b->stats.rt_alias_binds++;
            } else {
                nrb_depth* const depth_alias = nrb_texture_depth_alias(
                    b, texture, draw_depth, unit);
                if (depth_alias && depth_alias == draw_depth) {
                    nrb_note_first_texture_failure(
                        b, 3u, unit, -1, texture_mask, texture);
                    return -1;
                }
                if (depth_alias && !depth_alias->external) {
                    if (nrb_resolve_private_depth_sample(
                            b, depth_alias) != 0) {
                        nrb_note_first_texture_failure(
                            b, 4u, unit, -1, texture_mask, texture);
                        return -1;
                    }
                    nrb_write_depth_sample_srv(
                        b, depth_alias->sample_tex,
                        depth_alias->sample_srv_dxgi, unit);
                    source_slot = NRB_TEX_CAP + 1u + unit;
                    alias_resource =
                        (u64)(uintptr_t)depth_alias->sample_tex;
                    b->stats.rt_alias_binds++;
                    depth_aliases[unit] = depth_alias;
                } else if (depth_alias && depth_alias->sample_tex &&
                           b->resolve_depth_sample &&
                           b->resolve_depth_sample(
                               b->broker_user, depth_alias->space,
                               depth_alias->offset, texture->width,
                               texture->height) == 0) {
                    /* Live D32S8 is not the guest's sampled depth image. The
                     * established renderer resolves it to R32_FLOAT first;
                     * bind that exact snapshot and leave the source DSV in
                     * DEPTH_WRITE state. */
                    nrb_write_depth_sample_srv(
                        b, depth_alias->sample_tex,
                        depth_alias->sample_srv_dxgi, unit);
                    source_slot = NRB_TEX_CAP + 1u + unit;
                    alias_resource =
                        (u64)(uintptr_t)depth_alias->sample_tex;
                    b->stats.rt_alias_binds++;
                    depth_aliases[unit] = depth_alias;
                }
            }
            if (source_slot == null_slot && !aliases[unit] &&
                !depth_aliases[unit] && !alias_resource &&
                nrb_resolve_guest_texture(b, texture, &source_slot) != 0) {
                nrb_note_first_texture_failure(
                    b, 5u, unit, -1, texture_mask, texture);
                return -1;
            }
        }
        source_slots[unit] = source_slot;
        samplers[unit] = nrb_sampler(texture);
        table_key.sampler[unit] = samplers[unit];
        table_key.view[unit] = nrb_texture_key(texture);
        if (alias_resource)
            table_key.resource[unit] = alias_resource;
        else if (source_slot < b->textures.cap &&
                 b->textures.slots[source_slot].live)
            table_key.resource[unit] =
                b->textures.slots[source_slot].backend_id;
    }
    for (u32 unit = 0; unit < NRB_VTEX_UNITS; ++unit) {
        const rsx_nir_texture* texture = &st->vertex_textures[unit];
        u32 source_slot = null_slot;
        vtex_aliases[unit] = NULL;
        vtex_depth_aliases[unit] = NULL;
        if (vtex_mask & (1u << unit)) {
            if (!texture->enabled || nrb_resolve_guest_vertex_texture(
                    b, texture, &source_slot) != 0) {
                nrb_note_first_texture_failure(
                    b, 6u, NRB_TEX_UNITS + unit, -1,
                    vtex_mask, texture);
                return -1;
            }
        }
        vtex_source_slots[unit] = source_slot;
        table_key.view[NRB_TEX_UNITS + unit] = nrb_texture_key(texture);
        if (source_slot < b->textures.cap &&
            b->textures.slots[source_slot].live)
            table_key.resource[NRB_TEX_UNITS + unit] =
                b->textures.slots[source_slot].backend_id;
    }
    /* Descriptor tables are immutable for the lifetime of an open command
     * list. Exact resource/view/sampler reuse is therefore safe and avoids
     * consuming one of D3D12's 128 sampler tables for every draw. A refreshed
     * texture receives a different resource pointer while the old resource is
     * fence-retired, so it cannot alias an earlier key in this generation. */
    for (u32 i = 0; i < b->descriptor_tables_used; ++i) {
        if (memcmp(&b->descriptor_table_keys[i], &table_key,
                   sizeof(table_key)) == 0) {
            *cube_mask_out = cube_mask;
            *table_index_out = i;
            b->stats.descriptor_table_hits++;
            return 0;
        }
    }
    /* Resolve/upload may have submitted a full upload ring and therefore
     * reset this fence generation. Allocate the immutable descriptor table
     * only after all such work is complete. Each recorded draw receives a
     * distinct table; descriptors are never rewritten before its fence. */
    if (nrb_ensure_descriptor_capacity(b) != 0) {
        nrb_note_first_texture_failure(
            b, 7u, ~0u, -1, texture_mask, NULL);
        return -1;
    }
    const u32 table_index = b->descriptor_tables_used++;
    b->descriptor_table_keys[table_index] = table_key;
    b->stats.descriptor_table_builds++;
    const u32 srv_base = table_index * NRB_SRV_TABLE_STRIDE;
    const u32 sampler_base = table_index * NRB_SAMPLER_TABLE_STRIDE;
    for (u32 unit = 0; unit < NRB_TEX_UNITS; ++unit) {
        b->dev->lpVtbl->CopyDescriptorsSimple(
            b->dev, 1, nrb_texture_gpu_cpu_handle(b, srv_base + unit),
            nrb_texture_cpu_handle(b, source_slots[unit]),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        b->dev->lpVtbl->CreateSampler(
            b->dev, &samplers[unit],
            nrb_sampler_cpu_handle(b, sampler_base + unit));
    }
    for (u32 unit = 0; unit < NRB_VTEX_UNITS; ++unit)
        b->dev->lpVtbl->CopyDescriptorsSimple(
            b->dev, 1,
            nrb_texture_gpu_cpu_handle(
                b, srv_base + NRB_TEX_UNITS + unit),
            nrb_texture_cpu_handle(b, vtex_source_slots[unit]),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    *cube_mask_out = cube_mask;
    *table_index_out = table_index;
    return 0;
}

static u32 nrb_hana_vp_txl_mask(const u32* words, u32 word_count)
{
    u32 mask = 0;
    if (!words)
        return 0;
    for (u32 word = 0; word + 3u < word_count; word += 4u) {
        const u32 d1 = words[word + 1u];
        const u32 d2 = words[word + 2u];
        if (((d1 >> 22) & 0x1Fu) == 25u)
            mask |= 1u << ((d2 >> 8) & 3u);
    }
    return mask;
}

static int nrb_hana_input_match(const rsx_nir_pipeline* st,
                                const nrb_fp_info* fp, u64 fp_hash)
{
    if (!st || !fp || fp->size != 80u ||
        fp_hash != 0x5A76C48CAB4401BBull ||
        st->fragment_program.location != 1u ||
        st->fragment_program.offset != 0x01143600u ||
        st->surface.color_location[0] != 0u ||
        st->surface.color_offset[0] != 0x01800000u)
        return 0;
    const rsx_nir_texture* const a = &st->textures[14];
    const rsx_nir_texture* const z = &st->textures[15];
    return a->enabled && z->enabled && a->location == 0u && z->location == 0u &&
           a->offset == 0x02310000u && z->offset == 0x02910000u &&
           a->width == 1024u && a->height == 1024u &&
           z->width == 1024u && z->height == 1024u;
}

static void nrb_hana_depth_capture(
    rsx_nr_d3d12* b, nrb_hana_input_sample* sample, u32 sample_slot,
    const rsx_nir_pipeline* st, nrb_depth* const* texture_depth_aliases)
{
    static const u32 texture_units[NRB_HANA_DEPTH_UNITS] = {14u, 15u};
    for (u32 i = 0; i < NRB_HANA_DEPTH_UNITS; ++i) {
        const u32 unit = texture_units[i];
        const rsx_nir_texture* const texture = &st->textures[unit];
        nrb_depth* const depth = texture_depth_aliases[unit];
        nrb_hana_input_depth* const out = &sample->depth[i];
        out->space = texture->location;
        out->offset = texture->offset;
        out->texture_format = texture->format;
        out->texture_wrap = texture->wrap;
        out->texture_remap = texture->remap;
        out->texture_filter = texture->filter;
        out->texture_control = texture->control0;
        out->texture_border = texture->border_color;
        const D3D12_SAMPLER_DESC sampler = nrb_sampler(texture);
        out->sampler_filter = sampler.Filter;
        out->sampler_address_u = sampler.AddressU;
        out->sampler_address_v = sampler.AddressV;
        out->sampler_address_w = sampler.AddressW;
        out->sampler_comparison = sampler.ComparisonFunc;
        if (!depth)
            continue;
        out->resource_identity = (u64)(UINT_PTR)depth->tex;
        out->sample_identity = (u64)(UINT_PTR)depth->sample_tex;
        out->write_generation = depth->write_generation;
        out->resolve_generation = depth->resolve_generation;
        out->command_generation = b->shared_timeline
            ? b->shared_generation : b->stats.queue_submissions + 1u;
        out->recording_fence = b->shared_timeline
            ? b->shared_recording_fence : b->fence_value + 1u;
        out->completed_fence = b->shared_timeline
            ? b->shared_completed_fence
            : b->fence->lpVtbl->GetCompletedValue(b->fence);
        out->resource_state = (u32)depth->state;
        out->sample_state = (u32)depth->sample_state;
        out->srv_format = (u32)depth->sample_srv_dxgi;
        out->external = depth->external != 0;
        out->sample_valid = depth->sample_valid != 0;
        if (!b->hana_depth_readback || !depth->sample_tex ||
            !depth->sample_valid || depth->external ||
            depth->sample_srv_dxgi != DXGI_FORMAT_R32_FLOAT) {
            b->hana_depth_copy_failures++;
            continue;
        }

        const D3D12_RESOURCE_STATES before = depth->sample_state;
        if (before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER barrier = {0};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = depth->sample_tex;
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_COPY_SOURCE;
            b->list->lpVtbl->ResourceBarrier(b->list, 1, &barrier);
        }
        for (u32 gy = 0; gy < NRB_HANA_DEPTH_GRID; ++gy) {
            const u32 y =
                (gy * 1024u + 512u) / NRB_HANA_DEPTH_GRID;
            for (u32 gx = 0; gx < NRB_HANA_DEPTH_GRID; ++gx) {
                const u32 x =
                    (gx * 1024u + 512u) / NRB_HANA_DEPTH_GRID;
                const u32 point = gy * NRB_HANA_DEPTH_GRID + gx;
                D3D12_TEXTURE_COPY_LOCATION source = {0}, destination = {0};
                source.pResource = depth->sample_tex;
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                destination.pResource = b->hana_depth_readback;
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint.Offset =
                    (u64)sample_slot * NRB_HANA_DEPTH_SAMPLE_BYTES +
                    (u64)(i * NRB_HANA_DEPTH_POINTS + point) *
                        NRB_HANA_DEPTH_POINT_STRIDE;
                destination.PlacedFootprint.Footprint.Format =
                    DXGI_FORMAT_R32_FLOAT;
                destination.PlacedFootprint.Footprint.Width = 1u;
                destination.PlacedFootprint.Footprint.Height = 1u;
                destination.PlacedFootprint.Footprint.Depth = 1u;
                destination.PlacedFootprint.Footprint.RowPitch = 256u;
                D3D12_BOX box = {x, y, 0u, x + 1u, y + 1u, 1u};
                b->list->lpVtbl->CopyTextureRegion(
                    b->list, &destination, 0, 0, 0, &source, &box);
            }
        }
        if (before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER barrier = {0};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = depth->sample_tex;
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.StateAfter = before;
            b->list->lpVtbl->ResourceBarrier(b->list, 1, &barrier);
        }
        out->copy_recorded = 1u;
    }
}

static void nrb_hana_input_capture(
    rsx_nr_d3d12* b, const rsx_nir_pipeline* st, const nrb_fp_info* fp,
    const u32* vp_words, u32 vp_word_count, u32 bound_vtex_mask,
    const nrb_required_span* required, u32 required_count,
    const rsx_nir_draw* draw, nrb_depth* const* texture_depth_aliases)
{
    if (!b->hana_input_oracle)
        return;
    const u64 fp_hash = nrb_hana_fp_hash(fp->bytes, fp->size);
    if (!nrb_hana_input_match(st, fp, fp_hash))
        return;
    const u64 match = ++b->hana_input_matches;
    if (match > 4u && (match & 8191u) != 0u)
        return;

    const u32 sample_slot =
        b->hana_input_writes++ % NRB_HANA_INPUT_SAMPLES;
    nrb_hana_input_sample* const sample = &b->hana_input[sample_slot];
    memset(sample, 0, sizeof(*sample));
    sample->match = match;
    sample->vp_hash = rsx_nir_hash_words(vp_words, vp_word_count);
    sample->fp_hash = fp_hash;
    sample->constants_hash = nrb_hana_hash(
        st->constants, sizeof(st->constants));
    sample->vp_start = st->vertex_program.start_slot;
    sample->vp_words = vp_word_count;
    sample->vp_branch_bits = st->vertex_program.branch_bits;
    sample->render_condition_enabled = st->render_condition.enabled;
    sample->render_condition_dma = st->render_condition.dma_report;
    sample->render_condition_offset = st->render_condition.offset;
    sample->bound_vtex_mask = bound_vtex_mask;
    sample->used_vtex_mask = nrb_hana_vp_txl_mask(vp_words, vp_word_count);
    sample->required_count = required_count;
    sample->index_location = st->index_binding.location;
    sample->index_offset = st->index_binding.offset;
    sample->base_index = st->vertex_bindings.base_index;
    sample->batch_count = draw->batch_count;
    sample->total_count = draw->total_count;

    u64 required_hash = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < required_count; ++i) {
        const nrb_required_span* const span = &required[i];
        const u8* const source = b->guest_ptr(
            b->guest_user, span->space, span->offset, span->size);
        const u64 hash = source ? nrb_hana_hash(source, span->size) : 0u;
        required_hash ^= hash;
        required_hash *= 0x100000001B3ull;
        if (i >= NRB_HANA_INPUT_SPANS)
            continue;
        sample->required[i] = *span;
        sample->required_span_hash[i] = hash;
        sample->required_space_epoch[i] =
            rsx_guest_pages_space_epoch(&b->pages, span->space);
        const u32 first = span->offset >> RSX_GUEST_PAGE_SHIFT;
        const u32 last = (u32)(((u64)span->offset + span->size - 1u) >>
                               RSX_GUEST_PAGE_SHIFT);
        sample->required_first_page_gen[i] =
            rsx_guest_pages_page_gen(&b->pages, span->space, first);
        sample->required_last_page_gen[i] =
            rsx_guest_pages_page_gen(&b->pages, span->space, last);
    }
    sample->required_hash = required_hash;

    for (u32 unit = 0; unit < NRB_VTEX_UNITS; ++unit) {
        if (!(bound_vtex_mask & (1u << unit)))
            continue;
        nrb_hana_input_vtex* const out = &sample->vtex[unit];
        const rsx_nir_texture* const texture = &st->vertex_textures[unit];
        out->texture = *texture;
        out->span = nrb_vertex_texture_span(texture, NULL, NULL);
        if (!out->span)
            continue;
        const u8* const source = b->guest_ptr(
            b->guest_user, texture->location, texture->offset, out->span);
        out->source_hash = source ? nrb_hana_hash(source, out->span) : 0u;
        out->space_epoch = rsx_guest_pages_space_epoch(
            &b->pages, texture->location);
        const u32 first = texture->offset >> RSX_GUEST_PAGE_SHIFT;
        const u32 last = (u32)(((u64)texture->offset + out->span - 1u) >>
                               RSX_GUEST_PAGE_SHIFT);
        out->first_page_gen = rsx_guest_pages_page_gen(
            &b->pages, texture->location, first);
        out->last_page_gen = rsx_guest_pages_page_gen(
            &b->pages, texture->location, last);

        rsx_nr_res_key key = {0};
        key.kind = 2u;
        key.space = texture->location;
        key.offset = texture->offset;
        key.size = out->span;
        key.fmt = nrb_texture_key(texture);
        rsx_nr_res* const entry = rsx_nr_res_lookup(&b->textures, &key);
        if (!entry)
            continue;
        const u32 slot = (u32)(entry - b->textures.slots);
        out->cache_slot = slot + 1u;
        out->cache_current = rsx_nr_res_current(&b->textures, entry) != 0;
        out->uploaded_hash = b->hana_vtex_uploaded_hash[slot];
        out->resolution = b->hana_vtex_resolution[slot];
    }
    nrb_hana_depth_capture(
        b, sample, sample_slot, st, texture_depth_aliases);
}

static void nrb_restore_texture_alias_set(
    rsx_nr_d3d12* b, nrb_rt** aliases,
    nrb_depth** depth_aliases, u32 count)
{
    for (u32 unit = 0; unit < count; unit++) {
        if (aliases[unit])
            nrb_rt_transition(b, aliases[unit],
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (depth_aliases[unit])
            nrb_depth_transition(b, depth_aliases[unit],
                                 D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
}

static void nrb_restore_texture_aliases(
    rsx_nr_d3d12* b, nrb_rt* aliases[NRB_TEX_UNITS],
    nrb_depth* depth_aliases[NRB_TEX_UNITS],
    nrb_rt* vtex_aliases[NRB_VTEX_UNITS],
    nrb_depth* vtex_depth_aliases[NRB_VTEX_UNITS])
{
    nrb_restore_texture_alias_set(
        b, aliases, depth_aliases, NRB_TEX_UNITS);
    nrb_restore_texture_alias_set(
        b, vtex_aliases, vtex_depth_aliases, NRB_VTEX_UNITS);
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

static void nrb_depth_transition(rsx_nr_d3d12* b, nrb_depth* depth,
                                 D3D12_RESOURCE_STATES state)
{
    if (!depth || depth->state == state)
        return;
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = depth->tex;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = depth->state;
    barrier.Transition.StateAfter = state;
    b->list->lpVtbl->ResourceBarrier(b->list, 1, &barrier);
    depth->state = state;
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
        const u32 opcode = ((w0 >> 24) & 0x3Fu) |
                           ((w2 & 0x80000000u) ? 0x40u : 0u);
        const u32 unsupported_reason =
            !nrb_fp_opcode_supported(opcode) ? 1u : 0u;
        if (unsupported_reason) {
            if (!out->unsupported) {
                out->first_unsupported_offset = off;
                out->first_unsupported_opcode = opcode;
                out->first_unsupported_reason = unsupported_reason;
            }
            out->unsupported++;
        }
        if (opcode == 0x17u || opcode == 0x18u)
            out->texture_mask |= 1u << ((w0 >> 17) & 0xFu);
        off += 16u;
        if (opcode < 0x40u &&
            ((w1 & 3u) == 2u || (w2 & 3u) == 2u || (w3 & 3u) == 2u))
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

static void nrb_note_first_fp_failure(
    rsx_nr_d3d12* b, u32 stage, int result,
    const rsx_nir_pipeline* st, const nrb_fp_info* fp)
{
    if (!b || !st || b->stats.first_fp_failure_stage)
        return;

    b->stats.first_fp_failure_result = result;
    b->stats.first_fp_failure_location = st->fragment_program.location;
    b->stats.first_fp_failure_offset = st->fragment_program.offset;
    b->stats.first_fp_failure_control = st->fragment_program.control;
    if (fp) {
        b->stats.first_fp_failure_size = fp->size;
        b->stats.first_fp_failure_texture_mask = fp->texture_mask;
        b->stats.first_fp_failure_unsupported_count = fp->unsupported;
        b->stats.first_fp_failure_instruction_offset =
            fp->first_unsupported_offset;
        b->stats.first_fp_failure_opcode = fp->first_unsupported_opcode;
        b->stats.first_fp_failure_reason = fp->first_unsupported_reason;
        b->stats.first_fp_failure_structural_hash = fp->structural_hash;
        if (fp->bytes && fp->size) {
            const u32 word_count =
                fp->size / 4u < 16u ? fp->size / 4u : 16u;
            u64 hash = 1469598103934665603ull;
            for (u32 i = 0; i < fp->size; ++i) {
                hash ^= fp->bytes[i];
                hash *= 1099511628211ull;
            }
            b->stats.first_fp_failure_byte_hash = hash;
            for (u32 i = 0; i < word_count; ++i)
                b->stats.first_fp_failure_words[i] =
                    rsx_fp_read_word(fp->bytes + i * 4u);
        }
    }
    /* Publish the discriminator last so a concurrent shutdown snapshot can
     * never print a partially populated identity. */
    b->stats.first_fp_failure_stage = stage;
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

static int nrb_stencil_state_supported(const rsx_nir_depth_stencil* ds)
{
    if (!ds->stencil_test_enable || !ds->two_sided_stencil_enable)
        return 1;
    /* D3D12 exposes independent compare/op state for front and back faces,
     * but a single read mask, write mask and dynamic reference. */
    return (ds->back_stencil_mask & 0xFFu) ==
               (ds->stencil_mask & 0xFFu) &&
           (ds->back_stencil_write_mask & 0xFFu) ==
               (ds->stencil_write_mask & 0xFFu) &&
           (ds->back_stencil_ref & 0xFFu) == (ds->stencil_ref & 0xFFu);
}

static int nrb_depth_bounds_state_supported(
    const rsx_nr_d3d12* b, const rsx_nir_depth_stencil* ds)
{
    if (!ds->depth_bounds_test_enable)
        return 1;
    float zmin = 0.0f;
    float zmax = 1.0f;
    memcpy(&zmin, &ds->depth_bounds_min, sizeof(zmin));
    memcpy(&zmax, &ds->depth_bounds_max, sizeof(zmax));
    return b->depth_bounds_supported && zmin >= 0.0f && zmax <= 1.0f &&
           zmin <= zmax;
}

static void nrb_apply_render_state(
    D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, const rsx_nir_pipeline* st,
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type)
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
    const int polygon_offset_enable =
        (topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT &&
         ra->polygon_offset_point_enable) ||
        (topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE &&
         ra->polygon_offset_line_enable) ||
        (topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE &&
         ra->polygon_offset_fill_enable);
    if (polygon_offset_enable) {
        float scale = 0.0f;
        float bias = 0.0f;
        memcpy(&scale, &ra->polygon_offset_scale, sizeof(scale));
        memcpy(&bias, &ra->polygon_offset_bias, sizeof(bias));
        pd->RasterizerState.SlopeScaledDepthBias = scale;
        pd->RasterizerState.DepthBiasClamp = 0.0f;
        if (bias >= (float)INT_MAX)
            pd->RasterizerState.DepthBias = INT_MAX;
        else if (bias <= (float)INT_MIN)
            pd->RasterizerState.DepthBias = INT_MIN;
        else
            pd->RasterizerState.DepthBias =
                (INT)(bias >= 0.0f ? bias + 0.5f : bias - 0.5f);
    }
    /* Match the established renderer.  RSX fullscreen/post-process vertex
     * programs may emit clip-space Z outside D3D's [0,w] interval while
     * depth testing is disabled; legacy keeps those pixels visible. */
    pd->RasterizerState.DepthClipEnable = FALSE;
    pd->DepthStencilState.DepthEnable = ds->depth_test_enable ? TRUE : FALSE;
    pd->DepthStencilState.DepthWriteMask = ds->depth_write_enable
        ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd->DepthStencilState.DepthFunc = nrb_depth_func(ds->depth_func);
    if (ds->stencil_test_enable) {
        pd->DepthStencilState.StencilEnable = TRUE;
        pd->DepthStencilState.StencilReadMask = (UINT8)ds->stencil_mask;
        pd->DepthStencilState.StencilWriteMask =
            (UINT8)ds->stencil_write_mask;
        pd->DepthStencilState.FrontFace.StencilFunc =
            nrb_depth_func(ds->stencil_func);
        pd->DepthStencilState.FrontFace.StencilFailOp =
            nrb_stencil_op(ds->stencil_op_fail);
        pd->DepthStencilState.FrontFace.StencilDepthFailOp =
            nrb_stencil_op(ds->stencil_op_zfail);
        pd->DepthStencilState.FrontFace.StencilPassOp =
            nrb_stencil_op(ds->stencil_op_zpass);
        if (ds->two_sided_stencil_enable) {
            pd->DepthStencilState.BackFace.StencilFunc =
                nrb_depth_func(ds->back_stencil_func);
            pd->DepthStencilState.BackFace.StencilFailOp =
                nrb_stencil_op(ds->back_stencil_op_fail);
            pd->DepthStencilState.BackFace.StencilDepthFailOp =
                nrb_stencil_op(ds->back_stencil_op_zfail);
            pd->DepthStencilState.BackFace.StencilPassOp =
                nrb_stencil_op(ds->back_stencil_op_zpass);
        } else {
            pd->DepthStencilState.BackFace =
                pd->DepthStencilState.FrontFace;
        }
    }
}

/* Hash the exact fixed-function descriptor consumed by D3D12. Raw NIR
 * state includes inert retained registers (unused MRT masks, disabled blend
 * and stencil fields, and polygon-offset modes for other topologies). Those
 * values produced thousands of aliases for an identical driver PSO. */
static u64 nrb_pso_fixed_state_key(
    u64 seed, const rsx_nr_d3d12* b, const rsx_nir_pipeline* st,
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type, int strip_cut,
    DXGI_FORMAT color_dxgi)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.SampleMask = 0xFFFFFFFFu;
    nrb_apply_render_state(&pd, st, topology_type);
    pd.IBStripCutValue = strip_cut
        ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
        : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pd.PrimitiveTopologyType = topology_type;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = color_dxgi;
    pd.DSVFormat = nrb_depth_dsv_dxgi(b, st->surface.depth_format);
    pd.SampleDesc.Count = 1;

    u64 key = rsx_nr_hash_fold(seed, &pd.BlendState,
                               sizeof(pd.BlendState));
    key = rsx_nr_hash_fold(key, &pd.SampleMask, sizeof(pd.SampleMask));
    key = rsx_nr_hash_fold(key, &pd.RasterizerState,
                           sizeof(pd.RasterizerState));
    key = rsx_nr_hash_fold(key, &pd.DepthStencilState,
                           sizeof(pd.DepthStencilState));
    key = rsx_nr_hash_fold(key, &pd.IBStripCutValue,
                           sizeof(pd.IBStripCutValue));
    key = rsx_nr_hash_fold(key, &pd.PrimitiveTopologyType,
                           sizeof(pd.PrimitiveTopologyType));
    key = rsx_nr_hash_fold(key, &pd.NumRenderTargets,
                           sizeof(pd.NumRenderTargets));
    key = rsx_nr_hash_fold(key, &pd.RTVFormats, sizeof(pd.RTVFormats));
    key = rsx_nr_hash_fold(key, &pd.DSVFormat, sizeof(pd.DSVFormat));
    return rsx_nr_hash_fold(key, &pd.SampleDesc, sizeof(pd.SampleDesc));
}

static ID3DBlob* nrb_compile(rsx_nr_d3d12* b, const char* text, size_t len,
                             u32 stage, int* cache_hit, int* compiled)
{
    if (cache_hit)
        *cache_hit = 0;
    if (compiled)
        *compiled = 0;
    if (b->compile_shader) {
        ID3DBlob* const cached = (ID3DBlob*)b->compile_shader(
            b->content_cache_user, stage, text, (u32)len,
            D3DCOMPILE_OPTIMIZATION_LEVEL1, cache_hit, compiled);
        if (!cached)
            b->stats.compile_failures++;
        return cached;
    }
    ID3DBlob* blob = NULL;
    ID3DBlob* err = NULL;
    const char* const target = stage == 'V' ? "vs_5_0" : "ps_5_0";
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
    if (compiled)
        *compiled = 1;
    return blob;
}

static ID3D12PipelineState* nrb_get_pso(rsx_nr_d3d12* b,
                                        const rsx_nir_pipeline* st,
                                        const u32* vp_words, u32 vp_word_count,
                                        const nrb_fp_info* fp,
                                        const rsx_vertex_pull_plan* plan,
                                         D3D12_PRIMITIVE_TOPOLOGY_TYPE tt,
                                         int strip_cut, u32 cube_mask,
                                         u32 vtex_mask,
                                         DXGI_FORMAT color_dxgi)
{
    const u64 key_lookup_start = nrb_stall_now(b);
    /* Structural shader identity deliberately excludes inline FP constants
     * and alpha-ref: both are uploaded through b1.  Everything that shapes
     * the compiled source or PSO remains in the key. */
    u64 key = rsx_nir_hash_words(vp_words, vp_word_count);
    key = rsx_nr_hash_fold(key, &st->vertex_program.start_slot,
                           sizeof(st->vertex_program.start_slot));
    key = rsx_nr_hash_fold(key ^ rsx_vertex_pull_signature(plan), &tt,
                           sizeof(tt));
    key = rsx_nr_hash_fold(key, &strip_cut, sizeof(strip_cut));
    key = rsx_nr_hash_fold(key, &fp->structural_hash,
                           sizeof(fp->structural_hash));
    key = rsx_nr_hash_fold(key, &cube_mask, sizeof(cube_mask));
    key = rsx_nr_hash_fold(key, &vtex_mask, sizeof(vtex_mask));
    key = rsx_nr_hash_fold(key, &b->coherent_vp_options,
                           sizeof(b->coherent_vp_options));
    const u32 fp_ctrl = st->fragment_program.control & 0x40u;
    key = rsx_nr_hash_fold(key, &fp_ctrl, sizeof(fp_ctrl));
    key = rsx_nr_hash_fold(
        key, &st->fragment_program.texcoord_2d_mask,
        sizeof(st->fragment_program.texcoord_2d_mask));
    key = rsx_nr_hash_fold(
        key, &st->fragment_program.shader_window,
        sizeof(st->fragment_program.shader_window));
    const u32 alpha_enable = st->blend.alpha_test_enable ? 1u : 0u;
    const u32 alpha_func = alpha_enable ? st->blend.alpha_func : 0u;
    key = rsx_nr_hash_fold(key, &alpha_enable, sizeof(alpha_enable));
    key = rsx_nr_hash_fold(key, &alpha_func, sizeof(alpha_func));
    /* The live broker may canonicalize a guest R5G6B5 surface into RGBA8.
     * PSO identity follows the actual D3D descriptor, including the actual
     * RTV/DSV formats, and excludes dynamic OM state and inert RSX fields. */
    key = nrb_pso_fixed_state_key(
        key, b, st, tt, strip_cut, color_dxgi);
    u64 cached = 0;
    const int cache_hit = rsx_nr_pso_lookup(&b->psos, key, &cached);
    nrb_stall_finish(b, key_lookup_start,
                     &b->stats.stall_pso_key_lookup_count,
                     &b->stats.stall_pso_key_lookup_ticks);
    if (cache_hit) {
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
        n = rsx_vertex_pull_decompile_control_options(
            plan, (const u8*)vp_words, vp_word_count * 4, vtex_mask,
            st->vertex_program.start_slot, b->coherent_vp_options,
            b->vs_text, sizeof(b->vs_text));
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
                     "    o.col1=0; o.fog=0; o.t0=float4(-1,-1,0,0); o.t1=0; o.t2=0; o.t3=0;\n"
                     "    o.t4=0; o.t5=0; o.t6=0; o.t7=0; return o;\n"
                     "}\n",
                     b->pull_globals, b->pull_loads);
        if (n <= 0 || n >= (int)sizeof(b->vs_text))
            return NULL;
    }

    const u64 vertex_compile_start = nrb_stall_now(b);
    int vertex_cache_hit = 0, vertex_compiled = 0;
    ID3DBlob* vs = nrb_compile(
        b, b->vs_text, strlen(b->vs_text), 'V',
        &vertex_cache_hit, &vertex_compiled);
    if (vertex_cache_hit) {
        b->stats.vertex_shader_cache_hits++;
        nrb_stall_finish(b, vertex_compile_start,
                         &b->stats.stall_vertex_cache_count,
                         &b->stats.stall_vertex_cache_ticks);
    } else {
        b->stats.vertex_shader_builds += vertex_compiled != 0;
        nrb_stall_finish(b, vertex_compile_start,
                         &b->stats.stall_vertex_compile_count,
                         &b->stats.stall_vertex_compile_ticks);
    }
    if (!vs)
        return NULL;
    u32 constant_count = 0;
    int fi = rsx_fp_decompile_buffered_ex(
        fp->bytes, fp->size, st->fragment_program.control, cube_mask,
        b->ps_text, sizeof(b->ps_text), &constant_count);
    if (fi <= 0 || constant_count != fp->constants.count ||
        rsx_fp_apply_texcoord_control(
            b->ps_text, sizeof(b->ps_text),
            st->fragment_program.texcoord_2d_mask) < 0 ||
        rsx_fp_apply_shader_window(
            b->ps_text, sizeof(b->ps_text),
            st->fragment_program.shader_window) < 0 ||
        (alpha_enable && rsx_fp_apply_alpha_test_buffered(
            b->ps_text, sizeof(b->ps_text), alpha_func) < 0)) {
        vs->lpVtbl->Release(vs);
        return NULL;
    }
    const u64 pixel_compile_start = nrb_stall_now(b);
    int pixel_cache_hit = 0, pixel_compiled = 0;
    ID3DBlob* ps = nrb_compile(
        b, b->ps_text, strlen(b->ps_text), 'P',
        &pixel_cache_hit, &pixel_compiled);
    if (pixel_cache_hit) {
        b->stats.pixel_shader_cache_hits++;
        nrb_stall_finish(b, pixel_compile_start,
                         &b->stats.stall_pixel_cache_count,
                         &b->stats.stall_pixel_cache_ticks);
    } else {
        b->stats.pixel_shader_builds += pixel_compiled != 0;
        nrb_stall_finish(b, pixel_compile_start,
                         &b->stats.stall_pixel_compile_count,
                         &b->stats.stall_pixel_compile_ticks);
    }
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
    nrb_apply_render_state(&pd, st, tt);
    pd.InputLayout.NumElements = 0;  /* vertex pulling: no IA              */
    pd.IBStripCutValue = strip_cut
        ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
        : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pd.PrimitiveTopologyType = tt;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = color_dxgi;
    pd.DSVFormat = nrb_depth_dsv_dxgi(b, st->surface.depth_format);
    pd.SampleDesc.Count = 1;

    const u64 vertex_bytecode_hash = rsx_nr_hash_fold(
        0x56534844524E5242ull, pd.VS.pShaderBytecode,
        pd.VS.BytecodeLength);
    const u64 pixel_bytecode_hash = rsx_nr_hash_fold(
        0x50534844524E5242ull, pd.PS.pShaderBytecode,
        pd.PS.BytecodeLength);
    void* cached_pso = NULL;
    u32 cached_pso_size = 0;
    if (b->pso_load && b->pso_load(
            b->content_cache_user, key, vertex_bytecode_hash,
            pixel_bytecode_hash, &cached_pso, &cached_pso_size) == 0 &&
        cached_pso && cached_pso_size) {
        pd.CachedPSO.pCachedBlob = cached_pso;
        pd.CachedPSO.CachedBlobSizeInBytes = cached_pso_size;
    }

    ID3D12PipelineState* pso = NULL;
    const u64 driver_pso_start = nrb_stall_now(b);
    b->stats.driver_pso_creates++;
    HRESULT hr = b->dev->lpVtbl->CreateGraphicsPipelineState(
        b->dev, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    if (FAILED(hr) && cached_pso) {
        b->stats.driver_pso_cache_rejects++;
        pd.CachedPSO.pCachedBlob = NULL;
        pd.CachedPSO.CachedBlobSizeInBytes = 0;
        b->stats.driver_pso_creates++;
        hr = b->dev->lpVtbl->CreateGraphicsPipelineState(
            b->dev, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    } else if (SUCCEEDED(hr) && cached_pso) {
        b->stats.driver_pso_cache_hits++;
    }
    nrb_stall_finish(b, driver_pso_start,
                     &b->stats.stall_driver_pso_create_count,
                     &b->stats.stall_driver_pso_create_ticks);
    if (cached_pso && b->pso_free)
        b->pso_free(b->content_cache_user, cached_pso);
    vs->lpVtbl->Release(vs);
    ps->lpVtbl->Release(ps);
    if (FAILED(hr)) {
        b->stats.compile_failures++;
        return NULL;
    }
    if (!pd.CachedPSO.pCachedBlob && b->pso_store) {
        ID3DBlob* cached_blob = NULL;
        if (SUCCEEDED(pso->lpVtbl->GetCachedBlob(pso, &cached_blob)) &&
            cached_blob) {
            const size_t cached_size =
                cached_blob->lpVtbl->GetBufferSize(cached_blob);
            if (cached_size <= UINT32_MAX && b->pso_store(
                    b->content_cache_user, key, vertex_bytecode_hash,
                    pixel_bytecode_hash,
                    cached_blob->lpVtbl->GetBufferPointer(cached_blob),
                    (u32)cached_size) == 0)
                b->stats.driver_pso_cache_writes++;
            cached_blob->lpVtbl->Release(cached_blob);
        }
    }
    rsx_nr_pso_insert(&b->psos, key, (u64)(uintptr_t)pso);
    b->stats.pso_builds++;
    return pso;
}

/* Read one batch of the guest index array [first, first+count) as u32
 * values, translating the restart sentinel: strips keep it as the D3D12
 * cut value 0xFFFFFFFF; list topologies drop it. Returns the converted
 * count into b->idx_scratch, or ~0u when the span is unreadable. */
static int nrb_ensure_index_scratch(rsx_nr_d3d12* b, u32 count)
{
    if (count <= b->idx_scratch_cap)
        return 0;
    u32 capacity = b->idx_scratch_cap ? b->idx_scratch_cap : 4096u;
    while (capacity < count) {
        if (capacity > 0x7FFFFFFFu)
            return -1;
        capacity *= 2u;
    }
    u32* buffer = realloc(b->idx_scratch, (size_t)capacity * sizeof(u32));
    if (!buffer)
        return -1;
    b->idx_scratch = buffer;
    b->idx_scratch_cap = capacity;
    return 0;
}

static u32 nrb_read_indices(rsx_nr_d3d12* b, const rsx_nir_pipeline* st,
                            u32 first, u32 count, int strips)
{
    const u32 esize = st->index_binding.is_u32 ? 4u : 2u;
    const u8* src = b->guest_ptr(b->guest_user, st->index_binding.location,
                                 st->index_binding.offset + first * esize,
                                 count * esize);
    if (!src)
        return ~0u;
    if (nrb_ensure_index_scratch(b, count) != 0)
        return ~0u;
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

static int nrb_index_bounds(rsx_nr_d3d12* b,
                            const rsx_nir_pipeline* st,
                            const rsx_nir_draw* draw, const u32* batches,
                            u32* out_min, u32* out_max,
                            nrb_required_span* spans, u32* span_count,
                            int mirror_indices)
{
    const u32 esize = st->index_binding.is_u32 ? 4u : 2u;
    u32 min_value = UINT32_MAX, max_value = 0;
    u64 min_byte = UINT64_MAX, max_byte = 0;
    int have_value = 0;
    for (u32 bi = 0; bi < draw->batch_count; ++bi) {
        const u32 first = batches[bi * 2u];
        const u32 count = batches[bi * 2u + 1u];
        if (!count)
            continue;
        const u64 byte = (u64)st->index_binding.offset +
                         (u64)first * esize;
        const u64 bytes = (u64)count * esize;
        if (byte > UINT32_MAX || bytes > UINT32_MAX ||
            byte + bytes > 0x100000000ull)
            return -1;
        const u8* src = b->guest_ptr(
            b->guest_user, st->index_binding.location, (u32)byte,
            (u32)bytes);
        if (!src)
            return -1;
        if (byte < min_byte)
            min_byte = byte;
        if (byte + bytes > max_byte)
            max_byte = byte + bytes;
        for (u32 i = 0; i < count; ++i) {
            u32 value;
            if (esize == 4u)
                value = ((u32)src[i * 4u] << 24) |
                        ((u32)src[i * 4u + 1u] << 16) |
                        ((u32)src[i * 4u + 2u] << 8) |
                        (u32)src[i * 4u + 3u];
            else
                value = ((u32)src[i * 2u] << 8) |
                        (u32)src[i * 2u + 1u];
            if (st->index_binding.restart_enable &&
                value == st->index_binding.restart_index)
                continue;
            if (!have_value || value < min_value)
                min_value = value;
            if (!have_value || value > max_value)
                max_value = value;
            have_value = 1;
        }
    }
    if (mirror_indices && min_byte != UINT64_MAX &&
        nrb_add_required_span(spans, span_count,
                              st->index_binding.location, min_byte,
                              max_byte - min_byte) != 0)
        return -1;
    if (!have_value)
        return 1;                   /* restart-only draw: no vertex fetch */
    *out_min = min_value;
    *out_max = max_value;
    return 0;
}

static int nrb_add_attr_element_span(rsx_nr_d3d12* b,
                                     const rsx_vertex_pull_plan* plan,
                                     u32 attr, u32 elem_min, u32 elem_max,
                                     nrb_required_span* spans,
                                     u32* span_count)
{
    const rsx_vertex_pull_attr* a = &plan->attr[attr];
    if (!a->pulled || elem_max < elem_min)
        return 0;
    const u64 first = (u64)plan->base_offset + a->desc.offset +
                      (u64)elem_min * a->stride;
    const u64 last = (u64)plan->base_offset + a->desc.offset +
                     (u64)elem_max * a->stride + a->elem_size;
    /* HLSL address arithmetic is uint. Refuse a draw whose address would
     * wrap rather than approximating its modulo-2^32 reads. */
    if (first > UINT32_MAX || last > 0x100000000ull || last < first)
        return -1;
    const u32 space = a->desc.location ? 1u : 0u;
    const u64 limit = b->pages.space[space].size;
    if (first >= limit)
        return 0;                   /* shader returns the RSX default */
    const u64 clipped_last = last < limit ? last : limit;
    return nrb_add_required_span(spans, span_count, space, first,
                                 clipped_last - first);
}

static int nrb_prepare_draw_residency(rsx_nr_d3d12* b,
                                      const rsx_nir_pipeline* st,
                                      const rsx_vertex_pull_plan* plan,
                                      const rsx_nir_draw* draw,
                                      const u32* batches,
                                      int mirror_indices,
                                      nrb_required_span* spans,
                                      u32* span_count)
{
    u32 ref_min = UINT32_MAX, ref_max = 0;
    int have_refs = 0;
    if (draw->indexed) {
        const int bounds = nrb_index_bounds(
            b, st, draw, batches, &ref_min, &ref_max, spans, span_count,
            mirror_indices);
        if (bounds < 0)
            return -2;              /* unreadable index source */
        have_refs = bounds == 0;
    } else {
        for (u32 bi = 0; bi < draw->batch_count; ++bi) {
            const u32 first = batches[bi * 2u];
            const u32 count = batches[bi * 2u + 1u];
            if (!count)
                continue;
            const u64 last = (u64)first + count - 1u;
            if (last > UINT32_MAX)
                return -1;
            if (!have_refs || first < ref_min)
                ref_min = first;
            if (!have_refs || last > ref_max)
                ref_max = (u32)last;
            have_refs = 1;
        }
    }
    if (!have_refs)
        return 0;

    const u64 domain = 1ull << 20;
    for (u32 attr = 0; attr < RSX_NIR_NUM_VERTEX_ATTR; ++attr) {
        const rsx_vertex_pull_attr* a = &plan->attr[attr];
        if (!a->pulled)
            continue;
        if (a->desc.frequency <= 1u) {
            const u32 base_index = draw->indexed
                ? st->vertex_bindings.base_index : 0u;
            const u64 lo = (u64)ref_min + base_index;
            const u64 hi = (u64)ref_max + base_index;
            if (hi - lo >= domain - 1u) {
                if (nrb_add_attr_element_span(
                        b, plan, attr, 0, (u32)domain - 1u,
                        spans, span_count) != 0)
                    return -1;
            } else if ((lo >> 20) == (hi >> 20)) {
                if (nrb_add_attr_element_span(
                        b, plan, attr, (u32)lo & 0xFFFFFu,
                        (u32)hi & 0xFFFFFu, spans, span_count) != 0)
                    return -1;
            } else {
                if (nrb_add_attr_element_span(
                        b, plan, attr, (u32)lo & 0xFFFFFu, 0xFFFFFu,
                        spans, span_count) != 0 ||
                    nrb_add_attr_element_span(
                        b, plan, attr, 0, (u32)hi & 0xFFFFFu,
                        spans, span_count) != 0)
                    return -1;
            }
        } else if ((plan->divider_mask >> attr) & 1u) {
            if (nrb_add_attr_element_span(
                    b, plan, attr, 0, a->desc.frequency - 1u,
                    spans, span_count) != 0)
                return -1;
        } else {
            if (nrb_add_attr_element_span(
                    b, plan, attr, ref_min / a->desc.frequency,
                    ref_max / a->desc.frequency, spans, span_count) != 0)
                return -1;
        }
    }

    for (u32 i = 0; i < *span_count; ++i)
        if (nrb_require_span(b, spans[i].space, spans[i].offset,
                             spans[i].size) != 0)
            return -1;
    return 0;
}

static u32 nrb_expand_primitives(rsx_nr_d3d12* b,
                                 const rsx_nir_pipeline* st,
                                 const rsx_nir_draw* draw,
                                 u32 first, u32 count)
{
    if (!nrb_needs_expansion(draw->primitive) || count > 0x55555555u ||
        nrb_ensure_index_scratch(b, count * 3u + 2u) != 0)
        return ~0u;

    const u32 esize = st->index_binding.is_u32 ? 4u : 2u;
    const u8* source = NULL;
    if (draw->indexed) {
        source = b->guest_ptr(
            b->guest_user, st->index_binding.location,
            st->index_binding.offset + first * esize, count * esize);
        if (!source)
            return ~0u;
    }

    u32 out_count = 0;
    u32 group_count = 0;
    u32 first_value = 0, previous = 0;
    u32 quad[4] = {0};
    u32 pair0 = 0;
    const int restart_enabled =
        draw->indexed && st->index_binding.restart_enable;
    const u32 restart = st->index_binding.restart_index;

    for (u32 i = 0; i <= count; i++) {
        int separator = i == count;
        u32 value = first + i;
        if (!separator && source) {
            if (esize == 4u)
                value = ((u32)source[i * 4] << 24) |
                        ((u32)source[i * 4 + 1] << 16) |
                        ((u32)source[i * 4 + 2] << 8) |
                        source[i * 4 + 3];
            else
                value = ((u32)source[i * 2] << 8) | source[i * 2 + 1];
            separator = restart_enabled && value == restart;
        }

        if (separator) {
            if (draw->primitive == 3u) {
                if (group_count >= 2u) {
                    b->idx_scratch[out_count++] = previous;
                    b->idx_scratch[out_count++] = first_value;
                }
            }
            group_count = 0;
            continue;
        }

        switch (draw->primitive) {
        case 3:                         /* line loop -> explicit line list */
            if (!group_count) {
                first_value = value;
            } else {
                b->idx_scratch[out_count++] = previous;
                b->idx_scratch[out_count++] = value;
            }
            previous = value;
            group_count++;
            break;
        case 7:                         /* triangle fan */
        case 10:                        /* polygon */
            if (!group_count)
                first_value = value;
            else if (group_count >= 2u) {
                b->idx_scratch[out_count++] = first_value;
                b->idx_scratch[out_count++] = previous;
                b->idx_scratch[out_count++] = value;
            }
            previous = value;
            group_count++;
            break;
        case 8:                         /* independent quads */
            quad[group_count & 3u] = value;
            group_count++;
            if ((group_count & 3u) == 0u) {
                b->idx_scratch[out_count++] = quad[0];
                b->idx_scratch[out_count++] = quad[1];
                b->idx_scratch[out_count++] = quad[2];
                b->idx_scratch[out_count++] = quad[0];
                b->idx_scratch[out_count++] = quad[2];
                b->idx_scratch[out_count++] = quad[3];
            }
            break;
        case 9:                         /* quad strip */
            if ((group_count & 1u) == 0u)
                pair0 = value;
            else {
                const u32 new0 = pair0, new1 = value;
                if (group_count >= 3u) {
                    b->idx_scratch[out_count++] = quad[0];
                    b->idx_scratch[out_count++] = quad[1];
                    b->idx_scratch[out_count++] = new1;
                    b->idx_scratch[out_count++] = quad[0];
                    b->idx_scratch[out_count++] = new1;
                    b->idx_scratch[out_count++] = new0;
                }
                quad[0] = new0;
                quad[1] = new1;
            }
            group_count++;
            break;
        default:
            return ~0u;
        }
    }
    return out_count;
}

/* A BEGIN/END may contain many DRAW_INDEX_ARRAY/DRAW_ARRAYS methods.  RSX
 * triangle strips continue across those method boundaries; only an explicit
 * restart index cuts the strip.  The established renderer concatenates the
 * batches before expanding them.  Recording one D3D draw per batch silently
 * restarted the strip and corrupted the high-batch shadow-consumer passes.
 * Expand the complete action into one triangle list with the established
 * alternating winding so native and legacy ownership are interchangeable. */
static u32 nrb_expand_triangle_strip_batches(
    rsx_nr_d3d12* b, const rsx_nir_pipeline* st,
    const rsx_nir_draw* draw, const u32* batches)
{
    u64 source_count = 0;
    for (u32 batch = 0; batch < draw->batch_count; ++batch)
        source_count += batches[batch * 2u + 1u];
    if (source_count > 0x55555555u ||
        nrb_ensure_index_scratch(b, (u32)source_count * 3u + 2u) != 0)
        return ~0u;

    const u32 element_size = st->index_binding.is_u32 ? 4u : 2u;
    const int restart_enabled =
        draw->indexed && st->index_binding.restart_enable;
    const u32 restart = st->index_binding.restart_index;
    u32 previous2 = 0, previous1 = 0;
    u32 group_count = 0, out_count = 0;

    for (u32 batch = 0; batch < draw->batch_count; ++batch) {
        const u32 first = batches[batch * 2u];
        const u32 count = batches[batch * 2u + 1u];
        const u8* source = NULL;
        if (draw->indexed && count) {
            const u64 byte_offset =
                (u64)st->index_binding.offset + (u64)first * element_size;
            const u64 byte_count = (u64)count * element_size;
            if (byte_offset > UINT32_MAX || byte_count > UINT32_MAX ||
                byte_offset + byte_count > 0x100000000ull)
                return ~0u;
            source = b->guest_ptr(
                b->guest_user, st->index_binding.location,
                (u32)byte_offset, (u32)byte_count);
            if (!source)
                return ~0u;
        }

        for (u32 i = 0; i < count; ++i) {
            u32 value = first + i;
            if (source) {
                if (element_size == 4u)
                    value = ((u32)source[i * 4u] << 24) |
                            ((u32)source[i * 4u + 1u] << 16) |
                            ((u32)source[i * 4u + 2u] << 8) |
                            (u32)source[i * 4u + 3u];
                else
                    value = ((u32)source[i * 2u] << 8) |
                            (u32)source[i * 2u + 1u];
            }
            if (restart_enabled && value == restart) {
                group_count = 0;
                continue;
            }
            if (group_count == 0u) {
                previous2 = value;
            } else if (group_count == 1u) {
                previous1 = value;
            } else {
                const u32 triangle = group_count - 2u;
                if (triangle & 1u) {
                    b->idx_scratch[out_count++] = previous1;
                    b->idx_scratch[out_count++] = previous2;
                } else {
                    b->idx_scratch[out_count++] = previous2;
                    b->idx_scratch[out_count++] = previous1;
                }
                b->idx_scratch[out_count++] = value;
                previous2 = previous1;
                previous1 = value;
            }
            group_count++;
        }
    }
    return out_count;
}

static int nrb_guest_texture_preflight(rsx_nr_d3d12* b,
                                       const rsx_nir_texture* texture)
{
    const u32 span = nrb_texture_span(texture);
    if (!span || !b->guest_ptr(
            b->guest_user, texture->location, texture->offset, span))
        return -1;
    const u32 format = texture->format & NRB_TEX_BASE_MASK &
                       ~NRB_TEX_UNNORM;
    const int linear = (texture->format & NRB_TEX_LINEAR) != 0;
    if (!linear && ((texture->width & (texture->width - 1u)) ||
                    (texture->height & (texture->height - 1u))))
        return -1;
    if (format == NRB_TEX_DEPTH24_D8 && texture->cubemap)
        return -1;
    return nrb_watch_guest_span(
        b, texture->location, texture->offset, span);
}

static int nrb_texture_preflight(rsx_nr_d3d12* b,
                                 const rsx_nir_texture* texture,
                                 nrb_rt* draw_rt, nrb_depth* draw_depth,
                                 u32 unit)
{
    nrb_rt* rt_alias = nrb_texture_rt_alias(b, texture, draw_rt, unit);
    /* Live pass islands share the established renderer's exact D3D resource
     * identities. The vertical owner flushes the producing queue before a
     * consumer changes renderer and the backend restores sampled aliases to
     * render-target state after each draw, so a non-self alias is the safe
     * dependency boundary rather than a reason to decode stale guest bytes. */
    if (rt_alias)
        return rt_alias == draw_rt ? -1 : 0;
    nrb_depth* depth_alias = nrb_texture_depth_alias(
        b, texture, draw_depth, unit);
    if (depth_alias) {
        if (depth_alias == draw_depth)
            return -1;
        if (!depth_alias->external)
            return 0;
        /* The live D32S8 resource is not sample-equivalent. Require the
         * established R32 snapshot plus a same-list resolver, and also prove
         * the guest fallback up front so a clear-only/no-write zeta remains a
         * wholly supported atomic section. */
        if (!depth_alias->sample_tex || !b->resolve_depth_sample)
            return -1;
    }
    return nrb_guest_texture_preflight(b, texture);
}

static int nrb_vertex_texture_preflight(
    rsx_nr_d3d12* b, const rsx_nir_texture* texture)
{
    const u32 span = nrb_vertex_texture_span(texture, NULL, NULL);
    if (!span || !b->guest_ptr(
            b->guest_user, texture->location, texture->offset, span))
        return -1;
    return nrb_watch_guest_span(
        b, texture->location, texture->offset, span);
}

int rsx_nr_d3d12_preflight_clear(rsx_nr_d3d12* b,
                                 const rsx_nir_pipeline* st,
                                 const rsx_nir_clear* c)
{
    if (!b || !st || !c)
        return -1;
    if (!(c->mask & 0xF3u))
        return 0;
    if (st->surface.color_target != 1u)
        return -1;
    const u32 color_bits = c->mask & 0xF0u;
    if (color_bits && color_bits != 0xF0u)
        return -1;
    if (!nrb_rt_from_state(b, st, 1))
        return -1;
    if (c->mask & 0x03u) {
        nrb_depth* depth = nrb_depth_from_state(b, st, 1);
        if (!depth || ((c->mask & 0x02u) && depth->fmt != 2u))
            return -1;
    }
    return 0;
}

static int nrb_vp_program_supported(rsx_nr_d3d12* b,
                                    const u32* vp_words,
                                    u32 vp_word_count, u32 vtex_mask,
                                    u32 start_slot)
{
    rsx_vp_native_support_analysis analysis;
    return vp_words && vp_word_count &&
        rsx_vp_analyze_native_support_control_options(
            (const u8*)vp_words, vp_word_count * sizeof(u32), vtex_mask,
            start_slot, b->coherent_vp_options, &analysis);
}

int rsx_nr_d3d12_validate_draw_program_usage(
    rsx_nr_d3d12* b, const rsx_nir_pipeline* st,
    const u32* vp_words, u32 vp_word_count, u32* texture_mask)
{
    if (!b || !st)
        return -RSX_NR_DRAW_PF_BAD_ARGUMENT;
    nrb_fp_info fp;
    if (nrb_resolve_fp(b, st, &fp) != 0)
        return -RSX_NR_DRAW_PF_FRAGMENT_RESOLVE;
    if (fp.unsupported)
        return -RSX_NR_DRAW_PF_FRAGMENT_UNSUPPORTED;
    for (u32 unit = 0; unit < NRB_TEX_UNITS; ++unit)
        if ((fp.texture_mask & (1u << unit)) &&
            !st->textures[unit].enabled)
            return -RSX_NR_DRAW_PF_TEXTURE_DISABLED;

    u32 vtex_mask = 0;
    for (u32 unit = 0; unit < NRB_VTEX_UNITS; ++unit)
        if (st->vertex_textures[unit].enabled)
            vtex_mask |= 1u << unit;
    if (!nrb_vp_program_supported(
            b, vp_words, vp_word_count, vtex_mask,
            st->vertex_program.start_slot))
        return -RSX_NR_DRAW_PF_VERTEX_PROGRAM;
    for (u32 unit = 0; unit < NRB_VTEX_UNITS; ++unit)
        if ((vtex_mask & (1u << unit)) &&
            nrb_vertex_texture_preflight(
                b, &st->vertex_textures[unit]) != 0)
            return -RSX_NR_DRAW_PF_VERTEX_TEXTURE;
    if (texture_mask)
        *texture_mask = fp.texture_mask;
    return 0;
}

int rsx_nr_d3d12_validate_draw_program(rsx_nr_d3d12* b,
                                       const rsx_nir_pipeline* st,
                                       const u32* vp_words,
                                       u32 vp_word_count)
{
    return rsx_nr_d3d12_validate_draw_program_usage(
        b, st, vp_words, vp_word_count, NULL);
}

static int nrb_preflight_draw_impl(rsx_nr_d3d12* b,
                                   const rsx_nir_pipeline* st,
                                   const u32* vp_words, u32 vp_word_count,
                                   const rsx_nir_draw* d,
                                   const u32* batches)
{
    if (!b || !st || !d || !batches || !d->batch_count ||
        d->batch_count > NRB_MAX_DRAW_BATCHES)
        return -RSX_NR_DRAW_PF_BAD_ARGUMENT;
    if (st->render_condition.enabled) {
        u32 value = 0;
        if (!b->render_condition_read ||
            b->render_condition_read(
                b->render_condition_user,
                st->render_condition.dma_report,
                st->render_condition.offset, &value) != 0)
            return -RSX_NR_DRAW_PF_RENDER_CONDITION;
    }
    if (st->surface.color_target != 1u)
        return -RSX_NR_DRAW_PF_SURFACE_TARGET;
    D3D12_PRIMITIVE_TOPOLOGY topology;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type;
    if (nrb_topology(d->primitive, &topology, &topology_type) != 0)
        return -RSX_NR_DRAW_PF_TOPOLOGY;
    (void)topology;

    nrb_rt* rt = nrb_rt_from_state(b, st, 1);
    if (!rt)
        return -RSX_NR_DRAW_PF_COLOR_TARGET;
    const int depth_active = st->depth_stencil.depth_test_enable ||
                             st->depth_stencil.stencil_test_enable;
    nrb_depth* depth = depth_active ? nrb_depth_from_state(b, st, 1) : NULL;
    if (depth_active && !depth)
        return -RSX_NR_DRAW_PF_DEPTH_TARGET;
    if (!nrb_stencil_state_supported(&st->depth_stencil))
        return -RSX_NR_DRAW_PF_STENCIL_STATE;
    if (!nrb_depth_bounds_state_supported(b, &st->depth_stencil))
        return -RSX_NR_DRAW_PF_DEPTH_BOUNDS;

    nrb_fp_info fp;
    if (nrb_resolve_fp(b, st, &fp) != 0)
        return -RSX_NR_DRAW_PF_FRAGMENT_RESOLVE;
    if (fp.unsupported)
        return -RSX_NR_DRAW_PF_FRAGMENT_UNSUPPORTED;
    u32 vtex_mask = 0;
    for (u32 unit = 0; unit < NRB_VTEX_UNITS; ++unit)
        if (st->vertex_textures[unit].enabled)
            vtex_mask |= 1u << unit;
    if (!nrb_vp_program_supported(
            b, vp_words, vp_word_count, vtex_mask,
            st->vertex_program.start_slot))
        return -RSX_NR_DRAW_PF_VERTEX_PROGRAM;
    for (u32 unit = 0; unit < NRB_VTEX_UNITS; ++unit)
        if ((vtex_mask & (1u << unit)) &&
            nrb_vertex_texture_preflight(
                b, &st->vertex_textures[unit]) != 0)
            return -RSX_NR_DRAW_PF_VERTEX_TEXTURE;
    u32 cube_mask = 0;
    for (u32 unit = 0; unit < NRB_TEX_UNITS; ++unit) {
        if (!(fp.texture_mask & (1u << unit)))
            continue;
        if (!st->textures[unit].enabled ||
            nrb_texture_preflight(
                b, &st->textures[unit], rt, depth, unit) != 0)
            return st->textures[unit].enabled
                ? -RSX_NR_DRAW_PF_TEXTURE_INVALID
                : -RSX_NR_DRAW_PF_TEXTURE_DISABLED;
        if (st->textures[unit].cubemap)
            cube_mask |= 1u << unit;
    }
    rsx_vertex_layout_plan layout;
    u32 input_mask = st->vertex_program.attrib_input_mask;
    if (!input_mask)
        input_mask = 1u;
    rsx_vertex_layout_plan_init(&layout, input_mask);
    rsx_dsp_vertex_attr attrs[RSX_NIR_NUM_VERTEX_ATTR];
    float defaults[RSX_NIR_NUM_VERTEX_ATTR][4];
    for (u32 i = 0; i < RSX_NIR_NUM_VERTEX_ATTR; ++i) {
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
            RSX_PULL_TYPES_ALL))
        return -RSX_NR_DRAW_PF_VERTEX_PLAN;
    const int expand = nrb_needs_expansion(d->primitive);
    const int filter_restart =
        d->indexed && st->index_binding.restart_enable;
    const int strips = d->primitive == 4u || d->primitive == 6u;
    if (!nrb_get_pso(b, st, vp_words, vp_word_count, &fp, &plan,
                     topology_type,
                     filter_restart && strips && !expand, cube_mask,
                     vtex_mask, rt->dxgi))
        return -RSX_NR_DRAW_PF_PSO;

    nrb_required_span required[NRB_MAX_REQUIRED_SPANS];
    u32 required_count = 0;
    const u64 residency_prepare_start = nrb_stall_now(b);
    const int residency_prepare = nrb_prepare_draw_residency(
        b, st, &plan, d, batches,
        d->indexed && !expand && !filter_restart,
        required, &required_count);
    nrb_stall_finish(b, residency_prepare_start,
                     &b->stats.stall_residency_prepare_count,
                     &b->stats.stall_residency_prepare_ticks);
    if (residency_prepare != 0)
        return -RSX_NR_DRAW_PF_RESIDENCY;
    /* Admission must establish the same immutable GPU-visible byte point
     * used by execution. Registering page spans alone is insufficient: a
     * guest producer can publish while the mirror copy is being recorded.
     * Repair that race before the section becomes native-owned. */
    const u64 residency_stabilize_start = nrb_stall_now(b);
    const int residency_stabilize = nrb_stabilize_required_spans(
        b, required, required_count);
    nrb_stall_finish(b, residency_stabilize_start,
                     &b->stats.stall_residency_stabilize_count,
                     &b->stats.stall_residency_stabilize_ticks);
    if (residency_stabilize != 0)
        return -RSX_NR_DRAW_PF_RESIDENCY;
    (void)expand;
    (void)filter_restart;
    if (nrb_draw_upload_budget(st, &fp, d, batches) > NRB_UPLOAD_BYTES)
        return -RSX_NR_DRAW_PF_UPLOAD_SCRATCH;
    return 0;
}

int rsx_nr_d3d12_preflight_draw(rsx_nr_d3d12* b,
                                const rsx_nir_pipeline* st,
                                const u32* vp_words, u32 vp_word_count,
                                const rsx_nir_draw* d,
                                const u32* batches)
{
    if (!b)
        return -RSX_NR_DRAW_PF_BAD_ARGUMENT;
    const u64 stall_start = nrb_stall_now(b);
    const int result = nrb_preflight_draw_impl(
        b, st, vp_words, vp_word_count, d, batches);
    nrb_stall_finish(b, stall_start,
                     &b->stats.stall_preflight_draw_count,
                     &b->stats.stall_preflight_draw_ticks);
    return result;
}

static int nrb_scaled_copy_layout(const rsx_nir_transfer* t, u32* bpp)
{
    if (!t || t->ds_dx != 0x00100000u ||
        t->dt_dy != 0x00100000u || !t->in_w || !t->in_h ||
        t->in_w != t->out_w || t->in_h != t->out_h ||
        t->in_x || t->in_y || t->out_x || t->out_y ||
        t->clip_x || t->clip_y || t->clip_w != t->out_w ||
        t->clip_h != t->out_h)
        return -1;

    /* NV3089 source and NV3062 destination use different enum domains.
     * These pairs are representation-identical and need no conversion. */
    if ((t->src_format == 3u || t->src_format == 4u) &&
        t->dst_format == 10u)
        *bpp = 4u; /* A/X8R8G8B8 -> A8R8G8B8 */
    else if (t->src_format == 7u && t->dst_format == 4u)
        *bpp = 2u; /* R5G6B5 -> R5G6B5 */
    else
        return -1;
    return t->src_pitch >= t->in_w * *bpp &&
           t->dst_pitch >= t->out_w * *bpp ? 0 : -1;
}

int rsx_nr_d3d12_preflight_transfer(rsx_nr_d3d12* b,
                                    const rsx_nir_pipeline* st,
                                    const rsx_nir_transfer* t,
                                    const u32* words)
{
    (void)st;
    if (!b || !t || !b->writable_ptr)
        return -1;
    switch (t->kind) {
    case RSX_NIR_XFER_BUFFER: {
        if (t->src_format > 1u || t->dst_format > 1u ||
            !t->line_length || !t->line_count)
            return -1;
        const u64 src_size = (u64)t->src_pitch * (t->line_count - 1u) +
                             t->line_length;
        const u64 dst_size = (u64)t->dst_pitch * (t->line_count - 1u) +
                             t->line_length;
        if (src_size > UINT32_MAX || dst_size > UINT32_MAX ||
            !b->guest_ptr(b->guest_user, t->src_location, t->src_offset,
                          (u32)src_size) ||
            !b->writable_ptr(b->guest_user, t->dst_location, t->dst_offset,
                             (u32)dst_size))
            return -1;
        return 0;
    }
    case RSX_NIR_XFER_INLINE: {
        const u64 bytes = (u64)t->point_y * t->dst_pitch +
                          (u64)(t->point_x + t->word_count) * 4u;
        return words && t->word_count && bytes <= UINT32_MAX &&
               b->writable_ptr(b->guest_user, t->dst_location,
                               t->dst_offset, (u32)bytes) ? 0 : -1;
    }
    case RSX_NIR_XFER_SCALED: {
        u32 bpp = 0;
        if (nrb_scaled_copy_layout(t, &bpp) != 0)
            return -1;
        const u64 row_bytes = (u64)t->out_w * bpp;
        const u64 src_size = (u64)(t->in_h - 1u) * t->src_pitch + row_bytes;
        const u64 dst_size =
            (u64)(t->out_h - 1u) * t->dst_pitch + row_bytes;
        if (src_size > UINT32_MAX || dst_size > UINT32_MAX ||
            !b->guest_ptr(b->guest_user, t->src_location, t->src_offset,
                          (u32)src_size) ||
            !b->writable_ptr(b->guest_user, t->dst_location, t->dst_offset,
                             (u32)dst_size))
            return -1;
        return 0;
    }
    default:
        return -1;
    }
}

int rsx_nr_d3d12_preflight_present(rsx_nr_d3d12* b, u32 buffer)
{
    if (!b)
        return -1;
    nrb_rt* scanout = nrb_display_rt(b, buffer);
    if (!scanout)
        scanout = b->last_rt;
    return b->present_cb && !scanout ? -1 : 0;
}

static void nrb_hana_condition_note(rsx_nr_d3d12* b,
                                    const rsx_nir_pipeline* st,
                                    u32 value)
{
    if (!b->hana_input_oracle)
        return;

    nrb_hana_condition_key key;
    memset(&key, 0, sizeof(key));
    key.dma_report = st->render_condition.dma_report;
    key.report_offset = st->render_condition.offset;
    key.observed_value = value;
    key.fp_location = st->fragment_program.location;
    key.fp_offset = st->fragment_program.offset;
    key.fp_control = st->fragment_program.control;
    key.vp_start = st->vertex_program.start_slot;
    key.vp_branch_bits = st->vertex_program.branch_bits;
    key.color_location = st->surface.color_location[0];
    key.color_offset = st->surface.color_offset[0];
    key.color_format = st->surface.color_format;
    key.depth_location = st->surface.zeta_location;
    key.depth_offset = st->surface.zeta_offset;
    key.depth_format = st->surface.depth_format;
    key.depth_test = st->depth_stencil.depth_test_enable;
    key.depth_write = st->depth_stencil.depth_write_enable;
    key.blend_enable = st->blend.blend_enable;
    key.alpha_test = st->blend.alpha_test_enable;

    b->hana_condition_total++;
    for (u32 i = 0; i < b->hana_condition_count; ++i) {
        nrb_hana_condition_key* const found = &b->hana_condition[i];
        /* The report slot identifies the query/object; program offsets vary
         * across its material passes and would otherwise consume one table
         * slot per draw. Keep the most recent program identity as evidence,
         * but aggregate by the condition, target and relevant fixed state. */
        if (found->dma_report == key.dma_report &&
            found->report_offset == key.report_offset &&
            found->observed_value == key.observed_value &&
            found->vp_start == key.vp_start &&
            found->vp_branch_bits == key.vp_branch_bits &&
            found->color_location == key.color_location &&
            found->color_offset == key.color_offset &&
            found->color_format == key.color_format &&
            found->depth_location == key.depth_location &&
            found->depth_offset == key.depth_offset &&
            found->depth_format == key.depth_format &&
            found->depth_test == key.depth_test &&
            found->depth_write == key.depth_write &&
            found->blend_enable == key.blend_enable &&
            found->alpha_test == key.alpha_test) {
            found->attempts++;
            found->skipped += value == 0u;
            found->fp_location = key.fp_location;
            found->fp_offset = key.fp_offset;
            found->fp_control = key.fp_control;
            return;
        }
    }
    nrb_hana_condition_key* added = NULL;
    if (b->hana_condition_count == NRB_HANA_CONDITION_KEYS) {
        added = &b->hana_condition[
            b->hana_condition_replace++ % NRB_HANA_CONDITION_KEYS];
        b->hana_condition_overflow++;
    } else {
        added = &b->hana_condition[b->hana_condition_count++];
    }
    *added = key;
    added->attempts = 1u;
    added->skipped = value == 0u;
}

static int nrb_draw_impl(void* user, const rsx_nir_pipeline* st,
                         const u32* vp_words, u32 vp_word_count,
                         const rsx_nir_draw* d, const u32* batches)
{
    rsx_nr_d3d12* b = user;

    /* Evaluate as late as possible, before allocating/opening/recording any
     * draw work. Preflight proved only that the exact report address is
     * resolvable; a preceding report action or guest writer may have changed
     * its value since then. A false condition consumes this draw without
     * side effects, exactly as NV4097 conditional rendering does. */
    if (st->render_condition.enabled) {
        u32 value = 0;
        if (!b->render_condition_read ||
            b->render_condition_read(
                b->render_condition_user,
                st->render_condition.dma_report,
                st->render_condition.offset, &value) != 0)
            return -1;
        nrb_hana_condition_note(b, st, value);
        if (!value) {
            b->stats.conditional_draws_skipped++;
            return 0;
        }
    }

    D3D12_PRIMITIVE_TOPOLOGY topo;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE tt;
    if (nrb_topology(d->primitive, &topo, &tt) != 0) {
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_topology++;
        if (d->primitive < 16u)
            b->stats.unsup_topology_id[d->primitive]++;
        return -1;
    }
    const int strips = d->primitive == 4 || d->primitive == 6;
    const int expand_primitive = nrb_needs_expansion(d->primitive);
    const int combine_triangle_strip = d->primitive == 6u;
    const int filter_restart =
        d->indexed && st->index_binding.restart_enable;
    const int use_host_ib = expand_primitive || filter_restart;
    if (!d->batch_count || d->batch_count > NRB_MAX_DRAW_BATCHES) {
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_index++;
        return -1;
    }
    if (combine_triangle_strip)
        topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    nrb_rt* rt = nrb_rt_from_state(b, st, 1);
    if (!rt) {
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_rt++;
        if (st->surface.color_format < 32u)
            b->stats.unsup_rt_format[st->surface.color_format]++;
        return -1;
    }
    const int depth_active = st->depth_stencil.depth_test_enable ||
                             st->depth_stencil.stencil_test_enable;
    nrb_depth* depth = depth_active ? nrb_depth_from_state(b, st, 1) : NULL;
    if (depth_active && !depth) {
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_rt++;
        return -1;
    }
    if (!nrb_stencil_state_supported(&st->depth_stencil)) {
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_pso++;
        return -1;
    }
    if (!nrb_depth_bounds_state_supported(b, &st->depth_stencil)) {
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_pso++;
        return -1;
    }

    nrb_fp_info fp;
    const u64 fp_resolve_start = nrb_stall_now(b);
    const int fp_resolve = nrb_resolve_fp(b, st, &fp);
    nrb_stall_finish(b, fp_resolve_start,
                     &b->stats.stall_fp_resolve_count,
                     &b->stats.stall_fp_resolve_ticks);
    if (fp_resolve != 0 || fp.unsupported) {
        nrb_note_first_fp_failure(
            b, fp_resolve != 0 ? 1u : 2u, fp_resolve, st, &fp);
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_fp++;
        return -1;
    }
    u32 cube_mask = 0;
    for (u32 unit = 0; unit < NRB_TEX_UNITS; unit++) {
        if (!(fp.texture_mask & (1u << unit)))
            continue;
        if (!st->textures[unit].enabled) {
            nrb_note_first_texture_failure(
                b, 1u, unit, -1, fp.texture_mask,
                &st->textures[unit]);
            b->stats.unsupported_draws++;
            b->stats.unsup_draw_texture++;
            b->stats.texture_failures++;
            return -1;
        }
        if (st->textures[unit].cubemap)
            cube_mask |= 1u << unit;
    }
    u32 vtex_mask = 0;
    for (u32 unit = 0; unit < NRB_VTEX_UNITS; ++unit)
        if (st->vertex_textures[unit].enabled)
            vtex_mask |= 1u << unit;
    if (vp_word_count && !nrb_vp_program_supported(
            b, vp_words, vp_word_count, vtex_mask,
            st->vertex_program.start_slot)) {
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_pso++;
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
        nrb_note_first_residency_failure(b, 1u, 0u, NULL, 0u);
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_plan++;
        return -1;
    }

    const u64 pso_lookup_start = nrb_stall_now(b);
    ID3D12PipelineState* pso = nrb_get_pso(
        b, st, vp_words, vp_word_count, &fp, &plan, tt,
        filter_restart && strips && !expand_primitive, cube_mask, vtex_mask,
        rt->dxgi);
    nrb_stall_finish(b, pso_lookup_start,
                     &b->stats.stall_pso_lookup_count,
                     &b->stats.stall_pso_lookup_ticks);
    if (!pso) {
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_pso++;
        return -1;
    }

    const u64 upload_budget = nrb_draw_upload_budget(st, &fp, d, batches);
    if (nrb_ensure_draw_upload_capacity(b, upload_budget) != 0) {
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_index++;
        return -1;
    }

    nrb_required_span required[NRB_MAX_REQUIRED_SPANS];
    u32 required_count = 0;
    const u64 residency_prepare_start = nrb_stall_now(b);
    const int residency = nrb_prepare_draw_residency(
        b, st, &plan, d, batches, d->indexed && !use_host_ib,
        required, &required_count);
    nrb_stall_finish(b, residency_prepare_start,
                     &b->stats.stall_residency_prepare_count,
                     &b->stats.stall_residency_prepare_ticks);
    if (residency != 0) {
        nrb_note_first_residency_failure(
            b, 2u, (u32)(residency < 0 ? -residency : residency),
            required, required_count);
        b->stats.unsupported_draws++;
        if (residency == -2)
            b->stats.unsup_draw_index++;
        else
            b->stats.unsup_draw_plan++;
        b->stats.residency_failures++;
        return -1;
    }
    if (b->force_draw_input_refresh) {
        for (u32 i = 0; i < required_count; ++i) {
            const u32 space = required[i].space;
            const u32 first = required[i].offset >> RSX_GUEST_PAGE_SHIFT;
            const u32 last = (u32)(((u64)required[i].offset +
                                    required[i].size - 1u) >>
                                   RSX_GUEST_PAGE_SHIFT);
            for (u32 page = first; page <= last; ++page) {
                if (b->force_draw_input_page_epoch[space][page] ==
                    b->force_draw_input_epoch)
                    continue;
                b->force_draw_input_page_epoch[space][page] =
                    b->force_draw_input_epoch;
                rsx_guest_pages_note_write(
                    &b->pages, space, page << RSX_GUEST_PAGE_SHIFT,
                    RSX_GUEST_PAGE_SIZE);
                b->stats.forced_draw_input_refreshes++;
            }
        }
    }
    const u64 residency_stabilize_start = nrb_stall_now(b);
    const int residency_stabilize = nrb_stabilize_required_spans(
        b, required, required_count);
    nrb_stall_finish(b, residency_stabilize_start,
                     &b->stats.stall_residency_stabilize_count,
                     &b->stats.stall_residency_stabilize_ticks);
    if (residency_stabilize != 0) {
        nrb_note_first_residency_failure(
            b, 3u, 0u, required, required_count);
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_plan++;
        b->stats.residency_failures++;
        return -1;
    }

    nrb_rt* texture_aliases[NRB_TEX_UNITS] = {0};
    nrb_depth* texture_depth_aliases[NRB_TEX_UNITS] = {0};
    nrb_rt* vtex_aliases[NRB_VTEX_UNITS] = {0};
    nrb_depth* vtex_depth_aliases[NRB_VTEX_UNITS] = {0};
    u32 resolved_cube_mask = 0;
    u32 descriptor_table_index = 0;
    const u64 texture_prepare_start = nrb_stall_now(b);
    const int texture_prepare = nrb_prepare_textures(
        b, st, fp.texture_mask, vtex_mask, rt, depth,
        texture_aliases, texture_depth_aliases,
        vtex_aliases, vtex_depth_aliases,
        &resolved_cube_mask, &descriptor_table_index);
    nrb_stall_finish(b, texture_prepare_start,
                     &b->stats.stall_texture_prepare_count,
                     &b->stats.stall_texture_prepare_ticks);
    if (texture_prepare != 0 ||
        resolved_cube_mask != cube_mask) {
        if (texture_prepare == 0)
            nrb_note_first_texture_failure(
                b, 8u, ~0u, -1, fp.texture_mask, NULL);
        nrb_restore_texture_aliases(
            b, texture_aliases, texture_depth_aliases,
            vtex_aliases, vtex_depth_aliases);
        nrb_exec_wait(
            b, RSX_NR_D3D12_SUBMIT_REFUSAL_RETIREMENT, 0u);
        b->stats.unsupported_draws++;
        b->stats.unsup_draw_texture++;
        b->stats.texture_failures++;
        return -1;
    }

    nrb_hana_input_capture(
        b, st, &fp, vp_words, vp_word_count, vtex_mask,
        required, required_count, d, texture_depth_aliases);

    /* A newly uploaded texture can consume the headroom reserved above.
     * Retire its side-effect-free preparation, then rebuild the now-cached
     * descriptor table on a fresh list before allocating this draw. */
    if ((u64)b->upload_used + upload_budget > NRB_UPLOAD_BYTES) {
        nrb_restore_texture_aliases(
            b, texture_aliases, texture_depth_aliases,
            vtex_aliases, vtex_depth_aliases);
        if (nrb_exec_wait(b, RSX_NR_D3D12_SUBMIT_UPLOAD_ROLLOVER, 0u) ||
            nrb_open_list(b)) {
            nrb_note_first_texture_failure(
                b, 9u, ~0u, -1, fp.texture_mask, NULL);
            b->stats.unsupported_draws++;
            b->stats.unsup_draw_texture++;
            b->stats.texture_failures++;
            return -1;
        }
        b->stats.upload_rollovers++;
        memset(texture_aliases, 0, sizeof(texture_aliases));
        memset(texture_depth_aliases, 0, sizeof(texture_depth_aliases));
        memset(vtex_aliases, 0, sizeof(vtex_aliases));
        memset(vtex_depth_aliases, 0, sizeof(vtex_depth_aliases));
        resolved_cube_mask = 0;
        descriptor_table_index = 0;
        const u64 retry_texture_prepare_start = nrb_stall_now(b);
        const int retry_texture_prepare = nrb_prepare_textures(
            b, st, fp.texture_mask, vtex_mask, rt, depth,
            texture_aliases, texture_depth_aliases,
            vtex_aliases, vtex_depth_aliases,
            &resolved_cube_mask, &descriptor_table_index);
        nrb_stall_finish(b, retry_texture_prepare_start,
                         &b->stats.stall_texture_prepare_count,
                         &b->stats.stall_texture_prepare_ticks);
        if (retry_texture_prepare != 0 ||
            resolved_cube_mask != cube_mask ||
            (u64)b->upload_used + upload_budget > NRB_UPLOAD_BYTES) {
            nrb_restore_texture_aliases(
                b, texture_aliases, texture_depth_aliases,
                vtex_aliases, vtex_depth_aliases);
            nrb_exec_wait(
                b, RSX_NR_D3D12_SUBMIT_REFUSAL_RETIREMENT, 0u);
            b->stats.unsupported_draws++;
            b->stats.unsup_draw_texture++;
            b->stats.texture_failures++;
            return -1;
        }
    }

    /* Refusal-safe reservation: finish every host index read/expansion and
     * every upload-ring allocation before binding a target or recording the
     * first draw. In particular, an unreadable later batch can no longer
     * leave an earlier batch rendered before the action reports fallback. */
    const u32 source = (d->indexed && !use_host_ib)
                           ? (st->index_binding.is_u32
                                  ? RSX_PULL_SOURCE_INDEX_U32
                                  : RSX_PULL_SOURCE_INDEX_U16)
                           : RSX_PULL_SOURCE_ARRAYS;
    int prepare_failed = 0;
    int prepare_index_failed = 0;
    u32 prepare_upload_stage = 0;
    u32 prepare_upload_request = 0;
    const u32 prepared_batch_count = combine_triangle_strip
        ? 1u : d->batch_count;
    const u64 batch_prepare_start = nrb_stall_now(b);
    memset(b->prepared_batches, 0,
           prepared_batch_count * sizeof(b->prepared_batches[0]));
    for (u32 bi = 0; bi < prepared_batch_count; ++bi) {
        const u32 first = batches[bi * 2u];
        const u32 count = batches[bi * 2u + 1u];
        nrb_prepared_batch* prepared = &b->prepared_batches[bi];
        prepared->draw_count = count;

        if (use_host_ib) {
            const u32 n = combine_triangle_strip
                ? nrb_expand_triangle_strip_batches(b, st, d, batches)
                : expand_primitive
                ? nrb_expand_primitives(b, st, d, first, count)
                : nrb_read_indices(b, st, first, count, strips);
            if (n == ~0u) {
                prepare_failed = 1;
                prepare_index_failed = 1;
                break;
            }
            if (!n) {
                prepared->skip = 1;
                continue;
            }
            u8* index_bytes = nrb_upload_alloc(
                b, n * sizeof(u32), &prepared->index_va);
            if (!index_bytes) {
                prepare_failed = 1;
                prepare_upload_stage = 1u;
                prepare_upload_request = n * sizeof(u32);
                break;
            }
            memcpy(index_bytes, b->idx_scratch, (size_t)n * sizeof(u32));
            prepared->draw_count = n;
        }

        rsx_vertex_pull_constants pull;
        /* NV4097_VERTEX_DATA_BASE_INDEX applies to indexed fetches.  The
         * proven legacy decoder deliberately passes zero for DRAW_ARRAYS;
         * carrying the stale indexed base into an array draw shifts every
         * pulled attribute and corrupts otherwise valid live batches. */
        const u32 pull_base_index =
            d->indexed ? st->vertex_bindings.base_index : 0u;
        rsx_vertex_pull_fill_constants(
            &plan, pull_base_index,
            use_host_ib ? 0u : first, source,
            st->index_binding.offset, st->index_binding.location,
            rsx_gpu_mirror_d3d12_buffer_size(b->mirror_be, 0),
            rsx_gpu_mirror_d3d12_buffer_size(b->mirror_be, 1), &pull);
        u8* pull_bytes = nrb_upload_alloc(
            b, sizeof(pull), &prepared->pull_va);
        if (!pull_bytes) {
            prepare_failed = 1;
            prepare_upload_stage = 2u;
            prepare_upload_request = sizeof(pull);
            break;
        }
        memcpy(pull_bytes, &pull, sizeof(pull));
    }
    nrb_stall_finish(b, batch_prepare_start,
                     &b->stats.stall_batch_prepare_count,
                     &b->stats.stall_batch_prepare_ticks);
    if (prepare_failed) {
        if (prepare_index_failed)
            b->stats.unsup_draw_index++;
        if (prepare_upload_stage)
            nrb_note_draw_upload_failure(
                b, prepare_upload_stage, upload_budget,
                prepare_upload_request, prepared_batch_count);
        nrb_restore_texture_aliases(
            b, texture_aliases, texture_depth_aliases,
            vtex_aliases, vtex_depth_aliases);
        nrb_exec_wait(
            b, RSX_NR_D3D12_SUBMIT_REFUSAL_RETIREMENT, 0u);
        b->stats.unsupported_draws++;
        return -1;
    }

    const u64 command_record_start = nrb_stall_now(b);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = nrb_rt_handle(b, rt);
    nrb_rt_transition(b, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (depth) {
        nrb_depth_transition(b, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = nrb_depth_handle(b, depth);
        b->list->lpVtbl->OMSetRenderTargets(b->list, 1, &rtv, FALSE, &dsv);
    } else {
        b->list->lpVtbl->OMSetRenderTargets(b->list, 1, &rtv, FALSE, NULL);
    }

    /* The generated NV40 vertex-program epilogue already applies the guest
     * viewport scale/translate and produces final D3D clip coordinates. The
     * proven legacy renderer therefore rasterizes over the complete surface.
     * Applying the RSX x/y/w/h again here double-transformed live geometry
     * and could clip every world draw out of the target. */
    D3D12_VIEWPORT vp = {0};
    vp.Width = (float)rt->w;
    vp.Height = (float)rt->h;
    vp.MaxDepth = 1.0f;
    b->list->lpVtbl->RSSetViewports(b->list, 1, &vp);
    D3D12_RECT sc;
    sc.left = (LONG)st->scissor.x;
    sc.top = (LONG)st->scissor.y;
    sc.right = (LONG)(st->scissor.w ? st->scissor.x + st->scissor.w : rt->w);
    sc.bottom = (LONG)(st->scissor.h ? st->scissor.y + st->scissor.h : rt->h);
    if (sc.left < 0) sc.left = 0;
    if (sc.top < 0) sc.top = 0;
    if (sc.right > (LONG)rt->w) sc.right = (LONG)rt->w;
    if (sc.bottom > (LONG)rt->h) sc.bottom = (LONG)rt->h;
    if (sc.right < sc.left) sc.right = sc.left;
    if (sc.bottom < sc.top) sc.bottom = sc.top;
    b->list->lpVtbl->RSSetScissorRects(b->list, 1, &sc);
    if (b->depth_bounds_supported && b->list1) {
        float zmin = 0.0f;
        float zmax = 1.0f;
        if (st->depth_stencil.depth_bounds_test_enable) {
            memcpy(&zmin, &st->depth_stencil.depth_bounds_min, sizeof(zmin));
            memcpy(&zmax, &st->depth_stencil.depth_bounds_max, sizeof(zmax));
        }
        b->list1->lpVtbl->OMSetDepthBounds(b->list1, zmin, zmax);
    }

    ID3D12DescriptorHeap* descriptor_heaps[2] = {
        b->texture_gpu_heap, b->sampler_gpu_heap
    };
    b->list->lpVtbl->SetDescriptorHeaps(b->list, 2, descriptor_heaps);
    b->list->lpVtbl->SetGraphicsRootSignature(b->list, b->rootsig);
    const u32 srv_table_base =
        descriptor_table_index * NRB_SRV_TABLE_STRIDE;
    const u32 sampler_table_base =
        descriptor_table_index * NRB_SAMPLER_TABLE_STRIDE;
    b->list->lpVtbl->SetGraphicsRootDescriptorTable(
        b->list, 5, nrb_texture_gpu_handle(b, srv_table_base));
    b->list->lpVtbl->SetGraphicsRootDescriptorTable(
        b->list, 6, nrb_sampler_gpu_handle(b, sampler_table_base));
    b->list->lpVtbl->SetGraphicsRootDescriptorTable(
        b->list, 7,
        nrb_texture_gpu_handle(b, srv_table_base + NRB_TEX_UNITS));
    b->list->lpVtbl->SetPipelineState(b->list, pso);
    b->list->lpVtbl->IASetPrimitiveTopology(b->list, topo);

    /* b0: transform constants + the RSX viewport mapping consumed by the
     * generated VP epilogue. */
    u64 const_va = 0;
    const u32 vp_cb_bytes = sizeof(st->constants) + 12u * sizeof(float);
    u8* cp = nrb_upload_alloc(b, vp_cb_bytes, &const_va);
    if (!cp) {
        nrb_note_draw_upload_failure(
            b, 3u, upload_budget, vp_cb_bytes, prepared_batch_count);
        nrb_restore_texture_aliases(
            b, texture_aliases, texture_depth_aliases,
            vtex_aliases, vtex_depth_aliases);
        nrb_exec_wait(
            b, RSX_NR_D3D12_SUBMIT_REFUSAL_RETIREMENT, 0u);
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
    u32* branch_words = (u32*)(xf + 8u);
    branch_words[0] = st->vertex_program.branch_bits;
    branch_words[1] = 0u;
    branch_words[2] = 0u;
    branch_words[3] = 0u;
    b->list->lpVtbl->SetGraphicsRootConstantBufferView(b->list, 0, const_va);

    /* b1 (pixel): exact inline CONST words plus dynamic alpha reference.
     * A one-slot placeholder preserves the HLSL layout for no-CONST FPs. */
    const u32 fp_slots = fp.constants.count ? fp.constants.count : 1u;
    const u32 fp_cb_bytes = (fp_slots + 1u) * 16u;
    u64 fp_va = 0;
    u8* fp_cp = nrb_upload_alloc(b, fp_cb_bytes, &fp_va);
    if (!fp_cp) {
        nrb_note_draw_upload_failure(
            b, 4u, upload_budget, fp_cb_bytes, prepared_batch_count);
        nrb_restore_texture_aliases(
            b, texture_aliases, texture_depth_aliases,
            vtex_aliases, vtex_depth_aliases);
        nrb_exec_wait(
            b, RSX_NR_D3D12_SUBMIT_REFUSAL_RETIREMENT, 0u);
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

    /* Restart draws and non-native RSX primitive shapes go through a
     * host-built u32 index buffer. Triangle strips are expanded across the
     * complete BEGIN/END action so DRAW_* batch boundaries do not introduce
     * false cuts; loops/fans/quads are expanded exactly as well. The shader
     * then runs the ARRAYS source with first = 0, so SV_VertexID is the
     * fetched index and base_index still applies in-shader. */
    for (u32 bi = 0; bi < prepared_batch_count; bi++) {
        const nrb_prepared_batch* prepared = &b->prepared_batches[bi];
        if (prepared->skip) {
            b->stats.draw_batches++;
            continue;                /* batch was restart cuts only       */
        }
        if (use_host_ib) {
            D3D12_INDEX_BUFFER_VIEW ibv;
            ibv.BufferLocation = prepared->index_va;
            ibv.SizeInBytes = prepared->draw_count * sizeof(u32);
            ibv.Format = DXGI_FORMAT_R32_UINT;
            b->list->lpVtbl->IASetIndexBuffer(b->list, &ibv);
        }
        b->list->lpVtbl->SetGraphicsRootConstantBufferView(b->list, 1,
                                                           prepared->pull_va);
        if (use_host_ib)
            b->list->lpVtbl->DrawIndexedInstanced(
                b->list, prepared->draw_count, 1, 0, 0, 0);
        else
            b->list->lpVtbl->DrawInstanced(
                b->list, prepared->draw_count, 1, 0, 0);
        b->stats.draw_batches++;
    }

    if (depth && st->depth_stencil.depth_write_enable) {
        depth->sample_valid = 0;
        depth->write_generation++;
    }

    nrb_restore_texture_aliases(
        b, texture_aliases, texture_depth_aliases,
        vtex_aliases, vtex_depth_aliases);
    if (filter_restart)
        b->stats.restart_draws++;
    b->stats.real_fp_draws++;
    if (fp.texture_mask || vtex_mask)
        b->stats.texture_draws++;
    if (b->scanout_provenance)
        rt->draw_writes++;
    nrb_note_rt_write(b, rt);
    b->stats.draws++;
    nrb_stall_finish(b, command_record_start,
                     &b->stats.stall_command_record_count,
                     &b->stats.stall_command_record_ticks);
    return 0;
}

static int nrb_draw(void* user, const rsx_nir_pipeline* st,
                    const u32* vp_words, u32 vp_word_count,
                    const rsx_nir_draw* d, const u32* batches)
{
    rsx_nr_d3d12* const b = (rsx_nr_d3d12*)user;
    const u64 stall_start = nrb_stall_now(b);
    const int result = nrb_draw_impl(
        user, st, vp_words, vp_word_count, d, batches);
    nrb_stall_finish(b, stall_start,
                     &b->stats.stall_draw_count,
                     &b->stats.stall_draw_ticks);
    return result;
}

static void nrb_publish_guest_write(rsx_nr_d3d12* b, u32 space,
                                    u32 offset, u32 size)
{
    if (!size)
        return;
    if (b->publish_write)
        b->publish_write(b->publish_write_user, space, offset, size);
    else
        rsx_guest_pages_note_write(&b->pages, space, offset, size);
}

static int nrb_transfer_rt_format_ok(const nrb_rt* rt)
{
    return rt && (rt->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM ||
                  rt->dxgi == DXGI_FORMAT_B8G8R8A8_UNORM);
}

/* Locate the exact full-surface identity used by the capture-observed 1:1
 * transfer. Source selection follows the latest GPU writer. Destination
 * selection also permits an allocated-but-unwritten exact identity because
 * the transfer itself becomes its first writer. A same-address incompatible
 * identity is an explicit mismatch rather than a stale guest-memory copy. */
static nrb_rt* nrb_transfer_rt(rsx_nr_d3d12* b, u32 space, u32 offset,
                               u32 w, u32 h, int source, int* mismatch)
{
    nrb_rt* selected = NULL;
    int saw_address = 0;
    if (mismatch)
        *mismatch = 0;
    for (u32 i = 0; i < NRB_MAX_RTS; ++i) {
        nrb_rt* const candidate = &b->rts[i];
        if (!candidate->live || candidate->space != space ||
            candidate->offset != offset)
            continue;
        saw_address = 1;
        if (candidate->w != w || candidate->h != h ||
            !nrb_transfer_rt_format_ok(candidate) ||
            (source && !candidate->last_write_serial))
            continue;
        if (!selected || candidate->last_write_serial >
                             selected->last_write_serial)
            selected = candidate;
    }
    if (!selected && saw_address && mismatch)
        *mismatch = 1;
    return selected;
}

static void nrb_report_transfer_rt_failure(rsx_nr_d3d12* b,
                                           const char* stage,
                                           u32 space, u32 offset,
                                           u32 width, u32 height)
{
    fprintf(stderr,
            "[nrb-transfer-fatal stage=%s key=%u:%08X/%ux%u "
            "removed=%08lX candidates=",
            stage, space, offset, width, height,
            (unsigned long)b->dev->lpVtbl->GetDeviceRemovedReason(b->dev));
    u32 emitted = 0u;
    for (u32 i = 0; i < NRB_MAX_RTS && emitted < 8u; ++i) {
        const nrb_rt* const rt = &b->rts[i];
        if (!rt->live || rt->space != space || rt->offset != offset)
            continue;
        fprintf(stderr, "%s%u:f%u/%ux%u/dxgi%u/w%llu",
                emitted ? "," : "", i, rt->fmt, rt->w, rt->h,
                (u32)rt->dxgi,
                (unsigned long long)rt->last_write_serial);
        emitted++;
    }
    fprintf(stderr, "]\n");
}

static void nrb_rt_pixel_to_guest(const nrb_rt* rt, const u8* pixel,
                                  u8 guest[4])
{
    /* Guest A8R8G8B8 is byte-addressed A,R,G,B. */
    guest[0] = pixel[3];
    if (rt->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM) {
        guest[1] = pixel[0];
        guest[2] = pixel[1];
        guest[3] = pixel[2];
    } else {
        guest[1] = pixel[2];
        guest[2] = pixel[1];
        guest[3] = pixel[0];
    }
}

static void nrb_guest_pixel_to_rt(const nrb_rt* rt, const u8 guest[4],
                                  u8* pixel)
{
    if (rt->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM) {
        pixel[0] = guest[1];
        pixel[1] = guest[2];
        pixel[2] = guest[3];
    } else {
        pixel[0] = guest[3];
        pixel[1] = guest[2];
        pixel[2] = guest[1];
    }
    pixel[3] = guest[0];
}

static int nrb_rt_read_to_guest(rsx_nr_d3d12* b, nrb_rt* rt,
                                 u8* dst, u32 dst_pitch)
{
    const u32 row_pitch = (rt->w * 4u + 255u) & ~255u;
    const u64 need64 = (u64)row_pitch * rt->h;
    if (!dst || dst_pitch < rt->w * 4u || need64 > UINT32_MAX)
        return -1;
    const u64 attribution_start = nrb_submit_now(b);
    const u64 stall_start = nrb_stall_now(b);
    const u32 need = (u32)need64;
    if (!b->readback || b->readback_size < need) {
        if (b->readback)
            b->readback->lpVtbl->Release(b->readback);
        b->readback = nrb_make_buffer(b->dev, need,
                                      D3D12_HEAP_TYPE_READBACK,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
        if (!b->readback) {
            b->readback_size = 0u;
            return -1;
        }
        b->readback_size = need;
    }
    if (nrb_open_list(b))
        return -1;
    nrb_rt_transition(b, rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source = {0};
    source.pResource = rt->tex;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION target = {0};
    target.pResource = b->readback;
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint.Footprint.Format = rt->dxgi;
    target.PlacedFootprint.Footprint.Width = rt->w;
    target.PlacedFootprint.Footprint.Height = rt->h;
    target.PlacedFootprint.Footprint.Depth = 1u;
    target.PlacedFootprint.Footprint.RowPitch = row_pitch;
    b->list->lpVtbl->CopyTextureRegion(
        b->list, &target, 0u, 0u, 0u, &source, NULL);
    nrb_rt_transition(b, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (nrb_exec_wait(
            b, RSX_NR_D3D12_SUBMIT_TRANSFER_READBACK, need64))
        return -1;

    u8* mapped = NULL;
    D3D12_RANGE read_range = {0u, need};
    if (FAILED(b->readback->lpVtbl->Map(
            b->readback, 0u, &read_range, (void**)&mapped)))
        return -1;
    for (u32 y = 0; y < rt->h; ++y) {
        const u8* const source_row = mapped + (size_t)y * row_pitch;
        u8* const target_row = dst + (size_t)y * dst_pitch;
        for (u32 x = 0; x < rt->w; ++x)
            nrb_rt_pixel_to_guest(
                rt, source_row + (size_t)x * 4u,
                target_row + (size_t)x * 4u);
    }
    D3D12_RANGE no_write = {0u, 0u};
    b->readback->lpVtbl->Unmap(b->readback, 0u, &no_write);
    b->stats.transfer_gpu_readbacks++;
    if (b->stall_aggregate)
        b->stats.stall_transfer_readback_bytes += need64;
    nrb_stall_finish(b, stall_start,
                     &b->stats.stall_transfer_readback_count,
                     &b->stats.stall_transfer_readback_ticks);
    nrb_submit_transfer_finish(b, attribution_start, 0, need64);
    return 0;
}

static int nrb_rt_upload_from_guest(rsx_nr_d3d12* b, nrb_rt* rt,
                                    const u8* src, u32 src_pitch)
{
    const u32 row_pitch = (rt->w * 4u + 255u) & ~255u;
    const u64 need64 = (u64)row_pitch * rt->h;
    if (!src || src_pitch < rt->w * 4u || need64 > UINT32_MAX)
        return -1;
    const u64 attribution_start = nrb_submit_now(b);
    const u64 stall_start = nrb_stall_now(b);
    u64 upload_offset = 0u;
    u8* const upload = nrb_texture_upload_slice(
        b, (u32)need64, &upload_offset);
    if (!upload)
        return -1;
    for (u32 y = 0; y < rt->h; ++y) {
        const u8* const source_row = src + (size_t)y * src_pitch;
        u8* const target_row = upload + (size_t)y * row_pitch;
        for (u32 x = 0; x < rt->w; ++x)
            nrb_guest_pixel_to_rt(
                rt, source_row + (size_t)x * 4u,
                target_row + (size_t)x * 4u);
    }
    if (nrb_open_list(b))
        return -1;
    D3D12_TEXTURE_COPY_LOCATION source = {0};
    source.pResource = b->upload;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = upload_offset;
    source.PlacedFootprint.Footprint.Format = rt->dxgi;
    source.PlacedFootprint.Footprint.Width = rt->w;
    source.PlacedFootprint.Footprint.Height = rt->h;
    source.PlacedFootprint.Footprint.Depth = 1u;
    source.PlacedFootprint.Footprint.RowPitch = row_pitch;
    D3D12_TEXTURE_COPY_LOCATION target = {0};
    target.pResource = rt->tex;
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    nrb_rt_transition(b, rt, D3D12_RESOURCE_STATE_COPY_DEST);
    b->list->lpVtbl->CopyTextureRegion(
        b->list, &target, 0u, 0u, 0u, &source, NULL);
    nrb_rt_transition(b, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    nrb_note_rt_write(b, rt);
    b->stats.transfer_gpu_uploads++;
    if (b->stall_aggregate)
        b->stats.stall_transfer_upload_bytes += need64;
    nrb_stall_finish(b, stall_start,
                     &b->stats.stall_transfer_upload_count,
                     &b->stats.stall_transfer_upload_ticks);
    nrb_submit_transfer_finish(b, attribution_start, 1, need64);
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
        for (u32 l = 0; l < t->line_count; l++) {
            memmove(dst + (size_t)l * t->dst_pitch,
                    src + (size_t)l * t->src_pitch, t->line_length);
            nrb_publish_guest_write(
                b, t->dst_location, t->dst_offset + l * t->dst_pitch,
                t->line_length);
        }
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
        nrb_publish_guest_write(b, t->dst_location,
                                t->dst_offset +
                                    t->point_y * t->dst_pitch +
                                    t->point_x * 4,
                                t->word_count * 4);
        break;
    }
    case RSX_NIR_XFER_SCALED: {
        /* Exact 1:1 representation-compatible copy. Scaling, clipping and
         * true format conversion remain explicit refusals. */
        u32 bpp = 0;
        if (nrb_scaled_copy_layout(t, &bpp) != 0) {
            b->stats.unsupported_transfers++;
            return -1;
        }
        const u32 row_bytes = t->out_w * bpp;
        const u32 src_size = (t->in_h - 1u) * t->src_pitch + row_bytes;
        const u32 dst_size = (t->out_h - 1u) * t->dst_pitch + row_bytes;
        const u8* src = b->guest_ptr(b->guest_user, t->src_location,
                                     t->src_offset, src_size);
        u8* dst = b->writable_ptr(b->guest_user, t->dst_location,
                                  t->dst_offset, dst_size);
        if (!src || !dst) {
            b->stats.unsupported_transfers++;
            return -1;
        }
        int src_mismatch = 0, dst_mismatch = 0;
        nrb_rt* const src_rt = bpp == 4u ? nrb_transfer_rt(
            b, t->src_location, t->src_offset, t->in_w, t->in_h,
            1, &src_mismatch) : NULL;
        nrb_rt* const dst_rt = bpp == 4u ? nrb_transfer_rt(
            b, t->dst_location, t->dst_offset, t->out_w, t->out_h,
            0, &dst_mismatch) : NULL;
        if (src_mismatch || dst_mismatch) {
            if (src_mismatch)
                nrb_report_transfer_rt_failure(
                    b, "source-shape", t->src_location, t->src_offset,
                    t->in_w, t->in_h);
            if (dst_mismatch)
                nrb_report_transfer_rt_failure(
                    b, "destination-shape", t->dst_location,
                    t->dst_offset, t->out_w, t->out_h);
            b->stats.unsupported_transfers++;
            return -1;
        }
        if (src_rt) {
            if (nrb_rt_read_to_guest(b, src_rt, dst, t->dst_pitch) != 0) {
                nrb_report_transfer_rt_failure(
                    b, "source-readback", t->src_location, t->src_offset,
                    t->in_w, t->in_h);
                b->stats.unsupported_transfers++;
                return -1;
            }
        } else {
            const int reverse = t->src_location == t->dst_location &&
                t->dst_offset > t->src_offset &&
                t->dst_offset < t->src_offset + src_size;
            for (u32 row = 0; row < t->out_h; ++row) {
                const u32 y = reverse ? t->out_h - 1u - row : row;
                memmove(dst + (size_t)y * t->dst_pitch,
                        src + (size_t)y * t->src_pitch, row_bytes);
            }
        }
        for (u32 y = 0; y < t->out_h; ++y)
            nrb_publish_guest_write(
                b, t->dst_location,
                t->dst_offset + y * t->dst_pitch, row_bytes);
        if (dst_rt && nrb_rt_upload_from_guest(
                b, dst_rt, dst, t->dst_pitch) != 0) {
            nrb_report_transfer_rt_failure(
                b, "destination-upload", t->dst_location, t->dst_offset,
                t->out_w, t->out_h);
            b->stats.unsupported_transfers++;
            return -1;
        }
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
    if (!b->shared_timeline && nrb_exec_wait(
            b, RSX_NR_D3D12_SUBMIT_PRESENT, 0u))
        return -1;                   /* offscreen: complete the frame      */
    nrb_rt* scanout = nrb_display_rt(b, buffer);
    if (!scanout)
        scanout = b->last_rt;
    if (b->present_cb) {
        if (!scanout)
            return -1;
        const u32 attributed_descriptors = b->descriptor_tables_used;
        const u32 attributed_upload = b->upload_used;
        const u64 attribution_start = nrb_submit_now(b);
        if (b->present_cb(
                b->present_user, scanout->tex, (u32)scanout->dxgi,
                scanout->w, scanout->h, buffer) != 0)
            return -1;
        if (b->shared_timeline)
            nrb_submit_finish(
                b, RSX_NR_D3D12_SUBMIT_PRESENT, attribution_start,
                attributed_descriptors, attributed_upload, 0u);
    }
    if (scanout && b->scanout_provenance)
        scanout->present_count++;
    if (b->shared_timeline) {
        /* The shared presenter appended its copy to this exact list and
         * synchronously retired/reset the generation. */
        b->list_open = 0;
        nrb_release_retired_textures(b);
        b->upload_used = 0;
        b->descriptor_tables_used = 0;
    }
    rsx_nr_res_next_frame(&b->textures);
    if ((b->textures.frame & 127u) == 0u)
        rsx_nr_res_sweep(&b->textures, 600u, nrb_release_texture, b);
    b->stats.presents++;
    return 0;
}

static rsx_nr_d3d12_submit_cause nrb_publication_cause(u32 reason)
{
    switch ((rsx_nr_flush_reason)reason) {
    case RSX_NR_FLUSH_SEMAPHORE:
        return RSX_NR_D3D12_SUBMIT_SEMAPHORE_PUBLICATION;
    case RSX_NR_FLUSH_REFERENCE:
        return RSX_NR_D3D12_SUBMIT_REFERENCE_PUBLICATION;
    case RSX_NR_FLUSH_REPORT:
        return RSX_NR_D3D12_SUBMIT_REPORT_PUBLICATION;
    case RSX_NR_FLUSH_BARRIER:
        return RSX_NR_D3D12_SUBMIT_BARRIER_PUBLICATION;
    default:
        return RSX_NR_D3D12_SUBMIT_OTHER;
    }
}

static void nrb_flush_reason(void* user, u32 reason)
{
    rsx_nr_d3d12* const b = (rsx_nr_d3d12*)user;
    const u64 stall_start = nrb_stall_now(b);
    nrb_exec_wait(b, nrb_publication_cause(reason), 0u);
    nrb_stall_finish(b, stall_start,
                     &b->stats.stall_flush_count,
                     &b->stats.stall_flush_ticks);
}

static void nrb_flush(void* user)
{
    nrb_flush_reason(user, (u32)RSX_NR_FLUSH_BARRIER);
}

static int nrb_clear_op(void* user, const rsx_nir_pipeline* st,
                        const rsx_nir_clear* clear)
{
    rsx_nr_d3d12* b = user;
    const int result = nrb_clear(user, st, clear);
    nrb_release_timeline_lease(b);
    return result;
}

static int nrb_draw_op(void* user, const rsx_nir_pipeline* st,
                       const u32* vp, u32 vp_words,
                       const rsx_nir_draw* draw, const u32* batches)
{
    rsx_nr_d3d12* b = user;
    const int result = nrb_draw(user, st, vp, vp_words, draw, batches);
    nrb_release_timeline_lease(b);
    return result;
}

static int nrb_transfer_op(void* user, const rsx_nir_pipeline* st,
                           const rsx_nir_transfer* transfer,
                           const u32* words)
{
    rsx_nr_d3d12* b = user;
    const int result = nrb_transfer(user, st, transfer, words);
    nrb_release_timeline_lease(b);
    return result;
}

static int nrb_present_op(void* user, u32 buffer)
{
    rsx_nr_d3d12* b = user;
    const int result = nrb_present(user, buffer);
    nrb_release_timeline_lease(b);
    return result;
}

void rsx_nr_d3d12_get_exec_ops(rsx_nr_d3d12* b, rsx_nr_exec_ops* out)
{
    out->user = b;
    out->clear = nrb_clear_op;
    out->draw = nrb_draw_op;
    out->transfer = nrb_transfer_op;
    out->present = nrb_present_op;
    out->flush = nrb_flush;
    out->flush_reason = nrb_flush_reason;
}

int rsx_nr_d3d12_set_live_output(rsx_nr_d3d12* b, int rgba_targets,
                                 rsx_nr_d3d12_present_fn present,
                                 void* present_user)
{
    if (!b || b->rtv_used || b->last_rt)
        return -1;
    b->rgba_targets = rgba_targets != 0;
    b->present_cb = present;
    b->present_user = present_user;
    return 0;
}

void rsx_nr_d3d12_set_display_buffer(rsx_nr_d3d12* b, u32 buffer_id,
                                     u32 location, u32 offset,
                                     u32 width, u32 height)
{
    if (!b || buffer_id >= 8u)
        return;
    nrb_display* display = &b->displays[buffer_id];
    display->location = location;
    display->offset = offset;
    display->width = width;
    display->height = height;
    display->valid = 1;
}

void rsx_nr_d3d12_set_watch_page(rsx_nr_d3d12* b,
                                 rsx_nr_d3d12_watch_page_fn watch,
                                 void* watch_user)
{
    if (!b)
        return;
    b->watch_page = watch;
    b->watch_page_user = watch_user;
}

void rsx_nr_d3d12_set_resource_broker(
    rsx_nr_d3d12* b, rsx_nr_d3d12_borrow_color_fn color,
    rsx_nr_d3d12_borrow_depth_fn depth,
    rsx_nr_d3d12_resolve_depth_sample_fn resolve_depth_sample,
    void* broker_user)
{
    if (!b || b->rtv_used || b->dsv_used)
        return;
    b->borrow_color = color;
    b->borrow_depth = depth;
    b->resolve_depth_sample = resolve_depth_sample;
    b->broker_user = broker_user;
}

void rsx_nr_d3d12_set_publish_write(
    rsx_nr_d3d12* b, rsx_nr_d3d12_publish_write_fn publish, void* user)
{
    if (!b)
        return;
    b->publish_write = publish;
    b->publish_write_user = user;
}

void rsx_nr_d3d12_set_render_condition_reader(
    rsx_nr_d3d12* b, rsx_nr_d3d12_render_condition_fn read, void* user)
{
    if (!b)
        return;
    b->render_condition_read = read;
    b->render_condition_user = user;
}

int rsx_nr_d3d12_set_shared_timeline(
    rsx_nr_d3d12* b, rsx_nr_d3d12_timeline_acquire_fn acquire,
    rsx_nr_d3d12_timeline_release_fn release,
    rsx_nr_d3d12_timeline_flush_fn flush, void* user)
{
    if (!b || !acquire || !release || !flush || b->list_open ||
        b->rtv_used || b->dsv_used || b->last_rt || b->shared_timeline)
        return -1;
    b->timeline_acquire = acquire;
    b->timeline_release = release;
    b->timeline_flush = flush;
    b->timeline_user = user;
    b->shared_timeline = 1;
    if (nrb_acquire_shared_list(b) != 0) {
        nrb_release_timeline_lease(b);
        if (b->shared_list1) {
            b->shared_list1->lpVtbl->Release(b->shared_list1);
            b->shared_list1 = NULL;
        }
        b->shared_list = NULL;
        b->shared_timeline = 0;
        b->timeline_fault = 0;
        b->timeline_acquire = NULL;
        b->timeline_release = NULL;
        b->timeline_flush = NULL;
        b->timeline_user = NULL;
        b->list = b->owned_list;
        b->list1 = b->owned_list1;
        return -1;
    }
    nrb_release_timeline_lease(b);
    return 0;
}

int rsx_nr_d3d12_shared_timeline_enabled(const rsx_nr_d3d12* b)
{
    return b && b->shared_timeline && !b->timeline_fault;
}

int rsx_nr_d3d12_set_coherent_section_mode(rsx_nr_d3d12* b, int enabled)
{
    if (!b || b->stats.pso_builds || b->stats.draws)
        return -1;
    b->coherent_vp_options = enabled
        ? RSX_VP_NATIVE_COHERENT_SECTION_FLOW_TXL : 0u;
    return 0;
}
int rsx_nr_d3d12_set_force_draw_input_refresh(rsx_nr_d3d12* b,
                                              int enabled)
{
    if (!b)
        return -1;
    if (enabled && !b->force_draw_input_allocated) {
        for (u32 space = 0; space < RSX_GUEST_NUM_SPACES; ++space) {
            if (!b->resident_page_count[space])
                continue;
            b->force_draw_input_page_epoch[space] = calloc(
                b->resident_page_count[space],
                sizeof(b->force_draw_input_page_epoch[space][0]));
            if (!b->force_draw_input_page_epoch[space]) {
                for (u32 undo = 0; undo < RSX_GUEST_NUM_SPACES; ++undo) {
                    free(b->force_draw_input_page_epoch[undo]);
                    b->force_draw_input_page_epoch[undo] = NULL;
                }
                return -1;
            }
        }
        b->force_draw_input_allocated = 1;
        b->force_draw_input_epoch = 1u;
    }
    b->force_draw_input_refresh = enabled != 0;
    return 0;
}

void rsx_nr_d3d12_begin_draw_input_refresh_section(rsx_nr_d3d12* b)
{
    if (!b || !b->force_draw_input_refresh)
        return;
    if (++b->force_draw_input_epoch == 0u) {
        for (u32 space = 0; space < RSX_GUEST_NUM_SPACES; ++space)
            if (b->force_draw_input_page_epoch[space])
                memset(b->force_draw_input_page_epoch[space], 0,
                       (size_t)b->resident_page_count[space] * sizeof(u32));
        b->force_draw_input_epoch = 1u;
    }
}
void rsx_nr_d3d12_note_guest_write(rsx_nr_d3d12* b, u32 space,
                                   u32 offset, u32 size)
{
    if (b)
        rsx_guest_pages_note_write(&b->pages, space, offset, size);
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
    nrb_enable_device_oracle();
    rsx_nr_d3d12* b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->guest_ptr = guest_ptr;
    b->writable_ptr = writable_ptr;
    b->guest_user = user;
    b->local_size = local_size;
    b->main_size = main_size;
    {
        const char* const oracle = getenv("YZ_NR_HANA_INPUT_ORACLE");
        b->hana_input_oracle = oracle && oracle[0] &&
            strcmp(oracle, "0") != 0;
    }
    {
        const char* const aggregate = getenv("YZ_NR_STALL_AGGREGATE");
        LARGE_INTEGER frequency;
        b->stall_aggregate = aggregate && strcmp(aggregate, "1") == 0;
        if (b->stall_aggregate && QueryPerformanceFrequency(&frequency))
            b->stats.stall_qpc_frequency = (u64)frequency.QuadPart;
        else
            b->stall_aggregate = 0;
    }
    {
        const char* const attribution =
            getenv("YZ_NR_SUBMIT_ATTRIBUTION");
        LARGE_INTEGER frequency;
        b->submit_attribution = attribution &&
            strcmp(attribution, "1") == 0;
        if (b->submit_attribution && QueryPerformanceFrequency(&frequency))
            b->stats.submit_attribution_qpc_frequency =
                (u64)frequency.QuadPart;
        else
            b->submit_attribution = 0;
    }

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
    /* A complete archived gameplay frame intentionally executes thousands
     * of software-rasterized shader invocations.  The private WARP oracle is
     * deterministic work, not an interactive GPU queue; exempt only that
     * owned test queue from the OS GPU timeout. Live hardware queues are
     * supplied by the host and never receive this flag. */
    if (!device)
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT;
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
    b->owned_list = b->list;
    b->list->lpVtbl->Close(b->list);
    b->fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!b->fence_event)
        goto fail;
    if (SUCCEEDED(b->list->lpVtbl->QueryInterface(
            b->list, &IID_ID3D12GraphicsCommandList1, (void**)&b->list1))) {
        b->owned_list1 = b->list1;
        D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2 = {0};
        if (SUCCEEDED(b->dev->lpVtbl->CheckFeatureSupport(
                b->dev, D3D12_FEATURE_D3D12_OPTIONS2,
                &options2, sizeof(options2))) &&
            options2.DepthBoundsTestSupported)
            b->depth_bounds_supported = 1;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = NRB_MAX_RTS;
    if (FAILED(b->dev->lpVtbl->CreateDescriptorHeap(
            b->dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&b->rtv_heap)))
        goto fail;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = NRB_MAX_DEPTHS;
    if (FAILED(b->dev->lpVtbl->CreateDescriptorHeap(
            b->dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&b->dsv_heap)))
        goto fail;
    b->rtv_size = b->dev->lpVtbl->GetDescriptorHandleIncrementSize(
        b->dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    b->dsv_size = b->dev->lpVtbl->GetDescriptorHandleIncrementSize(
        b->dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = NRB_TEX_CAP + 1u + NRB_SRV_TABLE_STRIDE;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(b->dev->lpVtbl->CreateDescriptorHeap(
            b->dev, &hd, &IID_ID3D12DescriptorHeap,
            (void**)&b->texture_cpu_heap)))
        goto fail;
    hd.NumDescriptors = NRB_SHADER_DESCRIPTORS;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(b->dev->lpVtbl->CreateDescriptorHeap(
            b->dev, &hd, &IID_ID3D12DescriptorHeap,
            (void**)&b->texture_gpu_heap)))
        goto fail;
    hd.NumDescriptors = NRB_MAX_DEPTHS * 2u;
    if (FAILED(b->dev->lpVtbl->CreateDescriptorHeap(
            b->dev, &hd, &IID_ID3D12DescriptorHeap,
            (void**)&b->depth_snapshot_heap)))
        goto fail;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    hd.NumDescriptors = NRB_SHADER_SAMPLERS;
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
     * t20/t21 mirror buffers, pixel b1 for buffered FP constants, exact
     * fragment tables t0..t15/s0..s15, and vertex textures t16..t19 with
     * the same point/clamp static sampler contract as the legacy renderer. */
    {
        D3D12_DESCRIPTOR_RANGE ranges[3];
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
        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[2].NumDescriptors = NRB_VTEX_UNITS;
        ranges[2].BaseShaderRegister = 16;
        ranges[2].OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER params[8];
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
        params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[7].DescriptorTable.NumDescriptorRanges = 1;
        params[7].DescriptorTable.pDescriptorRanges = &ranges[2];
        params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_STATIC_SAMPLER_DESC vertex_samplers[NRB_VTEX_UNITS];
        memset(vertex_samplers, 0, sizeof(vertex_samplers));
        for (u32 unit = 0; unit < NRB_VTEX_UNITS; ++unit) {
            vertex_samplers[unit].Filter =
                D3D12_FILTER_MIN_MAG_MIP_POINT;
            vertex_samplers[unit].AddressU =
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            vertex_samplers[unit].AddressV =
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            vertex_samplers[unit].AddressW =
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            vertex_samplers[unit].MaxLOD = D3D12_FLOAT32_MAX;
            vertex_samplers[unit].ShaderRegister = unit;
            vertex_samplers[unit].ShaderVisibility =
                D3D12_SHADER_VISIBILITY_VERTEX;
        }
        D3D12_ROOT_SIGNATURE_DESC rsd = {0};
        rsd.NumParameters = 8;
        rsd.pParameters = params;
        rsd.NumStaticSamplers = NRB_VTEX_UNITS;
        rsd.pStaticSamplers = vertex_samplers;
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
    if (nrb_make_depth_snapshot_pipeline(b) != 0)
        goto fail;

    b->upload = nrb_make_buffer(b->dev, NRB_UPLOAD_BYTES,
                                D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!b->upload)
        goto fail;
    D3D12_RANGE none = {0, 0};
    if (FAILED(b->upload->lpVtbl->Map(b->upload, 0, &none,
                                      (void**)&b->upload_mapped)))
        goto fail;
    if (b->hana_input_oracle) {
        b->hana_depth_readback = nrb_make_buffer(
            b->dev, NRB_HANA_DEPTH_READBACK_BYTES,
            D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_STATE_COPY_DEST);
        if (!b->hana_depth_readback)
            goto fail;
    }

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
    {
        u32 total_pages = 0;
        for (u32 space = 0; space < RSX_GUEST_NUM_SPACES; ++space) {
            const u32 count = b->pages.space[space].npages;
            b->resident_page_count[space] = count;
            b->watched_host_page_count[space] =
                (b->pages.space[space].size + 4095u) >> 12;
            total_pages += count;
            if (count) {
                b->resident_page[space] = calloc(
                    count, sizeof(b->resident_page[space][0]));
                if (!b->resident_page[space])
                    goto fail;
            }
            if (b->watched_host_page_count[space]) {
                const u32 words =
                    (b->watched_host_page_count[space] + 63u) >> 6;
                b->watched_host_page_bits[space] = calloc(
                    words, sizeof(b->watched_host_page_bits[space][0]));
                if (!b->watched_host_page_bits[space])
                    goto fail;
            }
        }
        /* One permanent mirror handle per exact 1 KiB subpage is the maximum.
         * Reserving it here proves the render-thread registration path cannot
         * allocate, reallocate, or lose a page under memory pressure. */
        if (rsx_gpu_mirror_reserve_ranges(b->mirror, total_pages) != 0)
            goto fail;
    }

    if (rsx_nr_pso_cache_init(&b->psos, NRB_PSO_CAP))
        goto fail;
    return b;

fail:
    rsx_nr_d3d12_destroy(b);
    return NULL;
}

int rsx_nr_d3d12_set_content_cache(
    rsx_nr_d3d12* b, rsx_nr_d3d12_compile_shader_fn compile_shader,
    rsx_nr_d3d12_pso_load_fn pso_load,
    rsx_nr_d3d12_pso_store_fn pso_store,
    rsx_nr_d3d12_pso_free_fn pso_free, void* user)
{
    if (!b || !compile_shader || !pso_load || !pso_store || !pso_free ||
        b->stats.pso_hits || b->stats.pso_builds ||
        b->stats.vertex_shader_builds || b->stats.pixel_shader_builds)
        return -1;
    b->compile_shader = compile_shader;
    b->pso_load = pso_load;
    b->pso_store = pso_store;
    b->pso_free = pso_free;
    b->content_cache_user = user;
    return 0;
}

static void nrb_hana_input_dump(rsx_nr_d3d12* b)
{
    if (!b->hana_input_oracle || b->hana_input_dumped)
        return;
    b->hana_input_dumped = 1;
    u8* depth_bytes = NULL;
    HRESULT depth_map = E_FAIL;
    if (b->hana_depth_readback) {
        D3D12_RANGE read = {0, NRB_HANA_DEPTH_READBACK_BYTES};
        depth_map = b->hana_depth_readback->lpVtbl->Map(
            b->hana_depth_readback, 0, &read, (void**)&depth_bytes);
    }
    const u32 count = b->hana_input_writes < NRB_HANA_INPUT_SAMPLES
        ? b->hana_input_writes : NRB_HANA_INPUT_SAMPLES;
    const u32 start = b->hana_input_writes > NRB_HANA_INPUT_SAMPLES
        ? b->hana_input_writes % NRB_HANA_INPUT_SAMPLES : 0u;
    fprintf(stderr,
            "[nr-hana-input matches=%llu samples=%u writes=%u "
            "depth-copy-fail=%u depth-map=%08lX condition-total=%llu "
            "condition-keys=%u condition-replaced=%llu]\n",
            (unsigned long long)b->hana_input_matches, count,
            b->hana_input_writes, b->hana_depth_copy_failures,
            (unsigned long)depth_map,
            (unsigned long long)b->hana_condition_total,
            b->hana_condition_count,
            (unsigned long long)b->hana_condition_overflow);
    for (u32 i = 0; i < b->hana_condition_count; ++i) {
        const nrb_hana_condition_key* const key = &b->hana_condition[i];
        fprintf(stderr,
                "[nr-hana-condition i=%u attempts=%llu skipped=%llu "
                "report=%08X:%08X value=%08X "
                "fp=%u:%08X/ctl=%08X vp=%u/br=%08X "
                "color=%u:%08X/fmt=%u depth=%u:%08X/fmt=%u "
                "ztest=%u/zwrite=%u blend=%u alpha=%u]\n",
                i, (unsigned long long)key->attempts,
                (unsigned long long)key->skipped,
                key->dma_report, key->report_offset,
                key->observed_value,
                key->fp_location, key->fp_offset, key->fp_control,
                key->vp_start, key->vp_branch_bits,
                key->color_location, key->color_offset,
                key->color_format, key->depth_location,
                key->depth_offset, key->depth_format,
                key->depth_test, key->depth_write,
                key->blend_enable, key->alpha_test);
    }
    for (u32 ordinal = 0; ordinal < count; ++ordinal) {
        const u32 slot = (start + ordinal) % NRB_HANA_INPUT_SAMPLES;
        nrb_hana_input_sample* const sample = &b->hana_input[slot];
        if (depth_bytes) {
            for (u32 unit = 0; unit < NRB_HANA_DEPTH_UNITS; ++unit) {
                nrb_hana_input_depth* const depth = &sample->depth[unit];
                if (!depth->copy_recorded)
                    continue;
                u64 hash = 0xCBF29CE484222325ull;
                const size_t base =
                    (size_t)slot * NRB_HANA_DEPTH_SAMPLE_BYTES +
                    (size_t)unit * NRB_HANA_DEPTH_POINTS *
                        NRB_HANA_DEPTH_POINT_STRIDE;
                depth->zero_count = 0u;
                depth->one_count = 0u;
                for (u32 point = 0; point < NRB_HANA_DEPTH_POINTS; ++point) {
                    const u8* const value = depth_bytes + base +
                        (size_t)point * NRB_HANA_DEPTH_POINT_STRIDE;
                    u32 bits = 0u;
                    memcpy(&bits, value, sizeof(bits));
                    for (u32 byte = 0; byte < sizeof(bits); ++byte) {
                        hash ^= value[byte];
                        hash *= 0x100000001B3ull;
                    }
                    depth->zero_count += bits == 0u;
                    depth->one_count += bits == 0x3F800000u;
                }
                depth->content_hash = hash;
            }
        }
        fprintf(stderr,
                "[nr-hana-input-sample slot=%u match=%llu "
                "vp=%016llX/%u+%u/br=%08X fp=%016llX "
                "condition=%u/%08X:%08X "
                "vtex=bound:%X/used:%X constants=%016llX "
                "required=%016llX/%u index=%u:%08X base=%08X "
                "batches=%u count=%u]\n",
                slot, (unsigned long long)sample->match,
                (unsigned long long)sample->vp_hash,
                sample->vp_start, sample->vp_words,
                sample->vp_branch_bits,
                (unsigned long long)sample->fp_hash,
                sample->render_condition_enabled,
                sample->render_condition_dma,
                sample->render_condition_offset,
                sample->bound_vtex_mask, sample->used_vtex_mask,
                (unsigned long long)sample->constants_hash,
                (unsigned long long)sample->required_hash,
                sample->required_count, sample->index_location,
                sample->index_offset, sample->base_index,
                sample->batch_count, sample->total_count);
        const u32 spans = sample->required_count < NRB_HANA_INPUT_SPANS
            ? sample->required_count : NRB_HANA_INPUT_SPANS;
        for (u32 i = 0; i < spans; ++i) {
            const nrb_required_span* const span = &sample->required[i];
            fprintf(stderr,
                    "[nr-hana-input-span slot=%u i=%u %u:%08X+%X "
                    "hash=%016llX epoch=%llu gen=%u..%u]\n",
                    slot, i, span->space, span->offset, span->size,
                    (unsigned long long)sample->required_span_hash[i],
                    (unsigned long long)sample->required_space_epoch[i],
                    sample->required_first_page_gen[i],
                    sample->required_last_page_gen[i]);
        }
        for (u32 unit = 0; unit < NRB_VTEX_UNITS; ++unit) {
            const nrb_hana_input_vtex* const vtex = &sample->vtex[unit];
            if (!vtex->texture.enabled)
                continue;
            fprintf(stderr,
                    "[nr-hana-input-vtex slot=%u unit=%u "
                    "%u:%08X+%X fmt=%02X/%ux%u/pitch=%u "
                    "wrap=%08X/remap=%08X/filter=%08X/ctl=%08X "
                    "source=%016llX uploaded=%016llX cache=%u/%u/%u "
                    "epoch=%llu gen=%u..%u]\n",
                    slot, unit, vtex->texture.location,
                    vtex->texture.offset, vtex->span,
                    vtex->texture.format, vtex->texture.width,
                    vtex->texture.height, vtex->texture.pitch,
                    vtex->texture.wrap, vtex->texture.remap,
                    vtex->texture.filter, vtex->texture.control0,
                    (unsigned long long)vtex->source_hash,
                    (unsigned long long)vtex->uploaded_hash,
                    vtex->cache_slot, vtex->cache_current,
                    vtex->resolution,
                    (unsigned long long)vtex->space_epoch,
                    vtex->first_page_gen, vtex->last_page_gen);
        }
        for (u32 unit = 0; unit < NRB_HANA_DEPTH_UNITS; ++unit) {
            const nrb_hana_input_depth* const depth = &sample->depth[unit];
            fprintf(stderr,
                    "[nr-hana-input-depth slot=%u unit=%u guest=%u:%08X "
                    "resource=%016llX sample=%016llX owner=%s "
                    "generation=%llu/%llu command=%llu "
                    "fence=%llu/%llu state=%u/%u valid=%u "
                    "srv=%u texture=%02X/wrap=%08X/remap=%08X/"
                    "filter=%08X/ctl=%08X/border=%08X "
                    "sampler=%u/%u,%u,%u/cmp=%u "
                    "grid=%ux%u content=%s%016llX zero=%u one=%u]\n",
                    slot, unit, depth->space, depth->offset,
                    (unsigned long long)depth->resource_identity,
                    (unsigned long long)depth->sample_identity,
                    depth->external ? "external" : "private",
                    (unsigned long long)depth->write_generation,
                    (unsigned long long)depth->resolve_generation,
                    (unsigned long long)depth->command_generation,
                    (unsigned long long)depth->recording_fence,
                    (unsigned long long)depth->completed_fence,
                    depth->resource_state, depth->sample_state,
                    depth->sample_valid, depth->srv_format,
                    depth->texture_format, depth->texture_wrap,
                    depth->texture_remap, depth->texture_filter,
                    depth->texture_control, depth->texture_border,
                    depth->sampler_filter, depth->sampler_address_u,
                    depth->sampler_address_v, depth->sampler_address_w,
                    depth->sampler_comparison,
                    NRB_HANA_DEPTH_GRID, NRB_HANA_DEPTH_GRID,
                    depth_bytes && depth->copy_recorded ? "" : "unavailable/",
                    (unsigned long long)depth->content_hash,
                    depth->zero_count, depth->one_count);
        }
    }
    if (depth_bytes) {
        D3D12_RANGE written = {0, 0};
        b->hana_depth_readback->lpVtbl->Unmap(
            b->hana_depth_readback, 0, &written);
    }
}

int rsx_nr_d3d12_dump_hana_input(rsx_nr_d3d12* b)
{
    if (!b)
        return -1;
    if (!b->hana_input_oracle || b->hana_input_dumped)
        return 0;
    if (b->list_open && nrb_exec_wait(
            b, RSX_NR_D3D12_SUBMIT_DIAGNOSTIC_READBACK, 0u) != 0)
        return -1;
    if (b->queue && b->fence && nrb_wait_idle(b) != 0)
        return -1;
    nrb_hana_input_dump(b);
    return 0;
}

void rsx_nr_d3d12_destroy(rsx_nr_d3d12* b)
{
    if (!b)
        return;
    if (b->list_open)
        nrb_exec_wait(b, RSX_NR_D3D12_SUBMIT_SHUTDOWN_RESET, 0u);
    nrb_release_timeline_lease(b);
    if (b->queue && b->fence)
        nrb_wait_idle(b);
    nrb_hana_input_dump(b);
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
    }
    for (u32 i = 0; i < NRB_MAX_DEPTHS; i++) {
        if (b->depths[i].tex)
            b->depths[i].tex->lpVtbl->Release(b->depths[i].tex);
        if (b->depths[i].sample_tex)
            b->depths[i].sample_tex->lpVtbl->Release(
                b->depths[i].sample_tex);
    }
    if (b->mirror)
        rsx_gpu_mirror_destroy(b->mirror);
    if (b->mirror_be)
        rsx_gpu_mirror_d3d12_destroy(b->mirror_be);
    if (b->pages.space[0].page_gen || b->pages.space[1].page_gen)
        rsx_guest_pages_destroy(&b->pages);
    free(b->idx_scratch);
    for (u32 space = 0; space < RSX_GUEST_NUM_SPACES; ++space) {
        free(b->resident_page[space]);
        free(b->watched_host_page_bits[space]);
        free(b->force_draw_input_page_epoch[space]);
    }
    if (b->readback)
        b->readback->lpVtbl->Release(b->readback);
    if (b->hana_depth_readback)
        b->hana_depth_readback->lpVtbl->Release(
            b->hana_depth_readback);
    if (b->upload)
        b->upload->lpVtbl->Release(b->upload);
    if (b->rootsig)
        b->rootsig->lpVtbl->Release(b->rootsig);
    if (b->depth_snapshot_pso)
        b->depth_snapshot_pso->lpVtbl->Release(b->depth_snapshot_pso);
    if (b->depth_snapshot_rootsig)
        b->depth_snapshot_rootsig->lpVtbl->Release(
            b->depth_snapshot_rootsig);
    if (b->rtv_heap)
        b->rtv_heap->lpVtbl->Release(b->rtv_heap);
    if (b->dsv_heap)
        b->dsv_heap->lpVtbl->Release(b->dsv_heap);
    if (b->texture_cpu_heap)
        b->texture_cpu_heap->lpVtbl->Release(b->texture_cpu_heap);
    if (b->texture_gpu_heap)
        b->texture_gpu_heap->lpVtbl->Release(b->texture_gpu_heap);
    if (b->depth_snapshot_heap)
        b->depth_snapshot_heap->lpVtbl->Release(b->depth_snapshot_heap);
    if (b->sampler_gpu_heap)
        b->sampler_gpu_heap->lpVtbl->Release(b->sampler_gpu_heap);
    if (b->fence_event)
        CloseHandle(b->fence_event);
    if (b->fence)
        b->fence->lpVtbl->Release(b->fence);
    if (b->shared_list1)
        b->shared_list1->lpVtbl->Release(b->shared_list1);
    if (b->owned_list1)
        b->owned_list1->lpVtbl->Release(b->owned_list1);
    else if (b->list1 && !b->shared_timeline)
        b->list1->lpVtbl->Release(b->list1);
    if (b->owned_list)
        b->owned_list->lpVtbl->Release(b->owned_list);
    else if (b->list && !b->shared_timeline)
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

int rsx_nr_d3d12_depth_bounds_supported(const rsx_nr_d3d12* b)
{
    return b && b->depth_bounds_supported;
}

int rsx_nr_d3d12_read_rt(rsx_nr_d3d12* b, u32 space, u32 offset,
                         u32 w, u32 h, u8* out)
{
    nrb_rt* rt = nrb_latest_rt(b, space, offset, w, h, 1);
    if (!rt)
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
    dstl.PlacedFootprint.Footprint.Format = rt->dxgi;
    dstl.PlacedFootprint.Footprint.Width = w;
    dstl.PlacedFootprint.Footprint.Height = h;
    dstl.PlacedFootprint.Footprint.Depth = 1;
    dstl.PlacedFootprint.Footprint.RowPitch = row;
    b->list->lpVtbl->CopyTextureRegion(b->list, &dstl, 0, 0, 0, &srcl, NULL);

    nrb_rt_transition(b, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (nrb_exec_wait(
            b, RSX_NR_D3D12_SUBMIT_DIAGNOSTIC_READBACK, need))
        return -1;

    u8* mapped = NULL;
    D3D12_RANGE rr = {0, need};
    const HRESULT map_result = b->readback->lpVtbl->Map(
        b->readback, 0, &rr, (void**)&mapped);
    if (FAILED(map_result)) {
        fprintf(stderr,
                "[nrb-readback] Map failed hr=0x%08lX removed=0x%08lX\n",
                (unsigned long)map_result,
                (unsigned long)b->dev->lpVtbl->GetDeviceRemovedReason(b->dev));
        nrb_dump_device_oracle(b->dev, "readback Map", map_result);
        return -1;
    }

    for (u32 y = 0; y < h; y++)
        memcpy(out + (size_t)y * w * 4, mapped + (size_t)y * row, w * 4);
    D3D12_RANGE nw = {0, 0};
    b->readback->lpVtbl->Unmap(b->readback, 0, &nw);
    return 0;
}

int rsx_nr_d3d12_read_depth(rsx_nr_d3d12* b, u32 space, u32 offset,
                            u32 format, u32 w, u32 h, float* out)
{
    if (!b || !out)
        return -1;
    nrb_depth* const depth = nrb_get_depth(
        b, space, offset, format, w, h, 0, 0);
    if (!depth || depth->external ||
        depth->srv_dxgi != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS)
        return -1;

    const u32 row = (w * (u32)sizeof(float) + 255u) & ~255u;
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
    nrb_depth_transition(b, depth, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source = {0}, destination = {0};
    source.pResource = depth->tex;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource = b->readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    destination.PlacedFootprint.Footprint.Width = w;
    destination.PlacedFootprint.Footprint.Height = h;
    destination.PlacedFootprint.Footprint.Depth = 1;
    destination.PlacedFootprint.Footprint.RowPitch = row;
    b->list->lpVtbl->CopyTextureRegion(
        b->list, &destination, 0, 0, 0, &source, NULL);
    nrb_depth_transition(b, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (nrb_exec_wait(
            b, RSX_NR_D3D12_SUBMIT_DIAGNOSTIC_READBACK, need))
        return -1;

    u8* mapped = NULL;
    D3D12_RANGE read = {0, need};
    if (FAILED(b->readback->lpVtbl->Map(
            b->readback, 0, &read, (void**)&mapped)))
        return -1;
    for (u32 y = 0; y < h; ++y)
        memcpy(out + (size_t)y * w, mapped + (size_t)y * row,
               (size_t)w * sizeof(float));
    D3D12_RANGE written = {0, 0};
    b->readback->lpVtbl->Unmap(b->readback, 0, &written);
    return 0;
}

int rsx_nr_d3d12_set_scanout_provenance(rsx_nr_d3d12* b, int enabled)
{
    if (!b || b->rt_write_serial || b->stats.clears || b->stats.draws ||
        b->stats.presents)
        return -1;
    b->scanout_provenance = enabled != 0;
    return 0;
}

int rsx_nr_d3d12_rt_info(const rsx_nr_d3d12* b, u32 ordinal,
                         u32* space, u32* offset, u32* format,
                         u32* width, u32* height)
{
    if (!b)
        return -1;
    for (u32 i = 0; i < NRB_MAX_RTS; ++i) {
        const nrb_rt* const rt = &b->rts[i];
        if (!rt->live)
            continue;
        if (ordinal) {
            --ordinal;
            continue;
        }
        if (space)
            *space = rt->space;
        if (offset)
            *offset = rt->offset;
        if (format)
            *format = rt->fmt;
        if (width)
            *width = rt->w;
        if (height)
            *height = rt->h;
        return 0;
    }
    return -1;
}

int rsx_nr_d3d12_get_rt_provenance(
    const rsx_nr_d3d12* b, u32 ordinal,
    rsx_nr_d3d12_rt_provenance* out)
{
    if (!b || !b->scanout_provenance || !out)
        return -1;
    for (u32 i = 0; i < NRB_MAX_RTS; ++i) {
        const nrb_rt* const rt = &b->rts[i];
        if (!rt->live)
            continue;
        if (ordinal) {
            --ordinal;
            continue;
        }
        memset(out, 0, sizeof(*out));
        out->resource_identity = (unsigned long long)(UINT_PTR)rt->tex;
        out->write_serial = rt->last_write_serial;
        out->color_clear_writes = rt->color_clear_writes;
        out->draw_writes = rt->draw_writes;
        out->present_count = rt->present_count;
        out->space = rt->space;
        out->offset = rt->offset;
        out->format = rt->fmt;
        out->width = rt->w;
        out->height = rt->h;
        out->resource_state = (u32)rt->color_state;
        out->external = rt->external != 0;
        return 0;
    }
    return -1;
}

void rsx_nr_d3d12_get_stats(const rsx_nr_d3d12* b, rsx_nr_d3d12_stats* out)
{
    *out = b->stats;
    out->texture_cache_count = b->textures.count;
    out->texture_cache_capacity = b->textures.cap;
    out->texture_cache_table_full = b->textures.stats.table_full;
    out->texture_cache_arena_exhausted =
        b->textures.stats.arena_exhausted;
    out->pso_cache_count = b->psos.count;
    out->pso_cache_capacity = b->psos.cap;
    out->pso_cache_table_full = b->psos.stats.table_full;
    if (b->mirror) {
        rsx_gpu_mirror_stats mirror = {0};
        rsx_gpu_mirror_get_stats(b->mirror, &mirror);
        out->mirror_syncs = mirror.syncs;
        out->mirror_uploads = mirror.uploads;
        out->mirror_upload_bytes = mirror.upload_bytes;
        out->mirror_upload_rejects = mirror.upload_rejects;
        out->mirror_deferred_syncs = mirror.deferred_syncs;
        out->mirror_resolver_failures = mirror.resolver_failures;
    }
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
int rsx_nr_d3d12_dump_hana_input(rsx_nr_d3d12* b)
{ (void)b; return 0; }
rsx_guest_pages* rsx_nr_d3d12_pages(rsx_nr_d3d12* b) { (void)b; return 0; }
int rsx_nr_d3d12_set_content_cache(
    rsx_nr_d3d12* b, rsx_nr_d3d12_compile_shader_fn c,
    rsx_nr_d3d12_pso_load_fn l, rsx_nr_d3d12_pso_store_fn s,
    rsx_nr_d3d12_pso_free_fn f, void* u)
{ (void)b; (void)c; (void)l; (void)s; (void)f; (void)u; return -1; }
void rsx_nr_d3d12_get_exec_ops(rsx_nr_d3d12* b, rsx_nr_exec_ops* out)
{
    (void)b;
    if (out)
        memset(out, 0, sizeof(*out));
}
int rsx_nr_d3d12_set_live_output(rsx_nr_d3d12* b, int rgba_targets,
                                 rsx_nr_d3d12_present_fn present,
                                 void* present_user)
{
    (void)b; (void)rgba_targets; (void)present; (void)present_user;
    return -1;
}
void rsx_nr_d3d12_set_display_buffer(rsx_nr_d3d12* b, u32 buffer_id,
                                     u32 location, u32 offset,
                                     u32 width, u32 height)
{
    (void)b; (void)buffer_id; (void)location; (void)offset;
    (void)width; (void)height;
}
void rsx_nr_d3d12_set_watch_page(rsx_nr_d3d12* b,
                                 rsx_nr_d3d12_watch_page_fn watch,
                                 void* watch_user)
{
    (void)b; (void)watch; (void)watch_user;
}
void rsx_nr_d3d12_set_resource_broker(
    rsx_nr_d3d12* b, rsx_nr_d3d12_borrow_color_fn color,
    rsx_nr_d3d12_borrow_depth_fn depth,
    rsx_nr_d3d12_resolve_depth_sample_fn resolve_depth_sample,
    void* broker_user)
{
    (void)b; (void)color; (void)depth; (void)resolve_depth_sample;
    (void)broker_user;
}
void rsx_nr_d3d12_set_publish_write(
    rsx_nr_d3d12* b, rsx_nr_d3d12_publish_write_fn publish, void* user)
{
    (void)b; (void)publish; (void)user;
}
void rsx_nr_d3d12_set_render_condition_reader(
    rsx_nr_d3d12* b, rsx_nr_d3d12_render_condition_fn read, void* user)
{
    (void)b; (void)read; (void)user;
}
int rsx_nr_d3d12_set_shared_timeline(
    rsx_nr_d3d12* b, rsx_nr_d3d12_timeline_acquire_fn acquire,
    rsx_nr_d3d12_timeline_release_fn release,
    rsx_nr_d3d12_timeline_flush_fn flush, void* user)
{
    (void)b; (void)acquire; (void)release; (void)flush; (void)user;
    return -1;
}
int rsx_nr_d3d12_shared_timeline_enabled(const rsx_nr_d3d12* b)
{
    (void)b;
    return 0;
}
int rsx_nr_d3d12_validate_depth_sample_alias(
    rsx_nr_d3d12* b, const rsx_nir_texture* texture)
{
    (void)b; (void)texture;
    return -1;
}
int rsx_nr_d3d12_set_coherent_section_mode(rsx_nr_d3d12* b, int enabled)
{
    (void)b; (void)enabled;
    return -1;
}
int rsx_nr_d3d12_set_force_draw_input_refresh(rsx_nr_d3d12* b,
                                              int enabled)
{
    (void)b; (void)enabled;
    return -1;
}
void rsx_nr_d3d12_begin_draw_input_refresh_section(rsx_nr_d3d12* b)
{
    (void)b;
}
void rsx_nr_d3d12_note_guest_write(rsx_nr_d3d12* b, u32 space,
                                   u32 offset, u32 size)
{
    (void)b; (void)space; (void)offset; (void)size;
}
int rsx_nr_d3d12_read_rt(rsx_nr_d3d12* b, u32 space, u32 offset,
                         u32 w, u32 h, u8* out)
{
    (void)b; (void)space; (void)offset; (void)w; (void)h; (void)out;
    return -1;
}
int rsx_nr_d3d12_read_depth(rsx_nr_d3d12* b, u32 space, u32 offset,
                            u32 format, u32 w, u32 h, float* out)
{
    (void)b; (void)space; (void)offset; (void)format;
    (void)w; (void)h; (void)out;
    return -1;
}
int rsx_nr_d3d12_set_scanout_provenance(rsx_nr_d3d12* b, int enabled)
{
    (void)b; (void)enabled;
    return -1;
}
int rsx_nr_d3d12_rt_info(const rsx_nr_d3d12* b, u32 ordinal,
                         u32* space, u32* offset, u32* format,
                         u32* width, u32* height)
{
    (void)b; (void)ordinal; (void)space; (void)offset; (void)format;
    (void)width; (void)height;
    return -1;
}
int rsx_nr_d3d12_get_rt_provenance(
    const rsx_nr_d3d12* b, u32 ordinal,
    rsx_nr_d3d12_rt_provenance* out)
{
    (void)b; (void)ordinal; (void)out;
    return -1;
}
void rsx_nr_d3d12_get_stats(const rsx_nr_d3d12* b, rsx_nr_d3d12_stats* out)
{
    (void)b;
    if (out)
        memset(out, 0, sizeof(*out));
}

#endif /* _WIN32 */
