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
};

struct yz_nr_vertical_display {
    uint32_t location, offset, width, height;
    uint32_t valid;
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
    volatile LONG renderer_owner; /* 0 legacy/unknown, 1 native/shared */
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
    unsigned long long consumer_transfer_owned;
    unsigned long long consumer_transfer_fallback;
    unsigned long long consumer_sync_owned;
    unsigned long long consumer_sync_fallback;
    rsx_nr_span pending_span;
    rsx_nr_span_claim pending_claim;
    uint32_t pending_executed;
    uint32_t pending_expected;
    uint32_t pending_valid;
    uint32_t last_miss_ea;
    uint32_t last_miss_epoch;
};

static yz_nr_vertical_active_state g_active = {SRWLOCK_INIT};

extern "C" void yz_nr_vertical_exec_set_reference(uint32_t value);
extern "C" void yz_nr_vertical_exec_user_command(uint32_t cause);
extern "C" void yz_nr_vertical_exec_present(uint32_t buffer_id);
extern "C" void yz_nr_vertical_exec_present_complete(uint32_t buffer_id);
extern "C" int yz_nr_vertical_sem_read(uint32_t dma, uint32_t offset,
                                        uint32_t* value);
extern "C" void yz_nr_vertical_sem_write(uint32_t dma, uint32_t offset,
                                           uint32_t value,
                                           uint32_t texture_read);

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
    const int result = rsx_live_draw_present_external(
        texture, format, width, height, buffer_id);
    if (!result)
        yz_nr_vertical_exec_present_complete(buffer_id);
    return result;
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
    if (!g_active.guest_page_route || space > 1u ||
        (page_offset & 0xFFFu))
        return -1;
    uint32_t ea = 0;
    if (yz_nr_vertical_space_page_to_ea(space, page_offset, &ea) != 0 ||
        (ea & 0xFFFu))
        return -1;
    const uint32_t ea_page = ea >> 12;
    const LONG encoded = (LONG)(((space + 1u) << 28) |
                                (page_offset >> 12));
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
    if (InterlockedExchange(&g_active.renderer_owner, 1) == 0)
        rsx_live_draw_flush();
    const int result = g_active.gpu_ops.clear
        ? g_active.gpu_ops.clear(g_active.gpu_ops.user, st, clear) : -1;
    if (result)
        InterlockedExchange(&g_active.renderer_owner, 0);
    return result;
}

static int yz_nr_gpu_draw(void*, const rsx_nir_pipeline* st,
                          const uint32_t* vp, uint32_t vp_words,
                          const rsx_nir_draw* draw, const uint32_t* batches)
{
    if (InterlockedExchange(&g_active.renderer_owner, 1) == 0)
        rsx_live_draw_flush();
    const int result = g_active.gpu_ops.draw
        ? g_active.gpu_ops.draw(g_active.gpu_ops.user, st, vp, vp_words,
                                draw, batches) : -1;
    if (result)
        InterlockedExchange(&g_active.renderer_owner, 0);
    return result;
}

static int yz_nr_gpu_transfer(void*, const rsx_nir_pipeline* st,
                              const rsx_nir_transfer* transfer,
                              const uint32_t* words)
{
    if (InterlockedExchange(&g_active.renderer_owner, 1) == 0)
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
                              uint32_t* dsv_format, uint32_t* srv_format)
{
    return rsx_live_draw_borrow_depth(
        space, offset, depth_format, width, height, resource,
        resource_format, dsv_format, srv_format);
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
        d3d12, yz_nr_borrow_color, yz_nr_borrow_depth, nullptr);
    rsx_nr_d3d12_set_publish_write(
        d3d12, yz_nr_d3d_publish_write, nullptr);
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
        !InterlockedCompareExchange(&g_active.graphics_ready, 0, 0))
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
        if (yz_nr_active_init(graphics))
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
    if (!is_draw && !is_clear && !is_transfer && !is_sync)
        return 0;
    const auto note_fallback = [&]() {
        if (is_draw)
            g_active.consumer_draw_fallback++;
        else if (is_clear)
            g_active.consumer_clear_fallback++;
        else if (is_transfer)
            g_active.consumer_transfer_fallback++;
        else
            g_active.consumer_sync_fallback++;
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
    } else {
        g_active.consumer_sync_owned++;
    }
    return 1;
}

extern "C" void yz_nr_vertical_prepare_legacy_method(uint32_t method,
                                                        uint32_t arg)
{
    if (!InterlockedCompareExchange(
            &g_vertical.mode_active_graphics, 0, 0))
        return;
    const bool action =
        (method == 0x1808u && arg == 0u) || method == 0x1D94u ||
        method == 0x2328u || method == 0xC40Cu ||
        method == 0x0050u || method == 0x006Cu || method == 0x0110u ||
        method == 0x17C8u || method == 0x1800u ||
        method == 0x1D70u || method == 0x1D74u ||
        (method >= 0xE920u && method <= 0xE95Cu);
    if (!action)
        return;
    if (InterlockedExchange(&g_active.renderer_owner, 0) == 1 &&
        g_active.gpu_ops.flush)
        g_active.gpu_ops.flush(g_active.gpu_ops.user);
}

extern "C" void yz_nr_vertical_observe_method(uint32_t method, uint32_t arg,
                                                uint32_t packet_ea)
{
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
                "[nr-vertical-active "
                "ref=%llu/%llu user=%llu/%llu draw=%llu/%llu "
                "flip=%llu/%llu fallback-ref=%llu fallback-user=%llu "
                "fallback-draw=%llu fallback-flip=%llu "
                "wrong-context=%llu no-room=%llu publish-fail=%llu "
                "consumer-draw=%llu/%llu "
                "consumer-clear=%llu/%llu "
                "consumer-transfer=%llu/%llu "
                "consumer-sync=%llu/%llu "
                "wait=%llu late-fallback=%llu fatal=%llu "
                "depth=%u errors=%llu]\n",
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
                g_active.consumer_transfer_owned,
                g_active.consumer_transfer_fallback,
                g_active.consumer_sync_owned,
                g_active.consumer_sync_fallback, g_active.wait,
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
        fflush(stderr);
        if (g_active.d3d12) {
            rsx_nr_d3d12_stats d3d_stats = {};
            rsx_nr_d3d12_get_stats(g_active.d3d12, &d3d_stats);
            fprintf(stderr,
                    "[nr-vertical-d3d draws=%llu batches=%llu clears=%llu "
                    "presents=%llu submits=%llu fallback=%llu resident=%llu/%llu "
                    "residency-fail=%llu pso=%llu/%llu]\n",
                    d3d_stats.draws, d3d_stats.draw_batches,
                    d3d_stats.clears, d3d_stats.presents,
                    d3d_stats.queue_submissions,
                    d3d_stats.unsupported_draws,
                    d3d_stats.resident_pages[0],
                    d3d_stats.resident_pages[1],
                    d3d_stats.residency_failures,
                    d3d_stats.pso_hits, d3d_stats.pso_builds);
            rsx_nr_d3d12_destroy(g_active.d3d12);
            g_active.d3d12 = nullptr;
        }
        free(const_cast<LONG*>(g_active.guest_page_route));
        g_active.guest_page_route = nullptr;
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
