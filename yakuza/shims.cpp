/*
 * Runtime glue the generated code and the runtime library both expect the
 * game project to provide:
 *
 *   - vm_base                  (declared extern everywhere, defined nowhere)
 *   - vm_read* / vm_write*     (ppu_recomp.h declares them as extern
 *                               functions taking uint64_t; the runtime's
 *                               versions are static inline and take uint32_t,
 *                               so they don't produce linkable symbols)
 *   - lv2_syscall(ctx)         (the `sc` instruction lands here)
 *   - g_lv2_syscalls           (extern in lv2_syscall_table.h, defined here)
 *   - g_ps3_guest_caller       (extern in ps3emu/guest_call.h, defined here;
 *                               installed by main.cpp)
 *
 * NOTE: this TU uses the RUNTIME ppu_context (via lv2_syscall_table.h).
 * The generated context layout matches it for gpr/fpr/vr/cr/lr/ctr, which
 * is what syscall handlers consume. Do not include ppu_recomp.h here.
 */

/* lv2_syscall_table.h defines a `static inline lv2_syscall` convenience
 * wrapper. The generated obj needs an EXTERN symbol of that name, so rename
 * the inline one out of the way for this TU and define the real one below. */
#define lv2_syscall lv2_syscall_inline_unused
#include "../runtime/syscalls/lv2_syscall_table.h"
#undef lv2_syscall

#include "../include/ps3emu/endian.h"
#include "../include/ps3emu/guest_call.h"
#include "yakuza_runner.h"

#include <cstring>
#include <cstdio>
#include <intrin.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ---------------------------------------------------------------------------
 * Guest memory base + accessors (big-endian guest, LE host)
 * -----------------------------------------------------------------------*/

extern "C" uint8_t* vm_base = nullptr;

/* Guest effective addresses are computed in 64-bit by the lifted code but
 * the PS3 user address space is 32-bit: truncate, like the real PPU in
 * 32-bit mode. */
static inline uint8_t* ea(uint64_t addr) { return vm_base + (uint32_t)addr; }

extern "C" uint8_t  vm_read8 (uint64_t addr) { return *ea(addr); }
extern "C" uint16_t vm_read16(uint64_t addr) { uint16_t v; memcpy(&v, ea(addr), 2); return ps3_bswap16(v); }
extern "C" uint32_t vm_read32(uint64_t addr) { uint32_t v; memcpy(&v, ea(addr), 4); return ps3_bswap32(v); }
extern "C" uint64_t vm_read64(uint64_t addr) { uint64_t v; memcpy(&v, ea(addr), 8); return ps3_bswap64(v); }

extern "C" void vm_write8 (uint64_t addr, uint8_t  val) { *ea(addr) = val; }
extern "C" void vm_write16(uint64_t addr, uint16_t val) { uint16_t v = ps3_bswap16(val); memcpy(ea(addr), &v, 2); }
extern "C" void vm_write32(uint64_t addr, uint32_t val) { uint32_t v = ps3_bswap32(val); memcpy(ea(addr), &v, 4); }
extern "C" void vm_write64(uint64_t addr, uint64_t val) { uint64_t v = ps3_bswap64(val); memcpy(ea(addr), &v, 8); }

/* ---------------------------------------------------------------------------
 * LV2 syscall dispatch
 * -----------------------------------------------------------------------*/

lv2_syscall_table g_lv2_syscalls;

/* Syscall 988: issued by the CRT abort handler with diagnostic args (not in
 * any public table; identified from the abort path of this and other titles).
 * Acknowledge it so the abort report doesn't drown in ENOSYS noise. */
static int64_t yz_sc_abort_report(ppu_context* ctx)
{
    fprintf(stderr, "[LV2] abort-report syscall 988 (r3=0x%llX) "
            "[host tid %lu]\n",
            (unsigned long long)ctx->gpr[3], GetCurrentThreadId());
    /* The abort path runs as ordinary lifted calls on the host stack, so a
     * host backtrace here is the guest call chain that aborted. Print RVAs
     * (resolve against yakuza_recomp.map). */
    void* frames[48];
    USHORT n = RtlCaptureStackBackTrace(0, 48, frames, NULL);
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    fprintf(stderr, "[LV2] abort backtrace (rva):");
    for (USHORT i = 0; i < n; i++) {
        uintptr_t a = (uintptr_t)frames[i];
        if (a >= base && a < base + 0x40000000ull)
            fprintf(stderr, " 0x%llX", (unsigned long long)(a - base));
    }
    fprintf(stderr, "\n");
    return 0;
}

extern "C" void yz_init_syscalls(void)
{
    lv2_register_all_syscalls(&g_lv2_syscalls);
    lv2_syscall_register(&g_lv2_syscalls, 988, yz_sc_abort_report);

    /* PPU thread syscalls the game issues directly (43/44/47/48/49):
     * route them to the runner's thread runtime (threads.cpp) instead of
     * the runtime's standalone table, so ids and joins line up with
     * threads created through the sysPrxForUser import overrides. */
    lv2_syscall_register(&g_lv2_syscalls, 43, yz_sc_thread_yield);
    lv2_syscall_register(&g_lv2_syscalls, 44, yz_sc_thread_join);
    lv2_syscall_register(&g_lv2_syscalls, 47, yz_sc_thread_set_priority);
    lv2_syscall_register(&g_lv2_syscalls, 48, yz_sc_thread_get_priority);
    lv2_syscall_register(&g_lv2_syscalls, 49, yz_sc_thread_get_stack_information);
}

extern "C" void lv2_syscall(ppu_context* ctx)
{
    /* TEMP DEBUG (SPURS bring-up): log the first call of each syscall
     * number with args + result so silent failures inside the LLE module
     * are visible. Strip once SPURS init survives. */
    uint32_t num = (uint32_t)ctx->gpr[11];
    static unsigned char seen[1100];
    int first = (num < sizeof(seen)) && !seen[num];
    /* SPU-management family: log every call (SPURS bring-up), not just the
     * first — except the usleep-class noise. */
    int spu_range = (num >= 82 && num <= 200 && num != 141 && num != 145 &&
                     num != 147);
    uint64_t a3 = ctx->gpr[3], a4 = ctx->gpr[4], a5 = ctx->gpr[5], a6 = ctx->gpr[6];

    lv2_syscall_dispatch(&g_lv2_syscalls, ctx);

    if (first || spu_range) {
        seen[num] = 1;
        fprintf(stderr, "[LV2%s t%u] sc %u (r3=0x%llX r4=0x%llX r5=0x%llX r6=0x%llX)"
                " -> 0x%llX\n", first ? ":first" : "",
                yz_thread_current_id(), num,
                (unsigned long long)a3, (unsigned long long)a4,
                (unsigned long long)a5, (unsigned long long)a6,
                (unsigned long long)ctx->gpr[3]);
        fflush(stderr);
    }
}

/* Guest-callback hook g_ps3_guest_caller: defined by the runtime
 * (libs/system/cellSysutil.c), installed by main.cpp. */
