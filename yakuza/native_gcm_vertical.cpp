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
#include "rsx_nr_producer_contract.h"
#include "rsx_nr_span_router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

enum : uint32_t {
    YZ_NR_VERT_REFERENCE = 1,
    YZ_NR_VERT_ACQUIRE = 2,
    YZ_NR_VERT_USER = 3,
    YZ_NR_VERT_STATE_DIRECT = 4,
    YZ_NR_VERT_FAMILY_COUNT = 5,
    YZ_NR_VERT_SOURCE_COUNT = 16,
    YZ_NR_VERT_EVENT_COUNT = 8192,
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
    uint32_t fifo_semaphore_offset;
    uint32_t fifo_semaphore_packet_ea;
    volatile LONG mode_shadow;
    volatile LONG mode_active_basic;
    volatile LONG initialized;
};

static yz_nr_vertical_state g_vertical = {SRWLOCK_INIT};

enum : uint32_t {
    YZ_NR_ACTIVE_ROUTER_CAPACITY = 8192,
    YZ_NR_ACTIVE_RING_CAPACITY = 16,
    YZ_NR_ACTIVE_SIDE_CAPACITY = 64,
};

struct yz_nr_vertical_active_state {
    SRWLOCK producer_lock;
    rsx_nr_span_router router;
    rsx_nr_ring ring;
    rsx_nr_tokens tokens;
    rsx_nr_backend backend;
    rsx_nr_slot slots[YZ_NR_ACTIVE_RING_CAPACITY];
    uint32_t side[YZ_NR_ACTIVE_SIDE_CAPACITY];
    unsigned long long owned[YZ_NR_VERT_FAMILY_COUNT];
    unsigned long long fallback[YZ_NR_VERT_FAMILY_COUNT];
    unsigned long long executed[YZ_NR_VERT_FAMILY_COUNT];
    unsigned long long wait;
    unsigned long long fatal;
    unsigned long long wrong_context;
    unsigned long long no_room;
    unsigned long long publish_failure;
    uint32_t last_miss_ea;
    uint32_t last_miss_epoch;
};

static yz_nr_vertical_active_state g_active = {SRWLOCK_INIT};

extern "C" void yz_nr_vertical_exec_set_reference(uint32_t value);
extern "C" void yz_nr_vertical_exec_user_command(uint32_t cause);

static void yz_nr_exec_reference(void*, uint32_t value)
{
    yz_nr_vertical_exec_set_reference(value);
}

static void yz_nr_exec_user(void*, uint32_t cause)
{
    yz_nr_vertical_exec_user_command(cause);
}

static int yz_nr_active_init(void)
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
    rsx_nr_exec_ops ops = {};
    ops.set_reference = yz_nr_exec_reference;
    ops.user_command = yz_nr_exec_user;
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
            free_slot = YZ_NR_VERT_SOURCE_COUNT;
            break;
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
        bool found = false;
        for (uint32_t i = 0; i < YZ_NR_VERT_SOURCE_COUNT; ++i) {
            if (g_vertical.sources[i].context == context) {
                found = true;
                break;
            }
        }
        if (!found)
            g_vertical.source_overflow++;
    }
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

} // namespace

extern "C" void yz_nr_vertical_init(void)
{
    if (InterlockedExchange(&g_vertical.initialized, 1))
        return;
    const char* const mode = getenv("YZ_NR_VERTICAL");
    if (mode && strcmp(mode, "shadow") == 0)
        InterlockedExchange(&g_vertical.mode_shadow, 1);
    else if (mode && strcmp(mode, "active-basic") == 0) {
        if (yz_nr_active_init())
            InterlockedExchange(&g_vertical.mode_active_basic, 1);
        else {
            fprintf(stderr,
                    "[nr-vertical-active init=failed; legacy fallback only]\n");
            fflush(stderr);
        }
    }
}

extern "C" void yz_nr_vertical_observe_method(uint32_t method, uint32_t arg,
                                                uint32_t packet_ea)
{
    if (!InterlockedCompareExchange(&g_vertical.mode_shadow, 0, 0))
        return;

    /* yz_rsx_method receives subchannel-flattened methods. NV406E methods
     * remain in the low window; the game user-command method is 0xEB00/04. */
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

    const uint32_t epoch =
        rsx_nr_span_router_publication_epoch(&g_active.router);
    if (g_active.last_miss_ea == packet_ea &&
        g_active.last_miss_epoch == epoch) {
        return YZ_NR_VERTICAL_CONSUME_MISS;
    }

    rsx_nr_span span = {};
    const rsx_nr_span_take_result take =
        rsx_nr_span_router_take(&g_active.router, packet_ea, &span);
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
    if (take != RSX_NR_SPAN_TAKE_CLAIMED ||
        span.payload.side_count != 0 ||
        !rsx_nr_ring_can_accept(&g_active.ring,
                                span.payload.op_count, 0)) {
        g_active.fatal++;
        return YZ_NR_VERTICAL_CONSUME_FATAL;
    }

    const unsigned long long errors_before = g_active.backend.stats.exec_errors;
    for (uint32_t i = 0; i < span.payload.op_count; ++i) {
        if (rsx_nr_ring_push(&g_active.ring, &span.payload.ops[i]) != 0) {
            g_active.fatal++;
            return YZ_NR_VERTICAL_CONSUME_FATAL;
        }
    }
    const uint32_t executed =
        rsx_nr_backend_run(&g_active.backend, span.payload.op_count);
    if (executed != span.payload.op_count ||
        rsx_nr_ring_depth(&g_active.ring) != 0 ||
        g_active.backend.stats.exec_errors != errors_before) {
        g_active.fatal++;
        return YZ_NR_VERTICAL_CONSUME_FATAL;
    }

    for (uint32_t i = 0; i < span.payload.op_count; ++i) {
        const uint32_t kind = span.payload.ops[i].kind;
        if (kind == RSX_NIR_OP_SET_REFERENCE)
            g_active.executed[YZ_NR_VERT_REFERENCE]++;
        else if (kind == RSX_NIR_OP_USER_COMMAND)
            g_active.executed[YZ_NR_VERT_USER]++;
    }
    g_active.last_miss_ea = ~0u;
    g_active.last_miss_epoch = 0;
    if (word_count)
        *word_count = span.word_count;
    return YZ_NR_VERTICAL_CONSUME_EXECUTED;
}

extern "C" void yz_nr_vertical_shutdown(void)
{
    if (InterlockedExchange(&g_vertical.mode_active_basic, 0)) {
        rsx_nr_span_router_stats stats = {};
        rsx_nr_span_router_get_stats(&g_active.router, &stats);
        fprintf(stderr,
                "[nr-vertical-active "
                "ref=%llu/%llu user=%llu/%llu "
                "fallback-ref=%llu fallback-user=%llu "
                "wrong-context=%llu no-room=%llu publish-fail=%llu "
                "wait=%llu fatal=%llu depth=%u errors=%llu]\n",
                g_active.owned[YZ_NR_VERT_REFERENCE],
                g_active.executed[YZ_NR_VERT_REFERENCE],
                g_active.owned[YZ_NR_VERT_USER],
                g_active.executed[YZ_NR_VERT_USER],
                g_active.fallback[YZ_NR_VERT_REFERENCE],
                g_active.fallback[YZ_NR_VERT_USER],
                g_active.wrong_context, g_active.no_room,
                g_active.publish_failure, g_active.wait, g_active.fatal,
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
            mismatches, sequence_mismatches);
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
            "acq=%08X-%08X user=%08X-%08X state=%08X-%08X]\n",
            observed_ea_min[YZ_NR_VERT_REFERENCE],
            observed_ea_max[YZ_NR_VERT_REFERENCE],
            observed_ea_min[YZ_NR_VERT_ACQUIRE],
            observed_ea_max[YZ_NR_VERT_ACQUIRE],
            observed_ea_min[YZ_NR_VERT_USER],
            observed_ea_max[YZ_NR_VERT_USER],
            observed_ea_min[YZ_NR_VERT_STATE_DIRECT],
            observed_ea_max[YZ_NR_VERT_STATE_DIRECT]);
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
