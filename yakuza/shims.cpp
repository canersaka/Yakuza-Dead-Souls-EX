/*
 * Glue that the generated PPU code and the runtime expect the game to provide:
 *
 *   - vm_base                 the host mapping of the guest address space
 *   - vm_read* / vm_write*    the extern accessors ppu_recomp.h declares (the
 *                             runtime's versions are static inline, so they
 *                             produce no linkable symbols)
 *   - ppu_res_*               lwarx/ldarx/stwcx./stdcx.
 *   - lv2_syscall(ctx)        where the `sc` instruction lands
 *   - g_lv2_syscalls          the syscall table lv2_syscall_table.h declares
 *
 * This file uses the runtime ppu_context (through lv2_syscall_table.h). The
 * generated context matches it for every field touched here, so do not
 * include ppu_recomp.h.
 */

/* lv2_syscall_table.h defines a static inline lv2_syscall convenience wrapper.
 * The generated code needs an extern symbol of that name, so move the inline
 * one out of the way and define the real one below. */
#define lv2_syscall lv2_syscall_inline_unused
#include "../runtime/syscalls/lv2_syscall_table.h"
#undef lv2_syscall

#include "../include/ps3emu/endian.h"
#include "../runtime/ppu/ppu_guest_write.h"
#include "yakuza_runner.h"

#include <cstring>
#include <cstdio>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" void yz_ppu_cooperative_yield(void)
{
    /* The guest's worker wait is unbounded and its poll count is dead on
     * completion.  Yield after a short processor spin so the producer that
     * clears the completion word can run without changing the wait condition. */
    for (unsigned i = 0; i < 64; ++i)
        YieldProcessor();
    SwitchToThread();
}

/* ---------------------------------------------------------------------------
 * Guest memory (big-endian guest, little-endian host)
 * -----------------------------------------------------------------------*/

extern "C" uint8_t* vm_base = nullptr;

/* The lifted code computes effective addresses in 64 bits, but the PS3 user
 * address space is 32-bit: truncate, as the PPU does in 32-bit mode. */
static inline uint8_t* ea(uint64_t addr) { return vm_base + (uint32_t)addr; }

extern "C" uint8_t vm_read8(uint64_t addr)
{
    return *ea(addr);
}

extern "C" uint16_t vm_read16(uint64_t addr)
{
    uint16_t value;
    memcpy(&value, ea(addr), sizeof(value));
    return ps3_bswap16(value);
}

extern "C" uint32_t vm_read32(uint64_t addr)
{
    uint32_t value;
    memcpy(&value, ea(addr), sizeof(value));
    return ps3_bswap32(value);
}

extern "C" uint64_t vm_read64(uint64_t addr)
{
    uint64_t value;
    memcpy(&value, ea(addr), sizeof(value));
    return ps3_bswap64(value);
}

/* A PPU store to a 128-byte line that an SPU holds under GETLLAR must be
 * serialized with the SPU lock-line, so the SPU's PUTLLC compare sees it and
 * the SPU gets its lost-reservation event (spu_channels.c).  The reservation
 * check is a range test plus a bitmap probe, so ordinary stores never take
 * the lock. */
extern "C" int  spu_coh_is_reserved(uint32_t addr);
extern "C" void spu_lockline_lock(void);
extern "C" void spu_lockline_unlock(void);
extern "C" void spu_coh_notify_write(uint32_t addr);

static inline void store_guest(uint64_t addr, const void* src, uint32_t size)
{
    const uint32_t guest = (uint32_t)addr;
    if (spu_coh_is_reserved(guest)) {
        spu_lockline_lock();
        memcpy(ea(addr), src, size);
        spu_coh_notify_write(guest);
        spu_lockline_unlock();
    } else {
        memcpy(ea(addr), src, size);
    }
    vm_native_spurs_notify_write(guest, size);
}

extern "C" void vm_write8(uint64_t addr, uint8_t val)
{
    store_guest(addr, &val, sizeof(val));
}

extern "C" void vm_write16(uint64_t addr, uint16_t val)
{
    const uint16_t value = ps3_bswap16(val);
    store_guest(addr, &value, sizeof(value));
}

extern "C" void vm_write32(uint64_t addr, uint32_t val)
{
    const uint32_t value = ps3_bswap32(val);
    store_guest(addr, &value, sizeof(value));
}

extern "C" void vm_write64(uint64_t addr, uint64_t val)
{
    const uint64_t value = ps3_bswap64(val);
    store_guest(addr, &value, sizeof(value));
}

/* Raw-byte store for lifted VMX, byte-reversed and cache-block-zero stores.
 * The bytes are already in guest order, but the store must still serialize
 * with every reserved line it overlaps and notify native SPURS watchers. */
extern "C" void vm_write_bytes(uint64_t addr, const void* src, size_t size)
{
    if (!size) return;
    const uint32_t first = (uint32_t)addr;
    const uint64_t end = (uint64_t)first + size;
    if (end > 0x100000000ull) return;

    const uint32_t first_line = first & ~127u;
    const uint32_t last_line = (uint32_t)(end - 1u) & ~127u;
    bool reserved = false;
    for (uint32_t line = first_line;; line += 128u) {
        if (spu_coh_is_reserved(line)) reserved = true;
        if (line == last_line) break;
    }

    if (reserved) spu_lockline_lock();
    memcpy(ea(first), src, size);
    if (reserved) {
        for (uint32_t line = first_line;; line += 128u) {
            if (spu_coh_is_reserved(line)) spu_coh_notify_write(line);
            if (line == last_line) break;
        }
        spu_lockline_unlock();
    }
    vm_native_spurs_notify_write(first, (uint32_t)size);
}

/* ---------------------------------------------------------------------------
 * Reservations (lwarx/ldarx/stwcx./stdcx.)
 *
 * A conditional store succeeds only while the reservation is intact, and any
 * other processor's store to the granule breaks it (PowerISA Book II).  This
 * is modelled by value: lwarx remembers the raw word and stwcx. commits only
 * if memory still holds it, with a host compare-exchange.  Lines in the SPU
 * coherence set commit under the SPU lock-line instead, so a PPU commit cannot
 * land inside an SPU PUTLLC and reserved SPUs are notified.  ABA on a 32- or
 * 64-bit word is acceptable for the lock and counter idioms the guest uses.
 *
 * reserve_addr doubles as the valid flag (0 means no reservation).
 * -----------------------------------------------------------------------*/

extern "C" uint64_t ppu_res_lwarx(ppu_context* ctx, uint64_t addr)
{
    uint32_t raw;
    memcpy(&raw, ea(addr), 4);
    ctx->reserve_addr  = (uint32_t)addr;
    ctx->reserve_value = raw;
    return (uint64_t)ps3_bswap32(raw);
}

extern "C" uint64_t ppu_res_ldarx(ppu_context* ctx, uint64_t addr)
{
    uint64_t raw;
    memcpy(&raw, ea(addr), 8);
    ctx->reserve_addr  = (uint32_t)addr;
    ctx->reserve_value = raw;
    return ps3_bswap64(raw);
}

/* CR0 = 0b00 || store succeeded || XER[SO]. */
static inline void set_store_conditional_cr0(ppu_context* ctx, int ok)
{
    ctx->cr = (ctx->cr & ~(0xFu << 28))
              | (((ok ? 2u : 0u) | ((ctx->xer >> 31) & 1u)) << 28);
}

extern "C" void ppu_res_stwcx(ppu_context* ctx, uint64_t addr, uint32_t val)
{
    const uint32_t guest = (uint32_t)addr;
    int ok = 0;
    if (ctx->reserve_addr && ctx->reserve_addr == guest) {
        const uint32_t expected = (uint32_t)ctx->reserve_value;
        const uint32_t desired  = ps3_bswap32(val);
        volatile long* p = (volatile long*)ea(addr);
        if (spu_coh_is_reserved(guest)) {
            spu_lockline_lock();
            if (*(volatile uint32_t*)p == expected) {
                *(volatile uint32_t*)p = desired;
                spu_coh_notify_write(guest);
                ok = 1;
            }
            spu_lockline_unlock();
        } else {
            ok = _InterlockedCompareExchange(p, (long)desired, (long)expected)
                 == (long)expected;
        }
    }
    if (ok)
        vm_native_spurs_notify_write(guest, 4);
    ctx->reserve_addr = 0;
    set_store_conditional_cr0(ctx, ok);
}

extern "C" void ppu_res_stdcx(ppu_context* ctx, uint64_t addr, uint64_t val)
{
    const uint32_t guest = (uint32_t)addr;
    int ok = 0;
    if (ctx->reserve_addr && ctx->reserve_addr == guest) {
        const uint64_t expected = ctx->reserve_value;
        const uint64_t desired  = ps3_bswap64(val);
        volatile long long* p = (volatile long long*)ea(addr);
        if (spu_coh_is_reserved(guest)) {
            spu_lockline_lock();
            if (*(volatile uint64_t*)p == expected) {
                *(volatile uint64_t*)p = desired;
                spu_coh_notify_write(guest);
                ok = 1;
            }
            spu_lockline_unlock();
        } else {
            ok = _InterlockedCompareExchange64(p, (long long)desired,
                                               (long long)expected)
                 == (long long)expected;
        }
    }
    if (ok)
        vm_native_spurs_notify_write(guest, 8);
    ctx->reserve_addr = 0;
    set_store_conditional_cr0(ctx, ok);
}

/* ---------------------------------------------------------------------------
 * LV2 syscalls
 * -----------------------------------------------------------------------*/

lv2_syscall_table g_lv2_syscalls;

/* Syscall 988 is issued by the CRT abort handler.  Acknowledge it and print
 * the host backtrace, which is the guest call chain that aborted (lifted
 * calls run on the host stack); resolve the RVAs with yakuza_recomp.map. */
static int64_t sc_abort_report(ppu_context* ctx)
{
    fprintf(stderr, "[LV2] abort-report syscall 988 (r3=0x%llX) [host tid %lu]\n",
            (unsigned long long)ctx->gpr[3], GetCurrentThreadId());
    void* frames[48];
    const USHORT n = RtlCaptureStackBackTrace(0, 48, frames, NULL);
    const uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    fprintf(stderr, "[LV2] abort backtrace (rva):");
    for (USHORT i = 0; i < n; i++) {
        const uintptr_t a = (uintptr_t)frames[i];
        if (a >= base && a < base + 0x40000000ull)
            fprintf(stderr, " 0x%llX", (unsigned long long)(a - base));
    }
    fprintf(stderr, "\n");
    return 0;
}

extern "C" void yz_init_syscalls(void)
{
    lv2_register_all_syscalls(&g_lv2_syscalls);
    lv2_syscall_register(&g_lv2_syscalls, 988, sc_abort_report);

    /* PPU thread syscalls go to the game's thread runtime (threads.cpp), so
     * ids, joins and priorities match threads created through the
     * sysPrxForUser imports. */
    lv2_syscall_register(&g_lv2_syscalls, 43, yz_sc_thread_yield);
    lv2_syscall_register(&g_lv2_syscalls, 44, yz_sc_thread_join);
    lv2_syscall_register(&g_lv2_syscalls, 45, yz_sc_thread_detach);
    lv2_syscall_register(&g_lv2_syscalls, 46, yz_sc_thread_get_join_state);
    lv2_syscall_register(&g_lv2_syscalls, 47, yz_sc_thread_set_priority);
    lv2_syscall_register(&g_lv2_syscalls, 48, yz_sc_thread_get_priority);
    lv2_syscall_register(&g_lv2_syscalls, 49, yz_sc_thread_get_stack_information);

    /* sys_rsx (668-677): the RSX driver syscalls libgcm issues. They build the
     * driver info, DMA control and report areas and drive flip and vblank
     * completion (import_overrides.cpp). */
    lv2_syscall_register(&g_lv2_syscalls, 668, yz_sys_rsx_memory_allocate);
    lv2_syscall_register(&g_lv2_syscalls, 669, yz_sys_rsx_memory_free);
    lv2_syscall_register(&g_lv2_syscalls, 670, yz_sys_rsx_context_allocate);
    lv2_syscall_register(&g_lv2_syscalls, 671, yz_sys_rsx_context_free);
    lv2_syscall_register(&g_lv2_syscalls, 672, yz_sys_rsx_context_iomap);
    lv2_syscall_register(&g_lv2_syscalls, 673, yz_sys_rsx_context_iounmap);
    lv2_syscall_register(&g_lv2_syscalls, 674, yz_sys_rsx_context_attribute);
    lv2_syscall_register(&g_lv2_syscalls, 675, yz_sys_rsx_device_map);
    lv2_syscall_register(&g_lv2_syscalls, 676, yz_sys_rsx_device_unmap);
    lv2_syscall_register(&g_lv2_syscalls, 677, yz_sys_rsx_attribute);
}

extern "C" void lv2_syscall(ppu_context* ctx)
{
    const uint32_t num = (uint32_t)ctx->gpr[11];

    /* sys_dbg (972): libgcm calls it on its interrupt path and ignores the
     * result.  Succeed quietly instead of logging ENOSYS every time. */
    if (num == 972) { ctx->gpr[3] = 0; return; }

    /* Record the syscall while it runs so a stall dump can name what each
     * blocked thread is waiting in. */
    yz_wait_enter(num, ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
    lv2_syscall_dispatch(&g_lv2_syscalls, ctx);
    yz_wait_exit();
}
