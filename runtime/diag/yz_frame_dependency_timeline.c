#include "ps3emu/yz_frame_dependency_timeline.h"

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

#define YZ_FRAME_DEP_CAPACITY 131072u
#define YZ_FRAME_DEP_MASK (YZ_FRAME_DEP_CAPACITY - 1u)
#define YZ_FRAME_DEP_WAIT_SLOTS 64u

typedef struct yz_frame_dep_slot {
    _Atomic uint64_t published_sequence;
    yz_frame_dep_record record;
} yz_frame_dep_slot;

typedef struct yz_frame_dep_wait_slot {
    _Atomic uint64_t generation;
    _Atomic uint32_t address;
} yz_frame_dep_wait_slot;

static yz_frame_dep_slot g_records[YZ_FRAME_DEP_CAPACITY];
static yz_frame_dep_wait_slot g_waits[YZ_FRAME_DEP_WAIT_SLOTS];
static _Atomic uint64_t g_claimed;
static _Atomic uint64_t g_frame_generation;
static _Atomic uint64_t g_dependency_generation;
static _Atomic uint64_t g_completed_wait_generation;
static _Atomic uint64_t g_fifo_generation;
static _Atomic uint64_t g_consumed_fifo_generation;
static _Atomic uint64_t g_benchmark_game_updates;
static _Atomic uint64_t g_benchmark_image4_rounds;
static uint64_t g_frequency;
static int g_initialized;
static int g_shutdown;

#if defined(YZ_FRAME_DEP_TIMELINE_TEST)
static uint64_t g_test_clock;
static uint64_t g_test_clock_reads;
#endif

volatile long g_yz_frame_dependency_timeline_enabled;
volatile long g_yz_benchmark_invariants_enabled;

static uint32_t yz_frame_dep_thread_id(void)
{
#if defined(_WIN32)
    return (uint32_t)GetCurrentThreadId();
#else
    return (uint32_t)getpid();
#endif
}

static uint64_t yz_frame_dep_clock(void)
{
#if defined(YZ_FRAME_DEP_TIMELINE_TEST)
    ++g_test_clock_reads;
    return g_test_clock;
#elif defined(_WIN32)
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
#else
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
#endif
}

static int exact_flag(const char* name)
{
    const char* value = getenv(name);
    return value && value[0] == '1' && value[1] == '\0';
}

static void emit(yz_frame_dep_event_type type, uint64_t frame_generation,
                 uint64_t dependency_generation, uint32_t a0,
                 uint32_t a1, uint32_t a2, uint32_t a3)
{
    yz_frame_dep_record record;
    yz_frame_dep_slot* slot;
    uint64_t sequence;
    if (!g_yz_frame_dependency_timeline_enabled || g_shutdown)
        return;
    sequence = atomic_fetch_add_explicit(&g_claimed, 1u,
                                          memory_order_relaxed) + 1u;
    record.sequence = sequence;
    record.qpc = yz_frame_dep_clock();
    record.frame_generation = frame_generation;
    record.dependency_generation = dependency_generation;
    record.type = (uint32_t)type;
    record.thread_id = yz_frame_dep_thread_id();
    record.a0 = a0; record.a1 = a1; record.a2 = a2; record.a3 = a3;
    slot = &g_records[(sequence - 1u) & YZ_FRAME_DEP_MASK];
    atomic_store_explicit(&slot->published_sequence, 0u, memory_order_relaxed);
    slot->record = record;
    atomic_store_explicit(&slot->published_sequence, sequence,
                          memory_order_release);
}

static uint64_t current_frame(void)
{
    return atomic_load_explicit(&g_frame_generation, memory_order_acquire);
}

int yz_frame_dependency_timeline_init(void)
{
    uint32_t i;
    if (g_initialized)
        return g_yz_frame_dependency_timeline_enabled != 0;
    g_initialized = 1;
    g_shutdown = 0;
    atomic_store(&g_claimed, 0u);
    atomic_store(&g_frame_generation, 0u);
    atomic_store(&g_dependency_generation, 0u);
    atomic_store(&g_completed_wait_generation, 0u);
    atomic_store(&g_fifo_generation, 0u);
    atomic_store(&g_consumed_fifo_generation, 0u);
    atomic_store(&g_benchmark_game_updates, 0u);
    atomic_store(&g_benchmark_image4_rounds, 0u);
    for (i = 0; i < YZ_FRAME_DEP_WAIT_SLOTS; ++i) {
        atomic_store(&g_waits[i].generation, 0u);
        atomic_store(&g_waits[i].address, 0u);
    }
#if defined(_WIN32)
    { LARGE_INTEGER value; QueryPerformanceFrequency(&value);
      g_frequency = (uint64_t)value.QuadPart; }
#else
    g_frequency = 1000000000ull;
#endif
    g_yz_benchmark_invariants_enabled =
        exact_flag("YZ_BENCHMARK_INVARIANTS") ? 1 : 0;
    if (!exact_flag("YZ_FRAME_DEP_TIMELINE"))
        return 0;
    g_yz_frame_dependency_timeline_enabled = 1;
    return 1;
}

uint64_t yz_frame_dep_ppu_update_start(uint32_t function_address,
                                        uint32_t object_ea)
{
    uint64_t frame;
    if (g_yz_benchmark_invariants_enabled)
        atomic_fetch_add_explicit(&g_benchmark_game_updates, 1u,
                                  memory_order_relaxed);
    if (!g_yz_frame_dependency_timeline_enabled)
        return 0;
    frame = atomic_fetch_add_explicit(&g_frame_generation, 1u,
                                       memory_order_acq_rel) + 1u;
    emit(YZ_FRAME_DEP_PPU_UPDATE_START, frame, 0u,
         function_address, object_ea, 0u, 0u);
    return frame;
}

void yz_benchmark_note_image4_round(void)
{
    if (g_yz_benchmark_invariants_enabled)
        atomic_fetch_add_explicit(&g_benchmark_image4_rounds, 1u,
                                  memory_order_relaxed);
}

uint64_t yz_benchmark_game_updates(void)
{
    return atomic_load_explicit(&g_benchmark_game_updates,
                                memory_order_relaxed);
}

uint64_t yz_benchmark_image4_rounds(void)
{
    return atomic_load_explicit(&g_benchmark_image4_rounds,
                                memory_order_relaxed);
}

void yz_frame_dep_ppu_update_complete(uint64_t frame, uint32_t function_address,
                                      uint32_t result)
{
    if (frame)
        emit(YZ_FRAME_DEP_PPU_UPDATE_COMPLETE, frame, 0u,
             function_address, result, 0u, 0u);
}

void yz_frame_dep_spurs_schedule(uint32_t kind, uint32_t image_id,
                                 uint32_t workload_id, uint32_t item_id)
{ emit(YZ_FRAME_DEP_SPURS_SCHEDULE, current_frame(), 0u,
       kind, image_id, workload_id, item_id); }
void yz_frame_dep_spu_task_start(uint32_t image_id, uint32_t spu_id,
                                 uint32_t task_id, uint32_t pc)
{ emit(YZ_FRAME_DEP_SPU_TASK_START, current_frame(), 0u,
       image_id, spu_id, task_id, pc); }
void yz_frame_dep_spu_task_complete(uint32_t image_id, uint32_t spu_id,
                                    uint32_t task_id, uint32_t pc)
{ emit(YZ_FRAME_DEP_SPU_TASK_COMPLETE, current_frame(), 0u,
       image_id, spu_id, task_id, pc); }
void yz_frame_dep_spu_job_start(uint32_t image_id, uint32_t spu_id,
                                uint32_t workload_id, uint32_t descriptor_ea)
{ emit(YZ_FRAME_DEP_SPU_JOB_START, current_frame(), 0u,
       image_id, spu_id, workload_id, descriptor_ea); }
void yz_frame_dep_spu_job_complete(uint32_t image_id, uint32_t spu_id,
                                   uint32_t workload_id, uint32_t descriptor_ea)
{ emit(YZ_FRAME_DEP_SPU_JOB_COMPLETE, current_frame(), 0u,
       image_id, spu_id, workload_id, descriptor_ea); }

uint64_t yz_frame_dep_ppu_wait_enter(uint32_t address, uint32_t observed)
{
    uint64_t generation;
    uint32_t i;
    if (!g_yz_frame_dependency_timeline_enabled || !address || !observed)
        return 0;
    generation = atomic_fetch_add_explicit(&g_dependency_generation, 1u,
                                             memory_order_acq_rel) + 1u;
    for (i = 0; i < YZ_FRAME_DEP_WAIT_SLOTS; ++i) {
        uint64_t empty = 0u;
        if (atomic_compare_exchange_strong_explicit(
                &g_waits[i].generation, &empty, generation,
                memory_order_acq_rel, memory_order_relaxed)) {
            atomic_store_explicit(&g_waits[i].address, address,
                                  memory_order_release);
            emit(YZ_FRAME_DEP_PPU_WAIT_ENTER, current_frame(), generation,
                 address, observed, i, 0u);
            return (generation << 8) | (uint64_t)(i + 1u);
        }
    }
    emit(YZ_FRAME_DEP_PPU_WAIT_ENTER, current_frame(), generation,
         address, observed, UINT32_MAX, 1u);
    return generation << 8;
}

void yz_frame_dep_ppu_wait_exit(uint64_t token, uint32_t address,
                                uint32_t observed)
{
    uint64_t generation = token >> 8;
    uint32_t encoded = (uint32_t)(token & 0xffu);
    if (!g_yz_frame_dependency_timeline_enabled || !generation)
        return;
    emit(YZ_FRAME_DEP_PPU_WAIT_EXIT, current_frame(), generation,
         address, observed, encoded ? encoded - 1u : UINT32_MAX, 0u);
    atomic_store_explicit(&g_completed_wait_generation, generation,
                          memory_order_release);
    if (encoded && encoded <= YZ_FRAME_DEP_WAIT_SLOTS) {
        yz_frame_dep_wait_slot* slot = &g_waits[encoded - 1u];
        if (atomic_load_explicit(&slot->generation,
                                 memory_order_acquire) == generation) {
            atomic_store_explicit(&slot->address, 0u, memory_order_release);
            atomic_store_explicit(&slot->generation, 0u, memory_order_release);
        }
    }
}

void yz_frame_dep_dma_publish(uint32_t image_id, uint32_t spu_id,
                              uint32_t pc, uint32_t address,
                              uint32_t size, uint32_t command)
{
    uint32_t i;
    uint64_t end;
    if (!g_yz_frame_dependency_timeline_enabled || !size)
        return;
    end = (uint64_t)address + size;
    for (i = 0; i < YZ_FRAME_DEP_WAIT_SLOTS; ++i) {
        uint32_t watched = atomic_load_explicit(&g_waits[i].address,
                                                memory_order_acquire);
        uint64_t generation = atomic_load_explicit(&g_waits[i].generation,
                                                   memory_order_acquire);
        if (generation && watched && address <= watched &&
                end >= (uint64_t)watched + 4u)
            emit(YZ_FRAME_DEP_DMA_PUBLISH, current_frame(), generation,
                 image_id, spu_id, watched,
                 (pc & 0x3ffffu) | ((command & 0xffu) << 24));
    }
}

void yz_frame_dep_fifo_publish(uint32_t old_put, uint32_t new_put,
                               uint32_t source, uint32_t context_id)
{
    uint64_t generation;
    uint64_t dependency;
    if (!g_yz_frame_dependency_timeline_enabled || old_put == new_put)
        return;
    generation = atomic_fetch_add_explicit(&g_fifo_generation, 1u,
                                            memory_order_acq_rel) + 1u;
    dependency = atomic_load_explicit(&g_completed_wait_generation,
                                      memory_order_acquire);
    emit(YZ_FRAME_DEP_FIFO_PUBLISH, current_frame(), dependency,
         old_put, new_put, source, context_id);
    (void)generation; /* fetch-add already published the monotonic identity */
}

void yz_frame_dep_rsx_consume(uint32_t old_get, uint32_t new_get,
                              uint32_t put, uint32_t result)
{
    uint64_t published;
    uint64_t consumed;
    uint64_t dependency;
    if (!g_yz_frame_dependency_timeline_enabled || old_get == new_get)
        return;
    published = atomic_load_explicit(&g_fifo_generation, memory_order_acquire);
    consumed = atomic_load_explicit(&g_consumed_fifo_generation,
                                    memory_order_relaxed);
    if (!published || published <= consumed)
        return;
    if (!atomic_compare_exchange_strong_explicit(
            &g_consumed_fifo_generation, &consumed, published,
            memory_order_acq_rel, memory_order_relaxed))
        return;
    dependency = atomic_load_explicit(&g_completed_wait_generation,
                                      memory_order_acquire);
    emit(YZ_FRAME_DEP_RSX_CONSUME, current_frame(), dependency,
         old_get, new_get, put, result);
}

void yz_frame_dep_frame_complete(uint32_t buffer_id, uint64_t frame)
{ emit(YZ_FRAME_DEP_FRAME_COMPLETE, current_frame(), 0u,
       buffer_id, (uint32_t)frame, (uint32_t)(frame >> 32), 0u); }
void yz_frame_dep_submission(uint32_t reason, uint64_t frame, uint64_t fence)
{ emit(YZ_FRAME_DEP_SUBMISSION, current_frame(), 0u,
       reason, (uint32_t)frame, (uint32_t)fence, (uint32_t)(fence >> 32)); }
void yz_frame_dep_present(uint32_t buffer_id, uint64_t frame,
                          uint32_t present_kind)
{ emit(YZ_FRAME_DEP_PRESENT, current_frame(), 0u,
       buffer_id, (uint32_t)frame, (uint32_t)(frame >> 32), present_kind); }

static const char* event_name(uint32_t type)
{
    switch ((yz_frame_dep_event_type)type) {
    case YZ_FRAME_DEP_PPU_UPDATE_START: return "PPU_UPDATE_START";
    case YZ_FRAME_DEP_PPU_UPDATE_COMPLETE: return "PPU_UPDATE_COMPLETE";
    case YZ_FRAME_DEP_SPURS_SCHEDULE: return "SPURS_SCHEDULE";
    case YZ_FRAME_DEP_SPU_TASK_START: return "SPU_TASK_START";
    case YZ_FRAME_DEP_SPU_TASK_COMPLETE: return "SPU_TASK_COMPLETE";
    case YZ_FRAME_DEP_SPU_JOB_START: return "SPU_JOB_START";
    case YZ_FRAME_DEP_SPU_JOB_COMPLETE: return "SPU_JOB_COMPLETE";
    case YZ_FRAME_DEP_DMA_PUBLISH: return "DMA_PUBLISH";
    case YZ_FRAME_DEP_PPU_WAIT_ENTER: return "PPU_WAIT_ENTER";
    case YZ_FRAME_DEP_PPU_WAIT_EXIT: return "PPU_WAIT_EXIT";
    case YZ_FRAME_DEP_FIFO_PUBLISH: return "FIFO_PUBLISH";
    case YZ_FRAME_DEP_RSX_CONSUME: return "RSX_CONSUME";
    case YZ_FRAME_DEP_FRAME_COMPLETE: return "FRAME_COMPLETE";
    case YZ_FRAME_DEP_SUBMISSION: return "SUBMISSION";
    case YZ_FRAME_DEP_PRESENT: return "PRESENT";
    default: return "UNKNOWN";
    }
}

void yz_frame_dependency_timeline_shutdown(void)
{
    uint64_t claimed, first, sequence;
    if (!g_yz_frame_dependency_timeline_enabled || g_shutdown)
        return;
    g_shutdown = 1;
    claimed = atomic_load_explicit(&g_claimed, memory_order_acquire);
    first = claimed > YZ_FRAME_DEP_CAPACITY
        ? claimed - YZ_FRAME_DEP_CAPACITY + 1u : 1u;
    fprintf(stderr, "[frame-dep] version=2 frequency=%llu claimed=%llu retained=%llu first=%llu dropped=%llu\n",
            (unsigned long long)g_frequency,
            (unsigned long long)claimed,
            (unsigned long long)(claimed ? claimed - first + 1u : 0u),
            (unsigned long long)(claimed ? first : 0u),
            (unsigned long long)(claimed > YZ_FRAME_DEP_CAPACITY
                                 ? claimed - YZ_FRAME_DEP_CAPACITY : 0u));
    for (sequence = first; claimed && sequence <= claimed; ++sequence) {
        yz_frame_dep_slot* slot = &g_records[(sequence - 1u) & YZ_FRAME_DEP_MASK];
        yz_frame_dep_record record;
        if (atomic_load_explicit(&slot->published_sequence,
                                 memory_order_acquire) != sequence)
            continue;
        record = slot->record;
        fprintf(stderr,
                "[frame-dep-event] seq=%llu qpc=%llu type=%s frame=%llu dep=%llu tid=%u a0=%08X a1=%08X a2=%08X a3=%08X\n",
                (unsigned long long)record.sequence,
                (unsigned long long)record.qpc, event_name(record.type),
                (unsigned long long)record.frame_generation,
                (unsigned long long)record.dependency_generation,
                record.thread_id, record.a0, record.a1, record.a2, record.a3);
    }
    fflush(stderr);
    g_yz_frame_dependency_timeline_enabled = 0;
}

#if defined(YZ_FRAME_DEP_TIMELINE_TEST)
void yz_frame_dependency_test_reset(uint64_t frequency, int enabled)
{
    memset(g_records, 0, sizeof(g_records));
    memset(g_waits, 0, sizeof(g_waits));
    atomic_store(&g_claimed, 0u); atomic_store(&g_frame_generation, 0u);
    atomic_store(&g_dependency_generation, 0u);
    atomic_store(&g_completed_wait_generation, 0u);
    atomic_store(&g_fifo_generation, 0u);
    atomic_store(&g_consumed_fifo_generation, 0u);
    g_frequency = frequency; g_test_clock = 0u; g_test_clock_reads = 0u;
    g_initialized = 1; g_shutdown = 0;
    g_yz_frame_dependency_timeline_enabled = enabled ? 1 : 0;
}
void yz_frame_dependency_test_set_clock(uint64_t qpc) { g_test_clock = qpc; }
uint64_t yz_frame_dependency_test_clock_reads(void) { return g_test_clock_reads; }
uint64_t yz_frame_dependency_test_claimed(void) { return atomic_load(&g_claimed); }
int yz_frame_dependency_test_record(uint64_t sequence, yz_frame_dep_record* out)
{
    yz_frame_dep_slot* slot;
    if (!sequence || !out) return 0;
    slot = &g_records[(sequence - 1u) & YZ_FRAME_DEP_MASK];
    if (atomic_load_explicit(&slot->published_sequence,
                             memory_order_acquire) != sequence) return 0;
    *out = slot->record; return 1;
}
#endif
