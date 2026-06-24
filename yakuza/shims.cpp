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
#include <cstdlib>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
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

static void yz_mem_guard(uint32_t a, unsigned w, int is_write);  /* DIAG (YZ_GUARD), def below */

extern "C" uint8_t  vm_read8 (uint64_t addr) { yz_mem_guard((uint32_t)addr,1,0); return *ea(addr); }
extern "C" uint16_t vm_read16(uint64_t addr) { yz_mem_guard((uint32_t)addr,2,0); uint16_t v; memcpy(&v, ea(addr), 2); return ps3_bswap16(v); }
extern "C" uint32_t vm_read32(uint64_t addr) { yz_mem_guard((uint32_t)addr,4,0); uint32_t v; memcpy(&v, ea(addr), 4); return ps3_bswap32(v); }
extern "C" uint64_t vm_read64(uint64_t addr) { yz_mem_guard((uint32_t)addr,8,0); uint64_t v; memcpy(&v, ea(addr), 8); return ps3_bswap64(v); }

/* PPU<->SPU lock-line coherence (1f, spu_channels.c): a PPU write to a 128-byte
 * line the SPURS kernel has reserved (GETLLAR) must serialize through the SPU
 * lock-line so the kernel's PUTLLC memcmp sees it (else the bump is clobbered).
 * Fast path: spu_coh_is_reserved() is a range check + O(1) bitmap lookup, so the
 * overwhelming majority of writes (stack/game data/cmd buffer) skip the lock. */
extern "C" int  spu_coh_is_reserved(uint32_t addr);
extern "C" void spu_lockline_lock(void);
extern "C" void spu_lockline_unlock(void);
/* DIAG (env YZ_WATCH_BD): catch any PPU store covering spurs+0xBD
 * (sysSrvMsgUpdateWorkload @ 0x40197D3D) to find who writes the bad 0xE0. */
extern "C" void yz_watch_bd(uint32_t addr, const void* src, unsigned n);
/* Raise SPU_EVENT_LR on any SPU whose GETLLAR reservation covers this line --
 * the SPURS idle-service wakeup (spu_channels.c). */
extern "C" void spu_coh_notify_write(uint32_t addr);
#define VM_WRITE_COH(addr, src, n) do { \
        yz_watch_bd((uint32_t)(addr), (src), (n)); \
        if (spu_coh_is_reserved((uint32_t)(addr))) { \
            spu_lockline_lock(); memcpy(ea(addr), (src), (n)); \
            spu_coh_notify_write((uint32_t)(addr)); spu_lockline_unlock(); \
        } else memcpy(ea(addr), (src), (n)); } while (0)
extern "C" uint32_t yz_guest_addr_from_host(const void* rip);
extern "C" void yz_watch_bd(uint32_t addr, const void* src, unsigned n) {
    static int on = -1; if (on < 0) on = getenv("YZ_WATCH_BD") ? 1 : 0;
    if (!on) return;
    static unsigned long seq = 0; seq++;
    struct { uint32_t a; const char* nm; } tg[] = {
        /* pt35e: the codec workload's eligibility (wklSignal1 @ spurs+0x70=0x40197CF0,
         * readyCount[wid3] @ spurs+0x03=0x40197C83) + the codec taskset's pending_ready
         * (0x63D22580+0x20=0x63D225A0). The stack-walk names the LIFTED caller (task_start
         * / SendWorkloadSignal), not vm_write32. */
        {0x40197CF0u, "wklSignal1"}, {0x40197CF1u, "wklSignal1+1"}, {0x40197C83u, "readyCount[3]"},
        {0x63D225A0u, "codec.pending_ready"},
    };
    for (auto& t : tg) {
        if (addr <= t.a && t.a < addr + n) {
            uint8_t b = ((const uint8_t*)src)[t.a - addr];
            void* bt[20]; unsigned short got = RtlCaptureStackBackTrace(1, 20, bt, 0);
            char chain[256]; int ci = 0; chain[0] = 0;
            for (unsigned k = 0; k < got && ci < 230; k++) {
                uint32_t g = yz_guest_addr_from_host(bt[k]);
                if (g) ci += snprintf(chain + ci, sizeof(chain) - (size_t)ci, " %08X", g);
            }
            fprintf(stderr, "[watch] #%lu %-19s <- 0x%02X (n=%u) guest-callers:%s\n",
                    seq, t.nm, b, n, chain);
            fflush(stderr);
        }
    }
}
extern "C" void vm_write8 (uint64_t addr, uint8_t  val) { yz_mem_guard((uint32_t)addr,1,1); VM_WRITE_COH(addr, &val, 1); }
extern "C" void vm_write16(uint64_t addr, uint16_t val) { yz_mem_guard((uint32_t)addr,2,1); uint16_t v = ps3_bswap16(val); VM_WRITE_COH(addr, &v, 2); }
extern "C" void vm_write32(uint64_t addr, uint32_t val) { yz_mem_guard((uint32_t)addr,4,1); uint32_t v = ps3_bswap32(val); VM_WRITE_COH(addr, &v, 4); }
extern "C" void vm_write64(uint64_t addr, uint64_t val) { yz_mem_guard((uint32_t)addr,8,1); uint64_t v = ps3_bswap64(val); VM_WRITE_COH(addr, &v, 8); }

/* DIAG (env YZ_GUARD): catch a wild out-of-range guest access -- the firmware
 * coin-flip crasher. On a null-page or uncommitted EA, log the faulting LIFTED
 * guest-caller chain (host-stack walk -> yz_guest_addr_from_host, the proven
 * yz_watch_bd pattern), the bad EA, width and direction, then RETURN so the
 * natural AV fires and the crash handler still runs with the real RIP. Env-gated
 * because the per-access VirtualQuery is too slow for an always-on path. */
static void yz_mem_guard(uint32_t a, unsigned w, int is_write) {
    static int on = -1; if (on < 0) on = getenv("YZ_GUARD") ? 1 : 0;
    if (!on) return;
    int bad = 0;
    if (a < 0x10000u) bad = 1;                          /* null page (cheap; the r2=0 TOC case) */
    else {
        /* Full commit check only under YZ_GUARD_FULL -- VirtualQuery per access is
         * far too slow to reach the movie stage otherwise. */
        static int full = -1; if (full < 0) full = getenv("YZ_GUARD_FULL") ? 1 : 0;
        if (full) {
            uint8_t* hp = vm_base + a;
            /* per-thread last-good committed region cache: hot loops skip the syscall */
            static thread_local uintptr_t clo = 1, chi = 0;
            if ((uintptr_t)hp >= clo && (uintptr_t)hp + w <= chi) { /* cached-OK */ }
            else {
                MEMORY_BASIC_INFORMATION mbi;
                if (VirtualQuery(hp, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) bad = 1;
                else { clo = (uintptr_t)mbi.BaseAddress; chi = clo + mbi.RegionSize; }
            }
        }
    }
    if (!bad) return;
    static long seq = 0; if (++seq > 40) return;        /* cap the spam */
    void* bt[24]; unsigned short got = RtlCaptureStackBackTrace(1, 24, bt, 0);
    char chain[256]; int ci = 0; chain[0] = 0;
    for (unsigned k = 0; k < got && ci < 230; k++) {
        uint32_t g = yz_guest_addr_from_host(bt[k]);
        if (g) ci += snprintf(chain + ci, sizeof(chain) - (size_t)ci, " %08X", g);
    }
    fprintf(stderr, "[wild] #%ld %-5s EA=0x%08X w=%u tid=%lu guest-callers:%s\n",
            seq, is_write ? "WRITE" : "READ", a, w, (unsigned long)GetCurrentThreadId(), chain);
    fflush(stderr);
}

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

    /* sys_rsx (668-677): the RSX driver syscalls Sony's LLE libgcm issues via
     * `sc`. They build the RsxDriverInfo/dma_control/reports guest contract and
     * drive flip/vblank completion (import_overrides.cpp). */
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
    /* TEMP DEBUG (SPURS bring-up): log the first call of each syscall
     * number with args + result so silent failures inside the LLE module
     * are visible. Strip once SPURS init survives. */
    uint32_t num = (uint32_t)ctx->gpr[11];

    /* sys_dbg_* (972, 0x3CC): RPCS3 stubs this as null_func (no-op success);
     * Sony's libgcm calls it on the interrupt path and IGNORES the result, but
     * our default ENOSYS spams the log. Match the oracle: succeed silently. */
    if (num == 972) { ctx->gpr[3] = 0; return; }

    /* DIAG (flip-wait hunt): where does the render thread (t1) call usleep from?
     * lr = caller of the usleep loop = t1's actual spin. r4/r5 carry the address
     * it is polling. This pins t1's loop (the global indirect ring is not
     * per-thread, so it mis-attributed t7's pump calls to t1). */
    if (num == 141 && yz_thread_current_id() == 1) {
        static int n = 0;
        if (n < 16) { n++;
            fprintf(stderr, "[diag t1] usleep(%lld) lr=0x%08llX r4=0x%llX r5=0x%llX\n",
                    (long long)ctx->gpr[3], (unsigned long long)ctx->lr,
                    (unsigned long long)ctx->gpr[4], (unsigned long long)ctx->gpr[5]);
            fflush(stderr);
        }
    }

    static unsigned char seen[1100];
    int first = (num < sizeof(seen)) && !seen[num];
    /* SPU-management family: log every call (SPURS bring-up), not just the
     * first — except the usleep-class noise. */
    int spu_range = (num >= 82 && num <= 200 && num != 141 && num != 145 &&
                     num != 147);
    /* DIAG (vblank-dispatch hunt): log EVERY syscall the gcm interrupt thread
     * (t7) makes, so we can see whether it does anything beyond sc 130 receive
     * (e.g. a semaphore_post / lwcond_signal == the handler waking the render
     * thread). usleep-class excluded to avoid drowning the signal. */
    int intr = (yz_thread_current_id() == 7) && num != 141 && num != 145 &&
               num != 147 && num != 130;  /* drop sc130 receive spam */
    uint64_t a3 = ctx->gpr[3], a4 = ctx->gpr[4], a5 = ctx->gpr[5], a6 = ctx->gpr[6];

    /* Record the in-flight syscall so a stall dump can name exactly what each
     * blocked thread is parked in (object-id args survive in the GPRs while the
     * dispatch below blocks). Cleared on return. (blocker #21 diagnostic) */
    yz_wait_enter(num, a3, a4, a5);
    lv2_syscall_dispatch(&g_lv2_syscalls, ctx);
    yz_wait_exit();

    if (first || spu_range || intr) {
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
