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
#include "../include/ps3emu/yz_runtime_config.h"
#include "../include/ps3emu/yz_frontier_trace.h"
#include "../runtime/memory/vm.h"
#include "yakuza_runner.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" void yz_ppu_cooperative_yield(void)
{
    /* Keep very short worker completions cheap, then yield this logical CPU.
     * The lifted guest loop otherwise performs up to 0x02000000 full VM reads
     * before reaching its own 40-us backoff. */
    for (unsigned i = 0; i < 64; ++i)
        YieldProcessor();
    SwitchToThread();
}

/* Guest back-chain walker (defined in main.cpp, C++ linkage). Declared at file
 * scope: a block-scope declaration inside an extern "C" function would inherit
 * C linkage and fail to link against the mangled definition. */
void yz_dump_guest_state(const ppu_context* gc, const char* tag);

/* ---------------------------------------------------------------------------
 * Guest memory base + accessors (big-endian guest, LE host)
 * -----------------------------------------------------------------------*/

extern "C" uint8_t* vm_base = nullptr;

/* Guest effective addresses are computed in 64-bit by the lifted code but
 * the PS3 user address space is 32-bit: truncate, like the real PPU in
 * 32-bit mode. */
static inline uint8_t* ea(uint64_t addr) { return vm_base + (uint32_t)addr; }

static void yz_mem_guard(uint32_t a, unsigned w, int is_write);  /* DIAG (YZ_GUARD), def below */
static void yz_a010_constsrc_read64(
    uint32_t addr, uint64_t val, const void* retaddr);
static void yz_a010_camera_read32(
    uint32_t addr, uint32_t val, const void* retaddr);
/* DIAG (env YZ_VMGUARD / YZ_VMGUARD_SURVIVE): returns 1 if `a` is OUTSIDE every
 * committed guest region (wild), def below. Named-caller logger + optional
 * survive (skip the deref) for the intermittent mixer/CRI wild-read crash. */
static int yz_vmguard_check(uint32_t a, unsigned w, int is_write);
extern "C" void yz_a010_cmt_capture(uint32_t address, uint32_t size);
extern "C" void yz_stage_render_checkpoint(void* context, uint32_t phase);

#if !defined(YZ_PERF_CLEAN)
static inline void yz_stage_checkpoint_before_b0(uint32_t address)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("YZ_STAGE_RENDER_CHECKPOINT") ? 1 : 0;
    ppu_context* const ctx = g_yz_cur_ctx;
    if (enabled && ctx &&
        address == (uint32_t)ctx->gpr[24] + 0xB0u &&
        (uint32_t)ctx->lr == 0x00113508u)
        yz_stage_render_checkpoint(ctx, 0u);
}

static inline void yz_stage_checkpoint_after_object(uint32_t address)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("YZ_STAGE_RENDER_CHECKPOINT") ? 1 : 0;
    ppu_context* const ctx = g_yz_cur_ctx;
    if (enabled && ctx &&
        address == (uint32_t)ctx->gpr[1] + 0xB8u &&
        (uint32_t)ctx->lr == 0x00113508u)
        yz_stage_render_checkpoint(ctx, 1u);
}
#endif

#if defined(YZ_PERF_CLEAN)
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
    value = ps3_bswap32(value);
    /* CMT discovery is behavior state used by the optional camera repair,
     * not an observation-only memory watcher.  Its magic comparison is cold
     * and remains available in clean builds. */
    if (value == 0x434D5450u)
        yz_a010_cmt_capture((uint32_t)addr,
                            vm_read32((uint32_t)addr + 0xCu));
    return value;
}

extern "C" uint64_t vm_read64(uint64_t addr)
{
    uint64_t value;
    memcpy(&value, ea(addr), sizeof(value));
    return ps3_bswap64(value);
}
#else
extern "C" uint8_t  vm_read8 (uint64_t addr) { yz_mem_guard((uint32_t)addr,1,0); if (yz_vmguard_check((uint32_t)addr,1,0)) return 0; return *ea(addr); }
extern "C" uint16_t vm_read16(uint64_t addr) { yz_stage_checkpoint_before_b0((uint32_t)addr); yz_mem_guard((uint32_t)addr,2,0); if (yz_vmguard_check((uint32_t)addr,2,0)) return 0; uint16_t v; memcpy(&v, ea(addr), 2); return ps3_bswap16(v); }
extern "C" uint32_t vm_read32(uint64_t addr) { yz_mem_guard((uint32_t)addr,4,0); if (yz_vmguard_check((uint32_t)addr,4,0)) return 0; uint32_t v; memcpy(&v, ea(addr), 4); v = ps3_bswap32(v); if (v == 0x434D5450u) yz_a010_cmt_capture((uint32_t)addr, vm_read32((uint32_t)addr + 0xCu)); yz_a010_camera_read32((uint32_t)addr, v, _ReturnAddress()); return v; }
extern "C" uint64_t vm_read64(uint64_t addr) { yz_mem_guard((uint32_t)addr,8,0); if (yz_vmguard_check((uint32_t)addr,8,0)) return 0; uint64_t v; memcpy(&v, ea(addr), 8); v = ps3_bswap64(v); yz_a010_constsrc_read64((uint32_t)addr, v, _ReturnAddress()); yz_stage_checkpoint_after_object((uint32_t)addr); return v; }
#endif

/* PPU<->SPU lock-line coherence (1f, spu_channels.c): a PPU write to a 128-byte
 * line the SPURS kernel has reserved (GETLLAR) must serialize through the SPU
 * lock-line so the kernel's PUTLLC memcmp sees it (else the bump is clobbered).
 * Fast path: spu_coh_is_reserved() is a range check + O(1) bitmap lookup, so the
 * overwhelming majority of writes (stack/game data/cmd buffer) skip the lock. */
extern "C" int  spu_coh_is_reserved(uint32_t addr);
extern "C" void spu_lockline_lock(void);
extern "C" void spu_lockline_unlock(void);
extern "C" uint32_t yz_thread_current_id(void);   /* threads.cpp; the mgmt-CAS diag */
extern "C" volatile long g_yz_a010_root_active;
extern "C" volatile long g_yz_a010_stage_generation;
extern "C" uint32_t g_yz_game_toc;
/* threads.cpp: sc45/46 handlers (batch fix item 1 -- yakuza_runner.h is out
 * of scope for this batch, so declared here at the call site instead). */
extern "C" int64_t yz_sc_thread_detach(ppu_context* ctx);
extern "C" int64_t yz_sc_thread_get_join_state(ppu_context* ctx);
/* DIAG (env YZ_WATCH_BD): catch any PPU store covering spurs+0xBD
 * (sysSrvMsgUpdateWorkload @ 0x40197D3D) to find who writes the bad 0xE0. */
extern "C" void yz_watch_bd(uint32_t addr, const void* src, unsigned n);
/* Raise SPU_EVENT_LR on any SPU whose GETLLAR reservation covers this line --
 * the SPURS idle-service wakeup (spu_channels.c). */
extern "C" void spu_coh_notify_write(uint32_t addr);
#if defined(YZ_NATIVE_SPURS)
extern "C" void cellSpursNotifyPpuGuestWrite(uint32_t addr, uint32_t size);
extern "C" volatile uint32_t g_native_spurs_ppu_watch_lo;
extern "C" volatile uint32_t g_native_spurs_ppu_watch_hi;
static inline void yz_native_spurs_notify_write(uint32_t addr, uint32_t size)
{
    const uint64_t first = addr;
    const uint64_t last = first + size;
    if (first < g_native_spurs_ppu_watch_hi &&
        last > g_native_spurs_ppu_watch_lo)
        cellSpursNotifyPpuGuestWrite(addr, size);
}
#else
static inline void yz_native_spurs_notify_write(uint32_t, uint32_t) {}
#endif
#if defined(YZ_PERF_CLEAN)
#define VM_WRITE_COH(addr, src, n) do { \
        if (spu_coh_is_reserved((uint32_t)(addr))) { \
            spu_lockline_lock(); memcpy(ea(addr), (src), (n)); \
            spu_coh_notify_write((uint32_t)(addr)); spu_lockline_unlock(); \
        } else memcpy(ea(addr), (src), (n)); \
        yz_native_spurs_notify_write((uint32_t)(addr), (uint32_t)(n)); } while (0)
#else
#define VM_WRITE_COH(addr, src, n) do { \
        yz_watch_bd((uint32_t)(addr), (src), (n)); \
        if (spu_coh_is_reserved((uint32_t)(addr))) { \
            spu_lockline_lock(); memcpy(ea(addr), (src), (n)); \
            spu_coh_notify_write((uint32_t)(addr)); spu_lockline_unlock(); \
        } else memcpy(ea(addr), (src), (n)); \
        yz_native_spurs_notify_write((uint32_t)(addr), (uint32_t)(n)); } while (0)
#endif
extern "C" uint32_t yz_guest_addr_from_host(const void* rip);
extern "C" void yz_watch_arm(uint32_t guest_addr);
extern "C" void yz_a010_reltrace_ppu_store(
    uint32_t addr, uint32_t val, uint32_t guest_pc);
extern "C" volatile long g_yz_a010_release_scene_active;

/* YZ_A010_HANDOFF: name the last camera handoff gate without modifying the
 * generated source. func_00011DF0 starts by reading object+34 then object+30;
 * validate that pattern by its camera vtable and dump the active-list field.
 * Reads by func_00011260 prove that an active camera node reached evaluation. */
static void yz_a010_camera_read32(
    uint32_t addr, uint32_t val, const void* retaddr)
{
    static int on = -1;
    static unsigned object_hits = 0;
    static unsigned apply_reads = 0;
    static thread_local uint32_t previous_addr = 0;
    static thread_local uint32_t previous_caller = 0;
    if (on < 0) on = getenv("YZ_A010_HANDOFF") ? 1 : 0;
    if (!on || !g_yz_a010_root_active) return;

    const uint32_t caller = yz_guest_addr_from_host(retaddr);
    if (val == 0x434D5450u) {
        static unsigned cmt_magic_reads = 0;
        if (cmt_magic_reads < 64u) {
            cmt_magic_reads++;
            fprintf(stderr,
                    "[a010-cmt-read] n=%u addr=%08X caller=%08X tid=%u\n",
                    cmt_magic_reads, addr, caller, yz_thread_current_id());
            fflush(stderr);
        }
    }
    if (caller == 0x00011DF0u &&
        previous_caller == caller && addr + 4u == previous_addr) {
        const uint32_t object = addr - 0x30u;
        uint32_t raw_vtable = 0;
        memcpy(&raw_vtable, ea(object), sizeof(raw_vtable));
        if (ps3_bswap32(raw_vtable) == 0x011D3EA0u) {
            object_hits++;
            if (object_hits <= 32u ||
                (object_hits & (object_hits - 1u)) == 0u) {
                auto raw32 = [](uint32_t a) {
                    uint32_t raw;
                    memcpy(&raw, ea(a), sizeof(raw));
                    return ps3_bswap32(raw);
                };
                fprintf(stderr,
                        "[a010-camera-object] n=%u object=%08X "
                        "inactive=%08X active=%08X enabled=%08X "
                        "entries=%08X count=%u\n",
                        object_hits, object,
                        raw32(object + 0x1Cu), raw32(object + 0x20u),
                        raw32(object + 0x24u), raw32(object + 0x30u),
                        raw32(object + 0x34u));
                fflush(stderr);
            }
        }
    }

    if (caller == 0x00011260u && apply_reads < 96u) {
        apply_reads++;
        fprintf(stderr,
                "[a010-camera-eval-read] n=%u addr=%08X val=%08X\n",
                apply_reads, addr, val);
        fflush(stderr);
    }
    previous_addr = addr;
    previous_caller = caller;
}

static void yz_a010_camera_write(
    uint32_t addr, uint64_t val, unsigned width, const void* retaddr)
{
    static unsigned writes = 0;
    if (!g_yz_a010_root_active || writes >= 128u) return;
    const uint64_t end = (uint64_t)addr + width;
    if (end <= 0x01622650ull || addr >= 0x01622690u) return;
    writes++;
    fprintf(stderr,
            "[a010-camera-write] n=%u addr=%08X width=%u value=%016llX "
            "caller=%08X tid=%u\n",
            writes, addr, width, (unsigned long long)val,
            yz_guest_addr_from_host(retaddr), yz_thread_current_id());
    fflush(stderr);
}

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
         * level upstream of cellSpursSendWorkloadSignal. CellSpursTaskset layout
         * (base 0x40199D00): running@0x00,
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
 * PPU differential trace (tools/tracediff.py). Emitted by the --trace lifter
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
    /* DIAG (env YZ_F484_PROBE): confirm the func_00F00484 completion-notifier
     * divergence found by the producer trace-diff (2026-07-05). At entry r3=P
     * (the request object *(obj+0x440)), r4=V (the request id *(obj+0x444)). The
     * PC-only diff showed ours branch F004D4->F004B4 (P->[0x4]==0, skip
     * sys_cond_signal) where RPCS3 goes F004D4->F004D8 (P->[0x4]!=0, signals
     * cri_dlg). Printing P->[0x4] + P->[0x48] makes that a DIRECT value
     * measurement, not a PC inference. Retire once the notifier root is fixed. */
    {
        static int p484 = -1;
        if (p484 < 0) p484 = getenv("YZ_F484_PROBE") ? 1 : 0;
        if (p484 && pc == 0xF00484u && (int)yz_thread_current_id() == 1) {
            static unsigned long pn = 0;
            if (pn < 400) { pn++;
                uint64_t* gg = (uint64_t*)ctxv;
                uint32_t P = (uint32_t)gg[3], V = (uint32_t)gg[4];
                uint32_t p0  = P ? vm_read32(P + 0x00) : 0;
                uint32_t p4  = P ? vm_read32(P + 0x04) : 0;
                uint32_t p48 = P ? vm_read32(P + 0x48) : 0;
                uint32_t p14 = P ? vm_read32(P + 0x14) : 0;
                fprintf(stderr, "[f484] #%lu P=%08X V=%08X P0=%08X P4=%08X P48=%08X match48=%d P14=%08X\n",
                        pn, P, V, p0, p4, p48, (int)(p48 == V), p14);
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
               setvbuf(fp, nullptr, _IOFBF, 1 << 20);  /* 1 MB buffered. The per-line write()
                                                   * of the old _IONBF path was the dominant
                                                   * tracer slowdown; buffered + a periodic
                                                   * fflush keeps the traced thread near
                                                   * untraced speed (needed so the RPCS3 and
                                                   * our-side traces reach the same phase) while
                                                   * bounding a kill's tail loss to <256K lines.
                                                   * Self-terminates + flushes at budget==0. */
               const char* n = getenv("YZ_PPU_TRACE_N"); budget = n ? strtol(n, 0, 0) : 3000000; }
    if (budget <= 0) return;
    fprintf(fp, "%08X\n", pc);
    if (--budget == 0 || (budget & 0x3FFFF) == 0) fflush(fp);
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

/* s26 stager hunt (STATUS ⚡ frontier): the wid4 work-record stager's last
 * candidate class = PPU atomic stores (they bypass vm_write*, MFC, and HLE
 * watches). Logger + gate live in spu_channels.c / main.cpp. */
extern "C" void yz_slotstore_log(uint32_t a, unsigned long long v, int w, void* ra);
extern "C" int g_yz_slotstore;

extern "C" void ppu_res_stwcx(ppu_context* ctx, uint64_t addr, uint32_t val)
{
    if ((uint32_t)addr >= 0x42452880u && (uint32_t)addr < 0x424529E0u) {
        if (g_yz_slotstore) yz_slotstore_log((uint32_t)addr, val, 32, _ReturnAddress());
    }
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
        /* OBSERVATIONAL (env YZ_WIDSIG_ALL, 2026-07-06; made INDEPENDENT of
         * YZ_MGMT_CAS 2026-07-08 -- it was nested inside that gate, so setting
         * only YZ_WIDSIG_ALL silently armed nothing): UNCAPPED census of every
         * PPU commit that RAISES a wid signal bit on the wklSignal1 word
         * (0x40197CF0), broken out per-wid, with a running count so the
         * CRI-phase re-arm frequency of wid1 (bit 0x4000) is measurable past
         * the mgmt-cas per-word cap. Pure fprintf, no state mutation; it does
         * NOT force any gate. */
        { static int wsa = -1;
          if (wsa < 0) { wsa = getenv("YZ_WIDSIG_ALL") ? 1 : 0;
              if (wsa) { fprintf(stderr, "[widsig] armed (BT=%d)\n",
                                 getenv("YZ_WIDSIG_BT") ? 1 : 0); fflush(stderr); } }
          if (wsa && ((uint32_t)addr) == 0x40197CF0u) {
                    uint32_t oldv = ps3_bswap32(expected);
                    uint32_t raised = (uint32_t)val & ~oldv;   /* bits newly set */
                    /* wklSignal1 is the high 16 bits; per-wid bit = 0x8000>>wid */
                    static unsigned long cw0=0,cw1=0,cw2=0,cw3=0,cw4=0;
                    if (raised & 0x80000000u) cw0++;
                    if (raised & 0x40000000u) cw1++;
                    if (raised & 0x20000000u) cw2++;
                    if (raised & 0x10000000u) cw3++;
                    if (raised & 0x08000000u) cw4++;
                    if (raised & 0xFF000000u) {
                        fprintf(stderr, "[widsig] t%u raise=%08X old=%08X new=%08X "
                                "counts w0=%lu w1=%lu w2=%lu w3=%lu w4=%lu\n",
                                yz_thread_current_id(), raised, oldv, (uint32_t)val,
                                cw0, cw1, cw2, cw3, cw4);
                        fflush(stderr);
                        /* DIAG (env YZ_WIDSIG_BT, 2026-07-08): the wid1 (CRI jobchain)
                         * signal bit is raised exactly ONCE per boot and the raw lr
                         * captured near the raise proved to be a DATA pointer (the
                         * 0x01622200 device object), not the kicker. Walk the guest
                         * back-chain at the raise instead (the crash handler's
                         * walker) to name the real producer call chain. First few
                         * wid1 raises only; env-gated, observation only. */
                        { static int wbt = -1;
                          if (wbt < 0) wbt = getenv("YZ_WIDSIG_BT") ? 1 : 0;
                          static long wbtn = 0;
                          if (wbt && (raised & 0x40000000u) && wbtn < 4) { wbtn++;
                              yz_dump_guest_state(ctx, "widsig-bt");
                          } }
                        /* DIAG (s24, 2026-07-09): wid2 = the gs_task/EDGE taskset — the
                         * gcm journal's real consumer (patches + fenced releases +
                         * tag-zeroing). MEASURED s24fast: w2 raised ONCE per boot while
                         * w3/w4 got 1500/260 — the starvation behind the stopper-
                         * backlog deadlock class. Dump the raiser's state on the first
                         * raises (rides YZ_WIDSIG_BT) to name the kick path, s22
                         * wid4-playbook style. */
                        { static int wbt2 = -1;
                          if (wbt2 < 0) wbt2 = getenv("YZ_WIDSIG_BT") ? 1 : 0;
                          static long wbtn2 = 0;
                          if (wbt2 && (raised & 0x20000000u) && wbtn2 < 3) { wbtn2++;
                              yz_dump_guest_state(ctx, "wid2-bt");
                          } }
                        /* DIAG (s22 opener, 2026-07-08): wid4 = the pxd GPU-frame pool
                         * (image 4, elf 0x01284200) = the 0xFE0 decode-sync publisher
                         * (DONT_RECHASE #29). t1 raises its signal but image 4 never
                         * DMAs; dump the SELECT-gate fields AT the raise so the failing
                         * gate (readyCount/contention/state/prio/enabled) is measured,
                         * not inferred — the wid3/wid1 precedents each failed a
                         * different one. PPU reads off the mgmt base 0x40197C80
                         * (pt35 field map: rc@+0x00 cont@+0x20 pend@+0x30 maxc@+0x50
                         * state@+0x80 status@+0x90 enabled@+0xB0, wklInfo1[w].prio
                         * @+0xB00+w*0x20+0x18; byte lane = wid). Backtrace dump on the
                         * first raises rides YZ_WIDSIG_BT to name the kicker. */
                        if (raised & 0x08000000u) {
                            const uint32_t mg = 0x40197C80u;
                            uint32_t rc4  = (vm_read32(mg + 0x04u) >> 24) & 0xFFu;
                            uint32_t cc4  = (vm_read32(mg + 0x24u) >> 24) & 0xFFu;
                            uint32_t pd4  = (vm_read32(mg + 0x34u) >> 24) & 0xFFu;
                            uint32_t mc4  = (vm_read32(mg + 0x54u) >> 24) & 0xFFu;
                            uint32_t st4  = (vm_read32(mg + 0x84u) >> 24) & 0xFFu;
                            uint32_t sta4 = (vm_read32(mg + 0x94u) >> 24) & 0xFFu;
                            uint32_t en   = vm_read32(mg + 0xB0u);
                            uint32_t pHi  = vm_read32(mg + 0xB00u + 4u*0x20u + 0x18u);
                            uint32_t pLo  = vm_read32(mg + 0xB00u + 4u*0x20u + 0x1Cu);
                            fprintf(stderr, "[wid4gate] t%u AT-RAISE rc=%u cont=%u pend=%u "
                                    "maxc=%u state=0x%02X status=0x%02X enabled=%d "
                                    "prio=%08X_%08X\n",
                                    yz_thread_current_id(), rc4, cc4, pd4, mc4, st4, sta4,
                                    (en & (1u << (31 - 4))) ? 1 : 0, pHi, pLo);
                            fflush(stderr);
                            { static int wbt4 = -1;
                              if (wbt4 < 0) wbt4 = getenv("YZ_WIDSIG_BT") ? 1 : 0;
                              static long wbtn4 = 0;
                              if (wbt4 && wbtn4 < 3) { wbtn4++;
                                  yz_dump_guest_state(ctx, "wid4-bt");
                              } }
                        }
                    }
          } }
        /* DIAG (env YZ_JGUARD, 2026-07-08, uncapped): PPU 4-byte CAS commits to the
         * CRI jobchain's CellSpursJobGuard line 0x4019C700. Counts JobGuardNotify's
         * ncount0 decrement at the store itself, so direct bl callers the YZ_HOOK
         * indirect-call logger cannot see are still counted. ncount0/1 printed are
         * the POST-commit words. */
        { static int jg = -1;
          if (jg < 0) { jg = getenv("YZ_JGUARD") ? 1 : 0;
              if (jg) { fprintf(stderr, "[jguard] ppu probe armed\n"); fflush(stderr); } }
          if (jg && (((uint32_t)addr) & ~127u) == 0x4019C700u) {
              fprintf(stderr, "[jguard-ppu] t%u addr=0x%08X old=%08X new=%08X %s "
                      "ncount0=%08X ncount1=%08X\n",
                      yz_thread_current_id(), (uint32_t)addr,
                      ps3_bswap32(expected), (uint32_t)val, ok ? "OK" : "FAIL",
                      vm_read32(0x4019C700u), vm_read32(0x4019C704u));
              fflush(stderr);
          } }
        /* DIAG (env YZ_W4TS, 2026-07-08 s22): PPU 4-byte CAS commits to the wid4
         * pool TASKSET bitset line 0x42450E00 (running@+0x00 ready@+0x10
         * pending_ready@+0x20 enabled@+0x30 signalled@+0x40 waiting@+0x50,
         * docs/SPURS_TASKSET.md). The pool's 5 tasks park in WAIT_SIGNAL after
         * init (measured wid4boot1/ftboot14); a game work submission =
         * _cellSpursSendSignal setting a signalled bit HERE, then the wid4
         * wklSignal raise. Zero post-init hits while t1 sits in the
         * func_00EB3238 throttle = the submit site never executes = the
         * bootstrap circle is code-ordered, not a pacing race. Uncapped. */
        { static int w4 = -1;
          if (w4 < 0) { w4 = getenv("YZ_W4TS") ? 1 : 0;
              if (w4) { fprintf(stderr, "[w4ts] ppu probe armed (taskset 0x42450E00)\n"); fflush(stderr); } }
          if (w4 && (((uint32_t)addr) & ~127u) == 0x42450E00u) {
              fprintf(stderr, "[w4ts-ppu] t%u addr=0x%08X (+0x%02X) old=%08X new=%08X %s\n",
                      yz_thread_current_id(), (uint32_t)addr,
                      (uint32_t)addr & 0x7Fu,
                      ps3_bswap32(expected), (uint32_t)val, ok ? "OK" : "FAIL");
              fflush(stderr);
          } }
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
    if (ok)
        yz_native_spurs_notify_write((uint32_t)addr, 4);
#if !defined(YZ_PERF_CLEAN)
    if (ok && (uint32_t)addr >= 0x40400000u &&
        (uint32_t)addr < 0x40C00000u)
        yz_a010_reltrace_ppu_store(
            (uint32_t)addr, val,
            yz_guest_addr_from_host(_ReturnAddress()));
#endif
    ctx->reserve_addr = 0;
    /* CR0 = 0b00 || store_ok || XER[SO] (the conditional-store record form
     * folds the sticky SO like every compare/record op; SO is provably 0 in
     * this title today, so this is spec conformance, not a behavior change). */
    ctx->cr = (ctx->cr & ~(0xFu << 28))
              | ((((ok ? 2u : 0u) | ((ctx->xer >> 31) & 1u))) << 28);
}

extern "C" void ppu_res_stdcx(ppu_context* ctx, uint64_t addr, uint64_t val)
{
    if ((uint32_t)addr >= 0x42452880u && (uint32_t)addr < 0x424529E0u) {
        if (g_yz_slotstore) yz_slotstore_log((uint32_t)addr, val, 64, _ReturnAddress());
    }
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
        /* DIAG (YZ_MGMT_CAS, 8-byte path): count 8-byte mgmt-line CAS too, so
         * the census is complete across both commit widths; tid identifies
         * which thread pumps the line. NOTE the follow-up verdict (2026-07-06):
         * the mixer heartbeat IS a 4-byte stwcx on both sides. RPCS3's log
         * labels every reservation store "STCX8" (a shared diagnostic template
         * that dumps an 8-byte granule for u32 and u64 alike), so an "our 4-byte
         * store must be wrong" theory built on that label is refuted; this probe
         * exists to keep the width question measurable, not because 8-byte
         * traffic is expected. */
        /* DIAG (env YZ_JGUARD): 8-byte CAS commits to the JobGuard line, same
         * census as the 4-byte path above (libsre may use ldarx/stdcx here). */
        { static int jg8 = -1; if (jg8 < 0) jg8 = getenv("YZ_JGUARD") ? 1 : 0;
          if (jg8 && (((uint32_t)addr) & ~127u) == 0x4019C700u) {
              fprintf(stderr, "[jguard-ppu8] t%u addr=0x%08X old=%016llX new=%016llX %s\n",
                      yz_thread_current_id(), (uint32_t)addr,
                      (unsigned long long)ps3_bswap64(expected), (unsigned long long)val,
                      ok ? "OK" : "FAIL");
              fflush(stderr);
          } }
        /* DIAG (env YZ_W4TS, 8-byte path): same wid4-pool taskset bitset census
         * as the 4-byte block above (libsre task APIs use ldarx/stdcx on the
         * u128 bitsets). */
        { static int w48 = -1; if (w48 < 0) w48 = getenv("YZ_W4TS") ? 1 : 0;
          if (w48 && (((uint32_t)addr) & ~127u) == 0x42450E00u) {
              fprintf(stderr, "[w4ts-ppu8] t%u addr=0x%08X (+0x%02X) old=%016llX new=%016llX %s\n",
                      yz_thread_current_id(), (uint32_t)addr,
                      (uint32_t)addr & 0x7Fu,
                      (unsigned long long)ps3_bswap64(expected), (unsigned long long)val,
                      ok ? "OK" : "FAIL");
              fflush(stderr);
          } }
        { static int mc = -1; if (mc < 0) mc = getenv("YZ_MGMT_CAS") ? 1 : 0;
          if (mc && (((uint32_t)addr) & ~127u) == 0x40197C80u) {
              static long n8 = 0; long c = ++n8;
              if (c <= 40 || (c % 997) == 0) {
                  fprintf(stderr, "[mgmt-cas8] t%u addr=0x%08X old=%016llX new=%016llX %s (8B #%ld)\n",
                          yz_thread_current_id(), (uint32_t)addr,
                          (unsigned long long)ps3_bswap64(expected),
                          (unsigned long long)val, ok ? "OK" : "FAIL", c);
                  fflush(stderr);
              } } }
    }
    if (ok)
        yz_native_spurs_notify_write((uint32_t)addr, 8);
#if !defined(YZ_PERF_CLEAN)
    if (ok && (uint32_t)addr < 0x40C00000u &&
        (uint32_t)addr + 8u > 0x40400000u) {
        if ((uint32_t)addr >= 0x40400000u)
            yz_a010_reltrace_ppu_store(
                (uint32_t)addr, (uint32_t)(val >> 32),
                yz_guest_addr_from_host(_ReturnAddress()));
        if ((uint32_t)addr + 4u >= 0x40400000u)
            yz_a010_reltrace_ppu_store(
                (uint32_t)addr + 4u, (uint32_t)val,
                yz_guest_addr_from_host(_ReturnAddress()));
    }
#endif
    ctx->reserve_addr = 0;
    /* CR0 = 0b00 || store_ok || XER[SO] (the conditional-store record form
     * folds the sticky SO like every compare/record op; SO is provably 0 in
     * this title today, so this is spec conformance, not a behavior change). */
    ctx->cr = (ctx->cr & ~(0xFu << 28))
              | ((((ok ? 2u : 0u) | ((ctx->xer >> 31) & 1u))) << 28);
}

extern "C" void yz_rsx_inline_on_put(void);   /* import_overrides.cpp: inline FIFO drain on PUT flush (YZ_RSX_INLINE) */
/* YZ_JOBSTREAM_WATCH (s21): confirm the func_00E5F248 clobber hypothesis
 * (scratch/jobchain_pointer_re.md) -- log every store to the jobchain command
 * slots 1/2 (0x4019CA88/90) with the WRITER's guest address. Cheap: two
 * compares behind a cached flag, on hits only. */
static inline void yz_jobstream_watch(uint32_t a, uint64_t val, unsigned w, const void* retaddr)
{
    /*
     * Focused a010 command-arena publication witness.  The nine static-stage
     * records are present in this arena after FE70A0 but absent at gs_task
     * dispatch.  Name the PPU instruction that advances or resets the arena
     * head/count, without page guards or a broad memory trace.
     */
    static int stage_arena_on = -1;
    if (stage_arena_on < 0)
        stage_arena_on = getenv("YZ_A010_STAGE_ARENA") ? 1 : 0;
    if (stage_arena_on && g_yz_a010_stage_generation != 0 &&
        g_yz_game_toc != 0) {
        const uint32_t arena = vm_read32(g_yz_game_toc - 0x72E8u);
        const uint32_t head_ea = arena + 0x00u;
        const uint32_t count_ea = arena + 0x1Cu;
        const bool hits_head = a <= head_ea && a + w > head_ea;
        const bool hits_count = a <= count_ea && a + w > count_ea;
        if (arena >= 0x10000u && (hits_head || hits_count)) {
            static volatile long stage_arena_n = 0;
            const unsigned long n = (unsigned long)
                _InterlockedIncrement(&stage_arena_n);
            const uint32_t old_head = vm_read32(head_ea);
            const uint32_t old_count = vm_read32(count_ea);
            uint32_t new_word = (uint32_t)val;
            if (w == 8u && hits_head && a != head_ea)
                new_word = (uint32_t)(val >> 32);
            if (w == 8u && hits_count && a != count_ea)
                new_word = (uint32_t)(val >> 32);
            const bool reset =
                (hits_head && new_word < old_head) ||
                (hits_count && new_word <= old_count);
            if (n <= 256u || reset ||
                (n & (n - 1u)) == 0u) {
                fprintf(stderr,
                        "[a010-stage-arena] n=%lu gen=%ld tid=%u "
                        "store%u ea=%08X val=%016llX "
                        "oldHead=%08X oldCount=%u caller=%08X%s\n",
                        n, g_yz_a010_stage_generation,
                        yz_thread_current_id(), w * 8u, a,
                        (unsigned long long)val,
                        old_head, old_count,
                        yz_guest_addr_from_host(retaddr),
                        reset ? " RESET" : "");
                fflush(stderr);
            }
        }
    }

    /*
     * The frontier recorder replaces per-store diagnostic output. Test the
     * narrow address ranges first so ordinary vm_write32/64 calls do not pay
     * even an atomic armed-state read.
     */
    if ((a + w > 0x4019CA80u && a < 0x4019CB40u) ||
        (a + w > 0x10200FE0u && a < 0x10200FF0u)) {
        if (yz_frontier_trace_is_armed()) {
            yz_frontier_trace_emit(
                YZ_FT_PPU_STORE, yz_thread_current_id(),
                yz_guest_addr_from_host(retaddr),
                a, w, (uint32_t)(val >> 32), (uint32_t)val, 0, 0);
        }
    }

    static int on = -1;
    if (on < 0) { on = getenv("YZ_JOBSTREAM_WATCH") ? 1 : 0;
        if (on) fprintf(stderr, "[jsw] ARMED (YZ_JOBSTREAM_WATCH): 0x4019CA88/0x4019CA90 store watch\n"); }
    if (!on) return;
    if ((a + w > 0x4019CA80u && a < 0x4019CB40u) ||     /* jobchain cmd stream */
        (a + w > 0x10200FE0u && a < 0x10200FF0u)) {     /* movie decode-sync label */
        uint32_t caller = yz_guest_addr_from_host(retaddr);
        fprintf(stderr, "[jsw] tid=%u store%u ea=0x%08X val=0x%016llX caller=0x%08X\n",
                yz_thread_current_id(), w * 8, a, (unsigned long long)val, caller);
        fflush(stderr);
    }
}

/* YZ_A010_CONSTSRC: follow the 17-word transform-constant packet while the
 * PPU-side builder writes it.  This catches the source call stack before the
 * gs_task SPU copies the packet into the live FIFO. */
static void yz_a010_constsrc_read64(
    uint32_t addr, uint64_t val, const void* retaddr)
{
    static int on = -1;
    static int skin_trace = -1;
    static int skin_watch = -1;
    static unsigned hits = 0;
    static unsigned skin_hits = 0;
    if (on < 0) on = getenv("YZ_A010_CONSTSRC") ? 1 : 0;
    if (skin_trace < 0)
        skin_trace = getenv("YZ_A010_SKIN_TRACE") ? 1 : 0;
    if (skin_watch < 0)
        skin_watch = getenv("YZ_A010_SKIN_WATCH") ? 1 : 0;
    if ((!on && !skin_trace) || !g_yz_a010_root_active)
        return;
    if (yz_guest_addr_from_host(retaddr) != 0x00EB9D84u) return;

    const uint32_t hi = (uint32_t)(val >> 32);
    const uint32_t lo = (uint32_t)val;
    const bool hi_nan =
        (hi & 0x7F800000u) == 0x7F800000u && (hi & 0x007FFFFFu);
    const bool lo_nan =
        (lo & 0x7F800000u) == 0x7F800000u && (lo & 0x007FFFFFu);
    const bool skin_sentinel =
        (hi & ~1u) == 0x461C3FFEu || (lo & ~1u) == 0x461C3FFEu;
    if (on && (hi_nan || lo_nan) && hits < 64u) {
        hits++;
        void* bt[20];
        const unsigned short got =
            RtlCaptureStackBackTrace(1, 20, bt, nullptr);
        fprintf(stderr,
                "[a010-constsrc-read] hit=%u tid=%u src=%08X "
                "val=%016llX caller=%08X stack:",
                hits, yz_thread_current_id(), addr,
                (unsigned long long)val,
                yz_guest_addr_from_host(retaddr));
        for (unsigned k = 0; k < got; k++) {
            const uint32_t guest = yz_guest_addr_from_host(bt[k]);
            if (guest) fprintf(stderr, " %08X", guest);
        }
        fputc('\n', stderr);
        fflush(stderr);
    }

    /*
     * func_00EB9D84 reads source+0x18 first.  In the broken orphanage skin
     * palette that first read contains the exact 10000-unit sentinel seen in
     * c113.w.  Recover and dump the source matrix, then arm the existing
     * write-watch so the following frame names the routine that produces it.
     */
    if (skin_trace && skin_sentinel && skin_hits++ < 16u) {
        const uint32_t source = addr - 0x18u;
        fprintf(stderr, "[a010-skin-source] source=%08X", source);
        for (unsigned row = 0; row < 4; ++row) {
            fprintf(stderr, " c%u=", 112u + row);
            for (unsigned column = 0; column < 4; ++column) {
                const uint32_t bits =
                    vm_read32(source + (row * 4u + column) * 4u);
                float value;
                memcpy(&value, &bits, sizeof(value));
                fprintf(stderr, "%s%.7g",
                    column == 0 ? "(" : " ", (double)value);
            }
            fputc(')', stderr);
        }
        fputc('\n', stderr);
        fflush(stderr);

        static bool watch_armed = false;
        if (skin_watch && !watch_armed) {
            watch_armed = true;
            yz_watch_arm(source);
        }
    }
}

static inline void yz_a010_constsrc_write(
    uint32_t addr, uint64_t val, unsigned width, const void* retaddr)
{
    static int on = -1;
    static unsigned hits = 0;
    static thread_local uint32_t packet = 0;
    static thread_local int phase = 0;
    if (on < 0) on = getenv("YZ_A010_CONSTSRC") ? 1 : 0;
    if (!on || !g_yz_a010_root_active) return;

    if (width == 4 && (uint32_t)val == 0x00441EFCu) {
        /* The lifted builder stores the slot and one payload chunk before the
         * header, then restores its frame and writes the remaining chunks.
         * Capture the caller here when the already-written slot is 108. */
        if (vm_read32((uint64_t)addr + 4u) == 108u && hits < 32u) {
            hits++;
            void* bt[20];
            const unsigned short got =
                RtlCaptureStackBackTrace(1, 20, bt, nullptr);
            fprintf(stderr,
                    "[a010-constsrc] header=%u tid=%u packet=%08X "
                    "caller=%08X stack:",
                    hits, yz_thread_current_id(), addr,
                    yz_guest_addr_from_host(retaddr));
            for (unsigned k = 0; k < got; k++) {
                const uint32_t guest = yz_guest_addr_from_host(bt[k]);
                if (guest) fprintf(stderr, " %08X", guest);
            }
            fputc('\n', stderr);
            fflush(stderr);
        }
        packet = addr;
        phase = 1;
        return;
    }
    if (phase == 1) {
        if (width == 4 && addr == packet + 4u && (uint32_t)val == 108u)
            phase = 2;
        else
            phase = 0;
        return;
    }
    if (phase != 2) return;
    if (addr < packet + 8u || addr >= packet + 72u) {
        phase = 0;
        return;
    }

    const uint32_t hi = (uint32_t)(val >> 32);
    const uint32_t lo = (uint32_t)val;
    const bool hi_nan =
        (hi & 0x7F800000u) == 0x7F800000u && (hi & 0x007FFFFFu);
    const bool lo_nan =
        (lo & 0x7F800000u) == 0x7F800000u && (lo & 0x007FFFFFu);
    if ((hi_nan || lo_nan) && hits < 32u) {
        hits++;
        void* bt[20];
        const unsigned short got =
            RtlCaptureStackBackTrace(1, 20, bt, nullptr);
        fprintf(stderr,
                "[a010-constsrc] hit=%u tid=%u packet=%08X write=%08X "
                "val=%016llX caller=%08X stack:",
                hits, yz_thread_current_id(), packet, addr,
                (unsigned long long)val,
                yz_guest_addr_from_host(retaddr));
        for (unsigned k = 0; k < got; k++) {
            const uint32_t guest = yz_guest_addr_from_host(bt[k]);
            if (guest) fprintf(stderr, " %08X", guest);
        }
        fputc('\n', stderr);
        fflush(stderr);
    }
    if (addr + width >= packet + 72u)
        phase = 0;
}

/*
 * Exact, low-overhead producer attribution for the two transform banks and
 * the shared matrix copied into stage objects.  Unlike the page-protection
 * watch this does not single-step unrelated writes on the same 4 KB pages.
 */
static inline void yz_stage_transform_write(
    uint32_t addr, uint64_t val, unsigned width, const void* retaddr)
{
    static int on = -1;
    static unsigned hits = 0;
    if (on < 0) on = getenv("YZ_STAGE_TRANSFORM_TRACE") ? 1 : 0;
    if (!on || !g_yz_a010_root_active || hits >= 512u)
        return;

    const uint32_t end = addr + width;
    const bool primary =
        addr < 0x015BD080u && end > 0x015BCF40u;
    const bool secondary =
        addr < 0x015BD1C0u && end > 0x015BD080u;
    const bool shared =
        addr < 0x01622690u && end > 0x01622590u;
    if (!primary && !secondary && !shared)
        return;

    uint64_t old = 0;
    if (width == 4u) {
        uint32_t raw = 0;
        memcpy(&raw, vm_base + addr, sizeof(raw));
        old = ps3_bswap32(raw);
    } else if (width == 8u) {
        uint64_t raw = 0;
        memcpy(&raw, vm_base + addr, sizeof(raw));
        old = ps3_bswap64(raw);
    }
    if (old == val)
        return;

    ++hits;
    void* bt[20];
    const unsigned short got =
        RtlCaptureStackBackTrace(1, 20, bt, nullptr);
    fprintf(stderr,
            "[stage-transform-write] n=%u tid=%u region=%s ea=%08X "
            "width=%u old=%016llX new=%016llX caller=%08X stack:",
            hits, yz_thread_current_id(),
            primary ? "primary" : secondary ? "secondary" : "shared",
            addr, width,
            (unsigned long long)old, (unsigned long long)val,
            yz_guest_addr_from_host(retaddr));
    for (unsigned k = 0; k < got; ++k) {
        const uint32_t guest = yz_guest_addr_from_host(bt[k]);
        if (guest) fprintf(stderr, " %08X", guest);
    }
    fputc('\n', stderr);
    fflush(stderr);
}

#if defined(YZ_PERF_CLEAN)
extern "C" void vm_write8(uint64_t addr, uint8_t val)
{
    VM_WRITE_COH(addr, &val, sizeof(val));
}

extern "C" void vm_write16(uint64_t addr, uint16_t val)
{
    const uint16_t value = ps3_bswap16(val);
    VM_WRITE_COH(addr, &value, sizeof(value));
}

extern "C" void vm_write32(uint64_t addr, uint32_t val)
{
    const uint32_t value = ps3_bswap32(val);
    VM_WRITE_COH(addr, &value, sizeof(value));
    if ((uint32_t)addr == 0x10000040u)
        yz_rsx_inline_on_put();
}

extern "C" void vm_write64(uint64_t addr, uint64_t val)
{
    const uint64_t value = ps3_bswap64(val);
    VM_WRITE_COH(addr, &value, sizeof(value));
}
#else
extern "C" void vm_write8 (uint64_t addr, uint8_t  val) { yz_mem_guard((uint32_t)addr,1,1); if (yz_vmguard_check((uint32_t)addr,1,1)) return; VM_WRITE_COH(addr, &val, 1); }
extern "C" void vm_write16(uint64_t addr, uint16_t val) { yz_mem_guard((uint32_t)addr,2,1); if (yz_vmguard_check((uint32_t)addr,2,1)) return; uint16_t v = ps3_bswap16(val); VM_WRITE_COH(addr, &v, 2); }
extern "C" void vm_write32(uint64_t addr, uint32_t val) { yz_mem_guard((uint32_t)addr,4,1); if (yz_vmguard_check((uint32_t)addr,4,1)) return; yz_jobstream_watch((uint32_t)addr, val, 4, _ReturnAddress()); yz_a010_constsrc_write((uint32_t)addr, val, 4, _ReturnAddress()); yz_stage_transform_write((uint32_t)addr, val, 4, _ReturnAddress()); yz_a010_camera_write((uint32_t)addr, val, 4, _ReturnAddress()); if (g_yz_a010_release_scene_active && (uint32_t)addr >= 0x40400000u && (uint32_t)addr < 0x40C00000u) yz_a010_reltrace_ppu_store((uint32_t)addr, val, yz_guest_addr_from_host(_ReturnAddress())); uint32_t v = ps3_bswap32(val); VM_WRITE_COH(addr, &v, 4); if ((uint32_t)addr == 0x10000040u) yz_rsx_inline_on_put(); }
extern "C" void vm_write64(uint64_t addr, uint64_t val) { yz_mem_guard((uint32_t)addr,8,1); if (yz_vmguard_check((uint32_t)addr,8,1)) return; yz_jobstream_watch((uint32_t)addr, val, 8, _ReturnAddress()); yz_a010_constsrc_write((uint32_t)addr, val, 8, _ReturnAddress()); yz_stage_transform_write((uint32_t)addr, val, 8, _ReturnAddress()); yz_a010_camera_write((uint32_t)addr, val, 8, _ReturnAddress()); if (g_yz_a010_release_scene_active && (uint32_t)addr < 0x40C00000u && (uint64_t)(uint32_t)addr + 8u > 0x40400000ull) { const uint32_t pc = yz_guest_addr_from_host(_ReturnAddress()); if ((uint32_t)addr >= 0x40400000u) yz_a010_reltrace_ppu_store((uint32_t)addr, (uint32_t)(val >> 32), pc); if ((uint32_t)addr + 4u >= 0x40400000u && (uint32_t)addr + 4u < 0x40C00000u) yz_a010_reltrace_ppu_store((uint32_t)addr + 4u, (uint32_t)val, pc); } uint64_t v = ps3_bswap64(val); VM_WRITE_COH(addr, &v, 8); }
#endif

/* Raw-byte counterpart to vm_write8/16/32/64 for lifted VMX, byte-reversed,
 * and cache-block-zero stores.  The byte sequence is already in guest memory
 * order, but it must still serialize with every overlapped GETLLAR line and
 * notify native SPURS publication watchers. */
extern "C" void vm_write_bytes(uint64_t addr, const void* src, size_t size)
{
    if (!size) return;
    const uint32_t first = (uint32_t)addr;
    const uint64_t end64 = (uint64_t)first + size;
    if (end64 > 0x100000000ull) return;
#if !defined(YZ_PERF_CLEAN)
    yz_mem_guard(first, (unsigned)size, 1);
    if (yz_vmguard_check(first, (unsigned)size, 1)) return;
#endif

    const uint32_t first_line = first & ~127u;
    const uint32_t last_line = (uint32_t)(end64 - 1u) & ~127u;
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
    yz_native_spurs_notify_write(first, (uint32_t)size);
}

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

    /* PPU thread syscalls the game issues directly (43-49): route them to
     * the runner's thread runtime (threads.cpp) instead of the runtime's
     * standalone table, so ids and joins line up with threads created
     * through the sysPrxForUser import overrides. 45/46 (detach/
     * get_join_state) used to fall through to the generic runtime handlers,
     * which key off a table Yakuza threads never populate -- route them here
     * too (declared extern since yakuza_runner.h is out of scope for this
     * batch of fixes). */
    lv2_syscall_register(&g_lv2_syscalls, 43, yz_sc_thread_yield);
    lv2_syscall_register(&g_lv2_syscalls, 44, yz_sc_thread_join);
    lv2_syscall_register(&g_lv2_syscalls, 45, yz_sc_thread_detach);
    lv2_syscall_register(&g_lv2_syscalls, 46, yz_sc_thread_get_join_state);
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

/* s37 PROTOTYPES B/C: bounded-concurrency gate hooks (defined in threads.cpp). */
extern "C" void yz_gate_syscall_release(void);
extern "C" void yz_gate_syscall_acquire(void);

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
    if (
#if defined(YZ_PERF_CLEAN)
        g_yz_runtime_config.skip_voice &&
#endif
        num == 107 && (uint32_t)ctx->gpr[3] == 9 &&
        yz_thread_current_id() == 1) {
        static long vw = 0; long n = ++vw;
        uint32_t r31 = (uint32_t)ctx->gpr[31];
        uint32_t adxm = 0x01613368u, blk = 0x01654294u;   /* ADXM handle / cond block (pt47) */
#if !defined(YZ_PERF_CLEAN)
        if (n <= 3 || (n % 500) == 0) {
            fprintf(stderr, "[voice-wait] n=%ld r31=0x%08X [r31+298]=%08X "
                    "adxm[+294]=%08X [+298]=%08X [+29C]=%08X blk[+00]=%08X [+10]=%08X\n",
                    n, r31, vm_read32(r31 + 0x298),
                    vm_read32(adxm + 0x294), vm_read32(adxm + 0x298), vm_read32(adxm + 0x29C),
                    vm_read32(blk + 0x00), vm_read32(blk + 0x10));
            fflush(stderr);
        }
#endif
        if (g_yz_runtime_config.skip_voice && n >= 700) {
            static int once = 0;
            vm_write32(r31  + 0x298, 0xFFFFFFFFu);   /* innermost-frame predicate candidate */
            vm_write32(adxm + 0x298, 0xFFFFFFFFu);   /* ADXM handle predicate candidate */
            vm_write32(blk  + 0x10,  2u);            /* cond-block prime flag -> PLAYING */
            if (!once) { once = 1;
                fprintf(stderr, "[voice-skip] forcing readiness from n=%ld (r31=0x%08X)\n", n, r31);
                fflush(stderr); }
        }
    }

#if !defined(YZ_PERF_CLEAN)
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
#endif

    /* === YZ_SKIP_VOICE (re-keyed 2026-07-05): the real t1 movie-gate spin is
     * cond_signal(4), NOT cond_wait(9) (which fires only ~1-4x). The CRI voice
     * codec (ADX/HCA in adv_voice_talk.cvm) never decodes → the voice-readiness
     * predicate stays 0 → t1 spins here forever. Experiment: force the candidate
     * readiness fields on this spin (after init settles) to see if t1 advances
     * past the voice gate and the NEXT wall surfaces. OFF by default. === */
    if (g_yz_runtime_config.skip_voice &&
        num == 108 && (uint32_t)ctx->gpr[3] == 4 &&
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

#if !defined(YZ_PERF_CLEAN)
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

    /* YZ_T1POLL (s21, the phase-2 park): t1's terminal spin is func_00EB3238's
     * throttle `while (*counter < target) usleep(30)` (0xEB3340-58: r29=[r30],
     * counter=[r29], target=r31). On each t1 usleep, dump the polled counter
     * chain + the FIFO GET/PUT/REF + the flip counter 0x40C00000, rate-limited
     * ~1/s, so the starved counter (and who should bump it) is named directly.
     * ARMED banner printed on first hit (probe-liveness rule). */
    {
        static int t1poll = -1;
        if (t1poll < 0) t1poll = getenv("YZ_T1POLL") ? 1 : 0;
        if (t1poll && num == 141 && yz_thread_current_id() == 1) {
            static ULONGLONG last = 0;
            ULONGLONG now = GetTickCount64();
            if (now - last >= 1000) {
                last = now;
                uint32_t r30 = (uint32_t)ctx->gpr[30], r31 = (uint32_t)ctx->gpr[31];
                uint32_t r29 = (r30 >= 0x10000u && r30 < 0xE0000000u) ? vm_read32(r30) : 0;
                uint32_t cnt = (r29 >= 0x10000u && r29 < 0xE0000000u) ? vm_read32(r29) : 0;
                static int armed = 0;
                if (!armed) { armed = 1;
                    fprintf(stderr, "[t1poll] ARMED (YZ_T1POLL): t1 sc141 throttle probe live\n"); }
                fprintf(stderr, "[t1poll] usleep=%llu lr=0x%08llX ptr[r30=0x%08X]=0x%08X "
                        "counter[0x%08X]=0x%08X target(r31)=0x%08X | GET=0x%06X PUT=0x%06X "
                        "REF=0x%08X flipcnt=0x%08X\n",
                        (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->lr,
                        r30, r29, r29, cnt, r31,
                        vm_read32(0x10000044u) & 0xFFFFFF,   /* RSX_DMA_CONTROL GET */
                        vm_read32(0x10000040u) & 0xFFFFFF,   /* PUT */
                        vm_read32(0x10000048u),              /* REF */
                        vm_read32(0x40C00000u));
                fflush(stderr);
            }
        }
    }
#endif

#if !defined(YZ_PERF_CLEAN)
    static unsigned char seen[1100];
    int first = (num < sizeof(seen)) && !seen[num];
    /* YZ_LV2_LOG (SPU-SPEED item 2): the spu_range/intr clauses below log
     * EVERY matching syscall, not just the first -- and spu_range's window
     * (num 82..200) covers the whole SPURS management-syscall family, so it
     * fires at high rate during SPU activity (a hot unconditional stderr
     * write). Default OFF: only the cheap one-per-syscall-number `first`
     * print survives. `=1` restores the full original always-on behavior
     * (both clauses), unchanged, for A/B / deep syscall tracing. */
    static int lv2_log_full = -1;
    if (lv2_log_full < 0) lv2_log_full = getenv("YZ_LV2_LOG") ? 1 : 0;
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
#endif
    uint64_t a3 = ctx->gpr[3], a4 = ctx->gpr[4], a5 = ctx->gpr[5];
#if !defined(YZ_PERF_CLEAN)
    uint64_t a6 = ctx->gpr[6];
#endif

    /* Record the in-flight syscall so a stall dump can name exactly what each
     * blocked thread is parked in (object-id args survive in the GPRs while the
     * dispatch below blocks). Cleared on return. (blocker #21 diagnostic) */
    /* s37 PROTOTYPES B/C: the bounded-concurrency gate. A guest PPU thread holds
     * its run-slot while executing guest code; vacate it across the (possibly
     * blocking) syscall dispatch so a higher-priority runnable thread can run,
     * then re-acquire on return (the priority arbitration point). No-op when the
     * gate is OFF (default). */
    yz_wait_enter(num, a3, a4, a5);
    yz_gate_syscall_release();
    lv2_syscall_dispatch(&g_lv2_syscalls, ctx);
    yz_gate_syscall_acquire();
    yz_wait_exit();

#if !defined(YZ_PERF_CLEAN)
    if (first || (lv2_log_full && (spu_range || intr))) {
        seen[num] = 1;
        fprintf(stderr, "[LV2%s t%u] sc %u (r3=0x%llX r4=0x%llX r5=0x%llX r6=0x%llX)"
                " -> 0x%llX\n", first ? ":first" : "",
                yz_thread_current_id(), num,
                (unsigned long long)a3, (unsigned long long)a4,
                (unsigned long long)a5, (unsigned long long)a6,
                (unsigned long long)ctx->gpr[3]);
        fflush(stderr);
    }
#endif
}

/* Guest-callback hook g_ps3_guest_caller: defined by the runtime
 * (libs/system/cellSysutil.c), installed by main.cpp. */

/* s28: [t1-hb] rider globals (lv2_syscall_table.h externs) */
extern "C" uint32_t g_yz_t1_sc = 0;
extern "C" uint64_t g_yz_t1_sc_r3 = 0;
