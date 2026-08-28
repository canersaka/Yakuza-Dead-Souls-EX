#include "rsx_nr_residency.h"

#include <limits.h>
#include <string.h>

static int overlap(uint32_t a, uint32_t as, uint32_t b, uint32_t bs)
{
    if (!as || !bs) return 0;
    const uint64_t ae = (uint64_t)a + as;
    const uint64_t be = (uint64_t)b + bs;
    return (uint64_t)a < be && (uint64_t)b < ae;
}

void rsx_nr_residency_init(rsx_nr_residency_slot* slot)
{
    if (slot) memset(slot, 0, sizeof(*slot));
}

int rsx_nr_residency_begin(rsx_nr_residency_slot* slot, uint32_t ea,
                           uint32_t size, uint64_t writer_fence,
                           uint32_t* generation)
{
    if (!slot || !size || (uint64_t)ea + size > (uint64_t)UINT32_MAX + 1u ||
        atomic_load_explicit(&slot->state, memory_order_acquire) !=
            RSX_NR_RESIDENCY_IDLE)
        return -1;
    uint32_t next = atomic_fetch_add_explicit(
        &slot->generation, 1u, memory_order_relaxed) + 1u;
    if (!next) {
        atomic_store_explicit(&slot->generation, 1u, memory_order_relaxed);
        next = 1u;
    }
    for (uint32_t i = 0; i < RSX_NR_RESIDENCY_DIRTY_WORDS; ++i)
        atomic_store_explicit(&slot->dirty_pages[i], 0u,
                              memory_order_relaxed);
    atomic_store_explicit(&slot->guest_ea, ea, memory_order_relaxed);
    atomic_store_explicit(&slot->guest_size, size, memory_order_relaxed);
    atomic_store_explicit(&slot->writer_fence, writer_fence,
                          memory_order_relaxed);
    atomic_store_explicit(&slot->state, RSX_NR_RESIDENCY_GPU_PENDING,
                          memory_order_release);
    if (generation) *generation = next;
    return 0;
}

uint32_t rsx_nr_residency_access(rsx_nr_residency_slot* slot,
                                 uint32_t ea, uint32_t size, int write,
                                 int* need_materialize)
{
    if (need_materialize) *need_materialize = 0;
    if (!slot || !size) return 0;
    uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
    if (state == RSX_NR_RESIDENCY_IDLE) return 0;
    const uint32_t base = atomic_load_explicit(
        &slot->guest_ea, memory_order_relaxed);
    const uint32_t span = atomic_load_explicit(
        &slot->guest_size, memory_order_relaxed);
    if (!overlap(ea, size, base, span)) return 0;
    const uint32_t generation = atomic_load_explicit(
        &slot->generation, memory_order_relaxed);
    atomic_fetch_add_explicit(&slot->access_count, 1u, memory_order_relaxed);
    if (write)
        atomic_fetch_add_explicit(&slot->write_count, 1u,
                                  memory_order_relaxed);
    if (state == RSX_NR_RESIDENCY_GPU_PENDING) {
        uint32_t expected = RSX_NR_RESIDENCY_GPU_PENDING;
        atomic_compare_exchange_strong_explicit(
            &slot->state, &expected, RSX_NR_RESIDENCY_MATERIALIZING,
            memory_order_acq_rel, memory_order_acquire);
        state = atomic_load_explicit(&slot->state, memory_order_acquire);
    }
    if (need_materialize)
        *need_materialize = state == RSX_NR_RESIDENCY_MATERIALIZING;
    return generation;
}

int rsx_nr_residency_mark_coherent(rsx_nr_residency_slot* slot,
                                   uint32_t generation)
{
    if (!slot || !generation || generation != atomic_load_explicit(
            &slot->generation, memory_order_acquire))
        return -1;
    uint32_t expected = RSX_NR_RESIDENCY_MATERIALIZING;
    if (atomic_compare_exchange_strong_explicit(
            &slot->state, &expected, RSX_NR_RESIDENCY_GUEST_COHERENT,
            memory_order_release, memory_order_acquire))
        return 0;
    return expected == RSX_NR_RESIDENCY_GUEST_COHERENT ||
           expected == RSX_NR_RESIDENCY_GUEST_DIRTY ? 0 : -1;
}

int rsx_nr_residency_mark_dirty(rsx_nr_residency_slot* slot,
                                uint32_t generation,
                                uint32_t ea, uint32_t size)
{
    if (!slot || !generation || !size || generation != atomic_load_explicit(
            &slot->generation, memory_order_acquire))
        return -1;
    const uint32_t base = atomic_load_explicit(
        &slot->guest_ea, memory_order_relaxed);
    const uint32_t span = atomic_load_explicit(
        &slot->guest_size, memory_order_relaxed);
    if (!overlap(ea, size, base, span)) return -1;
    const uint64_t begin = ea > base ? ea : base;
    const uint64_t end0 = (uint64_t)ea + size;
    const uint64_t end1 = (uint64_t)base + span;
    const uint32_t first = (uint32_t)((begin - base) >> 12);
    const uint32_t last = (uint32_t)(((end0 < end1 ? end0 : end1) -
                                      base - 1u) >> 12);
    for (uint32_t page = first; page <= last &&
         page < RSX_NR_RESIDENCY_DIRTY_WORDS * 64u; ++page)
        atomic_fetch_or_explicit(&slot->dirty_pages[page >> 6],
                                 1ull << (page & 63u),
                                 memory_order_relaxed);
    uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
    for (;;) {
        if (state == RSX_NR_RESIDENCY_GUEST_DIRTY) return 0;
        if (state != RSX_NR_RESIDENCY_GUEST_COHERENT) return -1;
        if (atomic_compare_exchange_weak_explicit(
                &slot->state, &state, RSX_NR_RESIDENCY_GUEST_DIRTY,
                memory_order_release, memory_order_acquire))
            return 0;
    }
}

int rsx_nr_residency_finish(rsx_nr_residency_slot* slot,
                            uint32_t generation)
{
    if (!slot || !generation || generation != atomic_load_explicit(
            &slot->generation, memory_order_acquire))
        return -1;
    const uint32_t prior = atomic_exchange_explicit(
        &slot->state, RSX_NR_RESIDENCY_IDLE, memory_order_acq_rel);
    return prior == RSX_NR_RESIDENCY_IDLE ? -1 : 0;
}

void rsx_nr_residency_reset(rsx_nr_residency_slot* slot)
{
    if (!slot) return;
    atomic_fetch_add_explicit(&slot->reset_generation, 1u,
                              memory_order_relaxed);
    atomic_store_explicit(&slot->state, RSX_NR_RESIDENCY_IDLE,
                          memory_order_release);
}
