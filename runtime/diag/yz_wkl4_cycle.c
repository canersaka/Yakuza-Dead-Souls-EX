#include "ps3emu/yz_wkl4_cycle.h"
#include "ps3emu/yz_wkl4_cycle_interval.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <x86intrin.h>
#else
#include <time.h>
#endif

typedef struct yz_wkl4_cycle_aggregate {
    _Atomic uint64_t cycles;
    _Atomic uint64_t entries;
} yz_wkl4_cycle_aggregate;

typedef struct yz_wkl4_cycle_thread_state {
    uint64_t start;
    uint32_t tag;
    uint32_t generation;
    uint64_t cycles[YZ_WKL4_CYCLE_TAG_COUNT];
    uint64_t entries[YZ_WKL4_CYCLE_TAG_COUNT];
} yz_wkl4_cycle_thread_state;

static yz_wkl4_cycle_aggregate g_aggregates[2][YZ_WKL4_CYCLE_TAG_COUNT];
static _Atomic uint32_t g_generation;
static _Thread_local yz_wkl4_cycle_thread_state g_thread_state;
static int g_initialized;
static int g_shutdown;

#if defined(YZ_WKL4_CYCLE_TEST)
static uint64_t g_test_clock;
static uint64_t g_test_clock_reads;
#endif

int g_yz_wkl4_cycle_enabled;

static yz_wkl4_cycle_aggregate* yz_wkl4_cycle_current_aggregates(
    uint32_t generation)
{
    return g_aggregates[generation & 1u];
}

static uint64_t yz_wkl4_cycle_clock(void)
{
#if defined(YZ_WKL4_CYCLE_TEST)
    ++g_test_clock_reads;
    return g_test_clock;
#elif defined(_MSC_VER) || defined(__i386__) || defined(__x86_64__)
    return (uint64_t)__rdtsc();
#else
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000000ull +
           (uint64_t)value.tv_nsec;
#endif
}

static void yz_wkl4_cycle_reset_pending(yz_wkl4_cycle_thread_state* state)
{
    state->start = 0u;
    state->tag = YZ_WKL4_CYCLE_NONE;
    memset(state->cycles, 0, sizeof(state->cycles));
    memset(state->entries, 0, sizeof(state->entries));
}

static void yz_wkl4_cycle_flush_pending(
    yz_wkl4_cycle_thread_state* state, uint32_t generation)
{
    yz_wkl4_cycle_aggregate* aggregates =
        yz_wkl4_cycle_current_aggregates(generation);
    for (uint32_t tag = 1u; tag < YZ_WKL4_CYCLE_TAG_COUNT; ++tag) {
        if (state->cycles[tag]) {
            atomic_fetch_add_explicit(&aggregates[tag].cycles,
                                      state->cycles[tag],
                                      memory_order_relaxed);
            state->cycles[tag] = 0u;
        }
        if (state->entries[tag]) {
            atomic_fetch_add_explicit(&aggregates[tag].entries,
                                      state->entries[tag],
                                      memory_order_relaxed);
            state->entries[tag] = 0u;
        }
    }
}

static int yz_wkl4_cycle_exact_flag(const char* name)
{
    const char* value = getenv(name);
    return value && value[0] == '1' && value[1] == '\0';
}

int yz_wkl4_cycle_init(void)
{
    if (g_initialized)
        return g_yz_wkl4_cycle_enabled;
    g_initialized = 1;
    g_shutdown = 0;
    memset(g_aggregates, 0, sizeof(g_aggregates));
    atomic_store_explicit(&g_generation, 1u, memory_order_relaxed);
    memset(&g_thread_state, 0, sizeof(g_thread_state));
#if defined(YZ_WKL4_CYCLE_DIAGNOSTIC)
    g_yz_wkl4_cycle_enabled =
        yz_wkl4_cycle_exact_flag("YZ_WKL4_CYCLE");
#else
    g_yz_wkl4_cycle_enabled = 0;
#endif
    return g_yz_wkl4_cycle_enabled;
}

void yz_wkl4_cycle_mark(yz_wkl4_cycle_tag tag)
{
    yz_wkl4_cycle_thread_state* state = &g_thread_state;
    const uint32_t generation = atomic_load_explicit(
        &g_generation, memory_order_acquire);
    if (!g_yz_wkl4_cycle_enabled || tag <= YZ_WKL4_CYCLE_NONE ||
        tag >= YZ_WKL4_CYCLE_TAG_COUNT)
        return;

    if (state->generation != generation) {
        yz_wkl4_cycle_reset_pending(state);
        state->generation = generation;
    } else if (state->tag == (uint32_t)tag) {
        return;
    }

    const uint64_t now = yz_wkl4_cycle_clock();
    if (state->tag > YZ_WKL4_CYCLE_NONE &&
        state->tag < YZ_WKL4_CYCLE_TAG_COUNT) {
        state->cycles[state->tag] += now - state->start;
    }
    state->tag = (uint32_t)tag;
    state->start = now;
    ++state->entries[tag];
}

void yz_wkl4_cycle_leave(void)
{
    yz_wkl4_cycle_thread_state* state = &g_thread_state;
    const uint32_t generation = atomic_load_explicit(
        &g_generation, memory_order_acquire);
    if (!g_yz_wkl4_cycle_enabled ||
        state->tag <= YZ_WKL4_CYCLE_NONE ||
        state->tag >= YZ_WKL4_CYCLE_TAG_COUNT)
        return;

    if (state->generation != generation) {
        yz_wkl4_cycle_reset_pending(state);
        state->generation = generation;
        return;
    }

    const uint64_t now = yz_wkl4_cycle_clock();
    state->cycles[state->tag] += now - state->start;
    state->tag = YZ_WKL4_CYCLE_NONE;
    state->start = 0u;
    yz_wkl4_cycle_flush_pending(state, generation);
}

void yz_wkl4_cycle_begin_interval(void)
{
    if (!g_yz_wkl4_cycle_enabled)
        return;

    const uint32_t current = atomic_load_explicit(
        &g_generation, memory_order_relaxed);
    const uint32_t next = current + 1u;
    yz_wkl4_cycle_aggregate* aggregates =
        yz_wkl4_cycle_current_aggregates(next);
    for (uint32_t tag = 0u; tag < YZ_WKL4_CYCLE_TAG_COUNT; ++tag) {
        atomic_store_explicit(&aggregates[tag].cycles, 0u,
                              memory_order_relaxed);
        atomic_store_explicit(&aggregates[tag].entries, 0u,
                              memory_order_relaxed);
    }
    atomic_store_explicit(&g_generation, next, memory_order_release);
}

const char* yz_wkl4_cycle_tag_name(yz_wkl4_cycle_tag tag)
{
    static const char* names[YZ_WKL4_CYCLE_TAG_COUNT] = {
        "none",
        "7e50_setup", "7e50_loop_load", "7e50_loop_scatter",
        "7e50_loop_shuffle", "7e50_loop_commit", "7e50_tail",
        "8230_setup", "8230_loop_prepare", "8230_loop_compare",
        "8230_loop_store", "8230_tail",
        "8680_setup", "8680_loop", "8680_tail",
        "9808_setup", "9808_loop", "9808_tail"
    };
    return tag < YZ_WKL4_CYCLE_TAG_COUNT ? names[tag] : "unknown";
}

void yz_wkl4_cycle_shutdown(void)
{
    if (!g_yz_wkl4_cycle_enabled || g_shutdown)
        return;
    g_shutdown = 1;
    yz_wkl4_cycle_leave();
    const uint32_t generation = atomic_load_explicit(
        &g_generation, memory_order_acquire);
    yz_wkl4_cycle_aggregate* aggregates =
        yz_wkl4_cycle_current_aggregates(generation);
    uint64_t total = 0u;
    for (uint32_t tag = 1u; tag < YZ_WKL4_CYCLE_TAG_COUNT; ++tag)
        total += atomic_load_explicit(&aggregates[tag].cycles,
                                      memory_order_relaxed);
    fprintf(stderr, "[wkl4-cycle] clock=tsc total_cycles=%llu\n",
            (unsigned long long)total);
    for (uint32_t tag = 1u; tag < YZ_WKL4_CYCLE_TAG_COUNT; ++tag) {
        const uint64_t cycles = atomic_load_explicit(
            &aggregates[tag].cycles, memory_order_relaxed);
        const uint64_t entries = atomic_load_explicit(
            &aggregates[tag].entries, memory_order_relaxed);
        fprintf(stderr,
                "[wkl4-cycle] tag=%s cycles=%llu entries=%llu share=%.6f "
                "cycles_per_entry=%.1f\n",
                yz_wkl4_cycle_tag_name((yz_wkl4_cycle_tag)tag),
                (unsigned long long)cycles,
                (unsigned long long)entries,
                total ? (double)cycles / (double)total : 0.0,
                entries ? (double)cycles / (double)entries : 0.0);
    }
    fflush(stderr);
}

#if defined(YZ_WKL4_CYCLE_TEST)
void yz_wkl4_cycle_test_reset(int enabled)
{
    memset(g_aggregates, 0, sizeof(g_aggregates));
    atomic_store_explicit(&g_generation, 1u, memory_order_relaxed);
    memset(&g_thread_state, 0, sizeof(g_thread_state));
    g_test_clock = 0u;
    g_test_clock_reads = 0u;
    g_initialized = 1;
    g_shutdown = 0;
    g_yz_wkl4_cycle_enabled = enabled ? 1 : 0;
}

void yz_wkl4_cycle_test_set_clock(uint64_t cycles)
{
    g_test_clock = cycles;
}

uint64_t yz_wkl4_cycle_test_clock_reads(void)
{
    return g_test_clock_reads;
}

uint64_t yz_wkl4_cycle_test_cycles(yz_wkl4_cycle_tag tag)
{
    const uint32_t generation = atomic_load_explicit(
        &g_generation, memory_order_relaxed);
    yz_wkl4_cycle_aggregate* aggregates =
        yz_wkl4_cycle_current_aggregates(generation);
    if (tag >= YZ_WKL4_CYCLE_TAG_COUNT)
        return 0u;
    uint64_t value = atomic_load_explicit(&aggregates[tag].cycles,
                                          memory_order_relaxed);
    if (g_thread_state.generation == generation)
        value += g_thread_state.cycles[tag];
    return value;
}

uint64_t yz_wkl4_cycle_test_entries(yz_wkl4_cycle_tag tag)
{
    const uint32_t generation = atomic_load_explicit(
        &g_generation, memory_order_relaxed);
    yz_wkl4_cycle_aggregate* aggregates =
        yz_wkl4_cycle_current_aggregates(generation);
    if (tag >= YZ_WKL4_CYCLE_TAG_COUNT)
        return 0u;
    uint64_t value = atomic_load_explicit(&aggregates[tag].entries,
                                          memory_order_relaxed);
    if (g_thread_state.generation == generation)
        value += g_thread_state.entries[tag];
    return value;
}
#endif
