#include "ppu_guest_read.h"

#ifdef PS3RECOMP_NATIVE_REPORT_READ_WATCH
volatile uint64_t g_native_report_read_page_bits[16384];
volatile uint32_t g_native_report_read_enabled;
static _Atomic(vm_native_report_read_fn) g_read_observer;
static _Atomic(void*) g_read_user;

void vm_native_report_set_read_observer(
    vm_native_report_read_fn observer, void* user)
{
    if (!observer) {
        atomic_store_explicit(
            (_Atomic uint32_t*)&g_native_report_read_enabled,
            0u, memory_order_release);
        atomic_store_explicit(
            &g_read_observer, (vm_native_report_read_fn)0,
            memory_order_release);
        atomic_store_explicit(
            &g_read_user, (void*)0, memory_order_relaxed);
        return;
    }
    atomic_store_explicit(&g_read_user, user, memory_order_relaxed);
    atomic_store_explicit(&g_read_observer, observer, memory_order_release);
    atomic_store_explicit(
        (_Atomic uint32_t*)&g_native_report_read_enabled,
        1u, memory_order_release);
}

void vm_native_report_watch_read_range(uint32_t ea, uint32_t size)
{
    if (!size)
        return;
    const uint64_t end = (uint64_t)ea + size - 1u;
    const uint32_t first = ea >> 12;
    const uint32_t last = (uint32_t)(
        (end > UINT32_MAX ? UINT32_MAX : end) >> 12);
    for (uint32_t page = first; ; ++page) {
        atomic_fetch_or_explicit(
            (_Atomic uint64_t*)&g_native_report_read_page_bits[page >> 6],
            1ull << (page & 63u), memory_order_release);
        if (page == last)
            return;
    }
}

void vm_native_report_clear_read_watches(void)
{
    for (uint32_t word = 0;
         word < (uint32_t)(sizeof(g_native_report_read_page_bits) /
                           sizeof(g_native_report_read_page_bits[0]));
         ++word)
        atomic_store_explicit(
            (_Atomic uint64_t*)&g_native_report_read_page_bits[word],
            0u, memory_order_release);
}

void vm_native_report_notify_read_slow(
    uint32_t ea, uint32_t size, uint32_t source)
{
    vm_native_report_read_fn observer = atomic_load_explicit(
        &g_read_observer, memory_order_acquire);
    if (observer)
        observer(atomic_load_explicit(&g_read_user, memory_order_relaxed),
                 ea, size, source);
}
#endif
