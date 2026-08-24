/*
 * Vertical native-GCM producer gate. See native_gcm_vertical.h.
 *
 * The three initial wrappers are intentionally context-independent ordered
 * actions. They establish the interception/equivalence contract without
 * depending on draw state or changing guest FIFO ownership:
 *
 *   00EBC034  SetReferenceCommand(ctx, value)
 *   00EBC330  WaitLabel(ctx, index, value)
 *   00EBD6FC  SetUserCommand(ctx, cause)
 *
 * In shadow mode the original lifted function always runs. The producer and
 * FIFO-consumer hashes therefore describe two views of the same operations.
 * An unrecognized producer of one of these families appears as a count/hash
 * mismatch instead of being silently claimed.
 */

#include "native_gcm_vertical.h"

#include "ppu_recomp.h"
#include "yakuza_runner.h"
#include "rsx_nr_backend.h"
#include "rsx_nr_backend_d3d12.h"
#include "rsx_fp_decompiler.h"
#include "rsx_live_draw.h"
#include "rsx_nir_adapter.h"
#include "rsx_nr_producer_contract.h"
#include "rsx_nr_span_router.h"
#include "rsx_vp_decompiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" const uint8_t* yz_nr_vertical_guest_ptr(uint32_t location,
                                                     uint32_t offset,
                                                     uint32_t min_bytes);
extern "C" uint8_t* yz_nr_vertical_guest_writable_ptr(uint32_t location,
                                                        uint32_t offset,
                                                        uint32_t min_bytes);
extern "C" int yz_nr_vertical_space_page_to_ea(uint32_t location,
                                                 uint32_t page_offset,
                                                 uint32_t* out_ea);
extern "C" int yz_nr_vertical_space_range_to_ea(uint32_t location,
                                                  uint32_t offset,
                                                  uint32_t size,
                                                  uint32_t* out_ea);
extern "C" void cellSpursSetGuestWriteObserver(
    void (*observer)(uint32_t ea, uint32_t size));
extern "C" void cellSpursNotifyGuestWrite(uint32_t ea, uint32_t size);
extern "C" volatile uint64_t g_native_spurs_watch_page_bits[16384];
extern "C" void yz_drain_trampolines(ppu_context* ctx);
extern "C" uint32_t yz_nr_vertical_io_to_ea(uint32_t io_offset);
extern "C" int yz_nr_vertical_mirror_legacy_method(
    uint32_t method, uint32_t arg, int suppress_action);
extern "C" int yz_nr_vertical_report_can(uint32_t kind, uint32_t arg,
                                           uint32_t dma);
extern "C" int yz_nr_vertical_render_condition_read(
    uint32_t dma, uint32_t offset, uint32_t* value);

namespace {

enum : uint32_t {
    YZ_NR_VERT_REFERENCE = 1,
    YZ_NR_VERT_ACQUIRE = 2,
    YZ_NR_VERT_USER = 3,
    YZ_NR_VERT_STATE_DIRECT = 4,
    YZ_NR_VERT_DRAW_ARRAYS = 5,
    YZ_NR_VERT_FLIP = 6,
    YZ_NR_VERT_VERTEX_PROGRAM = 7,
    YZ_NR_VERT_FRAGMENT_PROGRAM = 8,
    YZ_NR_VERT_FAMILY_COUNT = 9,
    YZ_NR_VERT_SOURCE_COUNT = 16,
    YZ_NR_VERT_EVENT_COUNT = 8192,
    YZ_NR_VERT_VP_TEMPLATE_COUNT = 512,
    YZ_NR_VERT_FP_TEMPLATE_COUNT = 4096,
    YZ_NR_VERT_FP_STRUCTURAL_INDEX_COUNT = 8192,
};

enum : uint32_t {
    YZ_NR_EVENT_EMPTY = 0,
    YZ_NR_EVENT_READY = 1,
    YZ_NR_EVENT_TOMBSTONE = 2,
};

struct yz_nr_vertical_lane {
    unsigned long long count[YZ_NR_VERT_FAMILY_COUNT];
    unsigned long long ordered_hash[YZ_NR_VERT_FAMILY_COUNT];
    unsigned long long xor_hash[YZ_NR_VERT_FAMILY_COUNT];
    unsigned long long sum_hash[YZ_NR_VERT_FAMILY_COUNT];
};

struct yz_nr_vertical_source {
    uint32_t context;
    uint32_t current_min;
    uint32_t current_max;
    uint32_t family_mask;
    unsigned long long count;
};

struct yz_nr_vertical_event {
    uint32_t state;
    uint32_t packet_ea;
    uint32_t family;
    uint32_t a;
    uint32_t b;
};

struct yz_nr_vertical_vp_template {
    uint32_t start_slot;
    uint32_t code_hash;
    uint32_t word_count;
    uint32_t input_mask;
    uint32_t semantic_hash;
    unsigned long long build_count;
    unsigned long long replay_count;
};

struct yz_nr_vertical_fp_template {
    unsigned long long content_hash;
    unsigned long long structural_hash;
    unsigned long long semantic_hash;
    unsigned long long structural_semantic_hash;
    uint32_t byte_count;
    uint32_t control;
    unsigned long long build_count;
    unsigned long long replay_count;
    unsigned long long parameter_replay_count;
};

struct yz_nr_vertical_state {
    SRWLOCK lock;
    yz_nr_vertical_lane expected;
    yz_nr_vertical_lane observed;
    yz_nr_vertical_source sources[YZ_NR_VERT_SOURCE_COUNT];
    unsigned long long source_overflow;
    uint32_t observed_ea_min[YZ_NR_VERT_FAMILY_COUNT];
    uint32_t observed_ea_max[YZ_NR_VERT_FAMILY_COUNT];
    yz_nr_vertical_event events[YZ_NR_VERT_EVENT_COUNT];
    unsigned long long exact_matches;
    unsigned long long exact_mismatches;
    unsigned long long exact_unexpected;
    unsigned long long exact_unaddressed;
    unsigned long long exact_overflow;
    unsigned long long state_unowned_methods;
    unsigned long long draw_unowned;
    unsigned long long draw_unsupported;
    unsigned long long draw_bad_sequence;
    uint32_t fifo_semaphore_offset;
    uint32_t fifo_semaphore_packet_ea;
    uint32_t draw_state;          /* 0 none, 1 exact wrapper, 2 fallback */
    uint32_t draw_event_ea;
    uint32_t draw_primitive;
    uint32_t draw_hash;
    uint32_t flip_state;          /* 0 none, 1 exact wrapper, 2 fallback */
    uint32_t flip_event_ea;
    uint32_t flip_buffer;
    unsigned long long flip_unowned;
    unsigned long long flip_bad_sequence;
    uint32_t vp_state;            /* 0 none, 1 exact wrapper, 2 fallback */
    uint32_t vp_event_ea;
    uint32_t vp_start_slot;
    uint32_t vp_hash;
    uint32_t vp_word_count;
    unsigned long long vp_unowned;
    unsigned long long vp_bad_sequence;
    yz_nr_vertical_vp_template vp_templates[YZ_NR_VERT_VP_TEMPLATE_COUNT];
    yz_nr_vertical_vp_template vp_unknown_templates[
        YZ_NR_VERT_VP_TEMPLATE_COUNT];
    unsigned long long vp_template_builds;
    unsigned long long vp_template_replays;
    unsigned long long vp_template_mask_replays;
    unsigned long long vp_template_unknown;
    unsigned long long vp_template_overflow;
    unsigned long long vp_unknown_overflow;
    rsx_nr_fragment_binding_state fp_binding;
    uint32_t fp_packet_ea;
    yz_nr_vertical_fp_template fp_templates[YZ_NR_VERT_FP_TEMPLATE_COUNT];
    uint16_t fp_structural_index[YZ_NR_VERT_FP_STRUCTURAL_INDEX_COUNT];
    yz_nr_vertical_fp_template fp_unknown_templates[
        YZ_NR_VERT_FP_TEMPLATE_COUNT];
    unsigned long long fp_template_builds;
    unsigned long long fp_template_replays;
    unsigned long long fp_template_parameter_replays;
    unsigned long long fp_template_unknown;
    unsigned long long fp_template_overflow;
    unsigned long long fp_unknown_overflow;
    unsigned long long fp_unresolved;
    unsigned long long fp_bad_sequence;
    volatile LONG mode_shadow;
    volatile LONG mode_active_basic;
    volatile LONG mode_active_present;
    volatile LONG mode_active_graphics;
    volatile LONG initialized;
};

static yz_nr_vertical_state g_vertical = {SRWLOCK_INIT};

enum : uint32_t {
    YZ_NR_ACTIVE_ROUTER_CAPACITY = 8192,
    /* A first native action can fold every state group, a vertex program,
     * fragmented constant runs, and the action itself. Keep this fixed ring
     * large enough that consumer interception never partially emits before
     * falling back. The side capacity includes worst-case wrap padding. */
    YZ_NR_ACTIVE_RING_CAPACITY = 512,
    YZ_NR_ACTIVE_SIDE_CAPACITY = 32768,
    YZ_NR_ACTIVE_ACTION_OP_BOUND = 512,
    YZ_NR_ACTIVE_ACTION_SIDE_BOUND = 16384,
    YZ_NR_ACTIVE_GUEST_PAGE_COUNT = 1u << 20,
    YZ_NR_SECTION_METHOD_CAPACITY = 262144,
    YZ_NR_SECTION_OP_CAPACITY = 65536,
    YZ_NR_SECTION_SIDE_CAPACITY = 1u << 20,
    YZ_NR_SECTION_STEP_CAPACITY = 0x40000,
};

enum : uint32_t {
    YZ_NR_SECTION_FB_NONE = 0,
    YZ_NR_SECTION_FB_NOT_READY,
    YZ_NR_SECTION_FB_WINDOW,
    YZ_NR_SECTION_FB_UNMAPPED,
    YZ_NR_SECTION_FB_FLOW,
    YZ_NR_SECTION_FB_CAPACITY,
    YZ_NR_SECTION_FB_UNKNOWN_METHOD,
    YZ_NR_SECTION_FB_INCOMPLETE_ACTION,
    YZ_NR_SECTION_FB_NO_GPU_ACTION,
    YZ_NR_SECTION_FB_PREFLIGHT_CLEAR,
    YZ_NR_SECTION_FB_PREFLIGHT_DRAW,
    YZ_NR_SECTION_FB_PREFLIGHT_TRANSFER,
    YZ_NR_SECTION_FB_PREFLIGHT_SYNC,
    YZ_NR_SECTION_FB_PREFLIGHT_REPORT,
    YZ_NR_SECTION_FB_PREFLIGHT_PRESENT,
    YZ_NR_SECTION_FB_REASON_COUNT,
};

enum : uint32_t {
    YZ_NR_GRAPHICS_DRAW = 1u << 0,
    YZ_NR_GRAPHICS_CLEAR = 1u << 1,
    YZ_NR_GRAPHICS_TRANSFER = 1u << 2,
    YZ_NR_GRAPHICS_SYNC = 1u << 3,
    YZ_NR_GRAPHICS_REPORT = 1u << 4,
    YZ_NR_GRAPHICS_ALL = (1u << 5) - 1u,
};

enum : uint32_t {
    YZ_NR_CLEAR_ALL = 0,
    YZ_NR_CLEAR_COLOR_ONLY = 1,
    YZ_NR_CLEAR_DEPTH_ONLY = 2,
    YZ_NR_CLEAR_COMBINED = 3,
};

struct yz_nr_vertical_display {
    uint32_t location, offset, width, height;
    uint32_t valid;
};

struct yz_nr_section_method {
    uint32_t method;
    uint32_t arg;
    uint32_t suppress_action;
};

struct yz_nr_section_unknown_key {
    uint32_t method, arg;
    unsigned long long count;
};

struct yz_nr_section_report_key {
    uint32_t kind, arg, dma;
    unsigned long long count;
};

struct yz_nr_section_draw_preflight_key {
    uint32_t reason, primitive, color_target, fp_location, fp_offset;
    uint32_t vp_start, vp_inputs;
    uint32_t color_location, color_offset, color_format, color_pitch;
    uint32_t viewport_x, viewport_y, viewport_w, viewport_h;
    uint32_t scissor_x, scissor_y, scissor_w, scissor_h;
    uint32_t color_mask, depth_test, depth_write, texture_mask;
    uint32_t stencil_two_sided;
    uint32_t stencil_ref, back_stencil_ref;
    uint32_t stencil_mask, back_stencil_mask;
    uint32_t stencil_write_mask, back_stencil_write_mask;
    unsigned long long vp_hash;
    uint32_t vp_words, vp_bad_vec, vp_bad_sca, vp_missing_vtex;
    uint32_t vp_conditional, vp_terminated;
    unsigned long long count;
};

struct yz_nr_section_sync_preflight_key {
    uint32_t kind, dma, offset, value, release_kind;
    int32_t result;
    unsigned long long count;
};

struct yz_nr_section_transfer_preflight_key {
    uint32_t kind, src_location, dst_location;
    uint32_t src_format, dst_format;
    uint32_t line_length, line_count, word_count;
    uint32_t in_w, in_h, out_w, out_h;
    unsigned long long count;
};

struct yz_nr_section_window_key {
    uint32_t pc, ret, size, command, stage;
    uint32_t first_put, last_put, min_available, max_available;
    unsigned long long count, error;
};

enum { YZ_NR_FLOW_VP_DIAG_COUNT = 8 };

struct yz_nr_flow_vp_diag {
    unsigned long long hash;
    unsigned long long draws;
    uint32_t start_slot;
    uint32_t word_count;
    uint32_t branch_first;
    uint32_t branch_last;
    uint32_t branch_or;
    uint32_t branch_and;
    uint32_t words[RSX_NIR_VP_MAX_WORDS];
};

struct yz_nr_vertical_active_state {
    SRWLOCK producer_lock;
    rsx_nr_span_router router;
    rsx_nr_ring ring;
    rsx_nr_tokens tokens;
    rsx_nr_backend backend;
    rsx_nir_adapter adapter;
    rsx_nr_d3d12* d3d12;
    rsx_nr_exec_ops gpu_ops;
    rsx_nr_slot slots[YZ_NR_ACTIVE_RING_CAPACITY];
    uint32_t side[YZ_NR_ACTIVE_SIDE_CAPACITY];
    yz_nr_vertical_display displays[8];
    volatile LONG graphics_ready;
    uint32_t graphics_families;
    uint32_t clear_scope;
    uint32_t frame_islands;
    rsx_nir_stream section_stream;
    rsx_nir_op* section_ops;
    uint32_t* section_side;
    yz_nr_section_method* section_methods;
    rsx_nir_adapter* section_adapter;
    uint32_t section_method_count;
    uint32_t section_packet_count;
    uint32_t section_start_get;
    uint32_t section_exec_pos;
    uint32_t section_next_get;
    uint32_t section_next_ret;
    uint32_t section_pending;
    uint32_t section_fatal;
    uint32_t section_gpu_actions;
    uint32_t section_draws;
    uint32_t section_clears;
    uint32_t section_transfers;
    uint32_t section_presents;
    unsigned long long section_attempts;
    unsigned long long section_owned;
    unsigned long long section_methods_owned;
    unsigned long long section_ops_owned;
    unsigned long long section_fallback[YZ_NR_SECTION_FB_REASON_COUNT];
    uint32_t section_fallback_active;
    uint32_t section_fallback_until_get;
    unsigned long long section_fallback_fast_skips;
    uint32_t section_repeat_valid;
    uint32_t section_repeat_get;
    uint32_t section_repeat_put;
    uint32_t section_repeat_ret;
    uint32_t section_repeat_word;
    uint32_t section_scan_cacheable;
    uint32_t section_scan_get;
    uint32_t section_scan_put;
    uint32_t section_scan_ret;
    uint32_t section_scan_word;
    uint32_t section_legacy_path_active;
    uint32_t section_legacy_path_reason;
    unsigned long long section_legacy_path_skips;
    unsigned long long section_legacy_path_exits;
    uint32_t section_blocked_sem_get;
    uint32_t section_blocked_sem_dma;
    uint32_t section_blocked_sem_offset;
    uint32_t section_blocked_sem_value;
    uint32_t section_blocked_sem_valid;
    uint32_t section_preflight_sem_dma;
    uint32_t section_preflight_sem_offset;
    uint32_t section_preflight_sem_value;
    uint32_t section_preflight_sem_valid;
    uint32_t section_diag_enabled;
    uint32_t draw_primitive_filter;
    unsigned long long section_render_passes_owned;
    unsigned long long section_dependency_islands_owned;
    unsigned long long section_shadow_depth_fallback;
    unsigned long long section_shadow_consumer_fallback;
    yz_nr_section_unknown_key section_unknown[64];
    yz_nr_section_report_key section_reports[64];
    yz_nr_section_draw_preflight_key section_draw_preflight[64];
    yz_nr_section_sync_preflight_key section_sync_preflight[64];
    yz_nr_section_transfer_preflight_key section_transfer_preflight[64];
    yz_nr_section_window_key section_window[32];
    yz_nr_flow_vp_diag section_flow_vp[YZ_NR_FLOW_VP_DIAG_COUNT];
    rsx_nr_fifo_visit_set section_visits;
    unsigned long long section_unknown_overflow;
    unsigned long long section_report_overflow;
    unsigned long long section_draw_preflight_overflow;
    unsigned long long section_sync_preflight_overflow;
    unsigned long long section_transfer_preflight_overflow;
    unsigned long long section_flow_vp_overflow;
    unsigned long long section_exec_errors;
    volatile LONG renderer_owner; /* 0 legacy/unknown, 1 native/shared */
    volatile LONG shared_timeline;
    volatile LONG* guest_page_route;
    unsigned long long owned[YZ_NR_VERT_FAMILY_COUNT];
    unsigned long long fallback[YZ_NR_VERT_FAMILY_COUNT];
    unsigned long long executed[YZ_NR_VERT_FAMILY_COUNT];
    unsigned long long wait;
    unsigned long long late_fallback;
    unsigned long long fatal;
    unsigned long long wrong_context;
    unsigned long long no_room;
    unsigned long long publish_failure;
    unsigned long long consumer_draw_owned;
    unsigned long long consumer_draw_fallback;
    unsigned long long consumer_clear_owned;
    unsigned long long consumer_clear_fallback;
    unsigned long long consumer_clear_contract[3];
    unsigned long long consumer_transfer_owned;
    unsigned long long consumer_transfer_fallback;
    unsigned long long consumer_sync_owned;
    unsigned long long consumer_sync_fallback;
    unsigned long long consumer_report_owned;
    unsigned long long consumer_report_fallback;
    rsx_nr_span pending_span;
    rsx_nr_span_claim pending_claim;
    uint32_t pending_executed;
    uint32_t pending_expected;
    uint32_t pending_valid;
    uint32_t last_miss_ea;
    uint32_t last_miss_epoch;
};

static yz_nr_vertical_active_state g_active = {SRWLOCK_INIT};

static uint32_t yz_nr_graphics_family_mask(const char* text)
{
    if (!text || !*text)
        return YZ_NR_GRAPHICS_ALL;
    if (strcmp(text, "all") == 0)
        return YZ_NR_GRAPHICS_ALL;
    uint32_t mask = 0;
    const char* cursor = text;
    while (*cursor) {
        const char* comma = strchr(cursor, ',');
        const size_t length = comma ? static_cast<size_t>(comma - cursor)
                                    : strlen(cursor);
        if (length == 4u && memcmp(cursor, "draw", 4u) == 0)
            mask |= YZ_NR_GRAPHICS_DRAW;
        else if (length == 5u && memcmp(cursor, "clear", 5u) == 0)
            mask |= YZ_NR_GRAPHICS_CLEAR;
        else if (length == 8u && memcmp(cursor, "transfer", 8u) == 0)
            mask |= YZ_NR_GRAPHICS_TRANSFER;
        else if (length == 4u && memcmp(cursor, "sync", 4u) == 0)
            mask |= YZ_NR_GRAPHICS_SYNC;
        else if (length == 6u && memcmp(cursor, "report", 6u) == 0)
            mask |= YZ_NR_GRAPHICS_REPORT;
        else
            return 0;
        if (!comma)
            break;
        cursor = comma + 1;
    }
    return mask;
}

static uint32_t yz_nr_clear_scope(const char* text)
{
    if (!text || !*text || strcmp(text, "all") == 0)
        return YZ_NR_CLEAR_ALL;
    if (strcmp(text, "color-only") == 0)
        return YZ_NR_CLEAR_COLOR_ONLY;
    if (strcmp(text, "depth-only") == 0)
        return YZ_NR_CLEAR_DEPTH_ONLY;
    if (strcmp(text, "combined") == 0)
        return YZ_NR_CLEAR_COMBINED;
    return UINT32_MAX;
}

extern "C" void yz_nr_vertical_exec_set_reference(uint32_t value);
extern "C" void yz_nr_vertical_exec_user_command(uint32_t cause);
extern "C" void yz_nr_vertical_exec_present(uint32_t buffer_id);
extern "C" void yz_nr_vertical_exec_present_complete(uint32_t buffer_id);
extern "C" int yz_nr_vertical_sem_read(uint32_t dma, uint32_t offset,
                                        uint32_t* value);
extern "C" void yz_nr_vertical_sem_write(uint32_t dma, uint32_t offset,
                                           uint32_t value,
                                           uint32_t texture_read);
extern "C" int yz_nr_vertical_report(uint32_t kind, uint32_t arg,
                                       uint32_t dma);

static void yz_nr_exec_reference(void*, uint32_t value)
{
    yz_nr_vertical_exec_set_reference(value);
}

static void yz_nr_exec_user(void*, uint32_t cause)
{
    yz_nr_vertical_exec_user_command(cause);
}

static int yz_nr_exec_present(void*, uint32_t buffer_id)
{
    yz_nr_vertical_exec_present(buffer_id);
    return 0;
}

static int yz_nr_d3d_present(void*, void* texture, uint32_t format,
                             uint32_t width, uint32_t height,
                             uint32_t buffer_id)
{
    (void)texture;
    (void)format;
    (void)width;
    (void)height;
    /* Every live render target is an exact resource borrowed from the legacy
     * surface registry. In shared-timeline mode the preceding native commands
     * are still on the live list; sink_flip appends the scanout copy and
     * retires the complete ordered frame once. Present through the typed
     * semantic boundary so the
     * movie handoff, QPC ring and sparse renderer-owned visual/state gates all
     * observe the frame.  This calls sink_flip directly; it does not decode
     * or replay an RSX packet. */
    rsx_live_draw_typed_present(buffer_id);
    yz_nr_vertical_exec_present_complete(buffer_id);
    return 0;
}

static int yz_nr_d3d_timeline_acquire(
    void*, void** command_list, unsigned long long* generation,
    unsigned long long* recording_fence,
    unsigned long long* completed_fence)
{
    return rsx_live_draw_timeline_acquire(
        command_list, generation, recording_fence, completed_fence);
}

static void yz_nr_d3d_timeline_release(void*)
{
    rsx_live_draw_timeline_release();
}

static int yz_nr_d3d_timeline_flush(void*)
{
    return rsx_live_draw_timeline_flush();
}

static int yz_nr_exec_sem_read(void*, uint32_t dma, uint32_t offset,
                               uint32_t* value)
{
    return yz_nr_vertical_sem_read(dma, offset, value);
}

static void yz_nr_exec_sem_write(void*, uint32_t dma, uint32_t offset,
                                 uint32_t value, uint32_t texture_read)
{
    yz_nr_vertical_sem_write(dma, offset, value, texture_read);
}

static int yz_nr_exec_report(void*, uint32_t kind, uint32_t arg,
                             uint32_t dma)
{
    return yz_nr_vertical_report(kind, arg, dma);
}

static const uint8_t* yz_nr_d3d_guest_ptr(void*, uint32_t space,
                                           uint32_t offset,
                                           uint32_t min_bytes)
{
    return yz_nr_vertical_guest_ptr(space, offset, min_bytes);
}

static uint8_t* yz_nr_d3d_guest_writable_ptr(void*, uint32_t space,
                                              uint32_t offset,
                                              uint32_t min_bytes)
{
    return yz_nr_vertical_guest_writable_ptr(space, offset, min_bytes);
}

static int yz_nr_d3d_watch_page(void*, uint32_t space, uint32_t page_offset)
{
    if (!g_active.guest_page_route || space > 1u)
        return -1;
    /* The persistent mirror tracks 1 KiB subpages to avoid dynamic-buffer
     * false sharing. VM write rejection remains one sparse bit/route per host
     * 4 KiB page, so four adjacent mirror registrations intentionally map to
     * the same exact external route. */
    const uint32_t watch_offset = page_offset & ~0xFFFu;
    uint32_t ea = 0;
    if (yz_nr_vertical_space_page_to_ea(space, watch_offset, &ea) != 0 ||
        (ea & 0xFFFu))
        return -1;
    const uint32_t ea_page = ea >> 12;
    const LONG encoded = (LONG)(((space + 1u) << 28) |
                                (watch_offset >> 12));
    const LONG prior = InterlockedCompareExchange(
        &g_active.guest_page_route[ea_page], encoded, 0);
    if (prior && prior != encoded)
        return -1; /* one EA page aliased by two RSX offsets: stay legacy */
    InterlockedOr64(
        reinterpret_cast<volatile LONG64*>(
            const_cast<uint64_t*>(&g_native_spurs_watch_page_bits[
                ea_page >> 6])),
        static_cast<LONG64>(1ull << (ea_page & 63u)));
    return 0;
}

static void yz_nr_d3d_publish_write(void*, uint32_t space, uint32_t offset,
                                    uint32_t size)
{
    uint32_t ea = 0;
    if (yz_nr_vertical_space_range_to_ea(
            space, offset, size, &ea) == 0)
        cellSpursNotifyGuestWrite(ea, size);
}

static int yz_nr_gpu_clear(void*, const rsx_nir_pipeline* st,
                           const rsx_nir_clear* clear)
{
    const uint32_t mismatch = g_active.frame_islands ? 0u :
        rsx_live_draw_native_clear_contract_mismatch(st, clear);
    if (mismatch) {
        for (uint32_t bit = 0; bit < 3u; ++bit)
            if (mismatch & (1u << bit))
                g_active.consumer_clear_contract[bit]++;
        return -1;
    }
    if (InterlockedExchange(&g_active.renderer_owner, 1) == 0 &&
        !InterlockedCompareExchange(&g_active.shared_timeline, 0, 0))
        rsx_live_draw_flush();
    const int result = g_active.gpu_ops.clear
        ? g_active.gpu_ops.clear(g_active.gpu_ops.user, st, clear) : -1;
    if (result) {
        InterlockedExchange(&g_active.renderer_owner, 0);
    } else
        rsx_live_draw_native_clear_commit(st, clear);
    return result;
}

static int yz_nr_gpu_draw(void*, const rsx_nir_pipeline* st,
                          const uint32_t* vp, uint32_t vp_words,
                          const rsx_nir_draw* draw, const uint32_t* batches)
{
    if (InterlockedExchange(&g_active.renderer_owner, 1) == 0 &&
        !InterlockedCompareExchange(&g_active.shared_timeline, 0, 0))
        rsx_live_draw_flush();
    const int result = g_active.gpu_ops.draw
        ? g_active.gpu_ops.draw(g_active.gpu_ops.user, st, vp, vp_words,
                                draw, batches) : -1;
    if (result)
        InterlockedExchange(&g_active.renderer_owner, 0);
    else
        rsx_live_draw_native_draw_commit(st);
    return result;
}

static int yz_nr_gpu_transfer(void*, const rsx_nir_pipeline* st,
                              const rsx_nir_transfer* transfer,
                              const uint32_t* words)
{
    if (InterlockedExchange(&g_active.renderer_owner, 1) == 0 &&
        !InterlockedCompareExchange(&g_active.shared_timeline, 0, 0))
        rsx_live_draw_flush();
    const int result = g_active.gpu_ops.transfer
        ? g_active.gpu_ops.transfer(g_active.gpu_ops.user, st, transfer,
                                    words) : -1;
    if (result)
        InterlockedExchange(&g_active.renderer_owner, 0);
    return result;
}

static int yz_nr_gpu_present(void*, uint32_t buffer)
{
    return g_active.gpu_ops.present
        ? g_active.gpu_ops.present(g_active.gpu_ops.user, buffer) : -1;
}

static void yz_nr_gpu_flush(void*)
{
    if (g_active.gpu_ops.flush)
        g_active.gpu_ops.flush(g_active.gpu_ops.user);
}

static int yz_nr_borrow_color(void*, uint32_t space, uint32_t offset,
                              uint32_t width, uint32_t height,
                              void** resource, uint32_t* format)
{
    return rsx_live_draw_borrow_color(
        space, offset, width, height, resource, format);
}

static int yz_nr_borrow_depth(void*, uint32_t space, uint32_t offset,
                               uint32_t depth_format, uint32_t width,
                               uint32_t height, void** resource,
                               uint32_t* resource_format,
                               uint32_t* dsv_format, uint32_t* srv_format,
                               void** sample_resource,
                               uint32_t* sample_srv_format,
                               int* publication_required)
{
    const int result = rsx_live_draw_borrow_depth(
        space, offset, depth_format, width, height, resource,
        resource_format, dsv_format, srv_format, sample_resource,
        sample_srv_format, publication_required);
    if (!result && *publication_required &&
        !InterlockedCompareExchange(&g_active.shared_timeline, 0, 0)) {
        /* zdepth_get may have created this borrowed resource and recorded its
         * initialization clear on the legacy queue.  The native backend uses
         * a separate queue, so returning the resource before that list is
         * submitted is a cross-queue race.  A backend cache miss invokes this
         * broker only once per exact zeta identity; make that publication a
         * fenced handoff before the native list can reference the resource.
         * Existing legacy work was already flushed when renderer_owner moved
         * to native, so this retires only a newly created resource's
         * initialization. Re-resolving an unchanged cache entry is free of
         * queue handoffs. */
        rsx_live_draw_flush();
    }
    return result;
}

static int yz_nr_resolve_depth_sample(void*, uint32_t space, uint32_t offset,
                                      uint32_t width, uint32_t height)
{
    return rsx_live_draw_resolve_depth_sample(
        space, offset, width, height);
}

static int yz_nr_render_condition_read(void*, uint32_t dma,
                                        uint32_t offset, uint32_t* value)
{
    return yz_nr_vertical_render_condition_read(dma, offset, value);
}

static void yz_nr_active_ensure_graphics(void)
{
    if (!InterlockedCompareExchange(
            &g_vertical.mode_active_graphics, 0, 0) ||
        InterlockedCompareExchange(&g_active.graphics_ready, 0, 0))
        return;
    void* const device = rsx_live_draw_get_d3d12_device();
    if (!device)
        return;
    rsx_nr_d3d12* const d3d12 = rsx_nr_d3d12_create(
        device, YZ_GCM_LOCAL_SIZE, 0x10000000u,
        yz_nr_d3d_guest_ptr, yz_nr_d3d_guest_writable_ptr, nullptr);
    if (!d3d12)
        return;
    if (rsx_nr_d3d12_set_live_output(
            d3d12, 1, yz_nr_d3d_present, nullptr) != 0) {
        rsx_nr_d3d12_destroy(d3d12);
        return;
    }
    rsx_nr_d3d12_set_watch_page(d3d12, yz_nr_d3d_watch_page, nullptr);
    rsx_nr_d3d12_set_resource_broker(
        d3d12, yz_nr_borrow_color, yz_nr_borrow_depth,
        yz_nr_resolve_depth_sample, nullptr);
    rsx_nr_d3d12_set_publish_write(
        d3d12, yz_nr_d3d_publish_write, nullptr);
    rsx_nr_d3d12_set_render_condition_reader(
        d3d12, yz_nr_render_condition_read, nullptr);
    if (rsx_nr_d3d12_set_shared_timeline(
            d3d12, yz_nr_d3d_timeline_acquire,
            yz_nr_d3d_timeline_release, yz_nr_d3d_timeline_flush,
            nullptr) != 0) {
        rsx_nr_d3d12_destroy(d3d12);
        return;
    }
    InterlockedExchange(&g_active.shared_timeline, 1);
    for (uint32_t i = 0; i < 8u; ++i) {
        const yz_nr_vertical_display* const display = &g_active.displays[i];
        if (display->valid)
            rsx_nr_d3d12_set_display_buffer(
                d3d12, i, display->location, display->offset,
                display->width, display->height);
    }

    memset(&g_active.gpu_ops, 0, sizeof(g_active.gpu_ops));
    rsx_nr_d3d12_get_exec_ops(d3d12, &g_active.gpu_ops);
    rsx_nr_exec_ops combined = {};
    combined.clear = yz_nr_gpu_clear;
    combined.draw = yz_nr_gpu_draw;
    combined.transfer = yz_nr_gpu_transfer;
    combined.present = yz_nr_gpu_present;
    combined.flush = yz_nr_gpu_flush;
    combined.set_reference = yz_nr_exec_reference;
    combined.user_command = yz_nr_exec_user;
    combined.sem_read = yz_nr_exec_sem_read;
    combined.sem_write = yz_nr_exec_sem_write;
    combined.report = yz_nr_exec_report;
    g_active.backend.ops = combined;
    g_active.d3d12 = d3d12;
    MemoryBarrier();
    InterlockedExchange(&g_active.graphics_ready, 1);
}

static int yz_nr_active_init(int graphics)
{
    if (rsx_nr_span_router_init(&g_active.router,
                                YZ_NR_ACTIVE_ROUTER_CAPACITY) != 0)
        return 0;
    /* Fast/exact miss counts would turn every unrelated FIFO packet into an
     * atomic RMW. Owned/fallback/claim counts remain available at shutdown. */
    rsx_nr_span_router_set_miss_counting(&g_active.router, 0);
    rsx_nr_tokens_init(&g_active.tokens);
    if (rsx_nr_ring_init_fixed(&g_active.ring, g_active.slots,
                               YZ_NR_ACTIVE_RING_CAPACITY, g_active.side,
                               YZ_NR_ACTIVE_SIDE_CAPACITY) != 0) {
        rsx_nr_span_router_destroy(&g_active.router);
        return 0;
    }
    const rsx_nir_sink sink = rsx_nr_ring_sink(&g_active.ring);
    rsx_nir_adapter_init_sink(&g_active.adapter, &sink);
    g_active.adapter.shadow_mode = 1;
    if (graphics) {
        g_active.guest_page_route = (volatile LONG*)calloc(
            YZ_NR_ACTIVE_GUEST_PAGE_COUNT, sizeof(LONG));
        if (!g_active.guest_page_route) {
            rsx_nr_ring_destroy(&g_active.ring);
            rsx_nr_span_router_destroy(&g_active.router);
            return 0;
        }
        if (g_active.frame_islands) {
            g_active.section_ops = static_cast<rsx_nir_op*>(calloc(
                YZ_NR_SECTION_OP_CAPACITY, sizeof(rsx_nir_op)));
            g_active.section_side = static_cast<uint32_t*>(calloc(
                YZ_NR_SECTION_SIDE_CAPACITY, sizeof(uint32_t)));
            g_active.section_methods =
                static_cast<yz_nr_section_method*>(calloc(
                    YZ_NR_SECTION_METHOD_CAPACITY,
                    sizeof(yz_nr_section_method)));
            g_active.section_adapter =
                static_cast<rsx_nir_adapter*>(calloc(
                    1, sizeof(rsx_nir_adapter)));
            if (!g_active.section_ops || !g_active.section_side ||
                !g_active.section_methods || !g_active.section_adapter) {
                free(g_active.section_ops);
                free(g_active.section_side);
                free(g_active.section_methods);
                free(g_active.section_adapter);
                free(const_cast<LONG*>(g_active.guest_page_route));
                g_active.section_ops = nullptr;
                g_active.section_side = nullptr;
                g_active.section_methods = nullptr;
                g_active.section_adapter = nullptr;
                g_active.guest_page_route = nullptr;
                rsx_nr_ring_destroy(&g_active.ring);
                rsx_nr_span_router_destroy(&g_active.router);
                return 0;
            }
            rsx_nir_stream_init_fixed(
                &g_active.section_stream, g_active.section_ops,
                YZ_NR_SECTION_OP_CAPACITY, g_active.section_side,
                YZ_NR_SECTION_SIDE_CAPACITY);
        }
    }
    rsx_nr_exec_ops ops = {};
    ops.set_reference = yz_nr_exec_reference;
    ops.user_command = yz_nr_exec_user;
    ops.present = yz_nr_exec_present;
    ops.sem_read = yz_nr_exec_sem_read;
    rsx_nr_backend_init(&g_active.backend, &g_active.ring,
                        &g_active.tokens, &ops);
    return 1;
}

static int yz_nr_active_publish(ppu_context* ctx, uint32_t family,
                                uint32_t value)
{
    if (!InterlockedCompareExchange(&g_vertical.mode_active_basic, 0, 0))
        return 0;
    if (g_active.frame_islands)
        return 0; /* raw packet belongs to the transactional section gate */
    const uint32_t context = (uint32_t)ctx->gpr[3];
    if (context != YZ_GCM_CTX_ADDR) {
        g_active.wrong_context++;
        g_active.fallback[family]++;
        return 0;
    }

    AcquireSRWLockExclusive(&g_active.producer_lock);
    const uint32_t current = vm_read32((uint64_t)context + 8u);
    const uint32_t end = vm_read32((uint64_t)context + 4u);
    if ((current & 3u) || current > end || 8u > end - current) {
        g_active.no_room++;
        g_active.fallback[family]++;
        ReleaseSRWLockExclusive(&g_active.producer_lock);
        return 0;
    }

    rsx_nr_span span = {};
    span.ea = current;
    span.word_count = 2;
    span.generation = rsx_nr_span_router_generation(&g_active.router);
    span.flags = RSX_NR_SPAN_RETAINED_FALLBACK;
    span.payload.op_count = 1;
    if (family == YZ_NR_VERT_REFERENCE) {
        span.payload.ops[0].kind = RSX_NIR_OP_SET_REFERENCE;
        span.payload.ops[0].u.reference.value = value;
    } else if (family == YZ_NR_VERT_USER) {
        span.payload.ops[0].kind = RSX_NIR_OP_USER_COMMAND;
        span.payload.ops[0].u.user_command.cause = value;
    } else {
        ReleaseSRWLockExclusive(&g_active.producer_lock);
        return 0;
    }

    /* Preserve the exact wire packet even when this address is typed-owned.
     * The title's EDGE journal can copy or replay command bytes later at a
     * different address; that copy deliberately has no sidecar record and
     * must remain a complete legacy fallback, not a pair of lost NOPs. The
     * consumer skips decoding only at this exact original EA. */
    const uint32_t method =
        family == YZ_NR_VERT_REFERENCE ? 0x0050u : 0xEB00u;
    vm_write32(current + 0u, (1u << 18) | method);
    vm_write32(current + 4u, value);
    if (rsx_nr_span_router_publish(&g_active.router, &span) !=
        RSX_NR_SPAN_PUBLISHED) {
        g_active.publish_failure++;
        g_active.fallback[family]++;
        ReleaseSRWLockExclusive(&g_active.producer_lock);
        return 0;
    }
    MemoryBarrier();
    vm_write32((uint64_t)context + 8u, current + 8u);
    g_active.owned[family]++;
    ReleaseSRWLockExclusive(&g_active.producer_lock);
    return 1;
}

static int yz_nr_active_publish_flip(uint32_t context,
                                     const rsx_nr_flip_contract* flip)
{
    if (!InterlockedCompareExchange(&g_vertical.mode_active_present, 0, 0))
        return 0;
    if (g_active.frame_islands)
        return 0;
    if (context != YZ_GCM_CTX_ADDR) {
        g_active.wrong_context++;
        g_active.fallback[YZ_NR_VERT_FLIP]++;
        return 0;
    }

    AcquireSRWLockExclusive(&g_active.producer_lock);
    const uint32_t current = vm_read32((uint64_t)context + 8u);
    const uint32_t end = vm_read32((uint64_t)context + 4u);
    const uint32_t bytes = flip->word_count * 4u;
    if ((current & 3u) || current > end || bytes > end - current) {
        g_active.no_room++;
        g_active.fallback[YZ_NR_VERT_FLIP]++;
        ReleaseSRWLockExclusive(&g_active.producer_lock);
        return 0;
    }

    rsx_nr_span span = {};
    span.ea = current;
    span.word_count = flip->word_count;
    span.generation = rsx_nr_span_router_generation(&g_active.router);
    span.flags = RSX_NR_SPAN_RETAINED_FALLBACK;
    span.payload.op_count = flip->wait_for_label ? 2u : 1u;
    uint32_t op = 0;
    if (flip->wait_for_label) {
        span.payload.ops[op].kind = RSX_NIR_OP_SEMAPHORE_ACQUIRE;
        span.payload.ops[op].u.semaphore.dma_context = 0x66616661u;
        span.payload.ops[op].u.semaphore.offset = flip->label_offset;
        span.payload.ops[op].u.semaphore.value = flip->label_value;
        op++;
    }
    span.payload.ops[op].kind = RSX_NIR_OP_PRESENT;
    span.payload.ops[op].u.present.buffer = flip->buffer_id;

    for (uint32_t i = 0; i < flip->word_count; ++i)
        vm_write32((uint64_t)current + i * 4u, flip->words[i]);
    if (rsx_nr_span_router_publish(&g_active.router, &span) !=
        RSX_NR_SPAN_PUBLISHED) {
        g_active.publish_failure++;
        g_active.fallback[YZ_NR_VERT_FLIP]++;
        ReleaseSRWLockExclusive(&g_active.producer_lock);
        return 0;
    }
    MemoryBarrier();
    vm_write32((uint64_t)context + 8u, current + bytes);
    g_active.owned[YZ_NR_VERT_FLIP]++;
    ReleaseSRWLockExclusive(&g_active.producer_lock);
    return 1;
}

static int yz_nr_active_publish_draw_arrays(ppu_context* ctx)
{
    if (!InterlockedCompareExchange(
            &g_vertical.mode_active_graphics, 0, 0) ||
        !(g_active.graphics_families & YZ_NR_GRAPHICS_DRAW) ||
        !InterlockedCompareExchange(&g_active.graphics_ready, 0, 0) ||
        !rsx_live_draw_native_actions_allowed())
        return 0;
    if (g_active.frame_islands)
        return 0;
    const uint32_t context = static_cast<uint32_t>(ctx->gpr[3]);
    if (context != YZ_GCM_CTX_ADDR) {
        g_active.wrong_context++;
        g_active.fallback[YZ_NR_VERT_DRAW_ARRAYS]++;
        return 0;
    }
    rsx_nr_draw_arrays_contract draw = {};
    if (!rsx_nr_draw_arrays_contract_init(
            &draw, static_cast<uint32_t>(ctx->gpr[4]),
            static_cast<uint32_t>(ctx->gpr[5]),
            static_cast<uint32_t>(ctx->gpr[6])) ||
        draw.batch_count > RSX_NR_SPAN_MAX_SIDE / 2u ||
        draw.packet_word_count > 64u) {
        g_active.fallback[YZ_NR_VERT_DRAW_ARRAYS]++;
        return 0;
    }

    uint32_t packet[64] = {};
    if (rsx_nr_draw_arrays_packet(&draw, packet, 64u) !=
        draw.packet_word_count) {
        g_active.fallback[YZ_NR_VERT_DRAW_ARRAYS]++;
        return 0;
    }

    AcquireSRWLockExclusive(&g_active.producer_lock);
    const uint32_t current = vm_read32(static_cast<uint64_t>(context) + 8u);
    const uint32_t end = vm_read32(static_cast<uint64_t>(context) + 4u);
    const uint32_t bytes = draw.packet_word_count * 4u;
    if ((current & 3u) || current > end || bytes > end - current) {
        g_active.no_room++;
        g_active.fallback[YZ_NR_VERT_DRAW_ARRAYS]++;
        ReleaseSRWLockExclusive(&g_active.producer_lock);
        return 0;
    }

    rsx_nr_span span = {};
    span.ea = current;
    span.word_count = draw.packet_word_count;
    span.generation = rsx_nr_span_router_generation(&g_active.router);
    span.flags = RSX_NR_SPAN_RETAINED_FALLBACK;
    span.payload.op_count = 1u;
    span.payload.side_count = draw.batch_count * 2u;
    rsx_nir_op* const op = &span.payload.ops[0];
    op->kind = RSX_NIR_OP_DRAW;
    op->u.draw.primitive = draw.primitive;
    op->u.draw.indexed = 0;
    op->u.draw.batch_count = draw.batch_count;
    op->u.draw.total_count = draw.count;
    const uint32_t first_count = ((draw.count - 1u) & 0xFFu) + 1u;
    uint32_t cursor = draw.first;
    span.payload.side[0] = cursor;
    span.payload.side[1] = first_count;
    cursor += first_count;
    for (uint32_t i = 1u; i < draw.batch_count; ++i) {
        span.payload.side[i * 2u] = cursor;
        span.payload.side[i * 2u + 1u] = 256u;
        cursor += 256u;
    }

    for (uint32_t i = 0; i < draw.packet_word_count; ++i)
        vm_write32(static_cast<uint64_t>(current) + i * 4u, packet[i]);
    if (rsx_nr_span_router_publish(&g_active.router, &span) !=
        RSX_NR_SPAN_PUBLISHED) {
        g_active.publish_failure++;
        g_active.fallback[YZ_NR_VERT_DRAW_ARRAYS]++;
        ReleaseSRWLockExclusive(&g_active.producer_lock);
        return 0;
    }
    MemoryBarrier();
    vm_write32(static_cast<uint64_t>(context) + 8u, current + bytes);
    g_active.owned[YZ_NR_VERT_DRAW_ARRAYS]++;
    ReleaseSRWLockExclusive(&g_active.producer_lock);
    return 1;
}

static unsigned long long yz_nr_hash_word(unsigned long long hash,
                                          uint32_t word)
{
    if (!hash)
        hash = 1469598103934665603ull;
    for (unsigned i = 0; i < 4; ++i) {
        hash ^= (unsigned char)(word >> (i * 8));
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint32_t yz_nr_event_hash(uint32_t packet_ea)
{
    uint32_t value = packet_ea >> 2;
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    return value & (YZ_NR_VERT_EVENT_COUNT - 1u);
}

/* g_vertical.lock must be held. */
static void yz_nr_event_expect(uint32_t packet_ea, uint32_t family,
                               uint32_t a, uint32_t b)
{
    const uint32_t first = yz_nr_event_hash(packet_ea);
    uint32_t candidate = YZ_NR_VERT_EVENT_COUNT;
    for (uint32_t probe = 0; probe < YZ_NR_VERT_EVENT_COUNT; ++probe) {
        const uint32_t index =
            (first + probe) & (YZ_NR_VERT_EVENT_COUNT - 1u);
        yz_nr_vertical_event* const event = &g_vertical.events[index];
        if (event->state == YZ_NR_EVENT_READY &&
            event->packet_ea == packet_ea) {
            /* One command address cannot be republished until the consumer
             * retires it. Treat a duplicate as a coverage failure. */
            g_vertical.exact_overflow++;
            return;
        }
        if (event->state == YZ_NR_EVENT_TOMBSTONE &&
            candidate == YZ_NR_VERT_EVENT_COUNT)
            candidate = index;
        if (event->state == YZ_NR_EVENT_EMPTY) {
            if (candidate == YZ_NR_VERT_EVENT_COUNT)
                candidate = index;
            break;
        }
    }
    if (candidate == YZ_NR_VERT_EVENT_COUNT) {
        g_vertical.exact_overflow++;
        return;
    }
    yz_nr_vertical_event* const event = &g_vertical.events[candidate];
    event->packet_ea = packet_ea;
    event->family = family;
    event->a = a;
    event->b = b;
    event->state = YZ_NR_EVENT_READY;
}

/* g_vertical.lock must be held. */
static void yz_nr_event_observe(uint32_t packet_ea, uint32_t family,
                                uint32_t a, uint32_t b)
{
    const uint32_t first = yz_nr_event_hash(packet_ea);
    for (uint32_t probe = 0; probe < YZ_NR_VERT_EVENT_COUNT; ++probe) {
        const uint32_t index =
            (first + probe) & (YZ_NR_VERT_EVENT_COUNT - 1u);
        yz_nr_vertical_event* const event = &g_vertical.events[index];
        if (event->state == YZ_NR_EVENT_EMPTY)
            break;
        if (event->state != YZ_NR_EVENT_READY ||
            event->packet_ea != packet_ea)
            continue;
        if (event->family == family && event->a == a && event->b == b)
            g_vertical.exact_matches++;
        else
            g_vertical.exact_mismatches++;
        event->state = YZ_NR_EVENT_TOMBSTONE;
        return;
    }
    g_vertical.exact_unexpected++;
}

/* Find, but do not retire, an exact event. The draw contract is observed
 * across BEGIN, one or more batch packets, and END, so its event remains
 * ready until the normalized action is complete. g_vertical.lock is held. */
static yz_nr_vertical_event* yz_nr_event_find(uint32_t packet_ea,
                                              uint32_t family)
{
    const uint32_t first = yz_nr_event_hash(packet_ea);
    for (uint32_t probe = 0; probe < YZ_NR_VERT_EVENT_COUNT; ++probe) {
        yz_nr_vertical_event* const event =
            &g_vertical.events[(first + probe) &
                               (YZ_NR_VERT_EVENT_COUNT - 1u)];
        if (event->state == YZ_NR_EVENT_EMPTY)
            break;
        if (event->state == YZ_NR_EVENT_READY &&
            event->packet_ea == packet_ea && event->family == family)
            return event;
    }
    return nullptr;
}

/* State methods may also be emitted by prebuilt, copied, or SPU-published
 * command groups.  Those are valid fallback producers, not mismatches in the
 * selected wrapper contract.  Match only an exact outstanding wrapper EA;
 * account every other instance separately as unowned coverage.  The caller
 * holds g_vertical.lock. */
static bool yz_nr_event_observe_optional(uint32_t packet_ea, uint32_t family,
                                         uint32_t a, uint32_t b)
{
    const uint32_t first = yz_nr_event_hash(packet_ea);
    for (uint32_t probe = 0; probe < YZ_NR_VERT_EVENT_COUNT; ++probe) {
        const uint32_t index =
            (first + probe) & (YZ_NR_VERT_EVENT_COUNT - 1u);
        yz_nr_vertical_event* const event = &g_vertical.events[index];
        if (event->state == YZ_NR_EVENT_EMPTY)
            break;
        if (event->state != YZ_NR_EVENT_READY ||
            event->packet_ea != packet_ea)
            continue;
        if (event->family == family && event->a == a && event->b == b)
            g_vertical.exact_matches++;
        else
            g_vertical.exact_mismatches++;
        event->state = YZ_NR_EVENT_TOMBSTONE;
        return true;
    }
    g_vertical.state_unowned_methods++;
    return false;
}

static void yz_nr_note(yz_nr_vertical_lane* lane, uint32_t family,
                       uint32_t a, uint32_t b)
{
    if (family == 0 || family >= YZ_NR_VERT_FAMILY_COUNT)
        return;
    lane->count[family]++;
    unsigned long long ordered = lane->ordered_hash[family];
    ordered = yz_nr_hash_word(ordered, family);
    ordered = yz_nr_hash_word(ordered, a);
    ordered = yz_nr_hash_word(ordered, b);
    lane->ordered_hash[family] = ordered;

    unsigned long long event = yz_nr_hash_word(0, family);
    event = yz_nr_hash_word(event, a);
    event = yz_nr_hash_word(event, b);
    lane->xor_hash[family] ^= event;
    lane->sum_hash[family] += event;
}

/* g_vertical.lock must be held. */
static void yz_nr_note_source(uint32_t context, uint32_t current,
                              uint32_t family)
{
    uint32_t free_slot = YZ_NR_VERT_SOURCE_COUNT;
    for (uint32_t i = 0; i < YZ_NR_VERT_SOURCE_COUNT; ++i) {
        yz_nr_vertical_source* const source = &g_vertical.sources[i];
        if (source->context == context) {
            if (current < source->current_min)
                source->current_min = current;
            if (current > source->current_max)
                source->current_max = current;
            source->family_mask |= 1u << family;
            source->count++;
            return;
        }
        if (!source->count && free_slot == YZ_NR_VERT_SOURCE_COUNT)
            free_slot = i;
    }
    if (free_slot < YZ_NR_VERT_SOURCE_COUNT) {
        yz_nr_vertical_source* const source = &g_vertical.sources[free_slot];
        source->context = context;
        source->current_min = current;
        source->current_max = current;
        source->family_mask = 1u << family;
        source->count = 1;
    } else {
        g_vertical.source_overflow++;
    }
}

static void yz_nr_expected_from(ppu_context* ctx, uint32_t family,
                                uint32_t a, uint32_t b,
                                uint32_t packet_bytes)
{
    if (!InterlockedCompareExchange(&g_vertical.mode_shadow, 0, 0))
        return;

    const uint32_t context = (uint32_t)ctx->gpr[3];
    const uint32_t current = vm_read32((uint64_t)context + 8u);
    const uint32_t end = vm_read32((uint64_t)context + 4u);
    AcquireSRWLockExclusive(&g_vertical.lock);
    yz_nr_note(&g_vertical.expected, family, a, b);
    if (current <= end && packet_bytes <= end - current)
        yz_nr_event_expect(current, family, a, b);
    else
        g_vertical.exact_unaddressed++;
    yz_nr_note_source(context, current, family);
    ReleaseSRWLockExclusive(&g_vertical.lock);
}

static void yz_nr_expected_direct_setter(ppu_context* ctx,
                                         uint32_t function_ea)
{
    if (!InterlockedCompareExchange(&g_vertical.mode_shadow, 0, 0))
        return;
    const rsx_nr_direct_setter_contract* const contract =
        rsx_nr_direct_setter_by_function(function_ea);
    if (!contract)
        return;
    yz_nr_expected_from(ctx, YZ_NR_VERT_STATE_DIRECT, contract->method,
                        (uint32_t)ctx->gpr[4], 8u);
}

static void yz_nr_expected_draw_arrays(ppu_context* ctx)
{
    if (!InterlockedCompareExchange(&g_vertical.mode_shadow, 0, 0))
        return;

    rsx_nr_draw_arrays_contract draw = {};
    if (!rsx_nr_draw_arrays_contract_init(
            &draw, (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5],
            (uint32_t)ctx->gpr[6])) {
        AcquireSRWLockExclusive(&g_vertical.lock);
        g_vertical.draw_unsupported++;
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }

    const uint32_t context = (uint32_t)ctx->gpr[3];
    const uint32_t current = vm_read32((uint64_t)context + 8u);
    const uint32_t end = vm_read32((uint64_t)context + 4u);
    AcquireSRWLockExclusive(&g_vertical.lock);
    /* The audited wrapper's BEGIN header starts 16 bytes after its leading
     * three-word 0x1714 state packet. If the initial 0x20-byte reservation
     * cannot fit, its callback may move to another segment; leave that call
     * explicitly unaddressed instead of guessing across the callback. */
    if (!(current & 3u) && current <= end && 0x20u <= end - current) {
        const uint32_t begin_ea = current + 16u;
        yz_nr_note(&g_vertical.expected, YZ_NR_VERT_DRAW_ARRAYS,
                   draw.primitive, draw.semantic_hash);
        yz_nr_event_expect(begin_ea, YZ_NR_VERT_DRAW_ARRAYS,
                           draw.primitive, draw.semantic_hash);
        yz_nr_note_source(context, current, YZ_NR_VERT_DRAW_ARRAYS);
    } else {
        g_vertical.exact_unaddressed++;
    }
    ReleaseSRWLockExclusive(&g_vertical.lock);
}

static void yz_nr_expected_vertex_program(ppu_context* ctx)
{
    if (!InterlockedCompareExchange(&g_vertical.mode_shadow, 0, 0))
        return;

    const uint32_t program = (uint32_t)ctx->gpr[4];
    const uint32_t ucode = (uint32_t)ctx->gpr[5];
    const uint32_t descriptor = program + vm_read32((uint64_t)program + 0x14u);
    const uint32_t instruction_count = vm_read32(descriptor);
    const uint32_t start_slot = vm_read32((uint64_t)descriptor + 4u);
    const uint32_t input_mask = vm_read32((uint64_t)descriptor + 0xCu);
    if (!instruction_count ||
        instruction_count > RSX_NR_VERTEX_PROGRAM_MAX_WORDS / 4u) {
        AcquireSRWLockExclusive(&g_vertical.lock);
        g_vertical.exact_unaddressed++;
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }

    uint32_t words[RSX_NR_VERTEX_PROGRAM_MAX_WORDS];
    const uint32_t word_count = instruction_count * 4u;
    for (uint32_t i = 0; i < word_count; ++i)
        words[i] = vm_read32((uint64_t)ucode + i * 4u);

    rsx_nr_vertex_program_contract vp = {};
    if (!rsx_nr_vertex_program_contract_init(
            &vp, instruction_count, start_slot, input_mask, words)) {
        AcquireSRWLockExclusive(&g_vertical.lock);
        g_vertical.exact_unaddressed++;
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }

    AcquireSRWLockExclusive(&g_vertical.lock);
    g_vertical.vp_template_builds++;
    uint32_t free_slot = YZ_NR_VERT_VP_TEMPLATE_COUNT;
    for (uint32_t i = 0; i < YZ_NR_VERT_VP_TEMPLATE_COUNT; ++i) {
        yz_nr_vertical_vp_template* const entry =
            &g_vertical.vp_templates[i];
        if (entry->build_count && entry->start_slot == vp.start_slot &&
            entry->semantic_hash == vp.semantic_hash) {
            entry->build_count++;
            free_slot = YZ_NR_VERT_VP_TEMPLATE_COUNT;
            break;
        }
        if (!entry->build_count && free_slot == YZ_NR_VERT_VP_TEMPLATE_COUNT)
            free_slot = i;
    }
    if (free_slot < YZ_NR_VERT_VP_TEMPLATE_COUNT) {
        yz_nr_vertical_vp_template* const entry =
            &g_vertical.vp_templates[free_slot];
        entry->start_slot = vp.start_slot;
        entry->code_hash = vp.code_hash;
        entry->word_count = vp.word_count;
        entry->input_mask = vp.attrib_input_mask;
        entry->semantic_hash = vp.semantic_hash;
        entry->build_count = 1;
    } else {
        bool found = false;
        for (uint32_t i = 0; i < YZ_NR_VERT_VP_TEMPLATE_COUNT; ++i) {
            const yz_nr_vertical_vp_template* const entry =
                &g_vertical.vp_templates[i];
            if (entry->build_count && entry->start_slot == vp.start_slot &&
                entry->semantic_hash == vp.semantic_hash) {
                found = true;
                break;
            }
        }
        if (!found)
            g_vertical.vp_template_overflow++;
    }
    ReleaseSRWLockExclusive(&g_vertical.lock);
}

struct yz_nr_fragment_identity {
    uint32_t byte_count;
    unsigned long long content_hash;
    unsigned long long structural_hash;
};

static bool yz_nr_fragment_identity_from_binding(
    uint32_t program_word, yz_nr_fragment_identity* out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    /* Match the live dispatcher's audited NV4097 location interpretation:
     * low bits 2 select MAIN; all other title-produced encodings select
     * LOCAL. The address itself is always four-byte aligned. */
    const uint32_t location = (program_word & 3u) == 2u ? 1u : 0u;
    const uint32_t offset = program_word & ~3u;
    const uint8_t* bytes = yz_nr_vertical_guest_ptr(
        location, offset, RSX_NR_FRAGMENT_PROGRAM_MAX_BYTES);
    if (!bytes)
        return false;
    const uint32_t byte_count = rsx_fp_program_size(
        bytes, RSX_NR_FRAGMENT_PROGRAM_MAX_BYTES);
    if (!byte_count)
        return false;
    const unsigned long long content =
        rsx_nr_fragment_program_content_hash(bytes, byte_count);
    const unsigned long long structural = rsx_fp_structural_hash(
        bytes, byte_count, 1469598103934665603ull);
    if (!content || !structural)
        return false;
    out->byte_count = byte_count;
    out->content_hash = content;
    out->structural_hash = structural;
    return true;
}

/* Fixed-memory open addressing keeps the default-off shadow census bounded
 * without turning every draw into a linear scan of all built programs.  The
 * tables never delete entries, so an empty slot terminates a miss. */
static yz_nr_vertical_fp_template* yz_nr_find_fragment_exact(
    yz_nr_vertical_fp_template* table, unsigned long long semantic)
{
    const uint32_t mask = YZ_NR_VERT_FP_TEMPLATE_COUNT - 1u;
    uint32_t slot = (uint32_t)semantic & mask;
    for (uint32_t probe = 0; probe < YZ_NR_VERT_FP_TEMPLATE_COUNT; ++probe) {
        yz_nr_vertical_fp_template* const entry = &table[slot];
        if (!entry->build_count && !entry->replay_count)
            return nullptr;
        if (entry->semantic_hash == semantic)
            return entry;
        slot = (slot + 1u) & mask;
    }
    return nullptr;
}

static uint32_t yz_nr_find_fragment_free(
    yz_nr_vertical_fp_template* table, unsigned long long semantic)
{
    const uint32_t mask = YZ_NR_VERT_FP_TEMPLATE_COUNT - 1u;
    uint32_t slot = (uint32_t)semantic & mask;
    for (uint32_t probe = 0; probe < YZ_NR_VERT_FP_TEMPLATE_COUNT; ++probe) {
        yz_nr_vertical_fp_template* const entry = &table[slot];
        if (!entry->build_count && !entry->replay_count)
            return slot;
        if (entry->semantic_hash == semantic)
            return slot;
        slot = (slot + 1u) & mask;
    }
    return YZ_NR_VERT_FP_TEMPLATE_COUNT;
}

/* g_vertical.lock must be held. */
static bool yz_nr_register_fragment_template(
    const yz_nr_fragment_identity* identity, uint32_t control)
{
    if (!identity || !identity->byte_count)
        return false;
    const unsigned long long semantic =
        rsx_nr_fragment_program_semantic_hash(
            identity->content_hash, identity->byte_count, control);
    const unsigned long long structural_semantic =
        rsx_nr_fragment_program_semantic_hash(
            identity->structural_hash, identity->byte_count, control);
    if (!semantic || !structural_semantic)
        return false;

    const uint32_t free_slot = yz_nr_find_fragment_free(
        g_vertical.fp_templates, semantic);
    if (free_slot == YZ_NR_VERT_FP_TEMPLATE_COUNT) {
        g_vertical.fp_template_overflow++;
        return false;
    }
    yz_nr_vertical_fp_template* const entry =
        &g_vertical.fp_templates[free_slot];
    if (entry->build_count) {
        entry->build_count++;
        return true;
    }
    entry->content_hash = identity->content_hash;
    entry->structural_hash = identity->structural_hash;
    entry->semantic_hash = semantic;
    entry->structural_semantic_hash = structural_semantic;
    entry->byte_count = identity->byte_count;
    entry->control = control;
    entry->build_count = 1;

    const uint32_t structural_mask =
        YZ_NR_VERT_FP_STRUCTURAL_INDEX_COUNT - 1u;
    uint32_t structural_slot =
        (uint32_t)structural_semantic & structural_mask;
    for (uint32_t probe = 0;
         probe < YZ_NR_VERT_FP_STRUCTURAL_INDEX_COUNT; ++probe) {
        const uint16_t encoded =
            g_vertical.fp_structural_index[structural_slot];
        if (!encoded) {
            g_vertical.fp_structural_index[structural_slot] =
                (uint16_t)(free_slot + 1u);
            break;
        }
        const yz_nr_vertical_fp_template* const indexed =
            &g_vertical.fp_templates[(uint32_t)encoded - 1u];
        if (indexed->structural_semantic_hash == structural_semantic)
            break;
        structural_slot = (structural_slot + 1u) & structural_mask;
    }
    return true;
}

/* Passive-only postcondition oracle for the game's package builder. The
 * wrapper is entered before packet construction; after the unchanged lifted
 * body completes, output+4/output+8 identify its relocated reusable segment.
 * We inspect only the two exact one-argument shader words needed to prove the
 * future pre-packet typed contract. No active path calls this oracle. */
static void yz_nr_expected_fragment_program(uint32_t output)
{
    if (!InterlockedCompareExchange(&g_vertical.mode_shadow, 0, 0))
        return;
    const uint32_t segment = vm_read32((uint64_t)output + 4u);
    const uint32_t byte_count = vm_read32((uint64_t)output + 8u);
    if (!segment || byte_count < 16u || (byte_count & 3u) ||
        byte_count > 0x100000u) {
        AcquireSRWLockExclusive(&g_vertical.lock);
        g_vertical.fp_unresolved++;
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }

    uint32_t program_word = 0;
    uint32_t control = 0;
    bool have_program = false;
    bool have_control = false;
    const uint32_t words = byte_count / 4u;
    for (uint32_t i = 0; i + 1u < words; ++i) {
        const uint32_t word = vm_read32((uint64_t)segment + i * 4u);
        if (!have_program && word == 0x000408E4u) {
            program_word = vm_read32((uint64_t)segment + (i + 1u) * 4u);
            have_program = true;
        } else if (!have_control && word == 0x00041D60u) {
            control = vm_read32((uint64_t)segment + (i + 1u) * 4u);
            have_control = true;
        }
        if (have_program && have_control)
            break;
    }

    yz_nr_fragment_identity identity = {};
    if (!have_program || !have_control ||
        !yz_nr_fragment_identity_from_binding(program_word, &identity)) {
        AcquireSRWLockExclusive(&g_vertical.lock);
        g_vertical.fp_unresolved++;
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }

    AcquireSRWLockExclusive(&g_vertical.lock);
    g_vertical.fp_template_builds++;
    yz_nr_register_fragment_template(&identity, control);
    ReleaseSRWLockExclusive(&g_vertical.lock);
}

/* g_vertical.lock must be held. */
static void yz_nr_observe_fragment_template(
    const yz_nr_fragment_identity* identity, uint32_t control,
    uint32_t packet_ea)
{
    if (!identity || !identity->byte_count) {
        g_vertical.fp_unresolved++;
        return;
    }
    const unsigned long long semantic =
        rsx_nr_fragment_program_semantic_hash(
            identity->content_hash, identity->byte_count, control);
    const unsigned long long structural_semantic =
        rsx_nr_fragment_program_semantic_hash(
            identity->structural_hash, identity->byte_count, control);
    yz_nr_vertical_fp_template* entry = yz_nr_find_fragment_exact(
        g_vertical.fp_templates, semantic);
    bool matched = entry && entry->build_count;
    if (matched) {
        entry->replay_count++;
        g_vertical.fp_template_replays++;
    }
    if (!matched) {
        /* UpdateFragmentProgramParameter mutates inline constants in place.
         * Buffered native shaders deliberately retain the structural shader
         * and upload the new exact constants, so this is a covered data
         * variant rather than an unknown program. */
        const uint32_t structural_mask =
            YZ_NR_VERT_FP_STRUCTURAL_INDEX_COUNT - 1u;
        uint32_t structural_slot =
            (uint32_t)structural_semantic & structural_mask;
        for (uint32_t probe = 0;
             probe < YZ_NR_VERT_FP_STRUCTURAL_INDEX_COUNT; ++probe) {
            const uint16_t encoded =
                g_vertical.fp_structural_index[structural_slot];
            if (!encoded)
                break;
            entry = &g_vertical.fp_templates[(uint32_t)encoded - 1u];
            if (entry->build_count &&
                entry->structural_semantic_hash == structural_semantic) {
                entry->replay_count++;
                entry->parameter_replay_count++;
                g_vertical.fp_template_replays++;
                g_vertical.fp_template_parameter_replays++;
                matched = true;
                break;
            }
            structural_slot = (structural_slot + 1u) & structural_mask;
        }
    }
    if (!matched) {
        g_vertical.fp_template_unknown++;
        yz_nr_vertical_fp_template* const known_unknown =
            yz_nr_find_fragment_exact(
                g_vertical.fp_unknown_templates, semantic);
        if (known_unknown) {
            known_unknown->replay_count++;
        } else {
            const uint32_t free_slot = yz_nr_find_fragment_free(
                g_vertical.fp_unknown_templates, semantic);
            if (free_slot < YZ_NR_VERT_FP_TEMPLATE_COUNT) {
                yz_nr_vertical_fp_template* const unknown_entry =
                    &g_vertical.fp_unknown_templates[free_slot];
                unknown_entry->content_hash = identity->content_hash;
                unknown_entry->structural_hash = identity->structural_hash;
                unknown_entry->semantic_hash = semantic;
                unknown_entry->structural_semantic_hash =
                    structural_semantic;
                unknown_entry->byte_count = identity->byte_count;
                unknown_entry->control = control;
                unknown_entry->replay_count = 1;
            } else {
                g_vertical.fp_unknown_overflow++;
            }
        }
    }
    if (!g_vertical.observed_ea_min[YZ_NR_VERT_FRAGMENT_PROGRAM] ||
        packet_ea <
            g_vertical.observed_ea_min[YZ_NR_VERT_FRAGMENT_PROGRAM])
        g_vertical.observed_ea_min[YZ_NR_VERT_FRAGMENT_PROGRAM] = packet_ea;
    if (packet_ea >
        g_vertical.observed_ea_max[YZ_NR_VERT_FRAGMENT_PROGRAM])
        g_vertical.observed_ea_max[YZ_NR_VERT_FRAGMENT_PROGRAM] = packet_ea;
}

/* Program and control are independent persistent RSX state. Observe their
 * effective pair only at a real draw begin; repeated setters are ordinary
 * state updates, not malformed two-packet transactions. */
static void yz_nr_observe_active_fragment(uint32_t draw_packet_ea)
{
    uint32_t program_word = 0;
    uint32_t control = 0;
    uint32_t program_packet_ea = 0;
    AcquireSRWLockShared(&g_vertical.lock);
    const bool complete = rsx_nr_fragment_binding_snapshot(
        &g_vertical.fp_binding, &program_word, &control) != 0;
    program_packet_ea = g_vertical.fp_packet_ea;
    ReleaseSRWLockShared(&g_vertical.lock);

    if (!complete) {
        AcquireSRWLockExclusive(&g_vertical.lock);
        g_vertical.fp_bad_sequence++;
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }

    yz_nr_fragment_identity identity = {};
    const bool resolved =
        yz_nr_fragment_identity_from_binding(program_word, &identity);
    AcquireSRWLockExclusive(&g_vertical.lock);
    if (resolved)
        yz_nr_observe_fragment_template(
            &identity, control,
            program_packet_ea ? program_packet_ea : draw_packet_ea);
    else
        g_vertical.fp_unresolved++;
    ReleaseSRWLockExclusive(&g_vertical.lock);
}

static void yz_nr_expected_flip(uint32_t context,
                                const rsx_nr_flip_contract* flip)
{
    if (!InterlockedCompareExchange(&g_vertical.mode_shadow, 0, 0))
        return;
    const uint32_t current = vm_read32((uint64_t)context + 8u);
    const uint32_t end = vm_read32((uint64_t)context + 4u);
    const uint32_t bytes = flip->word_count * 4u;
    if (!current || (current & 3u) || current > end || bytes > end - current)
        return; /* The unchanged producer will report the same refusal. */

    AcquireSRWLockExclusive(&g_vertical.lock);
    if (flip->wait_for_label) {
        /* The consumer identifies an acquire at its OFFSET header. */
        yz_nr_note(&g_vertical.expected, YZ_NR_VERT_ACQUIRE,
                   flip->label_offset, flip->label_value);
        yz_nr_event_expect(current + 8u, YZ_NR_VERT_ACQUIRE,
                           flip->label_offset, flip->label_value);
    }
    const uint32_t flip_ea = current + flip->flip_word_index * 4u;
    yz_nr_note(&g_vertical.expected, YZ_NR_VERT_FLIP,
               flip->buffer_id, 0x8000010Fu);
    yz_nr_event_expect(flip_ea, YZ_NR_VERT_FLIP,
                       flip->buffer_id, 0x8000010Fu);
    yz_nr_note_source(context, current, YZ_NR_VERT_FLIP);
    ReleaseSRWLockExclusive(&g_vertical.lock);
}

} // namespace

extern "C" void yz_nr_vertical_init(void)
{
    if (InterlockedExchange(&g_vertical.initialized, 1))
        return;
    const char* const mode = getenv("YZ_NR_VERTICAL");
    if (mode && strcmp(mode, "shadow") == 0)
        InterlockedExchange(&g_vertical.mode_shadow, 1);
    else if (mode && (strcmp(mode, "active-basic") == 0 ||
                      strcmp(mode, "active-present") == 0 ||
                      strcmp(mode, "active-graphics") == 0)) {
        const int graphics = strcmp(mode, "active-graphics") == 0;
        g_active.graphics_families = graphics
            ? yz_nr_graphics_family_mask(getenv("YZ_NR_GRAPHICS_FAMILIES"))
            : 0u;
        g_active.clear_scope = graphics
            ? yz_nr_clear_scope(getenv("YZ_NR_CLEAR_SCOPE"))
            : YZ_NR_CLEAR_ALL;
        g_active.frame_islands = graphics &&
            getenv("YZ_NR_FRAME_ISLANDS") != nullptr;
        g_active.section_diag_enabled = graphics &&
            getenv("YZ_NR_PASS_DIAG") != nullptr;
        g_active.draw_primitive_filter = UINT32_MAX;
        if (graphics) {
            const char* const primitive = getenv("YZ_NR_DRAW_PRIMITIVE");
            if (primitive && *primitive) {
                char* end = nullptr;
                const unsigned long parsed = strtoul(primitive, &end, 0);
                if (!end || *end || parsed > 10u)
                    g_active.draw_primitive_filter = UINT32_MAX - 1u;
                else
                    g_active.draw_primitive_filter =
                        static_cast<uint32_t>(parsed);
            }
        }
        if (graphics && (!g_active.graphics_families ||
                         g_active.clear_scope == UINT32_MAX ||
                         g_active.draw_primitive_filter == UINT32_MAX - 1u)) {
            fprintf(stderr,
                    "[nr-vertical-active init=failed; invalid graphics "
                    "family/clear scope]\n");
            fflush(stderr);
        } else if (yz_nr_active_init(graphics))
            InterlockedExchange(&g_vertical.mode_active_basic, 1);
        else {
            fprintf(stderr,
                    "[nr-vertical-active init=failed; legacy fallback only]\n");
            fflush(stderr);
        }
        if (InterlockedCompareExchange(&g_vertical.mode_active_basic, 0, 0) &&
            (strcmp(mode, "active-present") == 0 || graphics))
            InterlockedExchange(&g_vertical.mode_active_present, 1);
        if (InterlockedCompareExchange(&g_vertical.mode_active_basic, 0, 0) &&
            graphics) {
            InterlockedExchange(&g_vertical.mode_active_graphics, 1);
            cellSpursSetGuestWriteObserver(yz_nr_vertical_notify_guest_write);
        }
    }
}

extern "C" void yz_nr_vertical_set_display_buffer(
    uint32_t buffer_id, uint32_t location, uint32_t offset,
    uint32_t width, uint32_t height)
{
    if (buffer_id >= 8u)
        return;
    yz_nr_vertical_display* const display = &g_active.displays[buffer_id];
    display->location = location;
    display->offset = offset;
    display->width = width;
    display->height = height;
    display->valid = 1;
    if (InterlockedCompareExchange(&g_active.graphics_ready, 0, 0) &&
        g_active.d3d12)
        rsx_nr_d3d12_set_display_buffer(
            g_active.d3d12, buffer_id, location, offset, width, height);
}

extern "C" void yz_nr_vertical_notify_guest_write(uint32_t ea,
                                                    uint32_t size)
{
    if (!size || !g_active.guest_page_route ||
        !InterlockedCompareExchange(&g_active.graphics_ready, 0, 0) ||
        !g_active.d3d12)
        return;
    const uint64_t end64 = static_cast<uint64_t>(ea) + size;
    if (end64 > 0x100000000ull)
        return;
    const uint32_t last = static_cast<uint32_t>(end64 - 1u);
    uint32_t page = ea >> 12;
    const uint32_t last_page = last >> 12;
    for (;;) {
        const LONG encoded = InterlockedCompareExchange(
            &g_active.guest_page_route[page], 0, 0);
        if (encoded) {
            const uint32_t space = (static_cast<uint32_t>(encoded) >> 28) - 1u;
            const uint32_t rsx_page =
                (static_cast<uint32_t>(encoded) & 0x0FFFFFFFu) << 12;
            const uint32_t guest_page = page << 12;
            const uint32_t begin = ea > guest_page ? ea : guest_page;
            const uint32_t page_last = guest_page | 0xFFFu;
            const uint32_t finish = last < page_last ? last : page_last;
            rsx_nr_d3d12_note_guest_write(
                g_active.d3d12, space, rsx_page + (begin - guest_page),
                finish - begin + 1u);
        }
        if (page == last_page)
            break;
        ++page;
    }
}

extern "C" int yz_nr_vertical_try_flip(uint32_t context,
                                         uint32_t buffer_id,
                                         int wait_for_label,
                                         uint32_t label_index,
                                         uint32_t label_value,
                                         int32_t* result)
{
    rsx_nr_flip_contract flip = {};
    if (!rsx_nr_flip_contract_init(&flip, buffer_id, wait_for_label,
                                   label_index, label_value))
        return 0;
    yz_nr_expected_flip(context, &flip);
    if (!yz_nr_active_publish_flip(context, &flip))
        return 0;
    if (result)
        *result = 0;
    return 1;
}

extern "C" int yz_nr_vertical_try_method(uint32_t method, uint32_t arg,
                                            uint32_t packet_ea)
{
    (void)packet_ea;
    if (!InterlockedCompareExchange(
            &g_vertical.mode_active_graphics, 0, 0))
        return 0;
    if (g_active.frame_islands)
        return 0;
    /* The host movie is the sole renderer/queue owner while armed. Falling
     * through preserves the mature legacy movie suppression and prevents a
     * native clear/draw/transfer from racing the host movie command list. */
    if (!rsx_live_draw_native_actions_allowed())
        return 0;
    yz_nr_active_ensure_graphics();
    if (!InterlockedCompareExchange(&g_active.graphics_ready, 0, 0) ||
        g_active.pending_valid || rsx_nr_ring_depth(&g_active.ring))
        return 0;

    /* State and draw-batch methods continue through the legacy register
     * decoder and mirror into the shadow adapter. At a terminal action,
     * emit the complete folded state/action before the matching legacy sink
     * can run. Unsupported or atomic refusal returns to the unchanged method
     * at the same GET; success suppresses it so the action cannot execute
     * twice. */
    const bool is_draw = method == 0x1808u && arg == 0u;
    const bool is_clear = method == 0x1D94u;
    const bool is_transfer = method == 0x2328u || method == 0xC40Cu;
    const bool is_sync =
        method == 0x0110u || method == 0x1D70u || method == 0x1D74u;
    const bool is_report = method == 0x17C8u || method == 0x1800u;
    if (!is_draw && !is_clear && !is_transfer && !is_sync && !is_report)
        return 0;
    const uint32_t family = is_draw ? YZ_NR_GRAPHICS_DRAW :
        is_clear ? YZ_NR_GRAPHICS_CLEAR :
        is_transfer ? YZ_NR_GRAPHICS_TRANSFER :
        is_sync ? YZ_NR_GRAPHICS_SYNC : YZ_NR_GRAPHICS_REPORT;
    if (!(g_active.graphics_families & family))
        return 0;
    if (is_clear && g_active.clear_scope != YZ_NR_CLEAR_ALL) {
        const bool color = (arg & 0xF0u) != 0;
        const bool depth = (arg & 0x03u) != 0;
        const bool selected =
            (g_active.clear_scope == YZ_NR_CLEAR_COLOR_ONLY &&
             color && !depth) ||
            (g_active.clear_scope == YZ_NR_CLEAR_DEPTH_ONLY &&
             depth && !color) ||
            (g_active.clear_scope == YZ_NR_CLEAR_COMBINED &&
             color && depth);
        if (!selected)
            return 0;
    }
    const auto note_fallback = [&]() {
        if (is_draw)
            g_active.consumer_draw_fallback++;
        else if (is_clear)
            g_active.consumer_clear_fallback++;
        else if (is_transfer)
            g_active.consumer_transfer_fallback++;
        else if (is_sync)
            g_active.consumer_sync_fallback++;
        else
            g_active.consumer_report_fallback++;
    };
    if (!rsx_nr_ring_can_accept(&g_active.ring,
                                YZ_NR_ACTIVE_ACTION_OP_BOUND,
                                YZ_NR_ACTIVE_ACTION_SIDE_BOUND)) {
        note_fallback();
        return 0;
    }

    rsx_nr_ring_clear_reject(&g_active.ring);
    const unsigned long long errors_before =
        g_active.backend.stats.exec_errors;
    if (!rsx_nir_adapter_shadow_action(&g_active.adapter, method, arg) ||
        rsx_nr_ring_reject_sticky(&g_active.ring) ||
        !rsx_nr_ring_depth(&g_active.ring)) {
        while (rsx_nr_ring_depth(&g_active.ring))
            rsx_nr_ring_pop(&g_active.ring);
        note_fallback();
        return 0;
    }

    while (rsx_nr_ring_depth(&g_active.ring)) {
        if (rsx_nr_backend_step(&g_active.backend) !=
            RSX_NR_STEP_EXECUTED) {
            while (rsx_nr_ring_depth(&g_active.ring))
                rsx_nr_ring_pop(&g_active.ring);
            note_fallback();
            return 0;
        }
    }
    if (g_active.backend.stats.exec_errors != errors_before) {
        note_fallback();
        return 0;
    }
    if (is_draw) {
        g_active.consumer_draw_owned++;
        g_active.executed[YZ_NR_VERT_DRAW_ARRAYS]++;
    } else if (is_clear) {
        g_active.consumer_clear_owned++;
    } else if (is_transfer) {
        g_active.consumer_transfer_owned++;
    } else if (is_sync) {
        g_active.consumer_sync_owned++;
    } else {
        g_active.consumer_report_owned++;
    }
    return 1;
}

extern "C" void yz_nr_vertical_prepare_legacy_method(uint32_t method,
                                                        uint32_t arg)
{
    if (!InterlockedCompareExchange(
            &g_vertical.mode_active_graphics, 0, 0))
        return;
    /* The A010-path inline transfer window publishes guest/shared-resource
     * writes directly in yz_rsx_method. It must not overtake an open native
     * clear/draw list. observe_method also recognizes this range, but
     * resetting the owner after dispatch is too late to preserve order. */
    const bool action = rsx_nr_legacy_gpu_action(method, arg) != 0;
    if (!action)
        return;
    if (InterlockedExchange(&g_active.renderer_owner, 0) == 1 &&
        !InterlockedCompareExchange(&g_active.shared_timeline, 0, 0) &&
        g_active.gpu_ops.flush)
        g_active.gpu_ops.flush(g_active.gpu_ops.user);
}

extern "C" void yz_nr_vertical_observe_method(uint32_t method, uint32_t arg,
                                                uint32_t packet_ea)
{
    /* Host movies own execution and presentation, not the guest RSX register
     * file. The guest continues publishing state while movie frames suppress
     * its visible actions. Keep the shadow adapter current through that
     * interval so the first post-movie pass starts from the exact live state;
     * shadow_mode guarantees that clears/draws/transfers/presents emit no
     * native operation and therefore cannot race the movie renderer. */
    if (InterlockedCompareExchange(
            &g_vertical.mode_active_graphics, 0, 0))
        rsx_nir_adapter_method(&g_active.adapter, method, arg);
    if (InterlockedCompareExchange(
            &g_vertical.mode_active_graphics, 0, 0) &&
        ((method == 0x1808u && arg == 0u) || method == 0x1D94u ||
         method == 0x2328u || method == 0xC40Cu ||
         (method >= 0xA400u && method <= 0xAAFCu)))
        InterlockedExchange(&g_active.renderer_owner, 0);
    if (!InterlockedCompareExchange(&g_vertical.mode_shadow, 0, 0))
        return;

    /* Fragment program and shader control are persistent independent state.
     * Keep their latest values; draw begin below validates the effective
     * pair against a producer-built content template. */
    if (method == 0x08E4u) {
        AcquireSRWLockExclusive(&g_vertical.lock);
        rsx_nr_fragment_binding_set_program(
            &g_vertical.fp_binding, arg);
        g_vertical.fp_packet_ea = packet_ea;
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }
    if (method == 0x1D60u) {
        AcquireSRWLockExclusive(&g_vertical.lock);
        rsx_nr_fragment_binding_set_control(
            &g_vertical.fp_binding, arg);
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }

    /* Normalize the transform-program producer's multi-packet operation.
     * The exact event begins at LOAD, hashes instruction words as the RSX
     * sees them, and closes at ATTRIB_EN.  Program-associated constants that
     * follow remain separately ordered state and are never swallowed here. */
    if (method == 0x1E9Cu) {
        AcquireSRWLockExclusive(&g_vertical.lock);
        if (g_vertical.vp_state)
            g_vertical.vp_bad_sequence++;
        g_vertical.vp_event_ea = packet_ea;
        g_vertical.vp_start_slot = arg;
        g_vertical.vp_hash = rsx_nr_vertex_program_hash_begin(arg);
        g_vertical.vp_word_count = 0;
        g_vertical.vp_state = 1;
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }
    if (method == 0x1EA0u) {
        AcquireSRWLockExclusive(&g_vertical.lock);
        if (g_vertical.vp_state && arg != g_vertical.vp_start_slot)
            g_vertical.vp_bad_sequence++;
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }
    if (method >= 0x0B80u && method <= 0x0BFCu) {
        AcquireSRWLockExclusive(&g_vertical.lock);
        if (g_vertical.vp_state) {
            g_vertical.vp_hash = rsx_nr_vertex_program_hash_word(
                g_vertical.vp_hash, arg);
            g_vertical.vp_word_count++;
        } else if (!g_vertical.vp_state) {
            g_vertical.vp_bad_sequence++;
        }
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }
    if (method == 0x1FF0u) {
        AcquireSRWLockExclusive(&g_vertical.lock);
        if (g_vertical.vp_state) {
            const uint32_t hash = rsx_nr_vertex_program_hash_end(
                g_vertical.vp_hash, g_vertical.vp_word_count, arg);
            bool matched = false;
            for (uint32_t i = 0; i < YZ_NR_VERT_VP_TEMPLATE_COUNT; ++i) {
                yz_nr_vertical_vp_template* const entry =
                    &g_vertical.vp_templates[i];
                if (entry->build_count &&
                    entry->start_slot == g_vertical.vp_start_slot &&
                    entry->semantic_hash == hash) {
                    entry->replay_count++;
                    g_vertical.vp_template_replays++;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                /* EDGE journals may patch ATTRIB_EN while replaying an
                 * otherwise byte-identical static program template. Accept
                 * that as code-template coverage only when slot, complete
                 * word count and code hash all agree; the observed mask
                 * remains a dynamic ordered state value. */
                for (uint32_t i = 0;
                     i < YZ_NR_VERT_VP_TEMPLATE_COUNT; ++i) {
                    yz_nr_vertical_vp_template* const entry =
                        &g_vertical.vp_templates[i];
                    if (entry->build_count &&
                        entry->start_slot == g_vertical.vp_start_slot &&
                        entry->code_hash == g_vertical.vp_hash &&
                        entry->word_count == g_vertical.vp_word_count) {
                        entry->replay_count++;
                        g_vertical.vp_template_replays++;
                        g_vertical.vp_template_mask_replays++;
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) {
                g_vertical.vp_template_unknown++;
                g_vertical.vp_unowned++;
                uint32_t free_slot = YZ_NR_VERT_VP_TEMPLATE_COUNT;
                for (uint32_t i = 0; i < YZ_NR_VERT_VP_TEMPLATE_COUNT; ++i) {
                    yz_nr_vertical_vp_template* const entry =
                        &g_vertical.vp_unknown_templates[i];
                    if (entry->build_count &&
                        entry->start_slot == g_vertical.vp_start_slot &&
                        entry->semantic_hash == hash) {
                        entry->replay_count++;
                        free_slot = YZ_NR_VERT_VP_TEMPLATE_COUNT;
                        break;
                    }
                    if (!entry->build_count &&
                        free_slot == YZ_NR_VERT_VP_TEMPLATE_COUNT)
                        free_slot = i;
                }
                if (free_slot < YZ_NR_VERT_VP_TEMPLATE_COUNT) {
                    yz_nr_vertical_vp_template* const entry =
                        &g_vertical.vp_unknown_templates[free_slot];
                    entry->start_slot = g_vertical.vp_start_slot;
                    entry->code_hash = g_vertical.vp_hash;
                    entry->word_count = g_vertical.vp_word_count;
                    entry->input_mask = arg;
                    entry->semantic_hash = hash;
                    entry->build_count = 1;
                    entry->replay_count = 1;
                } else {
                    bool found = false;
                    for (uint32_t i = 0;
                         i < YZ_NR_VERT_VP_TEMPLATE_COUNT; ++i) {
                        const yz_nr_vertical_vp_template* const entry =
                            &g_vertical.vp_unknown_templates[i];
                        if (entry->build_count &&
                            entry->start_slot == g_vertical.vp_start_slot &&
                            entry->semantic_hash == hash) {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        g_vertical.vp_unknown_overflow++;
                }
            }
        } else if (!g_vertical.vp_state) {
            /* A direct ATTRIB_EN setter is handled by the ordinary state
             * contract below; it is not a malformed program sequence. */
            ReleaseSRWLockExclusive(&g_vertical.lock);
            goto observe_single_method;
        }
        g_vertical.vp_state = 0;
        g_vertical.vp_event_ea = 0;
        g_vertical.vp_start_slot = 0;
        g_vertical.vp_hash = 0;
        g_vertical.vp_word_count = 0;
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }

    /* The imported flip producer emits queue-buffer followed by flip-head.
     * Keep the exact event live until both ordered methods agree. Other flip
     * producers remain counted fallback and are never claimed. */
    if (method >= 0xE940u && method <= 0xE95Cu) {
        AcquireSRWLockExclusive(&g_vertical.lock);
        if (g_vertical.flip_state)
            g_vertical.flip_bad_sequence++;
        g_vertical.flip_event_ea = packet_ea;
        g_vertical.flip_buffer = arg;
        if (method == 0xE944u &&
            yz_nr_event_find(packet_ea, YZ_NR_VERT_FLIP)) {
            g_vertical.flip_state = 1;
        } else {
            g_vertical.flip_state = 2;
            g_vertical.flip_unowned++;
        }
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }
    if (method >= 0xE920u && method <= 0xE93Cu) {
        AcquireSRWLockExclusive(&g_vertical.lock);
        if (g_vertical.flip_state == 1) {
            if (method != 0xE924u || arg != 0x8000010Fu) {
                g_vertical.flip_bad_sequence++;
            } else {
                yz_nr_note(&g_vertical.observed, YZ_NR_VERT_FLIP,
                           g_vertical.flip_buffer, arg);
                yz_nr_event_observe(g_vertical.flip_event_ea,
                                    YZ_NR_VERT_FLIP,
                                    g_vertical.flip_buffer, arg);
                const uint32_t family = YZ_NR_VERT_FLIP;
                if (g_vertical.observed.count[family] == 1 ||
                    g_vertical.flip_event_ea <
                        g_vertical.observed_ea_min[family])
                    g_vertical.observed_ea_min[family] =
                        g_vertical.flip_event_ea;
                if (g_vertical.flip_event_ea >
                    g_vertical.observed_ea_max[family])
                    g_vertical.observed_ea_max[family] =
                        g_vertical.flip_event_ea;
            }
        }
        g_vertical.flip_state = 0;
        g_vertical.flip_event_ea = 0;
        g_vertical.flip_buffer = 0;
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }

    /* Normalize complete DrawArrays actions at the consumer. An exact
     * wrapper event is keyed by the BEGIN packet address and stays live until
     * END, while copied/dynamic/SPU-authored draws are counted as fallback. */
    if (method == 0x1808u) {
        if (arg)
            yz_nr_observe_active_fragment(packet_ea);
        AcquireSRWLockExclusive(&g_vertical.lock);
        if (arg) {
            if (g_vertical.draw_state)
                g_vertical.draw_bad_sequence++;
            yz_nr_vertical_event* const event =
                yz_nr_event_find(packet_ea, YZ_NR_VERT_DRAW_ARRAYS);
            g_vertical.draw_event_ea = packet_ea;
            g_vertical.draw_primitive = arg;
            g_vertical.draw_hash = rsx_nr_draw_hash_begin(arg, 0);
            if (event) {
                g_vertical.draw_state = 1;
            } else {
                g_vertical.draw_state = 2;
                g_vertical.draw_unowned++;
            }
        } else {
            if (g_vertical.draw_state == 1) {
                yz_nr_note(&g_vertical.observed, YZ_NR_VERT_DRAW_ARRAYS,
                           g_vertical.draw_primitive,
                           g_vertical.draw_hash);
                yz_nr_event_observe(g_vertical.draw_event_ea,
                                    YZ_NR_VERT_DRAW_ARRAYS,
                                    g_vertical.draw_primitive,
                                    g_vertical.draw_hash);
                const uint32_t family = YZ_NR_VERT_DRAW_ARRAYS;
                if (g_vertical.observed.count[family] == 1 ||
                    g_vertical.draw_event_ea <
                        g_vertical.observed_ea_min[family])
                    g_vertical.observed_ea_min[family] =
                        g_vertical.draw_event_ea;
                if (g_vertical.draw_event_ea >
                    g_vertical.observed_ea_max[family])
                    g_vertical.observed_ea_max[family] =
                        g_vertical.draw_event_ea;
            } else if (!g_vertical.draw_state) {
                g_vertical.draw_bad_sequence++;
            }
            g_vertical.draw_state = 0;
            g_vertical.draw_event_ea = 0;
            g_vertical.draw_primitive = 0;
            g_vertical.draw_hash = 0;
        }
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }
    if (method == 0x1814u || method == 0x1824u) {
        AcquireSRWLockExclusive(&g_vertical.lock);
        if (g_vertical.draw_state == 1) {
            if (method == 0x1824u)
                g_vertical.draw_bad_sequence++;
            g_vertical.draw_hash = rsx_nr_draw_hash_batch(
                g_vertical.draw_hash, arg & 0x00FFFFFFu,
                (arg >> 24) + 1u);
        }
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }

    /* yz_rsx_method receives subchannel-flattened methods. NV406E methods
     * remain in the low window; the game user-command method is 0xEB00/04. */
observe_single_method:
    uint32_t family = 0, a = 0, b = 0;
    switch (method) {
    case 0x0050:
        family = YZ_NR_VERT_REFERENCE;
        a = arg;
        break;
    case 0x0064:
        AcquireSRWLockExclusive(&g_vertical.lock);
        g_vertical.fifo_semaphore_offset = arg;
        g_vertical.fifo_semaphore_packet_ea = packet_ea;
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    case 0x0068:
        AcquireSRWLockExclusive(&g_vertical.lock);
        a = g_vertical.fifo_semaphore_offset;
        b = arg;
        yz_nr_note(&g_vertical.observed, YZ_NR_VERT_ACQUIRE, a, b);
        yz_nr_event_observe(g_vertical.fifo_semaphore_packet_ea,
                            YZ_NR_VERT_ACQUIRE, a, b);
        if (g_vertical.observed.count[YZ_NR_VERT_ACQUIRE] == 1 ||
            g_vertical.fifo_semaphore_packet_ea <
                g_vertical.observed_ea_min[YZ_NR_VERT_ACQUIRE])
            g_vertical.observed_ea_min[YZ_NR_VERT_ACQUIRE] =
                g_vertical.fifo_semaphore_packet_ea;
        if (g_vertical.fifo_semaphore_packet_ea >
            g_vertical.observed_ea_max[YZ_NR_VERT_ACQUIRE])
            g_vertical.observed_ea_max[YZ_NR_VERT_ACQUIRE] =
                g_vertical.fifo_semaphore_packet_ea;
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    case 0xEB00:
    case 0xEB04:
        family = YZ_NR_VERT_USER;
        a = arg;
        break;
    default:
        if (!rsx_nr_direct_setter_by_method(method))
            return;
        family = YZ_NR_VERT_STATE_DIRECT;
        a = method;
        b = arg;
        AcquireSRWLockExclusive(&g_vertical.lock);
        if (yz_nr_event_observe_optional(packet_ea, family, a, b)) {
            yz_nr_note(&g_vertical.observed, family, a, b);
            if (g_vertical.observed.count[family] == 1 ||
                packet_ea < g_vertical.observed_ea_min[family])
                g_vertical.observed_ea_min[family] = packet_ea;
            if (packet_ea > g_vertical.observed_ea_max[family])
                g_vertical.observed_ea_max[family] = packet_ea;
        }
        ReleaseSRWLockExclusive(&g_vertical.lock);
        return;
    }

    AcquireSRWLockExclusive(&g_vertical.lock);
    yz_nr_note(&g_vertical.observed, family, a, b);
    yz_nr_event_observe(packet_ea, family, a, b);
    if (g_vertical.observed.count[family] == 1 ||
        packet_ea < g_vertical.observed_ea_min[family])
        g_vertical.observed_ea_min[family] = packet_ea;
    if (packet_ea > g_vertical.observed_ea_max[family])
        g_vertical.observed_ea_max[family] = packet_ea;
    ReleaseSRWLockExclusive(&g_vertical.lock);
}

static int yz_nr_section_gpu_action(uint32_t kind)
{
    return kind == RSX_NIR_OP_CLEAR || kind == RSX_NIR_OP_DRAW ||
        kind == RSX_NIR_OP_TRANSFER || kind == RSX_NIR_OP_PRESENT;
}

/* A native render-pass island may contain arbitrary folded graphics state and
 * any number of draws, but it must not straddle an operation that publishes
 * memory, changes framebuffer identity, transfers ownership, or presents.
 * These are semantic boundaries, not packet-count heuristics: the complete
 * island is preflighted before any legacy action is suppressed. */
static int yz_nr_section_dependency_method(uint32_t method)
{
    switch (method) {
    case 0x0050u: /* NV406E SET_REFERENCE */
    case 0x0068u: /* NV406E SEMAPHORE_ACQUIRE */
    case 0x006Cu: /* NV406E SEMAPHORE_RELEASE */
    case 0x0110u: /* WAIT_FOR_IDLE */
    case 0x17C8u: /* CLEAR_REPORT_VALUE */
    case 0x1800u: /* GET_REPORT */
    case 0x1D70u: /* BACK_END_WRITE_SEMAPHORE_RELEASE */
    case 0x1D74u: /* TEXTURE_READ_SEMAPHORE_RELEASE */
    case 0x2328u: /* NV0039 BUFFER_NOTIFY */
    case 0xC40Cu: /* NV3089 IMAGE_IN */
    case 0xE924u: /* typed PRESENT */
    case 0xE944u: /* legacy driver flip */
    case 0xEB00u: /* user command cause */
    case 0xEB04u: /* user command fire */
        return 1;
    default:
        return method >= 0xA400u && method <= 0xAAFCu;
    }
}

static int yz_nr_section_surface_method(uint32_t method)
{
    switch (method) {
    case 0x018Cu: /* DMA_COLOR1 */
    case 0x0194u: /* DMA_COLOR0 */
    case 0x0198u: /* DMA_ZETA */
    case 0x01B4u: /* DMA_COLOR2 */
    case 0x01B8u: /* DMA_COLOR3 */
    case 0x0200u: /* RT_HORIZ */
    case 0x0204u: /* RT_VERT */
    case 0x0208u: /* RT_FORMAT */
    case 0x020Cu: /* COLOR0_PITCH */
    case 0x0210u: /* COLOR0_OFFSET */
    case 0x0214u: /* ZETA_OFFSET */
    case 0x0218u: /* COLOR1_OFFSET */
    case 0x021Cu: /* COLOR1_PITCH */
    case 0x0220u: /* RT_ENABLE */
    case 0x022Cu: /* ZETA_PITCH */
    case 0x0280u: /* COLOR2_PITCH */
    case 0x0284u: /* COLOR3_PITCH */
    case 0x0288u: /* COLOR2_OFFSET */
    case 0x028Cu: /* COLOR3_OFFSET */
        return 1;
    default:
        return 0;
    }
}

static int yz_nr_section_starts_new_pass(uint32_t method)
{
    return method == 0x1D94u || yz_nr_section_surface_method(method);
}

static int yz_nr_section_op_side(const rsx_nir_op* op,
                                 uint32_t* offset, uint32_t* count)
{
    *offset = 0;
    *count = 0;
    switch (op->kind) {
    case RSX_NIR_OP_DRAW:
        *offset = op->u.draw.batches_ofs;
        *count = op->u.draw.batch_count * 2u;
        break;
    case RSX_NIR_OP_SET_VERTEX_PROGRAM:
        *offset = op->u.vertex_program.words_ofs;
        *count = op->u.vertex_program.word_count;
        break;
    case RSX_NIR_OP_SET_CONSTANTS:
        *offset = op->u.constants.words_ofs;
        *count = op->u.constants.slot_count * 4u;
        break;
    case RSX_NIR_OP_TRANSFER:
        *offset = op->u.transfer.words_ofs;
        *count = op->u.transfer.word_count;
        break;
    default:
        break;
    }
    return *count != 0;
}

static void yz_nr_section_set_side(rsx_nir_op* op, uint32_t offset)
{
    switch (op->kind) {
    case RSX_NIR_OP_DRAW:
        op->u.draw.batches_ofs = offset;
        break;
    case RSX_NIR_OP_SET_VERTEX_PROGRAM:
        op->u.vertex_program.words_ofs = offset;
        break;
    case RSX_NIR_OP_SET_CONSTANTS:
        op->u.constants.words_ofs = offset;
        break;
    case RSX_NIR_OP_TRANSFER:
        op->u.transfer.words_ofs = offset;
        break;
    default:
        break;
    }
}

static yz_nr_vertical_section_result yz_nr_section_fallback(uint32_t reason)
{
    if (reason >= YZ_NR_SECTION_FB_REASON_COUNT)
        reason = YZ_NR_SECTION_FB_FLOW;
    g_active.section_fallback[reason]++;
    /* A rejected, still-published FIFO head is immutable until GET, PUT, the
     * return cursor, or its command word changes. Cache only that exact
     * identity. This suppresses repeated whole-section decoding/preflight on
     * a legacy semaphore retry or self-stopper without broad address scans. */
    if (g_active.section_scan_cacheable) {
        g_active.section_repeat_get = g_active.section_scan_get;
        g_active.section_repeat_put = g_active.section_scan_put;
        g_active.section_repeat_ret = g_active.section_scan_ret;
        g_active.section_repeat_word = g_active.section_scan_word;
        g_active.section_repeat_valid = 1;
        g_active.section_scan_cacheable = 0;
    }
    return YZ_NR_VERTICAL_SECTION_FALLBACK;
}

static yz_nr_vertical_section_result yz_nr_section_path_fallback(
    uint32_t reason)
{
    /* FLOW and CAPACITY refusals have no proven transactional endpoint. Keep
     * every exact (PC,return) node already visited by this scan wholly legacy.
     * This is one-way safe: a patched word may make legacy leave the cached
     * path, but stale membership can never cause native execution. */
    g_active.section_legacy_path_active = 1;
    g_active.section_legacy_path_reason = reason;
    g_active.section_scan_cacheable = 0;
    return yz_nr_section_fallback(reason);
}

static yz_nr_vertical_section_result yz_nr_section_defer_fallback(
    uint32_t reason, uint32_t until_get, uint32_t budget)
{
    (void)budget;
    /* The scanner has already followed every control-flow edge to this exact
     * semantic boundary. Legacy execution of the rejected section follows
     * the same FIFO path, so keep it wholly legacy until GET reaches that
     * boundary. A retry at an acquire can remain here indefinitely without
     * causing another scan. */
    g_active.section_fallback_active = 1;
    g_active.section_fallback_until_get = until_get;
    return yz_nr_section_fallback(reason);
}

static void yz_nr_section_note_unknown(uint32_t method, uint32_t arg)
{
    for (uint32_t i = 0; i < 64u; ++i) {
        yz_nr_section_unknown_key* const entry =
            &g_active.section_unknown[i];
        if (!entry->count) {
            entry->method = method;
            entry->arg = arg;
            entry->count = 1;
            return;
        }
        if (entry->method == method && entry->arg == arg) {
            entry->count++;
            return;
        }
    }
    g_active.section_unknown_overflow++;
}

static void yz_nr_section_note_window(
    uint32_t pc, uint32_t put, uint32_t ret, uint32_t size,
    uint32_t command, uint32_t stage)
{
    const uint32_t ring = 0x800000u;
    const uint32_t available = pc < ring
        ? (put - pc + ring) & (ring - 1u) : UINT32_MAX;
    yz_nr_section_window_key* empty = nullptr;
    yz_nr_section_window_key* least = &g_active.section_window[0];
    for (uint32_t i = 0; i < 32u; ++i) {
        yz_nr_section_window_key* const entry =
            &g_active.section_window[i];
        if (entry->count && entry->pc == pc && entry->ret == ret &&
            entry->size == size && entry->command == command &&
            entry->stage == stage) {
            entry->last_put = put;
            if (available < entry->min_available)
                entry->min_available = available;
            if (available > entry->max_available)
                entry->max_available = available;
            entry->count++;
            return;
        }
        if (!entry->count && !empty)
            empty = entry;
        if (entry->count < least->count)
            least = entry;
    }
    yz_nr_section_window_key* const entry = empty ? empty : least;
    const unsigned long long inherited = empty ? 0u : entry->count;
    *entry = {pc, ret, size, command, stage, put, put,
              available, available, inherited + 1u, inherited};
}

static void yz_nr_section_note_report(
    uint32_t kind, uint32_t arg, uint32_t dma)
{
    for (uint32_t i = 0; i < 64u; ++i) {
        yz_nr_section_report_key* const entry =
            &g_active.section_reports[i];
        if (!entry->count) {
            entry->kind = kind;
            entry->arg = arg;
            entry->dma = dma;
            entry->count = 1;
            return;
        }
        if (entry->kind == kind && entry->arg == arg &&
            entry->dma == dma) {
            entry->count++;
            return;
        }
    }
    g_active.section_report_overflow++;
}

static void yz_nr_section_note_draw_preflight(
    uint32_t reason, const rsx_nir_pipeline* state,
    const rsx_nir_draw* draw, const uint32_t* vp_words,
    uint32_t vp_word_count)
{
    uint32_t texture_mask = 0;
    uint32_t vertex_texture_mask = 0;
    if (state)
        for (uint32_t i = 0; i < RSX_NIR_NUM_TEXTURES; ++i)
            if (state->textures[i].enabled)
                texture_mask |= 1u << i;
    if (state)
        for (uint32_t i = 0; i < RSX_NIR_NUM_VERTEX_TEXTURES; ++i)
            if (state->vertex_textures[i].enabled)
                vertex_texture_mask |= 1u << i;
    rsx_vp_native_support_analysis vp_support = {};
    if (vp_words && vp_word_count)
        rsx_vp_analyze_native_support_control(
            reinterpret_cast<const uint8_t*>(vp_words),
            vp_word_count * sizeof(uint32_t), vertex_texture_mask,
            state ? state->vertex_program.start_slot : 0u,
            &vp_support);
    const yz_nr_section_draw_preflight_key key = {
        reason, draw ? draw->primitive : 0u,
        state ? state->surface.color_target : 0u,
        state ? state->fragment_program.location : 0u,
        state ? state->fragment_program.offset : 0u,
        state ? state->vertex_program.start_slot : 0u,
        state ? state->vertex_program.attrib_input_mask : 0u,
        state ? state->surface.color_location[0] : 0u,
        state ? state->surface.color_offset[0] : 0u,
        state ? state->surface.color_format : 0u,
        state ? state->surface.color_pitch[0] : 0u,
        state ? state->viewport.x : 0u,
        state ? state->viewport.y : 0u,
        state ? state->viewport.w : 0u,
        state ? state->viewport.h : 0u,
        state ? state->scissor.x : 0u,
        state ? state->scissor.y : 0u,
        state ? state->scissor.w : 0u,
        state ? state->scissor.h : 0u,
        state ? state->raster.color_mask : 0u,
        state ? state->depth_stencil.depth_test_enable : 0u,
        state ? state->depth_stencil.depth_write_enable : 0u,
        texture_mask,
        state ? state->depth_stencil.two_sided_stencil_enable : 0u,
        state ? state->depth_stencil.stencil_ref : 0u,
        state ? state->depth_stencil.back_stencil_ref : 0u,
        state ? state->depth_stencil.stencil_mask : 0u,
        state ? state->depth_stencil.back_stencil_mask : 0u,
        state ? state->depth_stencil.stencil_write_mask : 0u,
        state ? state->depth_stencil.back_stencil_write_mask : 0u,
        state ? state->vertex_program.hash : 0u,
        vp_word_count,
        vp_support.unsupported_vec_mask,
        vp_support.unsupported_sca_mask,
        vp_support.missing_vtex_mask,
        vp_support.conditional_tests,
        vp_support.terminated,
        1u
    };
    for (uint32_t i = 0; i < 64u; ++i) {
        yz_nr_section_draw_preflight_key* const entry =
            &g_active.section_draw_preflight[i];
        if (!entry->count) {
            *entry = key;
            return;
        }
        if (entry->reason == key.reason &&
            entry->primitive == key.primitive &&
            entry->color_target == key.color_target &&
            entry->fp_location == key.fp_location &&
            entry->fp_offset == key.fp_offset &&
            entry->vp_start == key.vp_start &&
            entry->vp_inputs == key.vp_inputs &&
            entry->color_location == key.color_location &&
            entry->color_offset == key.color_offset &&
            entry->color_format == key.color_format &&
            entry->color_pitch == key.color_pitch &&
            entry->viewport_x == key.viewport_x &&
            entry->viewport_y == key.viewport_y &&
            entry->viewport_w == key.viewport_w &&
            entry->viewport_h == key.viewport_h &&
            entry->scissor_x == key.scissor_x &&
            entry->scissor_y == key.scissor_y &&
            entry->scissor_w == key.scissor_w &&
            entry->scissor_h == key.scissor_h &&
            entry->color_mask == key.color_mask &&
            entry->depth_test == key.depth_test &&
            entry->depth_write == key.depth_write &&
            entry->texture_mask == key.texture_mask &&
            entry->stencil_two_sided == key.stencil_two_sided &&
            entry->stencil_ref == key.stencil_ref &&
            entry->back_stencil_ref == key.back_stencil_ref &&
            entry->stencil_mask == key.stencil_mask &&
            entry->back_stencil_mask == key.back_stencil_mask &&
            entry->stencil_write_mask == key.stencil_write_mask &&
            entry->back_stencil_write_mask == key.back_stencil_write_mask &&
            entry->vp_hash == key.vp_hash &&
            entry->vp_words == key.vp_words &&
            entry->vp_bad_vec == key.vp_bad_vec &&
            entry->vp_bad_sca == key.vp_bad_sca &&
            entry->vp_missing_vtex == key.vp_missing_vtex &&
            entry->vp_conditional == key.vp_conditional &&
            entry->vp_terminated == key.vp_terminated) {
            entry->count++;
            return;
        }
    }
    g_active.section_draw_preflight_overflow++;
}

static void yz_nr_section_note_flow_vp(
    const rsx_nir_pipeline* state, const uint32_t* vp_words,
    uint32_t vp_word_count)
{
    if (!g_active.section_diag_enabled || !state || !vp_words ||
        !vp_word_count || vp_word_count > RSX_NIR_VP_MAX_WORDS)
        return;
    uint32_t vertex_texture_mask = 0;
    for (uint32_t i = 0; i < RSX_NIR_NUM_VERTEX_TEXTURES; ++i)
        if (state->vertex_textures[i].enabled)
            vertex_texture_mask |= 1u << i;
    rsx_vp_native_support_analysis support = {};
    rsx_vp_analyze_native_support_control(
        reinterpret_cast<const uint8_t*>(vp_words),
        vp_word_count * sizeof(uint32_t), vertex_texture_mask,
        state->vertex_program.start_slot, &support);
    if (!support.flow_instructions)
        return;

    yz_nr_flow_vp_diag* empty = nullptr;
    for (uint32_t i = 0; i < YZ_NR_FLOW_VP_DIAG_COUNT; ++i) {
        yz_nr_flow_vp_diag* const entry = &g_active.section_flow_vp[i];
        if (!entry->draws) {
            if (!empty)
                empty = entry;
            continue;
        }
        if (entry->hash != state->vertex_program.hash ||
            entry->start_slot != state->vertex_program.start_slot ||
            entry->word_count != vp_word_count)
            continue;
        const uint32_t branch_bits = state->vertex_program.branch_bits;
        entry->draws++;
        entry->branch_last = branch_bits;
        entry->branch_or |= branch_bits;
        entry->branch_and &= branch_bits;
        return;
    }
    if (!empty) {
        g_active.section_flow_vp_overflow++;
        return;
    }
    empty->hash = state->vertex_program.hash;
    empty->draws = 1;
    empty->start_slot = state->vertex_program.start_slot;
    empty->word_count = vp_word_count;
    empty->branch_first = state->vertex_program.branch_bits;
    empty->branch_last = state->vertex_program.branch_bits;
    empty->branch_or = state->vertex_program.branch_bits;
    empty->branch_and = state->vertex_program.branch_bits;
    memcpy(empty->words, vp_words,
           static_cast<size_t>(vp_word_count) * sizeof(uint32_t));
}

static void yz_nr_section_note_sync_preflight(
    const rsx_nir_op* op, int32_t result)
{
    const yz_nr_section_sync_preflight_key key = {
        op ? op->kind : 0u,
        op ? op->u.semaphore.dma_context : 0u,
        op ? op->u.semaphore.offset : 0u,
        op ? op->u.semaphore.value : 0u,
        op ? op->u.semaphore.texture_read : 0u,
        result, 1u
    };
    for (uint32_t i = 0; i < 64u; ++i) {
        yz_nr_section_sync_preflight_key* const entry =
            &g_active.section_sync_preflight[i];
        if (!entry->count) {
            *entry = key;
            return;
        }
        if (entry->kind == key.kind && entry->dma == key.dma &&
            entry->offset == key.offset && entry->value == key.value &&
            entry->release_kind == key.release_kind &&
            entry->result == key.result) {
            entry->count++;
            return;
        }
    }
    g_active.section_sync_preflight_overflow++;
}

static void yz_nr_section_note_transfer_preflight(
    const rsx_nir_transfer* transfer)
{
    const yz_nr_section_transfer_preflight_key key = {
        transfer ? transfer->kind : 0u,
        transfer ? transfer->src_location : 0u,
        transfer ? transfer->dst_location : 0u,
        transfer ? transfer->src_format : 0u,
        transfer ? transfer->dst_format : 0u,
        transfer ? transfer->line_length : 0u,
        transfer ? transfer->line_count : 0u,
        transfer ? transfer->word_count : 0u,
        transfer ? transfer->in_w : 0u,
        transfer ? transfer->in_h : 0u,
        transfer ? transfer->out_w : 0u,
        transfer ? transfer->out_h : 0u,
        1u
    };
    for (uint32_t i = 0; i < 64u; ++i) {
        yz_nr_section_transfer_preflight_key* const entry =
            &g_active.section_transfer_preflight[i];
        if (!entry->count) {
            *entry = key;
            return;
        }
        if (entry->kind == key.kind &&
            entry->src_location == key.src_location &&
            entry->dst_location == key.dst_location &&
            entry->src_format == key.src_format &&
            entry->dst_format == key.dst_format &&
            entry->line_length == key.line_length &&
            entry->line_count == key.line_count &&
            entry->word_count == key.word_count &&
            entry->in_w == key.in_w && entry->in_h == key.in_h &&
            entry->out_w == key.out_w && entry->out_h == key.out_h) {
            entry->count++;
            return;
        }
    }
    g_active.section_transfer_preflight_overflow++;
}

/* Reject unsupported shader/program families before the heavyweight D3D12
 * admission pass can create targets, compile PSOs, register mirror pages, or
 * record uploads for earlier draws in a section that must ultimately remain
 * legacy. This pass only folds the already-decoded fixed-memory op stream and
 * reads the referenced program bytes. */
static uint32_t yz_nr_section_program_preflight(void)
{
    rsx_nir_pipeline state = g_active.backend.st;
    uint32_t vp_words[RSX_NIR_VP_MAX_WORDS] = {};
    uint32_t vp_word_count = g_active.backend.vp_word_count;
    if (vp_word_count > RSX_NIR_VP_MAX_WORDS)
        return YZ_NR_SECTION_FB_PREFLIGHT_DRAW;
    if (vp_word_count)
        memcpy(vp_words, g_active.backend.vp_words,
               static_cast<size_t>(vp_word_count) * sizeof(uint32_t));

    for (uint32_t i = 0; i < g_active.section_stream.op_count; ++i) {
        const rsx_nir_op* const op = &g_active.section_stream.ops[i];
        if (!rsx_nir_op_is_action(op->kind)) {
            rsx_nir_pipeline_apply_op(
                &state, &g_active.section_stream, op);
            if (op->kind == RSX_NIR_OP_SET_VERTEX_PROGRAM) {
                vp_word_count = op->u.vertex_program.word_count;
                const uint32_t* const words = rsx_nir_side(
                    &g_active.section_stream,
                    op->u.vertex_program.words_ofs, vp_word_count);
                if (vp_word_count > RSX_NIR_VP_MAX_WORDS ||
                    (vp_word_count && !words))
                    return YZ_NR_SECTION_FB_PREFLIGHT_DRAW;
                if (vp_word_count)
                    memcpy(vp_words, words,
                           static_cast<size_t>(vp_word_count) *
                               sizeof(uint32_t));
            }
            continue;
        }
        if (op->kind != RSX_NIR_OP_DRAW)
            continue;
        /* These two depth-only targets feed later world-color sections. The
         * resolved SRV route is now correct, but live evidence shows that the
         * native-produced depth values are not yet equivalent. Refuse here,
         * before heavyweight preflight has any side effects, so the complete
         * producer section remains legacy and its later consumer may still
         * execute natively against the established depth snapshot. */
        if (rsx_nr_yz_unproven_shadow_depth_producer(
                state.surface.zeta_location, state.surface.zeta_offset,
                state.raster.color_mask,
                state.depth_stencil.depth_write_enable)) {
            g_active.section_shadow_depth_fallback++;
            return YZ_NR_SECTION_FB_PREFLIGHT_DRAW;
        }
        uint32_t texture_mask = 0;
        const int result = rsx_nr_d3d12_validate_draw_program_usage(
            g_active.d3d12, &state, vp_words, vp_word_count,
            &texture_mask);
        if (result != 0) {
            yz_nr_section_note_draw_preflight(
                (uint32_t)-result, &state, &op->u.draw,
                vp_words, vp_word_count);
            return YZ_NR_SECTION_FB_PREFLIGHT_DRAW;
        }
        for (uint32_t unit = 0; unit < RSX_NIR_NUM_TEXTURES; ++unit) {
            if (!(texture_mask & (1u << unit)))
                continue;
            const rsx_nir_texture* const texture = &state.textures[unit];
            if (rsx_nr_yz_unproven_shadow_depth_consumer(
                    texture->enabled, texture->location, texture->offset,
                    texture->format)) {
                g_active.section_shadow_consumer_fallback++;
                return YZ_NR_SECTION_FB_PREFLIGHT_DRAW;
            }
        }
    }
    return YZ_NR_SECTION_FB_NONE;
}

static uint32_t yz_nr_section_preflight(void)
{
    rsx_nir_pipeline state = g_active.backend.st;
    uint32_t vp_words[RSX_NIR_VP_MAX_WORDS] = {};
    uint32_t vp_word_count = g_active.backend.vp_word_count;
    if (vp_word_count > RSX_NIR_VP_MAX_WORDS)
        return YZ_NR_SECTION_FB_PREFLIGHT_DRAW;
    if (vp_word_count)
        memcpy(vp_words, g_active.backend.vp_words,
               static_cast<size_t>(vp_word_count) * sizeof(uint32_t));

    g_active.section_gpu_actions = 0;
    g_active.section_draws = 0;
    g_active.section_clears = 0;
    g_active.section_transfers = 0;
    g_active.section_presents = 0;
    g_active.section_preflight_sem_valid = 0;
    /* The whole op stream is already fixed and side-effect-free. Count draws
     * before family admission so a leading clear can be recognized as part of
     * the same indivisible render pass. */
    uint32_t section_draw_count = 0;
    for (uint32_t i = 0; i < g_active.section_stream.op_count; ++i)
        if (g_active.section_stream.ops[i].kind == RSX_NIR_OP_DRAW)
            section_draw_count++;
    for (uint32_t i = 0; i < g_active.section_stream.op_count; ++i) {
        const rsx_nir_op* const op = &g_active.section_stream.ops[i];
        if (!rsx_nir_op_is_action(op->kind)) {
            rsx_nir_pipeline_apply_op(
                &state, &g_active.section_stream, op);
            if (op->kind == RSX_NIR_OP_SET_VERTEX_PROGRAM) {
                vp_word_count = op->u.vertex_program.word_count;
                const uint32_t* const words = rsx_nir_side(
                    &g_active.section_stream,
                    op->u.vertex_program.words_ofs, vp_word_count);
                if (vp_word_count > RSX_NIR_VP_MAX_WORDS ||
                    (vp_word_count && !words))
                    return YZ_NR_SECTION_FB_PREFLIGHT_DRAW;
                if (vp_word_count)
                    memcpy(vp_words, words,
                           static_cast<size_t>(vp_word_count) *
                               sizeof(uint32_t));
            }
            continue;
        }

        if (yz_nr_section_gpu_action(op->kind))
            g_active.section_gpu_actions++;
        switch (op->kind) {
        case RSX_NIR_OP_CLEAR:
            if (!rsx_nr_complete_section_family_allowed(
                    g_active.graphics_families,
                    RSX_NR_GRAPHICS_FAMILY_CLEAR,
                    section_draw_count))
                return YZ_NR_SECTION_FB_PREFLIGHT_CLEAR;
            if (g_active.clear_scope != YZ_NR_CLEAR_ALL) {
                const bool color = (op->u.clear.mask & 0xF0u) != 0;
                const bool depth = (op->u.clear.mask & 0x03u) != 0;
                const bool selected =
                    (g_active.clear_scope == YZ_NR_CLEAR_COLOR_ONLY &&
                     color && !depth) ||
                    (g_active.clear_scope == YZ_NR_CLEAR_DEPTH_ONLY &&
                     depth && !color) ||
                    (g_active.clear_scope == YZ_NR_CLEAR_COMBINED &&
                     color && depth);
                if (!selected)
                    return YZ_NR_SECTION_FB_PREFLIGHT_CLEAR;
            }
            if (rsx_nr_d3d12_preflight_clear(
                    g_active.d3d12, &state, &op->u.clear) != 0)
                return YZ_NR_SECTION_FB_PREFLIGHT_CLEAR;
            g_active.section_clears++;
            break;
        case RSX_NIR_OP_DRAW: {
            if (!(g_active.graphics_families & YZ_NR_GRAPHICS_DRAW))
                return YZ_NR_SECTION_FB_PREFLIGHT_DRAW;
            if (g_active.draw_primitive_filter != UINT32_MAX &&
                op->u.draw.primitive != g_active.draw_primitive_filter)
                return YZ_NR_SECTION_FB_PREFLIGHT_DRAW;
            if (rsx_nr_yz_unproven_shadow_depth_producer(
                    state.surface.zeta_location,
                    state.surface.zeta_offset,
                    state.raster.color_mask,
                    state.depth_stencil.depth_write_enable))
                return YZ_NR_SECTION_FB_PREFLIGHT_DRAW;
            uint32_t texture_mask = 0;
            if (rsx_nr_d3d12_validate_draw_program_usage(
                    g_active.d3d12, &state, vp_words, vp_word_count,
                    &texture_mask) != 0)
                return YZ_NR_SECTION_FB_PREFLIGHT_DRAW;
            for (uint32_t unit = 0; unit < RSX_NIR_NUM_TEXTURES; ++unit) {
                if (!(texture_mask & (1u << unit)))
                    continue;
                const rsx_nir_texture* const texture = &state.textures[unit];
                if (rsx_nr_yz_unproven_shadow_depth_consumer(
                        texture->enabled, texture->location, texture->offset,
                        texture->format))
                    return YZ_NR_SECTION_FB_PREFLIGHT_DRAW;
            }
            const uint32_t* const batches = rsx_nir_side(
                &g_active.section_stream, op->u.draw.batches_ofs,
                op->u.draw.batch_count * 2u);
            yz_nr_section_note_flow_vp(&state, vp_words, vp_word_count);
            const int draw_preflight = batches
                ? rsx_nr_d3d12_preflight_draw(
                    g_active.d3d12, &state, vp_words, vp_word_count,
                    &op->u.draw, batches)
                : -RSX_NR_DRAW_PF_BAD_ARGUMENT;
            if (draw_preflight != 0) {
                yz_nr_section_note_draw_preflight(
                    (uint32_t)-draw_preflight, &state, &op->u.draw,
                    vp_words, vp_word_count);
                return YZ_NR_SECTION_FB_PREFLIGHT_DRAW;
            }
            g_active.section_draws++;
            break;
        }
        case RSX_NIR_OP_TRANSFER: {
            if (!(g_active.graphics_families & YZ_NR_GRAPHICS_TRANSFER))
                return YZ_NR_SECTION_FB_PREFLIGHT_TRANSFER;
            const uint32_t* words = nullptr;
            if (op->u.transfer.word_count)
                words = rsx_nir_side(
                    &g_active.section_stream, op->u.transfer.words_ofs,
                    op->u.transfer.word_count);
            if (rsx_nr_d3d12_preflight_transfer(
                    g_active.d3d12, &state, &op->u.transfer, words) != 0) {
                yz_nr_section_note_transfer_preflight(&op->u.transfer);
                return YZ_NR_SECTION_FB_PREFLIGHT_TRANSFER;
            }
            g_active.section_transfers++;
            break;
        }
        case RSX_NIR_OP_SEMAPHORE_ACQUIRE:
        case RSX_NIR_OP_SEMAPHORE_RELEASE: {
            if (!(g_active.graphics_families & YZ_NR_GRAPHICS_SYNC))
                return YZ_NR_SECTION_FB_PREFLIGHT_SYNC;
            uint32_t value = 0;
            const int32_t read_result = yz_nr_vertical_sem_read(
                    op->u.semaphore.dma_context,
                    op->u.semaphore.offset, &value);
            if (read_result != 0 ||
                (op->kind == RSX_NIR_OP_SEMAPHORE_ACQUIRE &&
                 value != op->u.semaphore.value)) {
                yz_nr_section_note_sync_preflight(op, read_result);
                if (read_result == 0 &&
                    op->kind == RSX_NIR_OP_SEMAPHORE_ACQUIRE) {
                    g_active.section_preflight_sem_dma =
                        op->u.semaphore.dma_context;
                    g_active.section_preflight_sem_offset =
                        op->u.semaphore.offset;
                    g_active.section_preflight_sem_value =
                        op->u.semaphore.value;
                    g_active.section_preflight_sem_valid = 1;
                }
                return YZ_NR_SECTION_FB_PREFLIGHT_SYNC;
            }
            break;
        }
        case RSX_NIR_OP_REPORT:
            if (!(g_active.graphics_families & YZ_NR_GRAPHICS_REPORT))
                return YZ_NR_SECTION_FB_PREFLIGHT_REPORT;
            if (yz_nr_vertical_report_can(
                    op->u.report.kind, op->u.report.arg,
                    op->u.report.dma_report) != 0) {
                yz_nr_section_note_report(
                    op->u.report.kind, op->u.report.arg,
                    op->u.report.dma_report);
                return YZ_NR_SECTION_FB_PREFLIGHT_REPORT;
            }
            break;
        case RSX_NIR_OP_PRESENT:
            if (rsx_nr_d3d12_preflight_present(
                    g_active.d3d12, op->u.present.buffer) != 0)
                return YZ_NR_SECTION_FB_PREFLIGHT_PRESENT;
            g_active.section_presents++;
            break;
        case RSX_NIR_OP_BARRIER:
        case RSX_NIR_OP_SET_REFERENCE:
        case RSX_NIR_OP_USER_COMMAND:
            if (!(g_active.graphics_families & YZ_NR_GRAPHICS_SYNC))
                return YZ_NR_SECTION_FB_PREFLIGHT_SYNC;
            break;
        default:
            return YZ_NR_SECTION_FB_PREFLIGHT_SYNC;
        }
    }
    return g_active.section_gpu_actions ? YZ_NR_SECTION_FB_NONE :
        YZ_NR_SECTION_FB_NO_GPU_ACTION;
}

static int yz_nr_section_push_op(uint32_t index)
{
    if (index >= g_active.section_stream.op_count ||
        rsx_nr_ring_depth(&g_active.ring))
        return -1;
    rsx_nir_op op = g_active.section_stream.ops[index];
    uint32_t offset = 0, count = 0;
    yz_nr_section_op_side(&op, &offset, &count);
    if (!rsx_nr_ring_can_accept(&g_active.ring, 1u, count))
        return -1;
    if (count) {
        const uint32_t* const source = rsx_nir_side(
            &g_active.section_stream, offset, count);
        uint32_t* destination = nullptr;
        const uint32_t ring_offset = rsx_nr_ring_side_reserve(
            &g_active.ring, count, &destination);
        if (!source || ring_offset == ~0u || !destination)
            return -1;
        memcpy(destination, source,
               static_cast<size_t>(count) * sizeof(uint32_t));
        yz_nr_section_set_side(&op, ring_offset);
    }
    return rsx_nr_ring_push(&g_active.ring, &op);
}

static void yz_nr_section_report_fatal(
    const char* phase, uint32_t index, rsx_nr_step_result step,
    unsigned long long errors_before)
{
    static LONG reported = 0;
    if (InterlockedExchange(&reported, 1))
        return;
    const rsx_nir_op* const op =
        index < g_active.section_stream.op_count
            ? &g_active.section_stream.ops[index] : nullptr;
    rsx_nr_d3d12_stats stats = {};
    if (g_active.d3d12)
        rsx_nr_d3d12_get_stats(g_active.d3d12, &stats);
    fprintf(stderr,
            "[nr-vertical-section-fatal phase=%s index=%u/%u kind=%u "
            "step=%u errors=%llu->%llu surface=%u:%08X target=%u "
            "fp=%u:%08X draw=%u/%u/%u clear=%08X transfer=%u "
            "present=%u d3d{draw=%llu clear=%llu xfer=%llu present=%llu "
            "fallback=%llu resident-fail=%llu}]\n",
            phase ? phase : "unknown", index,
            g_active.section_stream.op_count,
            op ? (uint32_t)op->kind : UINT32_MAX, (uint32_t)step,
            errors_before, g_active.backend.stats.exec_errors,
            g_active.backend.st.surface.color_location[0],
            g_active.backend.st.surface.color_offset[0],
            g_active.backend.st.surface.color_target,
            g_active.backend.st.fragment_program.location,
            g_active.backend.st.fragment_program.offset,
            op && op->kind == RSX_NIR_OP_DRAW ? op->u.draw.primitive : 0u,
            op && op->kind == RSX_NIR_OP_DRAW ? op->u.draw.batch_count : 0u,
            op && op->kind == RSX_NIR_OP_DRAW ? op->u.draw.total_count : 0u,
            op && op->kind == RSX_NIR_OP_CLEAR ? op->u.clear.mask : 0u,
            op && op->kind == RSX_NIR_OP_TRANSFER
                ? op->u.transfer.kind : 0u,
            op && op->kind == RSX_NIR_OP_PRESENT
                ? op->u.present.buffer : UINT32_MAX,
            stats.draws, stats.clears, stats.transfers, stats.presents,
            stats.unsupported_draws, stats.residency_failures);
    fflush(stderr);
}

static yz_nr_vertical_section_result yz_nr_section_execute(
    uint32_t* next_get, uint32_t* next_ret)
{
    while (g_active.section_exec_pos <
           g_active.section_stream.op_count) {
        if (!rsx_nr_ring_depth(&g_active.ring) &&
            yz_nr_section_push_op(g_active.section_exec_pos) != 0) {
            g_active.section_exec_errors++;
            g_active.section_fatal = 1;
            yz_nr_section_report_fatal(
                "push", g_active.section_exec_pos,
                RSX_NR_STEP_EMPTY, g_active.backend.stats.exec_errors);
            return YZ_NR_VERTICAL_SECTION_FATAL;
        }
        const unsigned long long errors_before =
            g_active.backend.stats.exec_errors;
        const rsx_nr_step_result step =
            rsx_nr_backend_step(&g_active.backend);
        if (step == RSX_NR_STEP_BLOCKED_TOKEN ||
            step == RSX_NR_STEP_BLOCKED_SEMAPHORE)
            return YZ_NR_VERTICAL_SECTION_WAIT;
        if (step != RSX_NR_STEP_EXECUTED ||
            g_active.backend.stats.exec_errors != errors_before) {
            g_active.section_exec_errors++;
            g_active.section_fatal = 1;
            yz_nr_section_report_fatal(
                "execute", g_active.section_exec_pos, step,
                errors_before);
            return YZ_NR_VERTICAL_SECTION_FATAL;
        }
        g_active.section_exec_pos++;
    }
    if (rsx_nr_ring_depth(&g_active.ring)) {
        g_active.section_exec_errors++;
        g_active.section_fatal = 1;
        yz_nr_section_report_fatal(
            "trailing", g_active.section_exec_pos,
            RSX_NR_STEP_EMPTY, g_active.backend.stats.exec_errors);
        return YZ_NR_VERTICAL_SECTION_FATAL;
    }
    if (next_get)
        *next_get = g_active.section_next_get;
    if (next_ret)
        *next_ret = g_active.section_next_ret;
    g_active.section_owned++;
    g_active.section_methods_owned += g_active.section_method_count;
    g_active.section_ops_owned += g_active.section_stream.op_count;
    if (g_active.section_draws || g_active.section_clears)
        g_active.section_render_passes_owned++;
    else
        g_active.section_dependency_islands_owned++;
    g_active.consumer_draw_owned += g_active.section_draws;
    g_active.consumer_clear_owned += g_active.section_clears;
    g_active.consumer_transfer_owned += g_active.section_transfers;
    g_active.executed[YZ_NR_VERT_DRAW_ARRAYS] += g_active.section_draws;
    g_active.section_pending = 0;
    g_active.section_exec_pos = 0;
    return YZ_NR_VERTICAL_SECTION_EXECUTED;
}

static yz_nr_vertical_section_result yz_nr_section_commit(
    uint32_t pc, uint32_t ret, uint32_t* next_get, uint32_t* next_ret)
{
    rsx_nir_adapter_finish(g_active.section_adapter);
    if (g_active.section_adapter->rsx.in_begin_end ||
        g_active.section_adapter->batch_count ||
        g_active.section_adapter->inline_count)
        return yz_nr_section_fallback(
            YZ_NR_SECTION_FB_INCOMPLETE_ACTION);

    uint32_t preflight = yz_nr_section_program_preflight();
    if (preflight == YZ_NR_SECTION_FB_NONE)
        preflight = yz_nr_section_preflight();
    if (preflight == YZ_NR_SECTION_FB_NO_GPU_ACTION) {
        /* A producer may publish a long state prefix before its terminal
         * draw/clear/transfer. Owning that prefix buys no GPU work and risks
         * suppressing legacy-only register side effects before the complete
         * pass exists. Keep it wholly legacy and reconsider only at the exact
         * next publication boundary. */
        return yz_nr_section_defer_fallback(
            preflight, pc, g_active.section_packet_count);
    }
    if (preflight == YZ_NR_SECTION_FB_PREFLIGHT_SYNC &&
        g_active.section_preflight_sem_valid) {
        g_active.section_blocked_sem_get = g_active.section_start_get;
        g_active.section_blocked_sem_dma =
            g_active.section_preflight_sem_dma;
        g_active.section_blocked_sem_offset =
            g_active.section_preflight_sem_offset;
        g_active.section_blocked_sem_value =
            g_active.section_preflight_sem_value;
        g_active.section_blocked_sem_valid = 1;
        /* The exact semaphore-value gate below is the cache. It must observe
         * satisfaction even when GET/PUT and the command word are unchanged. */
        g_active.section_scan_cacheable = 0;
        return yz_nr_section_fallback(preflight);
    }
    if (preflight != YZ_NR_SECTION_FB_NONE) {
        const uint32_t packet_budget =
            g_active.section_packet_count < 0x7FFFu
                ? g_active.section_packet_count * 2u + 16u
                : 0xFFFFu;
        return yz_nr_section_defer_fallback(
            preflight, pc, packet_budget);
    }

    /* The complete command-list section is now admitted. Keep the dormant
     * legacy register/resource model current before suppressing its terminal
     * actions. An executor failure after this point is fatal: legacy can
     * never repeat a partially executed section. */
    for (uint32_t m = 0; m < g_active.section_method_count; ++m) {
        const yz_nr_section_method* const retained =
            &g_active.section_methods[m];
        if (yz_nr_vertical_mirror_legacy_method(
                retained->method, retained->arg,
                retained->suppress_action) != 0)
            return yz_nr_section_fallback(
                YZ_NR_SECTION_FB_INCOMPLETE_ACTION);
    }
    const rsx_nir_sink ring_sink = rsx_nr_ring_sink(&g_active.ring);
    g_active.adapter = *g_active.section_adapter;
    rsx_nir_adapter_rebind(&g_active.adapter);
    g_active.adapter.em.out = ring_sink;
    g_active.adapter.shadow_mode = 1;
    g_active.section_next_get = pc;
    g_active.section_next_ret = ret;
    g_active.section_exec_pos = 0;
    g_active.section_fatal = 0;
    g_active.section_pending = 1;
    return yz_nr_section_execute(next_get, next_ret);
}

extern "C" yz_nr_vertical_section_result
yz_nr_vertical_consume_section(uint32_t get, uint32_t put,
                               uint32_t fifo_ret, uint32_t* next_get,
                               uint32_t* next_ret)
{
    if (next_get)
        *next_get = get;
    if (next_ret)
        *next_ret = fifo_ret;
    if (!g_active.frame_islands ||
        !InterlockedCompareExchange(
            &g_vertical.mode_active_graphics, 0, 0) ||
        !rsx_live_draw_native_actions_allowed())
        return YZ_NR_VERTICAL_SECTION_MISS;
    yz_nr_active_ensure_graphics();
    if (!InterlockedCompareExchange(&g_active.graphics_ready, 0, 0))
        return YZ_NR_VERTICAL_SECTION_MISS;
    if (g_active.section_pending)
        if (g_active.section_fatal)
            return YZ_NR_VERTICAL_SECTION_FATAL;
    if (g_active.section_pending)
        return yz_nr_section_execute(next_get, next_ret);
    if (get == put)
        return YZ_NR_VERTICAL_SECTION_WAIT;
    if (g_active.section_blocked_sem_valid) {
        if (get != g_active.section_blocked_sem_get) {
            g_active.section_blocked_sem_valid = 0;
        } else {
            uint32_t observed = 0;
            const int32_t result = yz_nr_vertical_sem_read(
                g_active.section_blocked_sem_dma,
                g_active.section_blocked_sem_offset, &observed);
            if (result != 0 ||
                observed != g_active.section_blocked_sem_value) {
                g_active.section_fallback_fast_skips++;
                return YZ_NR_VERTICAL_SECTION_FALLBACK;
            }
            g_active.section_blocked_sem_valid = 0;
            g_active.section_repeat_valid = 0;
        }
    }
    if (g_active.section_fallback_active) {
        if (get == g_active.section_fallback_until_get) {
            g_active.section_fallback_active = 0;
            g_active.section_repeat_valid = 0;
        } else {
            g_active.section_fallback_fast_skips++;
            return YZ_NR_VERTICAL_SECTION_FALLBACK;
        }
    }

    if (g_active.section_legacy_path_active) {
        if (rsx_nr_fifo_visit_contains(
                &g_active.section_visits, get, fifo_ret)) {
            g_active.section_fallback_fast_skips++;
            g_active.section_legacy_path_skips++;
            return YZ_NR_VERTICAL_SECTION_FALLBACK;
        }
        g_active.section_legacy_path_active = 0;
        g_active.section_legacy_path_exits++;
    }

    if (g_active.section_repeat_valid) {
        const uint32_t repeat_ea = yz_nr_vertical_io_to_ea(get);
        if (get == g_active.section_repeat_get &&
            put == g_active.section_repeat_put &&
            fifo_ret == g_active.section_repeat_ret && repeat_ea &&
            vm_read32(repeat_ea) == g_active.section_repeat_word) {
            g_active.section_fallback_fast_skips++;
            return YZ_NR_VERTICAL_SECTION_FALLBACK;
        }
        g_active.section_repeat_valid = 0;
    }

    g_active.section_attempts++;
    g_active.section_scan_get = get;
    g_active.section_scan_put = put;
    g_active.section_scan_ret = fifo_ret;
    const uint32_t scan_ea = yz_nr_vertical_io_to_ea(get);
    g_active.section_scan_word = scan_ea ? vm_read32(scan_ea) : 0u;
    g_active.section_scan_cacheable = scan_ea != 0;
    rsx_nir_stream_reset(&g_active.section_stream);
    g_active.section_method_count = 0;
    g_active.section_packet_count = 0;
    g_active.section_start_get = get;
    *g_active.section_adapter = g_active.adapter;
    rsx_nir_adapter_rebind(g_active.section_adapter);
    const rsx_nir_sink stream_sink =
        rsx_nir_stream_sink(&g_active.section_stream);
    g_active.section_adapter->em.out = stream_sink;
    g_active.section_adapter->shadow_mode = 0;

    const uint32_t ring = 0x800000u;
    const uint32_t mask = ring - 1u;
    uint32_t pc = get;
    uint32_t ret = fifo_ret;
    uint32_t gpu_actions_seen = 0;
    rsx_nr_fifo_visit_reset(&g_active.section_visits);

    for (uint32_t step = 0; step < YZ_NR_SECTION_STEP_CAPACITY; ++step) {
        const int visit = rsx_nr_fifo_visit_note(
            &g_active.section_visits, pc, ret);
        if (visit <= 0) {
            /* A general control-flow cycle has no proven transactional end.
             * Keep the entire cycle legacy even when it contains GPU actions;
             * committing a prefix would suppress one iteration and resume GET
             * in the middle of the loop. The explicit self-stopper case below
             * remains the only cyclic boundary eligible for native ownership.
             * Hash-table exhaustion is likewise a bounded refusal. */
            return yz_nr_section_path_fallback(
                visit == 0 ? YZ_NR_SECTION_FB_FLOW
                           : YZ_NR_SECTION_FB_CAPACITY);
        }
        const rsx_nr_fifo_range_status header_status =
            rsx_nr_fifo_section_range_status(pc, 4u, put, ret, ring);
        if (header_status == RSX_NR_FIFO_RANGE_NOT_READY) {
            /* PUT is a publication boundary. A state-only prefix is safe to
             * claim too: it is mirrored into both register models and has no
             * externally visible GPU action. This avoids rescanning a long
             * state prefix one packet at a time. */
            if (pc != get && ret == fifo_ret)
                return yz_nr_section_commit(
                    pc, ret, next_get, next_ret);
            return yz_nr_section_fallback(YZ_NR_SECTION_FB_NOT_READY);
        }
        if (header_status == RSX_NR_FIFO_RANGE_WINDOW) {
            yz_nr_section_note_window(pc, put, ret, 4u, 0u, 0u);
            return yz_nr_section_fallback(YZ_NR_SECTION_FB_WINDOW);
        }
        const uint32_t ea = yz_nr_vertical_io_to_ea(pc);
        if (!ea)
            return yz_nr_section_fallback(YZ_NR_SECTION_FB_UNMAPPED);
        const uint32_t command = vm_read32(ea);
        if (command == 0u) {
            pc = ret == ~0u ? ((pc + 4u) & mask) : pc + 4u;
            continue;
        }
        if ((command & 0xE0000003u) == 0x20000000u ||
            (command & 3u) == 1u) {
            const uint32_t target = (command & 3u) == 1u
                ? (command & 0xFFFFFFFCu) : (command & 0x1FFFFFFCu);
            if (target == pc && gpu_actions_seen)
                return yz_nr_section_commit(
                    pc, ret, next_get, next_ret);
            if (target == pc || !yz_nr_vertical_io_to_ea(target))
                return yz_nr_section_path_fallback(YZ_NR_SECTION_FB_FLOW);
            pc = target;
            continue;
        }
        if ((command & 3u) == 2u) {
            const uint32_t target = command & 0x1FFFFFFCu;
            if (ret != ~0u || !yz_nr_vertical_io_to_ea(target))
                return yz_nr_section_path_fallback(YZ_NR_SECTION_FB_FLOW);
            ret = pc < ring ? ((pc + 4u) & mask) : pc + 4u;
            pc = target;
            continue;
        }
        if ((command & 0xFFFF0003u) == 0x00020000u) {
            if (ret == ~0u)
                return yz_nr_section_path_fallback(YZ_NR_SECTION_FB_FLOW);
            pc = ret;
            ret = ~0u;
            continue;
        }
        if (command & 0xA0030003u)
            return yz_nr_section_fallback(YZ_NR_SECTION_FB_NOT_READY);

        const uint32_t count = (command >> 18) & 0x7FFu;
        const uint32_t non_incrementing = command & 0x40000000u;
        const uint32_t method = command & 0x3FFFCu;
        if (!count)
            return yz_nr_section_path_fallback(YZ_NR_SECTION_FB_FLOW);
        const uint32_t packet_size = 4u + count * 4u;
        const rsx_nr_fifo_range_status packet_status =
            rsx_nr_fifo_section_range_status(
                pc, packet_size, put, ret, ring);
        if (packet_status == RSX_NR_FIFO_RANGE_NOT_READY)
            return yz_nr_section_fallback(YZ_NR_SECTION_FB_NOT_READY);
        if (packet_status == RSX_NR_FIFO_RANGE_WINDOW) {
            yz_nr_section_note_window(
                pc, put, ret, packet_size, command, 1u);
            return yz_nr_section_fallback(YZ_NR_SECTION_FB_WINDOW);
        }
        /* Do not let a render pass absorb the first packet of the following
         * dependency or pass. This check happens before mutating the shadow
         * adapter, so fallback can never require a rollback. A FIFO packet is
         * indivisible; if it combines the boundary with preceding state it is
         * admitted as part of the next island. */
        if (gpu_actions_seen) {
            int boundary_before = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t effective = non_incrementing
                    ? method : method + i * 4u;
                if (yz_nr_section_dependency_method(effective) ||
                    yz_nr_section_starts_new_pass(effective)) {
                    boundary_before = 1;
                    break;
                }
            }
            if (boundary_before)
                return yz_nr_section_commit(
                    pc, ret, next_get, next_ret);
        }
        const uint32_t packet_ops_before =
            g_active.section_stream.op_count;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t argument_pc = pc + 4u + i * 4u;
            const uint32_t argument_ea =
                yz_nr_vertical_io_to_ea(argument_pc);
            if (!argument_ea)
                return yz_nr_section_fallback(
                    YZ_NR_SECTION_FB_UNMAPPED);
            const uint32_t effective = non_incrementing
                ? method : method + i * 4u;
            const uint32_t value = vm_read32(argument_ea);
            if (!rsx_nir_adapter_method_supported(
                    g_active.section_adapter, effective, value)) {
                yz_nr_section_note_unknown(effective, value);
                return yz_nr_section_fallback(
                    YZ_NR_SECTION_FB_UNKNOWN_METHOD);
            }
            if (g_active.section_method_count >=
                YZ_NR_SECTION_METHOD_CAPACITY)
                return yz_nr_section_path_fallback(
                    YZ_NR_SECTION_FB_CAPACITY);
            const uint32_t actions_before =
                g_active.section_adapter->actions_seen;
            rsx_nir_adapter_method(
                g_active.section_adapter, effective, value);
            const uint32_t action =
                g_active.section_adapter->actions_seen != actions_before;
            yz_nr_section_method* const retained =
                &g_active.section_methods[g_active.section_method_count++];
            retained->method = effective;
            retained->arg = value;
            retained->suppress_action = action ||
                rsx_nr_legacy_gpu_action(effective, value) ||
                effective == 0x1808u || effective == 0x1814u ||
                effective == 0x1824u;
            if (g_active.section_stream.overflow ||
                g_active.section_stream.oom ||
                g_active.section_adapter->batch_overflow ||
                g_active.section_adapter->inline_overflow)
                return yz_nr_section_path_fallback(
                    YZ_NR_SECTION_FB_CAPACITY);
        }
        pc = ret == ~0u ? ((pc + packet_size) & mask)
                        : pc + packet_size;
        g_active.section_packet_count++;

        int dependency_action = 0;
        for (uint32_t i = packet_ops_before;
             i < g_active.section_stream.op_count; ++i) {
            const uint32_t kind = g_active.section_stream.ops[i].kind;
            if (yz_nr_section_gpu_action(kind))
                gpu_actions_seen++;
            if (kind == RSX_NIR_OP_TRANSFER ||
                kind == RSX_NIR_OP_PRESENT ||
                kind == RSX_NIR_OP_SEMAPHORE_ACQUIRE ||
                kind == RSX_NIR_OP_SEMAPHORE_RELEASE ||
                kind == RSX_NIR_OP_REPORT ||
                kind == RSX_NIR_OP_BARRIER ||
                kind == RSX_NIR_OP_SET_REFERENCE ||
                kind == RSX_NIR_OP_USER_COMMAND)
                dependency_action = 1;
        }
        /* Draws remain grouped until the next semantic pass boundary. A
         * transfer/sync/report/present island is complete at its action and
         * is admitted independently, never mixed into a render pass. */
        if (dependency_action)
            return yz_nr_section_commit(
                pc, ret, next_get, next_ret);
    }
    return yz_nr_section_path_fallback(YZ_NR_SECTION_FB_CAPACITY);
}

extern "C" yz_nr_vertical_consume_result
yz_nr_vertical_consume(uint32_t packet_ea, uint32_t* word_count)
{
    if (word_count)
        *word_count = 0;
    if (!InterlockedCompareExchange(&g_vertical.mode_active_basic, 0, 0))
        return YZ_NR_VERTICAL_CONSUME_MISS;
    yz_nr_active_ensure_graphics();

    /* A retained claim owns GET until every typed op has completed. In
     * particular, an unsatisfied semaphore remains at the ring head; retries
     * re-read the label but never re-claim, re-push, or duplicate PRESENT. */
    if (g_active.pending_valid) {
        if (packet_ea != g_active.pending_span.ea) {
            g_active.fatal++;
            return YZ_NR_VERTICAL_CONSUME_FATAL;
        }
        while (g_active.pending_executed <
               g_active.pending_expected) {
            const unsigned long long errors_before =
                g_active.backend.stats.exec_errors;
            const rsx_nr_step_result step =
                rsx_nr_backend_step(&g_active.backend);
            if (step == RSX_NR_STEP_BLOCKED_TOKEN ||
                step == RSX_NR_STEP_BLOCKED_SEMAPHORE) {
                g_active.wait++;
                return YZ_NR_VERTICAL_CONSUME_WAIT;
            }
            if (step != RSX_NR_STEP_EXECUTED) {
                g_active.fatal++;
                return YZ_NR_VERTICAL_CONSUME_FATAL;
            }
            g_active.pending_executed++;

            if (g_active.backend.stats.exec_errors != errors_before) {
                /* A producer may grant this escape hatch only when it left
                 * the exact complete packet in guest memory and the failing
                 * action has an all-or-nothing backend contract. Stop before
                 * later typed ops, discard them, and leave GET unchanged so
                 * the ordinary decoder executes that retained packet once. */
                if (!(g_active.pending_span.flags &
                      RSX_NR_SPAN_RETAINED_FALLBACK)) {
                    g_active.fatal++;
                    return YZ_NR_VERTICAL_CONSUME_FATAL;
                }
                while (rsx_nr_ring_depth(&g_active.ring))
                    rsx_nr_ring_pop(&g_active.ring);
                if (rsx_nr_span_router_retire(
                        &g_active.router, &g_active.pending_claim) != 0) {
                    g_active.fatal++;
                    return YZ_NR_VERTICAL_CONSUME_FATAL;
                }
                g_active.pending_valid = 0;
                g_active.pending_executed = 0;
                g_active.pending_expected = 0;
                memset(&g_active.pending_span, 0,
                       sizeof(g_active.pending_span));
                memset(&g_active.pending_claim, 0,
                       sizeof(g_active.pending_claim));
                g_active.last_miss_ea = ~0u;
                g_active.last_miss_epoch = 0;
                g_active.late_fallback++;
                return YZ_NR_VERTICAL_CONSUME_FALLBACK;
            }
        }
        if (rsx_nr_ring_depth(&g_active.ring) != 0) {
            g_active.wait++;
            return YZ_NR_VERTICAL_CONSUME_WAIT;
        }
        if (g_active.pending_executed !=
                g_active.pending_expected ||
            rsx_nr_span_router_retire(&g_active.router,
                                      &g_active.pending_claim) != 0) {
            g_active.fatal++;
            return YZ_NR_VERTICAL_CONSUME_FATAL;
        }

        for (uint32_t i = 0;
             i < g_active.pending_span.payload.op_count; ++i) {
            const uint32_t kind =
                g_active.pending_span.payload.ops[i].kind;
            if (kind == RSX_NIR_OP_SET_REFERENCE)
                g_active.executed[YZ_NR_VERT_REFERENCE]++;
            else if (kind == RSX_NIR_OP_USER_COMMAND)
                g_active.executed[YZ_NR_VERT_USER]++;
            else if (kind == RSX_NIR_OP_PRESENT)
                g_active.executed[YZ_NR_VERT_FLIP]++;
            else if (kind == RSX_NIR_OP_DRAW)
                g_active.executed[YZ_NR_VERT_DRAW_ARRAYS]++;
        }
        if (word_count)
            *word_count = g_active.pending_span.word_count;
        g_active.pending_valid = 0;
        g_active.pending_executed = 0;
        g_active.pending_expected = 0;
        memset(&g_active.pending_span, 0, sizeof(g_active.pending_span));
        memset(&g_active.pending_claim, 0, sizeof(g_active.pending_claim));
        g_active.last_miss_ea = ~0u;
        g_active.last_miss_epoch = 0;
        return YZ_NR_VERTICAL_CONSUME_EXECUTED;
    }

    const uint32_t epoch =
        rsx_nr_span_router_publication_epoch(&g_active.router);
    if (g_active.last_miss_ea == packet_ea &&
        g_active.last_miss_epoch == epoch) {
        return YZ_NR_VERTICAL_CONSUME_MISS;
    }

    rsx_nr_span span = {};
    rsx_nr_span_claim claim = {};
    const rsx_nr_span_take_result take =
        rsx_nr_span_router_claim(&g_active.router, packet_ea, &span, &claim);
    if (take == RSX_NR_SPAN_TAKE_FAST_MISS ||
        take == RSX_NR_SPAN_TAKE_MISS) {
        g_active.last_miss_ea = packet_ea;
        g_active.last_miss_epoch = epoch;
        return YZ_NR_VERTICAL_CONSUME_MISS;
    }
    if (take == RSX_NR_SPAN_TAKE_NOT_READY) {
        g_active.wait++;
        return YZ_NR_VERTICAL_CONSUME_WAIT;
    }
    if (take != RSX_NR_SPAN_TAKE_CLAIMED) {
        g_active.fatal++;
        return YZ_NR_VERTICAL_CONSUME_FATAL;
    }

    const int typed_draw = span.payload.op_count == 1u &&
        span.payload.ops[0].kind == RSX_NIR_OP_DRAW;
    if (typed_draw) {
        const rsx_nir_draw* const draw = &span.payload.ops[0].u.draw;
        if (!InterlockedCompareExchange(&g_active.graphics_ready, 0, 0) ||
            !draw->batch_count ||
            span.payload.side_count != draw->batch_count * 2u ||
            !rsx_nr_ring_can_accept(&g_active.ring, 64u, 8192u)) {
            g_active.fatal++;
            return YZ_NR_VERTICAL_CONSUME_FATAL;
        }
        rsx_nr_ring_clear_reject(&g_active.ring);
        rsx_nir_adapter_stage_state(&g_active.adapter);
        rsx_nir_em_draw(&g_active.adapter.em, draw->primitive,
                        draw->indexed, span.payload.side,
                        draw->batch_count);
        if (rsx_nr_ring_reject_sticky(&g_active.ring)) {
            g_active.fatal++;
            return YZ_NR_VERTICAL_CONSUME_FATAL;
        }
    } else {
        if (span.payload.side_count != 0 ||
            !rsx_nr_ring_can_accept(&g_active.ring,
                                    span.payload.op_count, 0)) {
            g_active.fatal++;
            return YZ_NR_VERTICAL_CONSUME_FATAL;
        }
        for (uint32_t i = 0; i < span.payload.op_count; ++i) {
            if (rsx_nr_ring_push(&g_active.ring,
                                 &span.payload.ops[i]) != 0) {
                g_active.fatal++;
                return YZ_NR_VERTICAL_CONSUME_FATAL;
            }
        }
    }
    const uint32_t expected = rsx_nr_ring_depth(&g_active.ring);
    if (!expected) {
        g_active.fatal++;
        return YZ_NR_VERTICAL_CONSUME_FATAL;
    }
    g_active.pending_span = span;
    g_active.pending_claim = claim;
    g_active.pending_executed = 0;
    g_active.pending_expected = expected;
    g_active.pending_valid = 1;
    return yz_nr_vertical_consume(packet_ea, word_count);
}

extern "C" void yz_nr_vertical_shutdown(void)
{
    if (InterlockedExchange(&g_vertical.mode_active_basic, 0)) {
        InterlockedExchange(&g_vertical.mode_active_present, 0);
        InterlockedExchange(&g_vertical.mode_active_graphics, 0);
        cellSpursSetGuestWriteObserver(nullptr);
        InterlockedExchange(&g_active.graphics_ready, 0);
        rsx_nr_span_router_stats stats = {};
        rsx_nr_span_router_get_stats(&g_active.router, &stats);
        fprintf(stderr,
                "[nr-vertical-active families=0x%02X clear-scope=%u "
                "frame-islands=%u "
                "ref=%llu/%llu user=%llu/%llu draw=%llu/%llu "
                "flip=%llu/%llu fallback-ref=%llu fallback-user=%llu "
                "fallback-draw=%llu fallback-flip=%llu "
                "wrong-context=%llu no-room=%llu publish-fail=%llu "
                "consumer-draw=%llu/%llu "
                "consumer-clear=%llu/%llu "
                "clear-contract=%llu/%llu/%llu "
                "consumer-transfer=%llu/%llu "
                "consumer-sync=%llu/%llu "
                "consumer-report=%llu/%llu "
                "wait=%llu late-fallback=%llu fatal=%llu "
                "depth=%u errors=%llu]\n",
                g_active.graphics_families,
                g_active.clear_scope,
                g_active.frame_islands,
                g_active.owned[YZ_NR_VERT_REFERENCE],
                g_active.executed[YZ_NR_VERT_REFERENCE],
                g_active.owned[YZ_NR_VERT_USER],
                g_active.executed[YZ_NR_VERT_USER],
                g_active.owned[YZ_NR_VERT_DRAW_ARRAYS],
                g_active.executed[YZ_NR_VERT_DRAW_ARRAYS],
                g_active.owned[YZ_NR_VERT_FLIP],
                g_active.executed[YZ_NR_VERT_FLIP],
                g_active.fallback[YZ_NR_VERT_REFERENCE],
                g_active.fallback[YZ_NR_VERT_USER],
                g_active.fallback[YZ_NR_VERT_DRAW_ARRAYS],
                g_active.fallback[YZ_NR_VERT_FLIP],
                g_active.wrong_context, g_active.no_room,
                g_active.publish_failure,
                g_active.consumer_draw_owned,
                g_active.consumer_draw_fallback,
                g_active.consumer_clear_owned,
                g_active.consumer_clear_fallback,
                g_active.consumer_clear_contract[0],
                g_active.consumer_clear_contract[1],
                g_active.consumer_clear_contract[2],
                g_active.consumer_transfer_owned,
                g_active.consumer_transfer_fallback,
                g_active.consumer_sync_owned,
                g_active.consumer_sync_fallback,
                g_active.consumer_report_owned,
                g_active.consumer_report_fallback, g_active.wait,
                g_active.late_fallback, g_active.fatal,
                rsx_nr_ring_depth(&g_active.ring),
                g_active.backend.stats.exec_errors);
        fprintf(stderr,
                "[nr-vertical-router published=%llu claimed=%llu "
                "fast-miss=%llu exact-miss=%llu not-ready=%llu "
                "duplicate=%llu busy=%llu full=%llu corrupt=%llu]\n",
                stats.published, stats.claimed, stats.fast_misses,
                stats.exact_misses, stats.not_ready, stats.duplicates,
                stats.busy, stats.full, stats.corrupt);
        if (g_active.frame_islands) {
            fprintf(stderr,
                    "[nr-vertical-sections attempts=%llu owned=%llu "
                    "render-passes=%llu dependency-islands=%llu "
                    "methods=%llu ops=%llu exec-errors=%llu fast-skip=%llu "
                    "legacy-path=%u:%u/%llu/%llu shadow-depth=%llu "
                    "shadow-consumer=%llu "
                    "fallback="
                    "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                    "%llu,%llu,%llu,%llu,%llu,%llu,%llu]\n",
                    g_active.section_attempts, g_active.section_owned,
                    g_active.section_render_passes_owned,
                    g_active.section_dependency_islands_owned,
                    g_active.section_methods_owned,
                    g_active.section_ops_owned,
                    g_active.section_exec_errors,
                    g_active.section_fallback_fast_skips,
                    g_active.section_legacy_path_active,
                    g_active.section_legacy_path_reason,
                    g_active.section_legacy_path_skips,
                    g_active.section_legacy_path_exits,
                    g_active.section_shadow_depth_fallback,
                    g_active.section_shadow_consumer_fallback,
                    g_active.section_fallback[0],
                    g_active.section_fallback[1],
                    g_active.section_fallback[2],
                    g_active.section_fallback[3],
                    g_active.section_fallback[4],
                    g_active.section_fallback[5],
                    g_active.section_fallback[6],
                    g_active.section_fallback[7],
                    g_active.section_fallback[8],
                    g_active.section_fallback[9],
                    g_active.section_fallback[10],
                    g_active.section_fallback[11],
                    g_active.section_fallback[12],
                    g_active.section_fallback[13],
                    g_active.section_fallback[14]);
            for (uint32_t i = 0; i < 64u; ++i) {
                const yz_nr_section_unknown_key* const entry =
                    &g_active.section_unknown[i];
                if (entry->count)
                    fprintf(stderr,
                            "[nr-vertical-section-unknown "
                            "method=0x%05X arg=0x%08X count=%llu]\n",
                            entry->method, entry->arg, entry->count);
            }
            if (g_active.section_unknown_overflow)
                fprintf(stderr,
                        "[nr-vertical-section-unknown-overflow=%llu]\n",
                        g_active.section_unknown_overflow);
            for (uint32_t i = 0; i < 32u; ++i) {
                const yz_nr_section_window_key* const entry =
                    &g_active.section_window[i];
                if (entry->count)
                    fprintf(stderr,
                            "[nr-vertical-section-window stage=%u "
                            "pc=0x%08X ret=0x%08X size=%u cmd=0x%08X "
                            "put=0x%08X..0x%08X available=0x%08X..0x%08X "
                            "count=%llu error=%llu]\n",
                            entry->stage, entry->pc, entry->ret,
                            entry->size, entry->command,
                            entry->first_put, entry->last_put,
                            entry->min_available, entry->max_available,
                            entry->count, entry->error);
            }
            for (uint32_t i = 0; i < 64u; ++i) {
                const yz_nr_section_report_key* const entry =
                    &g_active.section_reports[i];
                if (entry->count)
                    fprintf(stderr,
                            "[nr-vertical-section-report kind=%u "
                            "arg=0x%08X dma=0x%08X count=%llu]\n",
                            entry->kind, entry->arg, entry->dma,
                            entry->count);
            }
            if (g_active.section_report_overflow)
                fprintf(stderr,
                        "[nr-vertical-section-report-overflow=%llu]\n",
                        g_active.section_report_overflow);
            if (g_active.section_diag_enabled) {
                for (uint32_t i = 0; i < YZ_NR_FLOW_VP_DIAG_COUNT; ++i) {
                    const yz_nr_flow_vp_diag* const entry =
                        &g_active.section_flow_vp[i];
                    if (!entry->draws)
                        continue;
                    fprintf(stderr,
                            "[nr-vertical-flow-vp slot=%u hash=%016llX "
                            "start=%u words=%u draws=%llu "
                            "branch=%08X..%08X or=%08X and=%08X]\n",
                            i, entry->hash, entry->start_slot,
                            entry->word_count, entry->draws,
                            entry->branch_first, entry->branch_last,
                            entry->branch_or, entry->branch_and);
                    for (uint32_t word = 0;
                         word + 3u < entry->word_count; word += 4u)
                        fprintf(stderr,
                                "[nr-vertical-flow-vp-word slot=%u i=%u "
                                "%08X %08X %08X %08X]\n",
                                i, word / 4u, entry->words[word],
                                entry->words[word + 1u],
                                entry->words[word + 2u],
                                entry->words[word + 3u]);
                }
                if (g_active.section_flow_vp_overflow)
                    fprintf(stderr,
                            "[nr-vertical-flow-vp-overflow=%llu]\n",
                            g_active.section_flow_vp_overflow);
            }
            for (uint32_t i = 0; i < 64u; ++i) {
                const yz_nr_section_draw_preflight_key* const entry =
                    &g_active.section_draw_preflight[i];
                if (entry->count)
                    fprintf(stderr,
                            "[nr-vertical-section-draw-preflight reason=%u "
                            "prim=%u target=0x%X fp=%u:0x%08X "
                            "vp=%u/in=%04X rt=%u:0x%08X/fmt=%u/pitch=%u "
                            "view=%u,%u+%ux%u sc=%u,%u+%ux%u "
                            "color-mask=%08X depth=%u/%u tex=%04X "
                            "stencil=%u ref=%02X/%02X mask=%02X/%02X "
                            "write=%02X/%02X vp-hash=%016llX words=%u "
                            "bad-vec=%08X bad-sca=%08X missing-vtex=%X "
                            "cond=%u term=%u "
                            "count=%llu]\n",
                            entry->reason, entry->primitive,
                            entry->color_target, entry->fp_location,
                            entry->fp_offset, entry->vp_start,
                            entry->vp_inputs & 0xFFFFu,
                            entry->color_location, entry->color_offset,
                            entry->color_format, entry->color_pitch,
                            entry->viewport_x, entry->viewport_y,
                            entry->viewport_w, entry->viewport_h,
                            entry->scissor_x, entry->scissor_y,
                            entry->scissor_w, entry->scissor_h,
                            entry->color_mask, entry->depth_test,
                            entry->depth_write,
                            entry->texture_mask & 0xFFFFu,
                            entry->stencil_two_sided,
                            entry->stencil_ref & 0xFFu,
                            entry->back_stencil_ref & 0xFFu,
                            entry->stencil_mask & 0xFFu,
                            entry->back_stencil_mask & 0xFFu,
                            entry->stencil_write_mask & 0xFFu,
                            entry->back_stencil_write_mask & 0xFFu,
                            entry->vp_hash, entry->vp_words,
                            entry->vp_bad_vec, entry->vp_bad_sca,
                            entry->vp_missing_vtex,
                            entry->vp_conditional, entry->vp_terminated,
                            entry->count);
            }
            if (g_active.section_draw_preflight_overflow)
                fprintf(stderr,
                        "[nr-vertical-section-draw-preflight-overflow=%llu]\n",
                        g_active.section_draw_preflight_overflow);
            for (uint32_t i = 0; i < 64u; ++i) {
                const yz_nr_section_sync_preflight_key* const entry =
                    &g_active.section_sync_preflight[i];
                if (entry->count)
                    fprintf(stderr,
                            "[nr-vertical-section-sync-preflight kind=%u "
                            "dma=0x%08X offset=0x%08X value=0x%08X "
                            "release-kind=%u result=%d count=%llu]\n",
                            entry->kind, entry->dma, entry->offset,
                            entry->value, entry->release_kind,
                            entry->result, entry->count);
            }
            if (g_active.section_sync_preflight_overflow)
                fprintf(stderr,
                        "[nr-vertical-section-sync-preflight-overflow=%llu]\n",
                        g_active.section_sync_preflight_overflow);
            for (uint32_t i = 0; i < 64u; ++i) {
                const yz_nr_section_transfer_preflight_key* const entry =
                    &g_active.section_transfer_preflight[i];
                if (entry->count)
                    fprintf(stderr,
                            "[nr-vertical-section-transfer-preflight kind=%u "
                            "src=%u dst=%u fmt=%u/%u line=%u/%u words=%u "
                            "in=%ux%u out=%ux%u count=%llu]\n",
                            entry->kind, entry->src_location,
                            entry->dst_location, entry->src_format,
                            entry->dst_format, entry->line_length,
                            entry->line_count, entry->word_count,
                            entry->in_w, entry->in_h, entry->out_w,
                            entry->out_h, entry->count);
            }
            if (g_active.section_transfer_preflight_overflow)
                fprintf(stderr,
                        "[nr-vertical-section-transfer-preflight-overflow=%llu]\n",
                        g_active.section_transfer_preflight_overflow);
        }
        fflush(stderr);
        if (g_active.d3d12) {
            rsx_nr_d3d12_stats d3d_stats = {};
            rsx_nr_d3d12_get_stats(g_active.d3d12, &d3d_stats);
            const unsigned long long legacy_draw_groups =
                rsx_live_draw_get_completed_draws();
            const unsigned long long all_draw_groups =
                d3d_stats.draw_batches + legacy_draw_groups;
            const unsigned long long native_draw_coverage_ppm =
                all_draw_groups
                    ? d3d_stats.draw_batches * 1000000ull / all_draw_groups
                    : 0ull;
            fprintf(stderr,
                    "[nr-vertical-d3d draws=%llu conditional-skip=%llu "
                    "batches=%llu legacy-groups=%llu coverage-ppm=%llu "
                    "clears=%llu "
                    "presents=%llu submits=%llu fallback=%llu resident=%llu/%llu "
                    "residency-fail=%llu mirror=%llu/%llu pso=%llu/%llu "
                    "rt=%llu/%llu depth=%llu/%llu "
                    "timeline=%d/%llu/%llu/%llu]\n",
                    d3d_stats.draws,
                    d3d_stats.conditional_draws_skipped,
                    d3d_stats.draw_batches,
                    legacy_draw_groups,
                    native_draw_coverage_ppm,
                    d3d_stats.clears, d3d_stats.presents,
                    d3d_stats.queue_submissions,
                    d3d_stats.unsupported_draws,
                    d3d_stats.resident_pages[0],
                    d3d_stats.resident_pages[1],
                    d3d_stats.residency_failures,
                    d3d_stats.mirror_resyncs,
                    d3d_stats.mirror_rollovers,
                    d3d_stats.pso_hits, d3d_stats.pso_builds,
                    d3d_stats.rt_builds, d3d_stats.rt_refreshes,
                    d3d_stats.depth_builds, d3d_stats.depth_refreshes,
                    rsx_nr_d3d12_shared_timeline_enabled(g_active.d3d12),
                    d3d_stats.shared_timeline_acquires,
                    d3d_stats.shared_timeline_generations,
                    d3d_stats.shared_timeline_forced_submissions);
            rsx_nr_d3d12_destroy(g_active.d3d12);
            g_active.d3d12 = nullptr;
            InterlockedExchange(&g_active.shared_timeline, 0);
        }
        free(const_cast<LONG*>(g_active.guest_page_route));
        g_active.guest_page_route = nullptr;
        free(g_active.section_ops);
        free(g_active.section_side);
        free(g_active.section_methods);
        free(g_active.section_adapter);
        g_active.section_ops = nullptr;
        g_active.section_side = nullptr;
        g_active.section_methods = nullptr;
        g_active.section_adapter = nullptr;
        rsx_nr_ring_destroy(&g_active.ring);
        rsx_nr_span_router_destroy(&g_active.router);
    }

    if (!InterlockedExchange(&g_vertical.mode_shadow, 0))
        return;

    AcquireSRWLockExclusive(&g_vertical.lock);
    const yz_nr_vertical_lane expected = g_vertical.expected;
    const yz_nr_vertical_lane observed = g_vertical.observed;
    yz_nr_vertical_source sources[YZ_NR_VERT_SOURCE_COUNT];
    memcpy(sources, g_vertical.sources, sizeof(sources));
    const unsigned long long source_overflow = g_vertical.source_overflow;
    unsigned long long exact_outstanding = 0;
    unsigned long long outstanding_by_family[YZ_NR_VERT_FAMILY_COUNT] = {};
    for (uint32_t i = 0; i < YZ_NR_VERT_EVENT_COUNT; ++i) {
        if (g_vertical.events[i].state == YZ_NR_EVENT_READY) {
            exact_outstanding++;
            const uint32_t family = g_vertical.events[i].family;
            if (family < YZ_NR_VERT_FAMILY_COUNT)
                outstanding_by_family[family]++;
        }
    }
    const unsigned long long exact_matches = g_vertical.exact_matches;
    const unsigned long long exact_mismatches = g_vertical.exact_mismatches;
    const unsigned long long exact_unexpected = g_vertical.exact_unexpected;
    const unsigned long long exact_unaddressed = g_vertical.exact_unaddressed;
    const unsigned long long exact_overflow = g_vertical.exact_overflow;
    const unsigned long long state_unowned_methods =
        g_vertical.state_unowned_methods;
    const unsigned long long draw_unowned = g_vertical.draw_unowned;
    const unsigned long long draw_unsupported = g_vertical.draw_unsupported;
    const unsigned long long draw_bad_sequence =
        g_vertical.draw_bad_sequence;
    const unsigned long long flip_unowned = g_vertical.flip_unowned;
    const unsigned long long flip_bad_sequence =
        g_vertical.flip_bad_sequence;
    const unsigned long long vp_bad_sequence =
        g_vertical.vp_bad_sequence;
    unsigned long long vp_template_unique = 0;
    unsigned long long vp_template_seen = 0;
    for (uint32_t i = 0; i < YZ_NR_VERT_VP_TEMPLATE_COUNT; ++i) {
        if (g_vertical.vp_templates[i].build_count) {
            vp_template_unique++;
            if (g_vertical.vp_templates[i].replay_count)
                vp_template_seen++;
        }
    }
    const unsigned long long vp_template_builds =
        g_vertical.vp_template_builds;
    const unsigned long long vp_template_replays =
        g_vertical.vp_template_replays;
    const unsigned long long vp_template_mask_replays =
        g_vertical.vp_template_mask_replays;
    const unsigned long long vp_template_unknown =
        g_vertical.vp_template_unknown;
    const unsigned long long vp_template_overflow =
        g_vertical.vp_template_overflow;
    const unsigned long long vp_unknown_overflow =
        g_vertical.vp_unknown_overflow;
    unsigned long long fp_template_unique = 0;
    unsigned long long fp_template_seen = 0;
    for (uint32_t i = 0; i < YZ_NR_VERT_FP_TEMPLATE_COUNT; ++i) {
        if (g_vertical.fp_templates[i].build_count) {
            fp_template_unique++;
            if (g_vertical.fp_templates[i].replay_count)
                fp_template_seen++;
        }
    }
    const unsigned long long fp_template_builds =
        g_vertical.fp_template_builds;
    const unsigned long long fp_template_replays =
        g_vertical.fp_template_replays;
    const unsigned long long fp_template_parameter_replays =
        g_vertical.fp_template_parameter_replays;
    const unsigned long long fp_template_unknown =
        g_vertical.fp_template_unknown;
    const unsigned long long fp_template_overflow =
        g_vertical.fp_template_overflow;
    const unsigned long long fp_unknown_overflow =
        g_vertical.fp_unknown_overflow;
    const unsigned long long fp_unresolved = g_vertical.fp_unresolved;
    const unsigned long long fp_bad_sequence = g_vertical.fp_bad_sequence;
    uint32_t observed_ea_min[YZ_NR_VERT_FAMILY_COUNT];
    uint32_t observed_ea_max[YZ_NR_VERT_FAMILY_COUNT];
    memcpy(observed_ea_min, g_vertical.observed_ea_min,
           sizeof(observed_ea_min));
    memcpy(observed_ea_max, g_vertical.observed_ea_max,
           sizeof(observed_ea_max));
    ReleaseSRWLockExclusive(&g_vertical.lock);

    unsigned mismatches = 0;
    for (uint32_t family = 1; family < YZ_NR_VERT_FAMILY_COUNT; ++family) {
        if (expected.count[family] != observed.count[family] +
                                          outstanding_by_family[family])
            mismatches++;
    }
    const bool ref_ok = expected.count[YZ_NR_VERT_REFERENCE] ==
                            observed.count[YZ_NR_VERT_REFERENCE] +
                            outstanding_by_family[YZ_NR_VERT_REFERENCE];
    const bool acquire_ok = expected.count[YZ_NR_VERT_ACQUIRE] ==
                                observed.count[YZ_NR_VERT_ACQUIRE] +
                                outstanding_by_family[YZ_NR_VERT_ACQUIRE];
    const bool user_ok = expected.count[YZ_NR_VERT_USER] ==
                             observed.count[YZ_NR_VERT_USER] +
                             outstanding_by_family[YZ_NR_VERT_USER];
    const bool state_ok = expected.count[YZ_NR_VERT_STATE_DIRECT] ==
                              observed.count[YZ_NR_VERT_STATE_DIRECT] +
                              outstanding_by_family[YZ_NR_VERT_STATE_DIRECT];
    const bool draw_ok = expected.count[YZ_NR_VERT_DRAW_ARRAYS] ==
                             observed.count[YZ_NR_VERT_DRAW_ARRAYS] +
                             outstanding_by_family[YZ_NR_VERT_DRAW_ARRAYS];
    const bool flip_ok = expected.count[YZ_NR_VERT_FLIP] ==
                             observed.count[YZ_NR_VERT_FLIP] +
                             outstanding_by_family[YZ_NR_VERT_FLIP];
    const bool vp_ok = vp_template_builds != 0 &&
                       vp_template_replays != 0 &&
                       vp_template_unknown == 0 &&
                       vp_template_overflow == 0 &&
                       vp_bad_sequence == 0;
    const bool fp_ok = fp_template_builds != 0 &&
                       fp_template_replays != 0 &&
                       fp_template_unknown == 0 &&
                       fp_template_overflow == 0 &&
                       fp_unknown_overflow == 0 &&
                       fp_unresolved == 0 &&
                       fp_bad_sequence == 0;
    unsigned sequence_mismatches = 0;
    for (uint32_t family = 1; family < YZ_NR_VERT_FAMILY_COUNT; ++family) {
        if (!outstanding_by_family[family] &&
            expected.ordered_hash[family] != observed.ordered_hash[family])
            sequence_mismatches++;
    }
    fprintf(stderr,
            "[nr-vertical-shadow ref=%llu/%llu+%llu:%s "
            "acq=%llu/%llu+%llu:%s user=%llu/%llu+%llu:%s "
            "state=%llu/%llu+%llu:%s state-unowned=%llu "
            "draw=%llu/%llu+%llu:%s draw-unowned=%llu "
            "draw-unsupported=%llu draw-seq=%llu "
            "flip=%llu/%llu+%llu:%s flip-unowned=%llu flip-seq=%llu "
            "vp-builds=%llu templates=%llu seen=%llu replays=%llu "
            "mask-variants=%llu "
            "unknown=%llu overflow=%llu/%llu:%s vp-seq=%llu "
            "fp-builds=%llu templates=%llu seen=%llu replays=%llu "
            "param-variants=%llu unknown=%llu overflow=%llu/%llu "
            "unresolved=%llu:%s fp-seq=%llu "
            "mismatch=%u seqdiff=%u]\n",
            expected.count[YZ_NR_VERT_REFERENCE],
            observed.count[YZ_NR_VERT_REFERENCE],
            outstanding_by_family[YZ_NR_VERT_REFERENCE],
            ref_ok ? "ok" : "bad",
            expected.count[YZ_NR_VERT_ACQUIRE],
            observed.count[YZ_NR_VERT_ACQUIRE],
            outstanding_by_family[YZ_NR_VERT_ACQUIRE],
            acquire_ok ? "ok" : "bad",
            expected.count[YZ_NR_VERT_USER],
            observed.count[YZ_NR_VERT_USER],
            outstanding_by_family[YZ_NR_VERT_USER],
            user_ok ? "ok" : "bad",
            expected.count[YZ_NR_VERT_STATE_DIRECT],
            observed.count[YZ_NR_VERT_STATE_DIRECT],
            outstanding_by_family[YZ_NR_VERT_STATE_DIRECT],
            state_ok ? "ok" : "bad", state_unowned_methods,
            expected.count[YZ_NR_VERT_DRAW_ARRAYS],
            observed.count[YZ_NR_VERT_DRAW_ARRAYS],
            outstanding_by_family[YZ_NR_VERT_DRAW_ARRAYS],
            draw_ok ? "ok" : "bad", draw_unowned, draw_unsupported,
            draw_bad_sequence,
            expected.count[YZ_NR_VERT_FLIP],
            observed.count[YZ_NR_VERT_FLIP],
            outstanding_by_family[YZ_NR_VERT_FLIP],
            flip_ok ? "ok" : "bad", flip_unowned, flip_bad_sequence,
            vp_template_builds, vp_template_unique, vp_template_seen,
            vp_template_replays, vp_template_mask_replays,
            vp_template_unknown,
            vp_template_overflow, vp_unknown_overflow,
            vp_ok ? "ok" : "bad",
            vp_bad_sequence,
            fp_template_builds, fp_template_unique, fp_template_seen,
            fp_template_replays, fp_template_parameter_replays,
            fp_template_unknown, fp_template_overflow,
            fp_unknown_overflow, fp_unresolved, fp_ok ? "ok" : "bad",
            fp_bad_sequence,
            mismatches, sequence_mismatches);
    /* Compact fixed-memory identity census.  These are semantic hashes of
     * complete uploads, not packet addresses or per-event logs. */
    bool selected_known[YZ_NR_VERT_VP_TEMPLATE_COUNT] = {};
    bool selected_unknown[YZ_NR_VERT_VP_TEMPLATE_COUNT] = {};
    for (uint32_t rank = 0; rank < 16u; ++rank) {
        uint32_t best = YZ_NR_VERT_VP_TEMPLATE_COUNT;
        for (uint32_t i = 0; i < YZ_NR_VERT_VP_TEMPLATE_COUNT; ++i) {
            if (selected_known[i] || !g_vertical.vp_templates[i].replay_count)
                continue;
            if (best == YZ_NR_VERT_VP_TEMPLATE_COUNT ||
                g_vertical.vp_templates[i].replay_count >
                    g_vertical.vp_templates[best].replay_count)
                best = i;
        }
        if (best == YZ_NR_VERT_VP_TEMPLATE_COUNT)
            break;
        selected_known[best] = true;
        const yz_nr_vertical_vp_template* const entry =
            &g_vertical.vp_templates[best];
        fprintf(stderr,
                "[nr-vertical-vp-known rank=%u start=%u code=%08X "
                "words=%u mask=%08X hash=%08X builds=%llu replays=%llu]\n",
                rank + 1u, entry->start_slot, entry->code_hash,
                entry->word_count, entry->input_mask, entry->semantic_hash,
                entry->build_count, entry->replay_count);
    }
    for (uint32_t rank = 0; rank < 16u; ++rank) {
        uint32_t best = YZ_NR_VERT_VP_TEMPLATE_COUNT;
        for (uint32_t i = 0; i < YZ_NR_VERT_VP_TEMPLATE_COUNT; ++i) {
            if (selected_unknown[i] ||
                !g_vertical.vp_unknown_templates[i].replay_count)
                continue;
            if (best == YZ_NR_VERT_VP_TEMPLATE_COUNT ||
                g_vertical.vp_unknown_templates[i].replay_count >
                    g_vertical.vp_unknown_templates[best].replay_count)
                best = i;
        }
        if (best == YZ_NR_VERT_VP_TEMPLATE_COUNT)
            break;
        selected_unknown[best] = true;
        const yz_nr_vertical_vp_template* const entry =
            &g_vertical.vp_unknown_templates[best];
        fprintf(stderr,
                "[nr-vertical-vp-unknown rank=%u start=%u code=%08X "
                "words=%u mask=%08X hash=%08X replays=%llu]\n",
                rank + 1u, entry->start_slot, entry->code_hash,
                entry->word_count, entry->input_mask, entry->semantic_hash,
                entry->replay_count);
    }
    bool selected_fp_known[YZ_NR_VERT_FP_TEMPLATE_COUNT] = {};
    bool selected_fp_unknown[YZ_NR_VERT_FP_TEMPLATE_COUNT] = {};
    for (uint32_t rank = 0; rank < 16u; ++rank) {
        uint32_t best = YZ_NR_VERT_FP_TEMPLATE_COUNT;
        for (uint32_t i = 0; i < YZ_NR_VERT_FP_TEMPLATE_COUNT; ++i) {
            if (selected_fp_known[i] ||
                !g_vertical.fp_templates[i].replay_count)
                continue;
            if (best == YZ_NR_VERT_FP_TEMPLATE_COUNT ||
                g_vertical.fp_templates[i].replay_count >
                    g_vertical.fp_templates[best].replay_count)
                best = i;
        }
        if (best == YZ_NR_VERT_FP_TEMPLATE_COUNT)
            break;
        selected_fp_known[best] = true;
        const yz_nr_vertical_fp_template* const entry =
            &g_vertical.fp_templates[best];
        fprintf(stderr,
                "[nr-vertical-fp-known rank=%u bytes=%u ctrl=%08X "
                "content=%016llX structural=%016llX builds=%llu "
                "replays=%llu params=%llu]\n",
                rank + 1u, entry->byte_count, entry->control,
                entry->content_hash, entry->structural_hash,
                entry->build_count, entry->replay_count,
                entry->parameter_replay_count);
    }
    for (uint32_t rank = 0; rank < 16u; ++rank) {
        uint32_t best = YZ_NR_VERT_FP_TEMPLATE_COUNT;
        for (uint32_t i = 0; i < YZ_NR_VERT_FP_TEMPLATE_COUNT; ++i) {
            if (selected_fp_unknown[i] ||
                !g_vertical.fp_unknown_templates[i].replay_count)
                continue;
            if (best == YZ_NR_VERT_FP_TEMPLATE_COUNT ||
                g_vertical.fp_unknown_templates[i].replay_count >
                    g_vertical.fp_unknown_templates[best].replay_count)
                best = i;
        }
        if (best == YZ_NR_VERT_FP_TEMPLATE_COUNT)
            break;
        selected_fp_unknown[best] = true;
        const yz_nr_vertical_fp_template* const entry =
            &g_vertical.fp_unknown_templates[best];
        fprintf(stderr,
                "[nr-vertical-fp-unknown rank=%u bytes=%u ctrl=%08X "
                "content=%016llX structural=%016llX replays=%llu]\n",
                rank + 1u, entry->byte_count, entry->control,
                entry->content_hash, entry->structural_hash,
                entry->replay_count);
    }
    for (uint32_t i = 0; i < YZ_NR_VERT_SOURCE_COUNT; ++i) {
        if (!sources[i].count)
            continue;
        fprintf(stderr,
                "[nr-vertical-source ctx=%08X current=%08X-%08X "
                "families=%X count=%llu]\n",
                sources[i].context, sources[i].current_min,
                sources[i].current_max, sources[i].family_mask,
                sources[i].count);
    }
    if (source_overflow)
        fprintf(stderr, "[nr-vertical-source overflow=%llu]\n",
                source_overflow);
    fprintf(stderr,
            "[nr-vertical-observed-ea ref=%08X-%08X "
            "acq=%08X-%08X user=%08X-%08X state=%08X-%08X "
            "draw=%08X-%08X flip=%08X-%08X vp=%08X-%08X "
            "fp=%08X-%08X]\n",
            observed_ea_min[YZ_NR_VERT_REFERENCE],
            observed_ea_max[YZ_NR_VERT_REFERENCE],
            observed_ea_min[YZ_NR_VERT_ACQUIRE],
            observed_ea_max[YZ_NR_VERT_ACQUIRE],
            observed_ea_min[YZ_NR_VERT_USER],
            observed_ea_max[YZ_NR_VERT_USER],
            observed_ea_min[YZ_NR_VERT_STATE_DIRECT],
            observed_ea_max[YZ_NR_VERT_STATE_DIRECT],
            observed_ea_min[YZ_NR_VERT_DRAW_ARRAYS],
            observed_ea_max[YZ_NR_VERT_DRAW_ARRAYS],
            observed_ea_min[YZ_NR_VERT_FLIP],
            observed_ea_max[YZ_NR_VERT_FLIP],
            observed_ea_min[YZ_NR_VERT_VERTEX_PROGRAM],
            observed_ea_max[YZ_NR_VERT_VERTEX_PROGRAM],
            observed_ea_min[YZ_NR_VERT_FRAGMENT_PROGRAM],
            observed_ea_max[YZ_NR_VERT_FRAGMENT_PROGRAM]);
    fprintf(stderr,
            "[nr-vertical-exact matched=%llu mismatch=%llu unexpected=%llu "
            "outstanding=%llu unaddressed=%llu overflow=%llu]\n",
            exact_matches, exact_mismatches, exact_unexpected,
            exact_outstanding, exact_unaddressed, exact_overflow);
}

/* Highest-safe producer wrappers. Shadow mode observes the typed operation
 * before packet construction, then the unchanged lifted implementation owns
 * guest context advancement, overflow callbacks and FIFO publication. */
void func_00EBC034(ppu_context* ctx)
{
    yz_nr_expected_from(ctx, YZ_NR_VERT_REFERENCE,
                        (uint32_t)ctx->gpr[4], 0, 8);
    if (!yz_nr_active_publish(ctx, YZ_NR_VERT_REFERENCE,
                              (uint32_t)ctx->gpr[4]))
        func_00EBC034_lifted(ctx);
}

void func_00EBC330(ppu_context* ctx)
{
    yz_nr_expected_from(ctx, YZ_NR_VERT_ACQUIRE,
                        ((uint32_t)ctx->gpr[4] << 4) & 0xFF0u,
                        (uint32_t)ctx->gpr[5], 16);
    func_00EBC330_lifted(ctx);
}

void func_00EBD6FC(ppu_context* ctx)
{
    yz_nr_expected_from(ctx, YZ_NR_VERT_USER,
                        (uint32_t)ctx->gpr[4], 0, 8);
    if (!yz_nr_active_publish(ctx, YZ_NR_VERT_USER,
                              (uint32_t)ctx->gpr[4]))
        func_00EBD6FC_lifted(ctx);
}

/* Highest complete fragment-package producer. The active design will derive
 * a typed shader template from the package ABI before invoking legacy code.
 * Shadow mode currently lets the builder and all of its continuations finish,
 * then validates that its relocated packet segment names the same exact and
 * structural fragment-program identities. This postcondition scan is an
 * oracle only and is never part of native execution. */
void func_00EB0D90(ppu_context* ctx)
{
    const uint32_t output = (uint32_t)ctx->gpr[3];
    func_00EB0D90_lifted(ctx);
    yz_drain_trampolines(ctx);
    yz_nr_expected_fragment_program(output);
}

/* Complete transform-program producer ABI entry. Passive mode derives the
 * typed upload from the descriptor and ucode arguments before the lifted
 * SDK body constructs LOAD/START/upload/mask packets. */
void func_00EBD92C(ppu_context* ctx)
{
    yz_nr_expected_vertex_program(ctx);
    func_00EBD92C_lifted(ctx);
}

/* Thin DrawArrays producer: active graphics publishes a typed action plus a
 * byte-exact complete fallback packet before advancing the command context.
 * Any readiness, range, router, or backend refusal leaves the lifted wrapper
 * as the sole producer. */
void func_00EBEA48(ppu_context* ctx)
{
    yz_nr_expected_draw_arrays(ctx);
    if (!yz_nr_active_publish_draw_arrays(ctx))
        func_00EBEA48_lifted(ctx);
}

/* Direct (context,value) state setters.  Shadow mode records a typed
 * compile-time method contract before packet construction.  The unchanged
 * lifted wrapper remains the sole executor in both OFF and shadow modes.
 * These wrappers are deliberately not part of active-basic yet. */
#define YZ_NR_DIRECT_SETTER(address)                                      \
    void func_##address(ppu_context* ctx)                                 \
    {                                                                     \
        yz_nr_expected_direct_setter(ctx, 0x##address##u);                 \
        func_##address##_lifted(ctx);                                     \
    }

YZ_NR_DIRECT_SETTER(00EBC488)
YZ_NR_DIRECT_SETTER(00EBC51C)
YZ_NR_DIRECT_SETTER(00EBC664)
YZ_NR_DIRECT_SETTER(00EBC6F8)
YZ_NR_DIRECT_SETTER(00EBC790)
YZ_NR_DIRECT_SETTER(00EBC824)
YZ_NR_DIRECT_SETTER(00EBC8B8)
YZ_NR_DIRECT_SETTER(00EBCA00)
YZ_NR_DIRECT_SETTER(00EBCA94)
YZ_NR_DIRECT_SETTER(00EBCB28)
YZ_NR_DIRECT_SETTER(00EBCBBC)
YZ_NR_DIRECT_SETTER(00EBCC50)
YZ_NR_DIRECT_SETTER(00EBCCE4)
YZ_NR_DIRECT_SETTER(00EBCD78)
YZ_NR_DIRECT_SETTER(00EBCE0C)
YZ_NR_DIRECT_SETTER(00EBCEA0)
YZ_NR_DIRECT_SETTER(00EBCF34)
YZ_NR_DIRECT_SETTER(00EBCFC8)
YZ_NR_DIRECT_SETTER(00EBD05C)
YZ_NR_DIRECT_SETTER(00EBD0F4)
YZ_NR_DIRECT_SETTER(00EBD188)
YZ_NR_DIRECT_SETTER(00EBD220)
YZ_NR_DIRECT_SETTER(00EBD3F8)
YZ_NR_DIRECT_SETTER(00EBD48C)
YZ_NR_DIRECT_SETTER(00EBD5CC)

#undef YZ_NR_DIRECT_SETTER
