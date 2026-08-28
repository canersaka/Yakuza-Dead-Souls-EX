/* Sparse exact guest-read notification for deferred RSX reports. */
#ifndef PS3RECOMP_PPU_GUEST_READ_H
#define PS3RECOMP_PPU_GUEST_READ_H

#include <stdint.h>
#include <stdatomic.h>
#ifdef __cplusplus
#  include <atomic>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PS3RECOMP_NATIVE_REPORT_READ_WATCH
typedef void (*vm_native_report_read_fn)(
    void* user, uint32_t ea, uint32_t size, uint32_t source);

extern volatile uint64_t g_native_report_read_page_bits[16384];
extern volatile uint32_t g_native_report_read_enabled;
void vm_native_report_set_read_observer(
    vm_native_report_read_fn observer, void* user);
void vm_native_report_watch_read_range(uint32_t ea, uint32_t size);
void vm_native_report_clear_read_watches(void);
void vm_native_report_notify_read_slow(
    uint32_t ea, uint32_t size, uint32_t source);

/* Independent exact-range hook for GPU-owned guest resources.  Unlike the
 * report observer this sees both reads and pre-publication writes and carries
 * the SPU image/PC/command identity when the access is an MFC operation.
 * Its disabled path is one cached flag test; enabled unrelated accesses use
 * the same sparse 4 KiB page rejection as deferred reports. */
typedef void (*vm_native_residency_access_fn)(
    void* user, uint32_t ea, uint32_t size, uint32_t source,
    uint32_t write, uint32_t image_id, uint32_t task_id,
    uint32_t pc, uint32_t command);
#define VM_NATIVE_RESIDENCY_READ        0u
#define VM_NATIVE_RESIDENCY_WRITE_BEGIN 1u
#define VM_NATIVE_RESIDENCY_WRITE_END   2u
extern volatile uint64_t g_native_residency_page_bits[16384];
extern volatile uint32_t g_native_residency_enabled;
void vm_native_residency_set_observer(
    vm_native_residency_access_fn observer, void* user);
void vm_native_residency_watch_range(uint32_t ea, uint32_t size);
void vm_native_residency_clear_watches(void);
void vm_native_residency_notify_slow(
    uint32_t ea, uint32_t size, uint32_t source, uint32_t write,
    uint32_t image_id, uint32_t task_id, uint32_t pc, uint32_t command);

static inline void vm_native_residency_notify(
    uint32_t ea, uint32_t size, uint32_t source, uint32_t write,
    uint32_t image_id, uint32_t task_id, uint32_t pc, uint32_t command)
{
    if (!size)
        return;
#ifdef __cplusplus
    const uint32_t enabled = std::atomic_ref<uint32_t>(
        const_cast<uint32_t&>(g_native_residency_enabled)).load(
            std::memory_order_relaxed);
#else
    const uint32_t enabled = atomic_load_explicit(
        (const _Atomic uint32_t*)&g_native_residency_enabled,
        memory_order_relaxed);
#endif
    if (!enabled)
        return;
    const uint64_t end = (uint64_t)ea + size - 1u;
    const uint32_t first = ea >> 12;
    const uint32_t last = (uint32_t)(
        (end > UINT32_MAX ? UINT32_MAX : end) >> 12);
    for (uint32_t page = first; ; ++page) {
#ifdef __cplusplus
        const uint64_t word = std::atomic_ref<uint64_t>(
            const_cast<uint64_t&>(g_native_residency_page_bits[
                page >> 6])).load(std::memory_order_relaxed);
#else
        const uint64_t word = atomic_load_explicit(
            (const _Atomic uint64_t*)&g_native_residency_page_bits[
                page >> 6], memory_order_relaxed);
#endif
        if (word & (1ull << (page & 63u))) {
            vm_native_residency_notify_slow(
                ea, size, source, write, image_id, task_id, pc, command);
            return;
        }
        if (page == last)
            return;
    }
}

static inline uint64_t vm_native_report_read_page_word(uint32_t word)
{
#ifdef __cplusplus
    return std::atomic_ref<uint64_t>(const_cast<uint64_t&>(
        g_native_report_read_page_bits[word])).load(std::memory_order_relaxed);
#else
    return atomic_load_explicit(
        (const _Atomic uint64_t*)&g_native_report_read_page_bits[word],
        memory_order_relaxed);
#endif
}

static inline uint32_t vm_native_report_read_is_enabled(void)
{
#ifdef __cplusplus
    return std::atomic_ref<uint32_t>(const_cast<uint32_t&>(
        g_native_report_read_enabled)).load(std::memory_order_relaxed);
#else
    return atomic_load_explicit(
        (const _Atomic uint32_t*)&g_native_report_read_enabled,
        memory_order_relaxed);
#endif
}

static inline void vm_native_report_notify_read(
    uint32_t ea, uint32_t size, uint32_t source)
{
    /* Default-off path touches one cache-hot word only; it never indexes the
     * sparse page bitmap, takes a clock/lock, allocates, or calls out. */
    if (!size || !vm_native_report_read_is_enabled())
        return;
    const uint64_t end = (uint64_t)ea + size - 1u;
    const uint32_t first = ea >> 12;
    const uint32_t last = (uint32_t)(
        (end > UINT32_MAX ? UINT32_MAX : end) >> 12);
    for (uint32_t page = first; ; ++page) {
        if (vm_native_report_read_page_word(page >> 6) &
            (1ull << (page & 63u))) {
            vm_native_report_notify_read_slow(ea, size, source);
            return;
        }
        if (page == last)
            return;
    }
}
#else
#define vm_native_report_notify_read(ea, size, source) ((void)0)
#define vm_native_residency_notify(ea, size, source, write, image, task, pc, cmd) \
    ((void)0)
#endif

#ifdef __cplusplus
}
#endif
#endif
