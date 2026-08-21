#include "ps3emu/yz_fe0_timeline.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

/* 65,536 records / 64 bytes = 4 MiB of fixed BSS, with no heap activity. */
#define YZ_FE0_TIMELINE_CAPACITY 65536u
#define YZ_FE0_TIMELINE_MASK (YZ_FE0_TIMELINE_CAPACITY - 1u)
#define YZ_FE0_BARRIER_FIRST_CAPACITY 64u
#define YZ_FE0_TASKSET_ATTEMPT_CAPACITY 128u

typedef struct yz_fe0_slot {
    _Atomic uint64_t published_sequence;
    yz_fe0_timeline_record record;
} yz_fe0_slot;

static yz_fe0_slot g_records[YZ_FE0_TIMELINE_CAPACITY];
static _Atomic uint64_t g_claimed;
static _Atomic uint32_t g_callback_cause;
static _Atomic uint32_t g_callback_epoch;
static _Atomic uint32_t g_callback_active;
static _Atomic uint32_t g_wkl4_barrier_ea;
static _Atomic uint32_t g_wkl4_taskset_ea;

typedef struct yz_fe0_barrier_first {
    _Atomic uint32_t published;
    uint32_t cause;
    uint32_t actor;
    uint32_t pc;
    uint32_t before_zero;
    uint32_t after_zero;
    uint32_t remained;
    uint32_t before_waiters;
    uint32_t after_waiters;
} yz_fe0_barrier_first;

/* The main timeline is deliberately a recent-history ring.  Preserve the
 * first few successful image-4 barrier transactions separately so a long
 * boot cannot hide an initialization/first-release error.  This diagnostic
 * uses fixed BSS, no clocks, allocation, or per-event I/O. */
static yz_fe0_barrier_first
    g_barrier_first[YZ_FE0_BARRIER_FIRST_CAPACITY];
static _Atomic uint32_t g_barrier_first_claimed;

typedef struct yz_fe0_taskset_attempt_first {
    _Atomic uint32_t published;
    uint32_t cause;
    uint32_t actor;
    uint32_t pc;
    uint32_t success;
    uint32_t old_signal;
    uint32_t new_signal;
    uint32_t old_waiting;
    uint32_t new_waiting;
    uint32_t enabled;
    uint32_t spurs_ea;
} yz_fe0_taskset_attempt_first;

/* Preserve the first SendSignal CAS attempts independently from the recent
 * timeline ring.  These samples use fixed BSS and no timestamps, allocation,
 * or per-event output. */
static yz_fe0_taskset_attempt_first
    g_taskset_attempt_first[YZ_FE0_TASKSET_ATTEMPT_CAPACITY];
static _Atomic uint32_t g_taskset_attempt_claimed;
static _Atomic uint32_t g_taskset_attempt_successes;
static _Atomic uint32_t g_taskset_attempt_changes;
static uint64_t g_frequency;
static int g_initialized;
static int g_shutdown;

typedef struct yz_fe0_acquire_state {
    int active;
    uint32_t context_dma;
    uint32_t offset;
    uint32_t address;
    uint32_t wanted;
} yz_fe0_acquire_state;

/* Only the RSX consumer touches this state. */
static yz_fe0_acquire_state g_acquire;

#if defined(YZ_FE0_TIMELINE_TEST)
static uint64_t g_test_clock;
static uint64_t g_test_clock_reads;
#endif

volatile long g_yz_fe0_timeline_enabled;

static uint32_t yz_fe0_thread_id(void)
{
#if defined(_WIN32)
    return (uint32_t)GetCurrentThreadId();
#else
    return (uint32_t)getpid();
#endif
}

static uint64_t yz_fe0_clock(void)
{
#if defined(YZ_FE0_TIMELINE_TEST)
    ++g_test_clock_reads;
    return g_test_clock;
#elif defined(_WIN32)
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
#else
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000000ull +
           (uint64_t)value.tv_nsec;
#endif
}

static int yz_fe0_exact_flag(const char* name)
{
    const char* value = getenv(name);
    return value && value[0] == '1' && value[1] == '\0';
}

int yz_fe0_timeline_init(void)
{
    if (g_initialized)
        return g_yz_fe0_timeline_enabled != 0;
    g_initialized = 1;
    g_shutdown = 0;
    memset(&g_acquire, 0, sizeof(g_acquire));
    atomic_store_explicit(&g_claimed, 0, memory_order_relaxed);
    atomic_store_explicit(&g_callback_active, 0, memory_order_relaxed);
    atomic_store_explicit(&g_wkl4_barrier_ea, 0, memory_order_relaxed);
    atomic_store_explicit(&g_wkl4_taskset_ea, 0, memory_order_relaxed);
    atomic_store_explicit(&g_barrier_first_claimed, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_taskset_attempt_claimed, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_taskset_attempt_successes, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_taskset_attempt_changes, 0,
                          memory_order_relaxed);
#if defined(_WIN32)
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        g_frequency = (uint64_t)frequency.QuadPart;
    }
#else
    g_frequency = 1000000000ull;
#endif
    if (!yz_fe0_exact_flag("YZ_FE0_TIMELINE"))
        return 0;
    g_yz_fe0_timeline_enabled = 1;
    return 1;
}

void yz_fe0_timeline_set_wkl4_barrier(uint32_t barrier_ea,
                                      uint32_t taskset_ea)
{
    if (!g_yz_fe0_timeline_enabled || (barrier_ea & 127u) || !taskset_ea)
        return;
    atomic_store_explicit(&g_wkl4_taskset_ea, taskset_ea,
                          memory_order_release);
    atomic_store_explicit(&g_wkl4_barrier_ea, barrier_ea,
                          memory_order_release);
}

static uint32_t yz_fe0_read_be32(const uint8_t* value)
{
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) | (uint32_t)value[3];
}

void yz_fe0_timeline_observe_wkl4_atomic(
    uint32_t image_id, uint32_t spu_id, uint32_t pc,
    uint32_t cause, uint32_t task, uint32_t line_ea,
    const uint8_t* before, const uint8_t* after)
{
    if (!g_yz_fe0_timeline_enabled || image_id != 4u || !before || !after)
        return;
    const uint32_t barrier = atomic_load_explicit(
        &g_wkl4_barrier_ea, memory_order_acquire);
    const uint32_t taskset = atomic_load_explicit(
        &g_wkl4_taskset_ea, memory_order_acquire);
    const uint32_t actor = ((spu_id & 0xffffu) << 16) | (task & 0xffffu);
    if (barrier && line_ea == barrier) {
        if (memcmp(before, after, 128u) != 0) {
            const uint32_t first = atomic_fetch_add_explicit(
                &g_barrier_first_claimed, 1u, memory_order_relaxed);
            if (first < YZ_FE0_BARRIER_FIRST_CAPACITY) {
                yz_fe0_barrier_first* sample = &g_barrier_first[first];
                atomic_store_explicit(&sample->published, 0u,
                                      memory_order_relaxed);
                sample->cause = cause;
                sample->actor = actor;
                sample->pc = pc;
                sample->before_zero = yz_fe0_read_be32(before + 0x00u);
                sample->after_zero = yz_fe0_read_be32(after + 0x00u);
                sample->remained = yz_fe0_read_be32(after + 0x04u);
                sample->before_waiters = yz_fe0_read_be32(before + 0x10u);
                sample->after_waiters = yz_fe0_read_be32(after + 0x10u);
                atomic_store_explicit(&sample->published, first + 1u,
                                      memory_order_release);
            }
            yz_fe0_timeline_emit(
                YZ_FE0_EVENT_WKL4_BARRIER_WRITE, cause, actor, pc,
                yz_fe0_read_be32(before + 0x00u),
                yz_fe0_read_be32(after + 0x00u),
                yz_fe0_read_be32(before + 0x10u) ^
                    yz_fe0_read_be32(after + 0x10u));
        }
        return;
    }
    if (taskset && line_ea == (taskset & ~127u)) {
        const uint32_t offset = taskset & 127u;
        if (offset <= 0x40u && offset + 0x50u <= 128u) {
            const uint32_t old_signal = yz_fe0_read_be32(
                before + offset + 0x40u);
            const uint32_t new_signal = yz_fe0_read_be32(
                after + offset + 0x40u);
            if (old_signal != new_signal)
                yz_fe0_timeline_emit(
                    YZ_FE0_EVENT_WKL4_TASKSET_WRITE, cause, actor, pc,
                    taskset, old_signal, new_signal);
        }
    }
}

void yz_fe0_timeline_observe_wkl4_taskset_attempt(
    uint32_t image_id, uint32_t spu_id, uint32_t pc,
    uint32_t cause, uint32_t task, uint32_t line_ea,
    int success, const uint8_t* current, const uint8_t* candidate)
{
    if (!g_yz_fe0_timeline_enabled || image_id != 4u ||
        pc != 0xAA48u || !current || !candidate)
        return;
    const uint32_t taskset = atomic_load_explicit(
        &g_wkl4_taskset_ea, memory_order_acquire);
    if (!taskset || line_ea != (taskset & ~127u))
        return;
    const uint32_t offset = taskset & 127u;
    if (offset > 0x20u || offset + 0x64u > 128u)
        return;

    const uint32_t old_signal = yz_fe0_read_be32(
        current + offset + 0x40u);
    const uint32_t new_signal = yz_fe0_read_be32(
        candidate + offset + 0x40u);
    const uint32_t old_waiting = yz_fe0_read_be32(
        current + offset + 0x50u);
    const uint32_t new_waiting = yz_fe0_read_be32(
        candidate + offset + 0x50u);
    const int changed = memcmp(current, candidate, 128u) != 0;
    const uint32_t index = atomic_fetch_add_explicit(
        &g_taskset_attempt_claimed, 1u, memory_order_relaxed);
    if (success)
        atomic_fetch_add_explicit(&g_taskset_attempt_successes, 1u,
                                  memory_order_relaxed);
    if (changed)
        atomic_fetch_add_explicit(&g_taskset_attempt_changes, 1u,
                                  memory_order_relaxed);
    if (index >= YZ_FE0_TASKSET_ATTEMPT_CAPACITY)
        return;

    yz_fe0_taskset_attempt_first* sample = &g_taskset_attempt_first[index];
    atomic_store_explicit(&sample->published, 0u, memory_order_relaxed);
    sample->cause = cause;
    sample->actor = ((spu_id & 0xffffu) << 16) | (task & 0xffffu);
    sample->pc = pc;
    sample->success = success ? 1u : 0u;
    sample->old_signal = old_signal;
    sample->new_signal = new_signal;
    sample->old_waiting = old_waiting;
    sample->new_waiting = new_waiting;
    sample->enabled = yz_fe0_read_be32(candidate + offset + 0x30u);
    sample->spurs_ea = yz_fe0_read_be32(candidate + offset + 0x60u);
    atomic_store_explicit(&sample->published, index + 1u,
                          memory_order_release);
}

void yz_fe0_timeline_emit(yz_fe0_event_type type, uint32_t cause,
                          uint32_t actor, uint32_t a0, uint32_t a1,
                          uint32_t a2, uint32_t a3)
{
    if (!g_yz_fe0_timeline_enabled)
        return;
    const uint64_t sequence = atomic_fetch_add_explicit(
        &g_claimed, 1, memory_order_relaxed) + 1u;
    yz_fe0_slot* slot = &g_records[(sequence - 1u) & YZ_FE0_TIMELINE_MASK];
    atomic_store_explicit(&slot->published_sequence, 0, memory_order_relaxed);
    slot->record.sequence = sequence;
    slot->record.qpc = yz_fe0_clock();
    slot->record.type = (uint32_t)type;
    slot->record.thread_id = yz_fe0_thread_id();
    slot->record.cause = cause;
    slot->record.actor = actor;
    slot->record.a0 = a0;
    slot->record.a1 = a1;
    slot->record.a2 = a2;
    slot->record.a3 = a3;
    atomic_store_explicit(&slot->published_sequence, sequence,
                          memory_order_release);
}

void yz_fe0_timeline_callback_begin(uint32_t cause, uint32_t epoch)
{
    if (!g_yz_fe0_timeline_enabled)
        return;
    atomic_store_explicit(&g_callback_cause, cause, memory_order_relaxed);
    atomic_store_explicit(&g_callback_epoch, epoch, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_callback_active, 1u, memory_order_release);
    yz_fe0_timeline_emit(YZ_FE0_EVENT_CALLBACK_BEGIN,
                         cause, epoch, 0, 0, 0, 0);
}

void yz_fe0_timeline_callback_end(uint32_t cause, uint32_t epoch)
{
    if (!g_yz_fe0_timeline_enabled)
        return;
    atomic_fetch_sub_explicit(&g_callback_active, 1u, memory_order_release);
    yz_fe0_timeline_emit(YZ_FE0_EVENT_CALLBACK_END,
                         cause, epoch, 0, 0, 0, 0);
}

int yz_fe0_timeline_callback_snapshot(uint32_t* cause, uint32_t* epoch)
{
    if (!g_yz_fe0_timeline_enabled ||
        atomic_load_explicit(&g_callback_active, memory_order_acquire) == 0u)
        return 0;
    const uint32_t observed_epoch = atomic_load_explicit(
        &g_callback_epoch, memory_order_acquire);
    const uint32_t observed_cause = atomic_load_explicit(
        &g_callback_cause, memory_order_relaxed);
    if (atomic_load_explicit(&g_callback_active, memory_order_acquire) == 0u)
        return 0;
    if (cause) *cause = observed_cause;
    if (epoch) *epoch = observed_epoch;
    return 1;
}

void yz_fe0_timeline_rsx_acquire(uint32_t context_dma, uint32_t offset,
                                 uint32_t address, uint32_t wanted,
                                 uint32_t observed, int stalled)
{
    if (!g_yz_fe0_timeline_enabled || address != 0x10200FE0u)
        return;
    if (stalled) {
        if (g_acquire.active && g_acquire.context_dma == context_dma &&
            g_acquire.offset == offset && g_acquire.address == address &&
            g_acquire.wanted == wanted)
            return;
        g_acquire.active = 1;
        g_acquire.context_dma = context_dma;
        g_acquire.offset = offset;
        g_acquire.address = address;
        g_acquire.wanted = wanted;
        yz_fe0_timeline_emit(YZ_FE0_EVENT_RSX_WAIT, wanted, context_dma,
                             offset, address, observed, 0);
        return;
    }
    if (!g_acquire.active || g_acquire.context_dma != context_dma ||
        g_acquire.offset != offset || g_acquire.address != address ||
        g_acquire.wanted != wanted)
        return;
    g_acquire.active = 0;
    yz_fe0_timeline_emit(YZ_FE0_EVENT_RSX_READY, wanted, context_dma,
                         offset, address, observed, 0);
}

static const char* yz_fe0_event_name(uint32_t type)
{
    static const char* names[] = {
        "INVALID", "UCMD_DISPATCH", "CALLBACK_BEGIN", "CALLBACK_END",
        "WKL4_SIGNAL", "WKL4_WAKE", "WKL4_RESUME", "WKL4_HANDOFF",
        "WKL4_RECORD", "DMA_BEGIN", "PUBLISHED", "RSX_WAIT",
        "RSX_READY", "WKL4_WAIT_ABI", "WKL4_EVENT_OBSERVE",
        "WKL4_EVENT_WRITE", "WKL4_QUEUE_WAIT", "CALLBACK_STATE",
        "WKL4_QUEUE_INIT", "WKL4_QUEUE_OBSERVE", "WKL4_QUEUE_WRITE",
        "WKL4_QUEUE_PPU_PUSH", "WKL4_QUEUE_LAYOUT",
        "WKL4_BARRIER_WAIT", "WKL4_BARRIER_LAYOUT",
        "WKL4_BARRIER_WRITE", "WKL4_TASKSET_WRITE"
    };
    return type < sizeof(names) / sizeof(names[0]) ? names[type] : "UNKNOWN";
}

void yz_fe0_timeline_shutdown(void)
{
    static char dump_buffer[64u * 1024u];
    size_t dump_used = 0;
    if (!g_yz_fe0_timeline_enabled || g_shutdown)
        return;
    g_yz_fe0_timeline_enabled = 0;
    g_shutdown = 1;
    const uint64_t total = atomic_load_explicit(&g_claimed,
                                                 memory_order_acquire);
    const uint64_t first = total > YZ_FE0_TIMELINE_CAPACITY
        ? total - YZ_FE0_TIMELINE_CAPACITY + 1u : 1u;
    const uint64_t retained = total < YZ_FE0_TIMELINE_CAPACITY
        ? total : YZ_FE0_TIMELINE_CAPACITY;
    fprintf(stderr,
            "[fe0-timeline] version=1 frequency=%llu claimed=%llu retained=%llu dropped=%llu\n",
            (unsigned long long)g_frequency,
            (unsigned long long)total,
            (unsigned long long)retained,
            (unsigned long long)(total - retained));
    {
        const uint32_t claimed = atomic_load_explicit(
            &g_barrier_first_claimed, memory_order_acquire);
        const uint32_t retained_first =
            claimed < YZ_FE0_BARRIER_FIRST_CAPACITY
                ? claimed : YZ_FE0_BARRIER_FIRST_CAPACITY;
        fprintf(stderr,
                "[fe0-barrier-first] claimed=%u retained=%u\n",
                claimed, retained_first);
        for (uint32_t index = 0; index < retained_first; ++index) {
            const yz_fe0_barrier_first* sample = &g_barrier_first[index];
            if (atomic_load_explicit(&sample->published,
                                     memory_order_acquire) != index + 1u)
                continue;
            fprintf(stderr,
                    "[fe0-barrier-first] n=%u cause=%08X actor=%08X pc=%05X zero=%08X->%08X remained=%08X waiters=%08X->%08X\n",
                    index + 1u, sample->cause, sample->actor, sample->pc,
                    sample->before_zero, sample->after_zero,
                    sample->remained, sample->before_waiters,
                    sample->after_waiters);
        }
    }
    {
        const uint32_t claimed = atomic_load_explicit(
            &g_taskset_attempt_claimed, memory_order_acquire);
        const uint32_t successes = atomic_load_explicit(
            &g_taskset_attempt_successes, memory_order_acquire);
        const uint32_t changes = atomic_load_explicit(
            &g_taskset_attempt_changes, memory_order_acquire);
        const uint32_t retained_first =
            claimed < YZ_FE0_TASKSET_ATTEMPT_CAPACITY
                ? claimed : YZ_FE0_TASKSET_ATTEMPT_CAPACITY;
        fprintf(stderr,
                "[fe0-taskset-attempt] claimed=%u success=%u fail=%u changed=%u nochange=%u retained=%u\n",
                claimed, successes, claimed - successes, changes,
                claimed - changes, retained_first);
        for (uint32_t index = 0; index < retained_first; ++index) {
            const yz_fe0_taskset_attempt_first* sample =
                &g_taskset_attempt_first[index];
            if (atomic_load_explicit(&sample->published,
                                     memory_order_acquire) != index + 1u)
                continue;
            fprintf(stderr,
                    "[fe0-taskset-attempt] n=%u cause=%08X actor=%08X pc=%05X verdict=%s signal=%08X->%08X waiting=%08X->%08X enabled=%08X spurs=%08X\n",
                    index + 1u, sample->cause, sample->actor, sample->pc,
                    sample->success ? "SUCCESS" : "FAIL",
                    sample->old_signal, sample->new_signal,
                    sample->old_waiting, sample->new_waiting,
                    sample->enabled, sample->spurs_ea);
        }
    }
    for (uint64_t sequence = first; sequence <= total; ++sequence) {
        yz_fe0_slot* slot =
            &g_records[(sequence - 1u) & YZ_FE0_TIMELINE_MASK];
        if (atomic_load_explicit(&slot->published_sequence,
                                 memory_order_acquire) != sequence)
            continue;
        const yz_fe0_timeline_record* record = &slot->record;
        char line[256];
        const int formatted = snprintf(
            line, sizeof(line),
            "[fe0-event] seq=%llu qpc=%llu type=%s tid=%u cause=%08X actor=%08X a0=%08X a1=%08X a2=%08X a3=%08X\n",
            (unsigned long long)record->sequence,
            (unsigned long long)record->qpc,
            yz_fe0_event_name(record->type), record->thread_id,
            record->cause, record->actor, record->a0, record->a1,
            record->a2, record->a3);
        if (formatted <= 0)
            continue;
        const size_t line_size = (size_t)formatted < sizeof(line)
            ? (size_t)formatted : sizeof(line) - 1u;
        if (dump_used + line_size > sizeof(dump_buffer)) {
            fwrite(dump_buffer, 1, dump_used, stderr);
            dump_used = 0;
        }
        memcpy(dump_buffer + dump_used, line, line_size);
        dump_used += line_size;
    }
    if (dump_used)
        fwrite(dump_buffer, 1, dump_used, stderr);
    fflush(stderr);
}

#if defined(YZ_FE0_TIMELINE_TEST)
void yz_fe0_timeline_test_reset(uint64_t frequency, int enabled)
{
    memset(g_records, 0, sizeof(g_records));
    memset(&g_acquire, 0, sizeof(g_acquire));
    atomic_store_explicit(&g_claimed, 0, memory_order_relaxed);
    atomic_store_explicit(&g_callback_cause, 0, memory_order_relaxed);
    atomic_store_explicit(&g_callback_epoch, 0, memory_order_relaxed);
    atomic_store_explicit(&g_callback_active, 0, memory_order_relaxed);
    atomic_store_explicit(&g_wkl4_barrier_ea, 0, memory_order_relaxed);
    atomic_store_explicit(&g_wkl4_taskset_ea, 0, memory_order_relaxed);
    memset(g_barrier_first, 0, sizeof(g_barrier_first));
    atomic_store_explicit(&g_barrier_first_claimed, 0,
                          memory_order_relaxed);
    memset(g_taskset_attempt_first, 0, sizeof(g_taskset_attempt_first));
    atomic_store_explicit(&g_taskset_attempt_claimed, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_taskset_attempt_successes, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_taskset_attempt_changes, 0,
                          memory_order_relaxed);
    g_frequency = frequency;
    g_initialized = 1;
    g_shutdown = 0;
    g_test_clock = 0;
    g_test_clock_reads = 0;
    g_yz_fe0_timeline_enabled = enabled ? 1 : 0;
}

void yz_fe0_timeline_test_set_clock(uint64_t qpc)
{
    g_test_clock = qpc;
}

uint64_t yz_fe0_timeline_test_clock_reads(void)
{
    return g_test_clock_reads;
}

uint64_t yz_fe0_timeline_test_claimed(void)
{
    return atomic_load_explicit(&g_claimed, memory_order_relaxed);
}

int yz_fe0_timeline_test_record(uint64_t sequence,
                                yz_fe0_timeline_record* out)
{
    if (!sequence || !out)
        return 0;
    yz_fe0_slot* slot =
        &g_records[(sequence - 1u) & YZ_FE0_TIMELINE_MASK];
    if (atomic_load_explicit(&slot->published_sequence,
                             memory_order_acquire) != sequence)
        return 0;
    *out = slot->record;
    return 1;
}

uint32_t yz_fe0_timeline_test_taskset_attempts(void)
{
    return atomic_load_explicit(&g_taskset_attempt_claimed,
                                memory_order_relaxed);
}

uint32_t yz_fe0_timeline_test_taskset_successes(void)
{
    return atomic_load_explicit(&g_taskset_attempt_successes,
                                memory_order_relaxed);
}

uint32_t yz_fe0_timeline_test_taskset_changes(void)
{
    return atomic_load_explicit(&g_taskset_attempt_changes,
                                memory_order_relaxed);
}
#endif
