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
#include "../runtime/memory/vm.h"
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
/* DIAG (env YZ_VMGUARD / YZ_VMGUARD_SURVIVE): returns 1 if `a` is OUTSIDE every
 * committed guest region (wild), def below. Named-caller logger + optional
 * survive (skip the deref) for the intermittent mixer/CRI wild-read crash. */
static int yz_vmguard_check(uint32_t a, unsigned w, int is_write);

extern "C" uint8_t  vm_read8 (uint64_t addr) { yz_mem_guard((uint32_t)addr,1,0); if (yz_vmguard_check((uint32_t)addr,1,0)) return 0; return *ea(addr); }
extern "C" uint16_t vm_read16(uint64_t addr) { yz_mem_guard((uint32_t)addr,2,0); if (yz_vmguard_check((uint32_t)addr,2,0)) return 0; uint16_t v; memcpy(&v, ea(addr), 2); return ps3_bswap16(v); }
extern "C" uint32_t vm_read32(uint64_t addr) { yz_mem_guard((uint32_t)addr,4,0); if (yz_vmguard_check((uint32_t)addr,4,0)) return 0; uint32_t v; memcpy(&v, ea(addr), 4); return ps3_bswap32(v); }
extern "C" uint64_t vm_read64(uint64_t addr) { yz_mem_guard((uint32_t)addr,8,0); if (yz_vmguard_check((uint32_t)addr,8,0)) return 0; uint64_t v; memcpy(&v, ea(addr), 8); return ps3_bswap64(v); }

/* PPU<->SPU lock-line coherence (1f, spu_channels.c): a PPU write to a 128-byte
 * line the SPURS kernel has reserved (GETLLAR) must serialize through the SPU
 * lock-line so the kernel's PUTLLC memcmp sees it (else the bump is clobbered).
 * Fast path: spu_coh_is_reserved() is a range check + O(1) bitmap lookup, so the
 * overwhelming majority of writes (stack/game data/cmd buffer) skip the lock. */
extern "C" int  spu_coh_is_reserved(uint32_t addr);
extern "C" void spu_lockline_lock(void);
extern "C" void spu_lockline_unlock(void);
extern "C" uint32_t yz_thread_current_id(void);   /* threads.cpp; the mgmt-CAS diag */
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
        /* 2026-07-05: the pxd/wid0 dispatch root — who bumps wid0's workload readyCount
         * (@0x40197C80+0) + wklSignal1 bit0, vs the WORKING wid2/gs_task (@+0x02)? */
        {0x40197C80u, "readyCount[0]=wid0"}, {0x40197C82u, "readyCount[2]=wid2"},
        /* 2026-07-05 (YZ_SWS_WATCH task): the pxd TASKSET's own bitset words, one
         * level upstream of cellSpursSendWorkloadSignal. Layout per
         * docs/SPURS_TASKSET.md (CellSpursTaskset, base 0x40199D00): running@0x00,
         * ready@0x10, pending_ready@0x20, enabled@0x30, signalled@0x40, waiting@0x50
         * (each a 4xu32 bitset, one bit per taskId, task 0's bit = MSB of word0).
         * Does _spurs::task_start / cellSpursCreateTask ever flip pending_ready/
         * enabled for THIS taskset (the thing that must precede any
         * SendWorkloadSignal(wid0) call), or does it stay all-zero all boot? */
        {0x40199D00u, "pxd.running[0]"},
        {0x40199D20u, "pxd.pending_ready[0]"},
        {0x40199D30u, "pxd.enabled[0]"},
        {0x40199D40u, "pxd.signalled[0]"},
        {0x40199D74u, "pxd.wid"},
        /* 2026-07-05: the CRI voice-readiness predicate fields (adxm 0x01613368
         * +294/+298/+29C, all measured 0 at the gate). Does the voice-decode
         * PRODUCER ever write them (→ what value = the ready predicate), or are
         * they never written (→ producer never runs)? Names the writer's caller. */
        {0x016135FCu, "adxm+294"},
        {0x01613600u, "adxm+298"},
        {0x01613604u, "adxm+29C"},
        /* 2026-07-05: cri_dlg (ctrl=0x01661470) dispatcher. The producer registers a
         * callback at ctrl+0x40 but never sets the pending-flag ctrl+0x4=1, so cri_dlg
         * never runs it. Watch the flag (+4) AND the callback field (+40): the +40 writer
         * IS the producer — its caller chain names the fn; check if it also writes +4=1
         * (missing/mis-lifted store = the bug). */
        {0x01661474u, "cridlg.workflag+4"},
        {0x016614B0u, "cridlg.callback+40"},
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
            /* 2026-07-05: also print the full write span (up to 8B) as hex --
             * a single byte can't tell us e.g. WHICH wid got stamped into
             * taskset+0x74 (be_t<u32>), only that byte0 changed. */
            char full[24]; int fi = 0; unsigned fn = n < 8 ? n : 8;
            for (unsigned k = 0; k < fn; k++)
                fi += snprintf(full + fi, sizeof(full) - (size_t)fi, "%02X", ((const uint8_t*)src)[k]);
            fprintf(stderr, "[watch] #%lu %-19s <- 0x%02X (n=%u full=%s) guest-callers:%s\n",
                    seq, t.nm, b, n, full, chain);
            fflush(stderr);
        }
    }
}

/* ---------------------------------------------------------------------------
 * PPU differential trace (docs/TRACEDIFF.md). Emitted by the --trace lifter
 * build before EVERY instruction. Gated to ONE thread (env YZ_PPU_TRACE +
 * YZ_PPU_TRACE_TID, default 1 -- set 11 for cri_dlg) so the log isn't a
 * multi-thread interleave; optionally armed on the first hit of a start PC
 * (YZ_PPU_TRACE_ARM=0xADDR) to scope to a window; bounded (YZ_PPU_TRACE_N,
 * default 3M). One PC per line (hex) -> scratch/ppu_trace.txt, the exact
 * format tools/tracediff.py + the RPCS3 emitter both use. ctx is ignored
 * (PC-only). Inert in the normal (non-trace) build -- the calls aren't emitted.
 * -----------------------------------------------------------------------*/
extern "C" void ppu_trace_pc(void* ctxv, uint32_t pc) {
    /* DIAG (env YZ_ARM_PC=0xADDR): step-1b producer catcher. On the --trace build
     * ppu_trace_pc runs before EVERY instr on EVERY thread, so this catches WHICH
     * thread executes a target PC (the cri_dlg enqueue func_00F00580 @ 0xF00580),
     * printing tid + r3(ctrl)/r5(arg=request record) + the request's BE pos/len
     * (rec+0x10 / rec+0x18, my producer-driver decode) + the guest caller chain.
     * Answers "who arms cri_dlg's work flag 0x01661474" WITHOUT a page-guard, and
     * shows whether the stream position advances across the ~51 arms then stalls.
     * Zero cost when unset. Retire once the producer stall is rooted. */
    {
        static int armc = -1; static uint32_t armpc2 = 0;
        if (armc < 0) { const char* e = getenv("YZ_ARM_PC");
                        armpc2 = e ? (uint32_t)strtoul(e, 0, 0) : 0u; armc = armpc2 ? 1 : 0; }
        if (armc && pc == armpc2) {
            static unsigned long an = 0;
            if (an < 120) { an++;
                uint64_t* gg = (uint64_t*)ctxv;   /* gpr[] is offset 0 of ppu_context */
                uint32_t ctrl = (uint32_t)gg[3], arg = (uint32_t)gg[5];
                unsigned long long posv = (unsigned long long)vm_read64(arg + 0x10);
                unsigned long long lenv = (unsigned long long)vm_read64(arg + 0x18);
                void* bt[20]; unsigned short got = RtlCaptureStackBackTrace(1, 20, bt, 0);
                char chain[256]; int ci = 0; chain[0] = 0;
                for (unsigned k = 0; k < got && ci < 230; k++) {
                    uint32_t gpc = yz_guest_addr_from_host(bt[k]);
                    if (gpc) ci += snprintf(chain + ci, sizeof(chain) - (size_t)ci, " %08X", gpc);
                }
                fprintf(stderr, "[arm-pc] #%lu pc=%08X tid=%u r3=%08X r5=%08X pos=%llX len=%llX callers:%s\n",
                        an, pc, yz_thread_current_id(), ctrl, arg, posv, lenv, chain);
                fflush(stderr);
            }
        }
    }
    static int mode = -1;
    if (mode < 0) mode = getenv("YZ_PPU_TRACE") ? 1 : 0;
    if (!mode) return;
    static int ttid = -2;
    if (ttid == -2) { const char* e = getenv("YZ_PPU_TRACE_TID"); ttid = e ? (int)strtol(e, 0, 0) : 1; }
    if ((int)yz_thread_current_id() != ttid) return;
    static int armed = -1; static uint32_t armpc = 0;
    if (armed < 0) { const char* a = getenv("YZ_PPU_TRACE_ARM");
                     armpc = a ? (uint32_t)strtoul(a, 0, 0) : 0u; armed = armpc ? 0 : 1; }
    if (!armed) { if (pc != armpc) return; armed = 1; }
    static FILE* fp = nullptr; static long budget = -1;
    if (!fp) { fp = fopen("scratch/ppu_trace.txt", "w"); if (!fp) fp = stderr;
               setvbuf(fp, nullptr, _IONBF, 0);   /* unbuffered: last line survives a kill,
                                                   * so the real block point (not a stale flushed
                                                   * PC) is visible. */
               const char* n = getenv("YZ_PPU_TRACE_N"); budget = n ? strtol(n, 0, 0) : 3000000; }
    if (budget <= 0) return;
    fprintf(fp, "%08X\n", pc);
    if (--budget == 0) fflush(fp);
}

/* ---------------------------------------------------------------------------
 * PPU reservation atomics (lwarx/stwcx/ldarx/stdcx) -- the REAL semantics.
 *
 * The old lifter emission was an address-only per-thread reservation with a
 * PLAIN store on "success": any concurrent writer (another PPU thread, or an
 * SPU PUTLLC on the same word) between lwarx and stwcx was silently reverted
 * -- a lost-update race whose probability scales with traffic on the line.
 * MEASURED 2026-07-03 (CBEA audit P1 + the wid-0 frontier): Sony's PPU-side
 * SendWorkloadSignal CAS loop on the hot SPURS mgmt line 0x40197C80 lost its
 * readyCount update to the four SPU kernels hammering that line, so the
 * image-5 IO-service taskset was never selected and the asset pipeline froze.
 *
 * Real rule (PowerISA Book II): stwcx. succeeds only if the reservation is
 * intact, and ANY other processor's store to the granule kills it. We model
 * that value-based: remember the raw word at lwarx, commit at stwcx only if
 * it is still byte-identical (x86 lock cmpxchg). For lines in the SPU
 * lock-line coherence set, commit under spu_lockline_lock and notify -- the
 * same serialization vm_write32 uses -- so a PPU commit can never land inside
 * an SPU PUTLLC's memcmp/memcpy window (and vice versa), and reserved SPUs
 * get their lost-reservation event. ABA on a 32/64-bit word is acceptable
 * for the lock/CAS idioms lv2-era code uses (same model as ppu_memory.h's
 * reference helpers and other recompilers).
 *
 * reserve_addr doubles as the valid flag (0 = none), matching the old
 * emission's convention and the generated ppu_context mirror. */
extern "C" uint64_t ppu_res_lwarx(ppu_context* ctx, uint64_t addr)
{
    uint32_t raw;
    memcpy(&raw, ea(addr), 4);            /* x86 aligned load = acquire */
    ctx->reserve_addr  = (uint32_t)addr;
    ctx->reserve_value = raw;             /* raw guest-BE word */
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

extern "C" void ppu_res_stwcx(ppu_context* ctx, uint64_t addr, uint32_t val)
{
    int ok = 0;
    if (ctx->reserve_addr && ctx->reserve_addr == (uint32_t)addr) {
        uint32_t expected = (uint32_t)ctx->reserve_value;   /* raw BE */
        uint32_t desired  = ps3_bswap32(val);
        volatile long* p  = (volatile long*)ea(addr);
        if (spu_coh_is_reserved((uint32_t)addr)) {
            spu_lockline_lock();
            if (*(volatile uint32_t*)p == expected) {
                *(volatile uint32_t*)p = desired;
                spu_coh_notify_write((uint32_t)addr);
                ok = 1;
            }
            spu_lockline_unlock();
        } else {
            ok = (_InterlockedCompareExchange(p, (long)desired, (long)expected)
                  == (long)expected);
        }
        /* DIAG (env YZ_MGMT_CAS, 2026-07-03 wid-0 fork a/c discriminator —
         * REMOVE with the pxd-dispatch frontier): every PPU stwcx on the
         * SPURS mgmt line, with old/new/ok. If readyCount commits land here
         * but wid 0 never dispatches, the bug moved to the kernel select's
         * wid-0 path; if no commit ever targets readyCount[0], the signal
         * path bails before its CAS. */
        { static int mc = -1; if (mc < 0) mc = getenv("YZ_MGMT_CAS") ? 1 : 0;
          if (mc && (((uint32_t)addr) & ~127u) == 0x40197C80u) {
              /* wid-0 wklSignal bit (0x80000000 at +0x70 = 0x40197CF0) is the
               * IO-service wake -- log it UNCAPPED (rare; the per-word caps hid
               * it behind wid-1/3/4 signal noise, 2026-07-03). Everything else
               * per-word capped. */
              uint32_t w0sig = (((uint32_t)addr) == 0x40197CF0u)
                               && (val & 0x80000000u) && !(ps3_bswap32(expected) & 0x80000000u);
              static long n[32] = {0};
              int slot = ((uint32_t)addr >> 2) & 31;
              if (w0sig || n[slot] < 24) { if (!w0sig) n[slot]++;
                  /* host retaddr = the generated function containing the stwcx
                   * (guest lr is 0 on this path -- bctr chains don't write it);
                   * resolve via the linker map (2026-07-03 late: localizes OUR
                   * doorbell's function; the publish-vs-signal branch is above it). */
                  fprintf(stderr, "[mgmt-cas]%s t%u addr=0x%08X old=%08X new=%08X %s lr=0x%08X host=%p\n",
                          w0sig ? " WID0-SIGNAL" : "", yz_thread_current_id(),
                          (uint32_t)addr, ps3_bswap32(expected), val, ok ? "OK" : "FAIL",
                          (uint32_t)ctx->lr, _ReturnAddress());
                  fflush(stderr); } } }
    }
    ctx->reserve_addr = 0;
    ctx->cr = ok ? ((ctx->cr & ~(0xFu << 28)) | (2u << 28))
                 :  (ctx->cr & ~(0xFu << 28));
}

extern "C" void ppu_res_stdcx(ppu_context* ctx, uint64_t addr, uint64_t val)
{
    int ok = 0;
    if (ctx->reserve_addr && ctx->reserve_addr == (uint32_t)addr) {
        uint64_t expected = ctx->reserve_value;
        uint64_t desired  = ps3_bswap64(val);
        volatile long long* p = (volatile long long*)ea(addr);
        if (spu_coh_is_reserved((uint32_t)addr)) {
            spu_lockline_lock();
            if (*(volatile uint64_t*)p == expected) {
                *(volatile uint64_t*)p = desired;
                spu_coh_notify_write((uint32_t)addr);
                ok = 1;
            }
            spu_lockline_unlock();
        } else {
            ok = (_InterlockedCompareExchange64(p, (long long)desired,
                                                (long long)expected)
                  == (long long)expected);
        }
    }
    ctx->reserve_addr = 0;
    ctx->cr = ok ? ((ctx->cr & ~(0xFu << 28)) | (2u << 28))
                 :  (ctx->cr & ~(0xFu << 28));
}

extern "C" void vm_write8 (uint64_t addr, uint8_t  val) { yz_mem_guard((uint32_t)addr,1,1); if (yz_vmguard_check((uint32_t)addr,1,1)) return; VM_WRITE_COH(addr, &val, 1); }
extern "C" void vm_write16(uint64_t addr, uint16_t val) { yz_mem_guard((uint32_t)addr,2,1); if (yz_vmguard_check((uint32_t)addr,2,1)) return; uint16_t v = ps3_bswap16(val); VM_WRITE_COH(addr, &v, 2); }
extern "C" void yz_rsx_inline_on_put(void);   /* import_overrides.cpp: inline FIFO drain on PUT flush (YZ_RSX_INLINE) */
extern "C" void vm_write32(uint64_t addr, uint32_t val) { yz_mem_guard((uint32_t)addr,4,1); if (yz_vmguard_check((uint32_t)addr,4,1)) return; uint32_t v = ps3_bswap32(val); VM_WRITE_COH(addr, &v, 4); if ((uint32_t)addr == 0x10000040u) yz_rsx_inline_on_put(); }
extern "C" void vm_write64(uint64_t addr, uint64_t val) { yz_mem_guard((uint32_t)addr,8,1); if (yz_vmguard_check((uint32_t)addr,8,1)) return; uint64_t v = ps3_bswap64(val); VM_WRITE_COH(addr, &v, 8); }

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

/* DIAG (env YZ_VMGUARD / YZ_VMGUARD_SURVIVE): named-caller diagnostic for the
 * intermittent 0xC0000005 on the mixer/CRI thread startup (~32 frames in):
 * a lifted guest fn vm_read*'s a WILD pointer (~535 MB, outside every
 * committed guest region) and ea(addr) derefs it with no bounds check,
 * hitting uncommitted VM. The crash handler can't name the caller (trampoline
 * hop: cia=0 lr=0), so this resolves the REAL host-stack RIPs to guest func
 * addrs via yz_guest_addr_from_host (the yz_watch_bd/yz_mem_guard pattern) --
 * cheap range check only (no VirtualQuery) so it can stay on for a full boot.
 * Returns 1 ("wild, don't touch it") only when YZ_VMGUARD_SURVIVE is also set;
 * otherwise returns 0 after logging so the natural path (and any YZ_GUARD AV)
 * is unaffected. Diagnostic-only (env-gated, default-off, kill-switched --
 * band-aid hygiene) -- NOT a shipping fix; the
 * real bug is whatever hands the lifted code this address in the first
 * place (lift/race bug TBD from the logged caller).
 *
 * MEASURED 2026-07-04: vm.h's 4 static regions are NOT the full committed
 * set -- several syscall handlers / libs vm_commit() or VirtualAlloc() their
 * OWN windows outside vm.h: sys_memory.c commits [0x40000000,0x50000000) in
 * full up front (SYS_MEM_ALLOC_BASE/END), cellAudio.c commits
 * [0x50000000,0x50400000) (AUDIO_VM_BASE/SIZE), sys_vm.c bump-commits INSIDE
 * [0x60000000,0x70000000) (SYS_VM_REGION_BASE/END) per sys_vm_memory_map
 * call, and import_overrides.cpp VirtualAlloc's the GCM local (RSX video)
 * memory at [0xC0000000,0xC0000000+0x0F900000) (YZ_GCM_LOCAL_BASE/SIZE,
 * yakuza_runner.h). First pass of this guard treated all of those as wild
 * and false-positived TWICE: a dlmalloc chunk-list walk (func_00071894)
 * legitimately touching a sys_vm_memory_map'd heap at boot (20/20 hits, sys_vm
 * region, well before frame 1, no crash), and a GCM-local zero-init/copy loop
 * (func_00FBE68C/00EAD4B4/00E80B34 chain) touching 0xC0000000 (20/20 hits,
 * same). Widen the known-good set to match; the sys_vm sub-range is
 * approximated as the whole reserved window (not just the bump pointer)
 * since that's still tighter than "anything goes" and avoids a cross-TU
 * dependency on sys_vm.c's bump-pointer state. */
#define YZ_VMGUARD_SYS_MEM_BASE   0x40000000u
#define YZ_VMGUARD_SYS_MEM_END    0x50000000u
#define YZ_VMGUARD_AUDIO_VM_BASE  0x50000000u
#define YZ_VMGUARD_AUDIO_VM_SIZE  0x00400000u
#define YZ_VMGUARD_SYS_VM_BASE    0x60000000u
#define YZ_VMGUARD_SYS_VM_END     0x70000000u
#define YZ_VMGUARD_GCM_LOCAL_BASE 0xC0000000u
#define YZ_VMGUARD_GCM_LOCAL_SIZE 0x0F900000u
static int yz_vmguard_check(uint32_t a, unsigned w, int is_write) {
    static int on = -1; if (on < 0) on = getenv("YZ_VMGUARD") ? 1 : 0;
    if (!on) return 0;
    /* Committed-region check mirrors vm_is_valid_addr (runtime/memory/vm.h)
     * PLUS the syscall-handler/libs bump windows above -- anything outside
     * all of those is uncommitted (PAGE_NOACCESS from the 4 GB MEM_RESERVE)
     * and a deref there is the wild-pointer fault. */
    int valid =
        (a >= VM_MAIN_MEM_BASE && a < VM_MAIN_MEM_BASE + VM_MAIN_MEM_SIZE) ||
        (a >= VM_STACK_BASE    && a < VM_STACK_BASE    + VM_STACK_REGION)  ||
        (a >= VM_SPU_BASE      && a < VM_SPU_BASE      + 8 * VM_SPU_WINDOW_SIZE) ||
        (a >= VM_RSX_BASE      && a < VM_RSX_BASE      + VM_RSX_SIZE) ||
        (a >= YZ_VMGUARD_SYS_MEM_BASE  && a < YZ_VMGUARD_SYS_MEM_END) ||
        (a >= YZ_VMGUARD_AUDIO_VM_BASE && a < YZ_VMGUARD_AUDIO_VM_BASE + YZ_VMGUARD_AUDIO_VM_SIZE) ||
        (a >= YZ_VMGUARD_SYS_VM_BASE   && a < YZ_VMGUARD_SYS_VM_END) ||
        (a >= YZ_VMGUARD_GCM_LOCAL_BASE && a < YZ_VMGUARD_GCM_LOCAL_BASE + YZ_VMGUARD_GCM_LOCAL_SIZE);
    if (valid) return 0;
    static int survive = -1; if (survive < 0) survive = getenv("YZ_VMGUARD_SURVIVE") ? 1 : 0;
    static long seq = 0; long n = ++seq;
    if (n <= 20) {                                       /* volume-cap the spam */
        void* bt[24]; unsigned short got = RtlCaptureStackBackTrace(1, 24, bt, 0);
        char chain[256]; int ci = 0; chain[0] = 0;
        for (unsigned k = 0; k < got && ci < 230; k++) {
            uint32_t g = yz_guest_addr_from_host(bt[k]);
            if (g) ci += snprintf(chain + ci, sizeof(chain) - (size_t)ci, " %08X", g);
        }
        fprintf(stderr, "[vmguard] %-5s wild addr=0x%08X w=%u tid=%lu%s guest-callers:%s\n",
                is_write ? "WRITE" : "READ", a, w, (unsigned long)GetCurrentThreadId(),
                survive ? " SURVIVED" : "", chain);
        fflush(stderr);
    }
    return survive;
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

    /* === YZ_SKIP_VOICE: bypass the CRI voice-readiness gate (the codec decode is
     * a separate, deep frontier). t1 cond-waits on cond 9 forever, re-checking a
     * readiness predicate the broken codec never sets. Diagnose the candidate
     * handles t1 polls; with YZ_SKIP_VOICE set, poke them "ready" so t1 proceeds
     * and the NEXT wall surfaces. OFF by default; experiment only. === */
    if (num == 107 && (uint32_t)ctx->gpr[3] == 9 && yz_thread_current_id() == 1) {
        static long vw = 0; long n = ++vw;
        uint32_t r31 = (uint32_t)ctx->gpr[31];
        uint32_t adxm = 0x01613368u, blk = 0x01654294u;   /* ADXM handle / cond block (pt47) */
        if (n <= 3 || (n % 500) == 0) {
            fprintf(stderr, "[voice-wait] n=%ld r31=0x%08X [r31+298]=%08X "
                    "adxm[+294]=%08X [+298]=%08X [+29C]=%08X blk[+00]=%08X [+10]=%08X\n",
                    n, r31, vm_read32(r31 + 0x298),
                    vm_read32(adxm + 0x294), vm_read32(adxm + 0x298), vm_read32(adxm + 0x29C),
                    vm_read32(blk + 0x00), vm_read32(blk + 0x10));
            fflush(stderr);
        }
        if (getenv("YZ_SKIP_VOICE") && n >= 700) {
            static int once = 0;
            vm_write32(r31  + 0x298, 0xFFFFFFFFu);   /* innermost-frame predicate candidate */
            vm_write32(adxm + 0x298, 0xFFFFFFFFu);   /* ADXM handle predicate candidate */
            vm_write32(blk  + 0x10,  2u);            /* cond-block prime flag -> PLAYING */
            if (!once) { once = 1;
                fprintf(stderr, "[voice-skip] forcing readiness from n=%ld (r31=0x%08X)\n", n, r31);
                fflush(stderr); }
        }
    }

    /* [t1-spin] (clean binary): t1 spins signaling cond-4 forever at the movie
     * gate. Log the caller (lr) + working regs to locate its loop + the object
     * it polls, so we can force it forward. YZ_T1SPIN to enable. */
    if (getenv("YZ_T1SPIN") && num == 108 && (uint32_t)ctx->gpr[3] == 4 &&
        yz_thread_current_id() == 1) {
        static long n = 0; if (++n <= 24) {
            fprintf(stderr, "[t1-spin] #%ld lr=0x%08X r4=0x%llX r5=0x%08X r30=0x%08X r31=0x%08X\n",
                    n, (uint32_t)ctx->lr, (unsigned long long)ctx->gpr[4],
                    (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[30], (uint32_t)ctx->gpr[31]);
            fflush(stderr);
        }
    }

    /* [cri_dlg] (YZ_CRIDLG, 2026-07-05): cri_dlg (tid 11/12) is the CRI asset-load
     * dispatcher. Its processor func_00F00208 runs callback [ctrl+0x40](arg [ctrl+0x44])
     * ONLY when work-flag [ctrl+0x4]!=0. RPCS3's cri_dlg loads scenario.bin -> player_pos.bin
     * -> ... -> all_csb.par -> movie; ours opens scenario.bin then stalls. Log the control-block
     * state at each cond_wait: is work EVER queued (flag/callback set) or does the producer stop
     * feeding it after the first asset? Forks the fix (producer bug vs callback bug). */
    if (getenv("YZ_CRIDLG") && num == 107 &&
        (yz_thread_current_id() == 11 || yz_thread_current_id() == 12)) {
        static long n = 0; long m = ++n;
        uint32_t r31 = (uint32_t)ctx->gpr[31];
        if (m <= 12 || (m % 250) == 0) {
            fprintf(stderr, "[cri_dlg] #%ld tid=%u cond_wait ctrl=0x%08X workflag[+4]=0x%X "
                    "cb[+40]=0x%08X arg[+44]=0x%08X run[+50]=0x%X cond[+14]=0x%X\n",
                    m, yz_thread_current_id(), r31, vm_read32(r31 + 0x4), vm_read32(r31 + 0x40),
                    vm_read32(r31 + 0x44), vm_read32(r31 + 0x50), vm_read32(r31 + 0x14));
            fflush(stderr);
        }
    }

    /* === YZ_SKIP_VOICE (re-keyed 2026-07-05): the real t1 movie-gate spin is
     * cond_signal(4), NOT cond_wait(9) (which fires only ~1-4x). The CRI voice
     * codec (ADX/HCA in adv_voice_talk.cvm) never decodes → the voice-readiness
     * predicate stays 0 → t1 spins here forever. Experiment: force the candidate
     * readiness fields on this spin (after init settles) to see if t1 advances
     * past the voice gate and the NEXT wall surfaces. OFF by default. === */
    if (getenv("YZ_SKIP_VOICE") && num == 108 && (uint32_t)ctx->gpr[3] == 4 &&
        yz_thread_current_id() == 1) {
        static long sn = 0; long n = ++sn;
        const uint32_t adxm = 0x01613368u, blk = 0x01654294u;
        /* NB (2026-07-05): t1 does cond_signal(4) only ~95x total then blocks in
         * sema_wait(4) — a producer/consumer handshake, NOT a tight spin. Threshold
         * kept low so the force arms; but the adxm predicate is likely checked at a
         * DIFFERENT hook point, so pin the exact readiness field before trusting a
         * negative result here (see STATUS "ROOT GATE"). */
        if (n >= 40) {
            /* poke every watched voice-readiness candidate to a plausible "ready" */
            vm_write32(adxm + 0x294, 0x00000001u);
            vm_write32(adxm + 0x298, 0xFFFFFFFFu);
            vm_write32(adxm + 0x29C, 0x00000001u);
            vm_write32(blk  + 0x10,  2u);            /* cond-block state -> PLAYING */
            static int once = 0; if (!once) { once = 1;
                fprintf(stderr, "[voice-skip] forcing voice-ready at cond_signal(4) spin n=%ld "
                        "(adxm+294/+298/+29C, blk+10)\n", n);
                fflush(stderr); }
        }
    }

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
