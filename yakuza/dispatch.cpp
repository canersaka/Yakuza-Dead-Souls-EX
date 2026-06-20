/*
 * Indirect-call dispatcher for the recompiled Yakuza: Dead Souls EBOOT.
 *
 * The generated code (ppu_recomp.c) calls ps3_indirect_call(ctx) for
 * bctr/bctrl and expects:
 *   - the guest target address in ctx->ctr,
 *   - the dispatcher to resolve it to a host function (with OPD fallback:
 *     if ctr points at an OPD descriptor, word0 = code addr, word1 = TOC),
 *   - cross-fragment tail branches to be flattened via the thread-local
 *     g_trampoline_fn + the DRAIN_TRAMPOLINE loops the lifter emits.
 *
 * We resolve the target and set g_trampoline_fn instead of calling directly,
 * so long bctr tail-chains don't grow the host stack; the drain loop at the
 * call site (or in main) executes it iteratively.
 */

#include "ppu_recomp.h"
#include "yakuza_runner.h"

#include <cstdio>
#include <cstdlib>

/* TLS trampoline slot -- the generated code declares this extern and both
 * sets it (direct tail branches) and drains it. */
extern "C" __declspec(thread) void (*g_trampoline_fn)(void*) = nullptr;

static bool addr_readable(uint32_t a)
{
    /* main memory (incl. loaded ELF) or stack region */
    return (a >= 0x00010000u && a < 0x10000000u) ||
           (a >= 0xD0000000u && a < 0xE0000000u);
}

extern "C" yz_ppu_fn yz_lookup_func(uint32_t guest_addr)
{
    unsigned lo = 0, hi = g_yz_func_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        uint32_t a = g_yz_func_table[mid].addr;
        if (a == guest_addr) return g_yz_func_table[mid].fn;
        if (a < guest_addr)  lo = mid + 1;
        else                 hi = mid;
    }
    return nullptr;
}

/* The game module's TOC (set from the entry OPD in main). Used to repair a
 * lost TOC when a guest function is dispatched with r2==0 (see yz_tramp_guard). */
extern "C" uint32_t g_yz_game_toc = 0;

/* Reverse-map a host fn ptr to its guest address (linear scan, cached miss). */
static uint32_t yz_guest_of_host(void* hf)
{
    for (unsigned i = 0; i < g_yz_func_count; i++)
        if ((void*)g_yz_func_table[i].fn == hf) return g_yz_func_table[i].addr;
    return 0;
}

/* TOC repair, called by every trampoline dispatch (inline DRAIN_TRAMPOLINE
 * macro + yz_drain_trampolines). A gcm callback (e.g. the game's flip/vblank
 * handler) can be invoked by its bare code address rather than its OPD, so the
 * OPD's TOC word is never loaded and the callee runs with r2==0 -> it faults on
 * its first TOC-relative access. The game module has ONE TOC, so restoring it
 * for a game-range target (addr < firmware base 0x02000000) is correct. */
/* Trampoline-hop ring (per thread). Records the host fn ptr of every chunk the
 * trampoline driver dispatches. The host stack is FLAT for cross-chunk chains
 * (each chunk returns to the DRAIN_TRAMPOLINE loop, then the next is called),
 * so a normal stack walk can't reconstruct the guest control-flow path. The
 * crash handler reverse-maps this ring to name the exact chunk sequence that
 * led to a fault. TEMP DIAG (#19d r31=0): strip once the cause is pinned. */
extern "C" __declspec(thread) void*    g_yz_tramp_ring[256] = {};
extern "C" __declspec(thread) uint64_t g_yz_tramp_r31[256]  = {};
extern "C" __declspec(thread) uint64_t g_yz_tramp_r1[256]   = {};
extern "C" __declspec(thread) unsigned g_yz_tramp_idx      = 0;

extern "C" void yz_tramp_guard(void* tf, void* ctxv)
{
    ppu_context* ctx0 = (ppu_context*)ctxv;
    unsigned _ri = g_yz_tramp_idx++ & 255;
    g_yz_tramp_ring[_ri] = tf;
    g_yz_tramp_r31[_ri]  = ctx0->gpr[31];
    g_yz_tramp_r1[_ri]   = ctx0->gpr[1];

    /* DIAG (2026-06-16, drain hunt, env YZ_DRAIN_TRACE): persistent counters for the
     * gcm release/defer/drain functions, to settle whether the producer EVER reaches
     * the drain (func_00E9B7C0/B8E0 -- applies the op-list tag-0x7F releases) vs only
     * the defer path (func_00E9BDD0). func_00E9BF14 clears S[0x1C]. */
    { static int en = -1; if (en < 0) en = getenv("YZ_DRAIN_TRACE") ? 1 : 0;
      if (en) {
        static void* fn[4]; static long c[4]; static int init = 0;
        if (!init) { init = 1;
            fn[0]=(void*)yz_lookup_func(0x00E9BDD0u);   /* defer (sets S[0x1C]) */
            fn[1]=(void*)yz_lookup_func(0x00E9BF14u);   /* flush (clears S[0x1C]) */
            fn[2]=(void*)yz_lookup_func(0x00E9B7C0u);   /* drain-family flush */
            fn[3]=(void*)yz_lookup_func(0x00E9B8E0u); } /* drain loop */
        for (int i = 0; i < 4; i++) if (tf == fn[i] && fn[i]) {
            static const uint32_t a[4] = {0xE9BDD0u,0xE9BF14u,0xE9B7C0u,0xE9B8E0u};
            if ((c[i]++ & 0x1FF) == 0)
                fprintf(stderr, "[drain] func_%08X hit #%ld tid=%u\n", a[i], c[i], yz_thread_current_id());
        }
      } }
    ppu_context* ctx = (ppu_context*)ctxv;
    if (!g_yz_game_toc || ctx->gpr[2] == g_yz_game_toc) return;
    uint32_t g = yz_guest_of_host(tf);
    if (!g || g >= 0x02000000u) return;   /* game module: single TOC */
    static int n = 0;
    if (n < 6) { n++;
        fprintf(stderr, "[toc-fix] r2 0x%08llX -> 0x%08X for func_%08X "
                "(game fn invoked with wrong/no TOC)\n",
                (unsigned long long)ctx->gpr[2], g_yz_game_toc, g);
    }
    ctx->gpr[2] = g_yz_game_toc;
}

extern "C" void yz_drain_trampolines(ppu_context* ctx)
{
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        yz_tramp_guard((void*)tf, (void*)ctx);
        tf((void*)ctx);
    }
}

/* Imported firmware functions: fake-key range -> host HLE bridge index. */
static yz_ppu_fn import_bridge_for(uint32_t code)
{
    if (code >= YZ_IMPORT_FAKE_BASE &&
        code <  YZ_IMPORT_FAKE_BASE + g_yz_import_count * 4)
        return g_yz_import_bridges[(code - YZ_IMPORT_FAKE_BASE) / 4];
    if (code == YZ_GCM_CB_FAKE_KEY)   /* runner-owned gcm fifo callback */
        return yz_gcm_fifo_callback;
    return nullptr;
}

/* Ring buffer of recent indirect-call targets, dumped by the crash handler. */
uint32_t g_yz_last_targets[16];
unsigned g_yz_last_idx;

/* Defined in import_overrides.cpp (extern "C"); set by YZ_TASK_TRACE below. */
extern "C" uint32_t g_yz_spurs_taskset;

extern "C" void ps3_indirect_call(ppu_context* ctx)
{
    uint32_t target = (uint32_t)ctx->ctr;
    g_yz_last_targets[g_yz_last_idx++ & 15] = target;

    /* DIAG (2026-06-16, drain trigger, env YZ_CB_TRACE): does libgcm fire the game's
     * gcm CONTEXT CALLBACK (gcm_ctx+0xC = OPD 0x01355208 -- the drain lives inside it)
     * on our per-segment buffer-fills, and does the op-list drain across it? RPCS3 (same
     * LLE code) drains continuously here; if ours never fires, that's the fixable gap. */
    { static int en = -1; if (en < 0) en = getenv("YZ_CB_TRACE") ? 1 : 0;
      if (en) {
        static uint32_t cb_code = 0; static int cbi = 0;
        if (!cbi) { cbi = 1; cb_code = vm_read32(0x01355208u); }
        if (cb_code && target == cb_code) {
            uint32_t depth = 0;
            if (g_yz_game_toc) { uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
                if (S >= 0x10000u && S < 0xE0000000u) {
                    uint32_t base = vm_read32(S+8u), head = vm_read32(S+0u);
                    if (head >= base && head-base <= 0x40000u) depth = (head-base)/0x20u; } }
            static long n = 0;
            fprintf(stderr, "[cb] gcm context-callback fired #%ld tid=%u oplist=%u\n",
                    ++n, yz_thread_current_id(), depth);
        }
      } }

    /* DIAG (env YZ_TASK_TRACE, 2026-06-16b): identify the game's render SPU task.
     * cellSpursCreateTask2WithBinInfo (libsre OPD 0x0203161C) args: r3=taskset2,
     * r4=taskId(out), r5=binInfo, r6=arg, r7=attr. binInfo->eaElf (u64 @+0) is the
     * task's SPU ELF ea -> the image to LIFT for 1f (SPU task dispatch). Log it +
     * the eaElf words so we know which of the EBOOT's 10 SPU images it is. */
    { static int en = -1; if (en < 0) en = getenv("YZ_TASK_TRACE") ? 1 : 0;
      if (en) {
        static uint32_t t2_code = 0; static int t2i = 0;
        if (!t2i) { t2i = 1; t2_code = vm_read32(0x0203161Cu); }
        if (target == 0x0203161Cu || (t2_code && target == t2_code)) {
            g_yz_spurs_taskset = (uint32_t)ctx->gpr[3];   /* for the SPURS-state dump */
            uint32_t bi = (uint32_t)ctx->gpr[5];
            uint64_t eaElf = bi ? ((uint64_t)vm_read32(bi) << 32 | vm_read32(bi + 4)) : 0;
            uint32_t elf = (uint32_t)eaElf;
            fprintf(stderr, "[task] CreateTask2WithBinInfo taskset=0x%llX taskId=0x%llX "
                    "binInfo=0x%X arg=0x%llX -> eaElf=0x%08X (w0=0x%08X w1=0x%08X)\n",
                    (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4], bi,
                    (unsigned long long)ctx->gpr[6], elf,
                    elf ? vm_read32(elf) : 0, elf ? vm_read32(elf + 4) : 0);
            fflush(stderr);
        }
      } }

    /* DIAG (vblank-dispatch hunt): the gcm interrupt thread (t7) runs inside
     * libgcm (0x021xxxxx); if it makes an indirect CALL into GAME range it is
     * dispatching the game's registered vblank/flip handler. Catch it -- this
     * proves whether the handler actually fires per interrupt. */
    if (yz_thread_current_id() == 7 && target >= 0x10000u && target < 0x02000000u) {
        static int n = 0;
        if (n < 60) { n++;
            fprintf(stderr, "[diag t7] intr -> GAME handler dispatch target=0x%08X "
                    "(r3=0x%llX r4=0x%llX)\n", target,
                    (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4]);
            fflush(stderr);
        }
    }
    /* DIAG (flip-wait hunt): capture WHERE the render thread (t1) calls the
     * vblank-pump 0x00E7ED8C from -- ctx->lr is the return addr = t1's poll
     * loop. That loop is the busy-poll we need to compare to RPCS3's semaphore
     * wait. Also log t1's other indirect targets to map the loop body. */
    if (yz_thread_current_id() == 1 && target == 0x00E7ED8Cu) {
        static int n = 0;
        if (n < 20) { n++;
            fprintf(stderr, "[diag t1] pump 0xE7ED8C from lr=0x%08llX r3=0x%llX\n",
                    (unsigned long long)ctx->lr, (unsigned long long)ctx->gpr[3]);
            fflush(stderr);
        }
    }

    /* SINGLE-SEGMENT override (env YZ_ONESEG, 2026-06-20): libgcm's segment-recycle
     * reserve (func_02103AAC) wedges when GET is parked outside the ring (in an
     * unfinalized display list) and the producer must recycle a 1 MB segment. Promote
     * the FIFO to ONE buffer-spanning segment so the producer writes linearly and never
     * recycles mid-frame. Returns 1 -> handled, skip the real reserve (don't set the
     * trampoline = the bctrl returns to the producer with end already extended). */
    if (target == 0x02103AACu) {
        extern int yz_gcm_reserve_oneseg(ppu_context*);
        static int en = -1; if (en < 0) en = getenv("YZ_ONESEG") ? 1 : 0;
        if (en && yz_gcm_reserve_oneseg(ctx)) return;
    }

    /* Bridges complete entirely on the host, so call directly. */
    if (yz_ppu_fn bridge = import_bridge_for(target)) {
        /* TEMP DEBUG (SPURS bring-up): trace every import call made by the
         * LLE firmware module (indices >= g_yz_lle_import_first) with args
         * and result. Strip once SPURS init survives. */
        unsigned idx = (target - YZ_IMPORT_FAKE_BASE) / 4;
        int trace = target != YZ_GCM_CB_FAKE_KEY &&
                    idx >= g_yz_lle_import_first && idx < g_yz_import_count;
        if (trace)
            fprintf(stderr, "[lle-call t%u] %s(r3=0x%llX r4=0x%llX r5=0x%llX r6=0x%llX)\n",
                    yz_thread_current_id(), g_yz_import_names[idx],
                    (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],
                    (unsigned long long)ctx->gpr[5], (unsigned long long)ctx->gpr[6]);
        bridge(ctx);
        if (trace) {
            fprintf(stderr, "[lle-call t%u]   -> 0x%llX\n",
                    yz_thread_current_id(), (unsigned long long)ctx->gpr[3]);
            fflush(stderr);
        }
        return;
    }

    yz_ppu_fn fn = yz_lookup_func(target);
    /* TOC repair: a gcm callback invoked by its bare code address (target is a
     * game function found directly, no OPD) keeps the CALLER's TOC (libgcm's, or
     * 0). The game module has exactly one TOC, so force it for any game target. */
    if (fn && target < 0x02000000u && g_yz_game_toc && ctx->gpr[2] != g_yz_game_toc)
        ctx->gpr[2] = g_yz_game_toc;
    if (!fn && addr_readable(target)) {
        /* OPD fallback: ctr may hold a descriptor address rather than code.
         * The descriptor's code word may itself be an import-bridge key
         * (our synthetic OPDs, or game-held pointers to import slots). */
        uint32_t code = vm_read32(target);
        uint32_t toc  = vm_read32(target + 4);
        if (yz_ppu_fn bridge = import_bridge_for(code)) {
            bridge(ctx);
            return;
        }
        fn = yz_lookup_func(code);
        if (fn) ctx->gpr[2] = toc;
    }

    if (!fn) {
        fprintf(stderr,
                "[dispatch] no host function for guest target 0x%08X "
                "(lr=0x%08llX r2=0x%08llX r3=0x%08llX)\n",
                target,
                (unsigned long long)ctx->lr,
                (unsigned long long)ctx->gpr[2],
                (unsigned long long)ctx->gpr[3]);
        exit(2);
    }

    g_trampoline_fn = (void (*)(void*))fn;
}

extern "C" void yz_call_guest_opd(uint32_t opd_addr, ppu_context* ctx)
{
    if (!addr_readable(opd_addr)) {
        fprintf(stderr, "[dispatch] guest callback OPD 0x%08X out of range\n", opd_addr);
        return;
    }
    uint32_t code = vm_read32(opd_addr);
    uint32_t toc  = vm_read32(opd_addr + 4);
    yz_ppu_fn fn = yz_lookup_func(code);
    if (!fn) {
        fprintf(stderr, "[dispatch] guest callback code 0x%08X (opd 0x%08X) not lifted\n",
                code, opd_addr);
        return;
    }
    uint64_t saved_toc = ctx->gpr[2];
    ctx->gpr[2] = toc;
    fn(ctx);
    yz_drain_trampolines(ctx);
    ctx->gpr[2] = saved_toc;
}
