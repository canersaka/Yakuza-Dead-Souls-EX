/*
 * Sparse post-publication guest-write notification shared by generated PPU
 * stores and host HLE bulk writers.
 *
 * The 4 GiB guest address space is represented by one bit per 4 KiB page.
 * When no native SPURS/graphics waiter has registered a page, the common path
 * is only relaxed atomic loads and returns without entering the exact router.
 */
#ifndef PS3RECOMP_PPU_GUEST_WRITE_H
#define PS3RECOMP_PPU_GUEST_WRITE_H

#include <stdint.h>
#include <stdatomic.h>
#ifdef __cplusplus
#  include <atomic>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PS3RECOMP_NATIVE_SPURS_PPU_WATCH
extern volatile uint64_t g_native_spurs_watch_page_bits[16384];
void cellSpursNotifyPpuGuestWrite(uint32_t ea, uint32_t size);

static inline uint64_t vm_native_spurs_watch_page_word(uint32_t word)
{
#ifdef __cplusplus
    return std::atomic_ref<uint64_t>(const_cast<uint64_t&>(
        g_native_spurs_watch_page_bits[word])).load(std::memory_order_relaxed);
#else
    return atomic_load_explicit(
        (const _Atomic uint64_t*)&g_native_spurs_watch_page_bits[word],
        memory_order_relaxed);
#endif
}

static inline void vm_native_spurs_notify_write(uint32_t addr, uint32_t size)
{
    if (!size) return;
    const uint64_t end = (uint64_t)addr + size - 1u;
    const uint32_t first_page = addr >> 12;
    const uint32_t last_page =
        (uint32_t)((end > UINT32_MAX ? UINT32_MAX : end) >> 12);
    for (uint32_t page = first_page; ; ++page) {
        if (vm_native_spurs_watch_page_word(page >> 6) &
            (1ull << (page & 63u))) {
            cellSpursNotifyPpuGuestWrite(addr, size);
            return;
        }
        if (page == last_page) return;
    }
}
#else
#define vm_native_spurs_notify_write(addr, size) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
