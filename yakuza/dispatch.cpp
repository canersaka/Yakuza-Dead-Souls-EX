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
#include <chrono>
#include <cstdlib>
#include <cstdlib>

/* ntdll/kernel32 stack-walk (no windows.h here); links via kernel32.lib. */
extern "C" unsigned short __stdcall RtlCaptureStackBackTrace(
    unsigned long FramesToSkip, unsigned long FramesToCapture,
    void** BackTrace, unsigned long* BackTraceHash);

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
extern "C" __declspec(thread) uint64_t g_yz_tramp_lr[256]   = {};   /* caller return addr at the hop */
extern "C" __declspec(thread) unsigned g_yz_tramp_idx      = 0;

/* USLEEP-CALLER CAPTURE (pt26 producer-stop hunt). When g_yz_catch_caller is armed
 * (by flipadv at the stall), the next time t1 dispatches the usleep helper
 * func_00E5DB94 we grab the LIVE host backtrace -> the real guest poll-loop chain
 * (reliable, unlike the flat async stack scan). main.cpp resolves the addrs. */
extern "C" volatile long g_yz_catch_caller = 0;
extern "C" void*         g_yz_caller_bt[32] = {};
extern "C" volatile long g_yz_caller_bt_n  = 0;

extern "C" void yz_tramp_guard(void* tf, void* ctxv)
{
    ppu_context* ctx0 = (ppu_context*)ctxv;
    unsigned _ri = g_yz_tramp_idx++ & 255;
    g_yz_tramp_ring[_ri] = tf;
    g_yz_tramp_r31[_ri]  = ctx0->gpr[31];
    g_yz_tramp_r1[_ri]   = ctx0->gpr[1];
    g_yz_tramp_lr[_ri]   = ctx0->lr;     /* = caller's return addr (names the caller) */

    if (g_yz_catch_caller) {
        static void* usleep_fn = nullptr;
        if (!usleep_fn) usleep_fn = (void*)yz_lookup_func(0x00E5DB94u);
        if (tf == usleep_fn) {
            g_yz_caller_bt_n = (long)RtlCaptureStackBackTrace(0, 32, g_yz_caller_bt, nullptr);
            g_yz_catch_caller = 0;
        }
    }

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
extern "C" uint32_t g_yz_codec_taskset;   /* pt35: cri_audio codec taskset (wid 3) */

extern "C" void ps3_indirect_call(ppu_context* ctx)
{
    uint32_t target = (uint32_t)ctx->ctr;
    g_yz_last_targets[g_yz_last_idx++ & 15] = target;

    /* GENERIC indirect-call hook (env YZ_HOOK, 2026-07-02): comma-separated hex
     * guest code/OPD addresses (up to 8); logs args + lr on every bctrl to a
     * match. Replaces per-question recompiled probes like YZ_SIGCALL below.
     * LIMITATION: only sees INDIRECT calls (bctrl / function-table dispatch);
     * direct `bl` targets inside one lift never reach this dispatcher.
     * Example: YZ_HOOK=020125D8,020169D0  (names: scratch/libsre_lle_map.txt) */
    { static int hn = -1; static uint32_t hk[8];
      if (hn < 0) { hn = 0;
          const char* s = getenv("YZ_HOOK");
          while (s && *s && hn < 8) {
              char* end = NULL;
              unsigned long v = strtoul(s, &end, 16);
              if (end == s) break;
              hk[hn++] = (uint32_t)v;
              s = (*end == ',') ? end + 1 : end;
              if (*s == '\0') break;
          } }
      for (int i = 0; i < hn; i++) {
          if (target == hk[i]) {
              static int n = 0; if (n < 60) { n++;
                  fprintf(stderr, "[hook] 0x%08X(r3=0x%llX r4=0x%llX r5=0x%llX r6=0x%llX) lr=0x%llX\n",
                          target, (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],
                          (unsigned long long)ctx->gpr[5], (unsigned long long)ctx->gpr[6],
                          (unsigned long long)ctx->lr);
                  fflush(stderr); }
              break;
          }
      } }
    /* SPURS task-signal call watch (env YZ_SIGCALL, 2026-07-02, diag — REMOVE
     * when the voice frontier closes): all 6 SPU tasks park in WAIT_SIGNAL and
     * the tasksets' `signalled` bitmaps never set — does the PPU EVER call the
     * signal/queue-push family? Match both the LLE code addrs
     * (scratch/libsre_lle_map.txt) and their export OPD addrs (a bctrl may
     * carry either before the OPD fallback resolves). */
    { static int sw = -1; if (sw < 0) sw = getenv("YZ_SIGCALL") ? 1 : 0;
      if (sw) {
          const char* nm =
              (target == 0x020125D8u || target == 0x02031634u) ? "_cellSpursSendSignal" :
              (target == 0x020169D0u || target == 0x0203181Cu) ? "cellSpursQueuePushBody" :
              (target == 0x020171B8u)                          ? "_cellSpursLFQueuePushBody" :
              (target == 0x02016CFCu || target == 0x02031824u) ? "cellSpursQueuePopBody" : 0;
          if (nm) {
              static int n = 0; if (n < 40) { n++;
                  fprintf(stderr, "[sigcall] %s(r3=0x%llX r4=0x%llX r5=0x%llX) lr=0x%llX\n",
                          nm, (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],
                          (unsigned long long)ctx->gpr[5], (unsigned long long)ctx->lr);
                  fflush(stderr); }
          }
      } }

    /* LAYER-1 RECURSION PROBE (env YZ_RECPROBE, pt48b): the t7 _gcm_intr_thread
     * host-stack-overflows through the gcm ring; func_00EDC6B0 is bctrl'd on the
     * path. Watch the GUEST r1 trend across successive bctrl hits to this target:
     * a MONOTONIC decrease == genuine recursion (each level nests a host frame),
     * an oscillation == flat loop (overflow is elsewhere). After N hits, dump the
     * guest back-chain (r1 -> *(r1)=caller SP, saved LR at SP+0x10) = the exact
     * recursive cycle, captured BEFORE the host stack dies. One-shot. */
    if (target == 0x00F83AECu && getenv("YZ_RECPROBE")) {
        static __declspec(thread) uint32_t prev_r1 = 0;
        static __declspec(thread) uint32_t hits = 0, dec_run = 0;
        static __declspec(thread) int dumped = 0;
        uint32_t r1 = (uint32_t)ctx->gpr[1];
        uint32_t obj = (uint32_t)ctx->gpr[3];
        hits++;
        if (prev_r1 && r1 < prev_r1) dec_run++; else dec_run = 0;
        prev_r1 = r1;
        if ((hits & 31u) == 1u)
            fprintf(stderr, "[recprobe] tid=%u hit#%u r1=0x%08X dec_run=%u obj(r3)=0x%08X\n",
                    yz_thread_current_id(), hits, r1, dec_run, obj);
        if (!dumped && dec_run >= 80u) {
            dumped = 1;
            fprintf(stderr, "[recprobe] *** RUNAWAY recursion on tid=%u: r1 fell %u steps to 0x%08X. "
                    "obj(r3)=0x%08X\n", yz_thread_current_id(), dec_run, r1, obj);
            if (obj >= 0x10000u && obj < 0xE0000000u) {
                fprintf(stderr, "[recprobe] obj dump:");
                for (uint32_t o = 0; o <= 0x60u; o += 4u) fprintf(stderr, " +%02X=%08X", o, vm_read32(obj + o));
                fprintf(stderr, "\n");
                uint32_t l = vm_read32(obj + 0x5Cu);    /* func_00F83A10 reads obj[0x5C] */
                fprintf(stderr, "[recprobe] obj[0x5C]=0x%08X", l);
                if (l >= 0x10000u && l < 0xE0000000u)
                    for (uint32_t o = 0; o <= 0x18u; o += 4u) fprintf(stderr, " *(+%02X)=%08X", o, vm_read32(l + o));
                fprintf(stderr, "\n");
            }
            uint32_t sp = r1;
            for (int f = 0; f < 24 && sp >= 0x10000u && sp < 0xE0000000u; f++) {
                uint32_t nxt = vm_read32(sp);
                uint32_t lr  = (nxt >= 0x10000u && nxt < 0xE0000000u) ? vm_read32(nxt + 0x10u) : 0;
                fprintf(stderr, "[recprobe]   frame %2d sp=0x%08X savedLR=0x%08X\n", f, sp, lr);
                if (nxt <= sp) break;
                sp = nxt;
            }
            fflush(stderr);
        }
    }

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

    /* DIAG (env YZ_TASK_TRACE, pt31): broaden beyond CreateTask2WithBinInfo -- catch
     * ALL SPURS task/taskset creation so we can see if the cri_audio decode task
     * (ELF 0x012B4980) is ever created in ANY taskset (e.g. the late wid-3 taskset).
     * Logs the ELF EA whether it's r5 directly (CreateTask) or inside a struct. */
    { static int en2 = -1; if (en2 < 0) en2 = getenv("YZ_TASK_TRACE") ? 1 : 0;
      if (en2) {
        static const struct { uint32_t opd; const char* nm; } creates[] = {
            {0x0203162Cu, "CreateTask"}, {0x02031624u, "CreateTaskWithAttr"},
            {0x02031764u, "CreateTaskset"}, {0x02031784u, "CreateTaskset2"},
            {0x0203175Cu, "CreateTasksetWithAttr"},
        };
        for (unsigned i = 0; i < 5; i++) {
            uint32_t code = vm_read32(creates[i].opd);
            if (target == creates[i].opd || (code && target == code)) {
                uint32_t r5 = (uint32_t)ctx->gpr[5];
                fprintf(stderr, "[task+] %s r3=0x%llX r4=0x%llX r5=0x%08X r6=0x%llX",
                        creates[i].nm, (unsigned long long)ctx->gpr[3],
                        (unsigned long long)ctx->gpr[4], r5, (unsigned long long)ctx->gpr[6]);
                uint32_t elf_in_attr = 0;
                if (r5 >= 0x10000u && r5 < 0xE0000000u) {
                    fprintf(stderr, " attr[");
                    for (int w = 0; w < 8; w++) { uint32_t v = vm_read32(r5 + (uint32_t)w*4u);
                        fprintf(stderr, "%s0x%08X", w?" ":"", v);
                        if (v == 0x012B4980u) elf_in_attr = 1; }
                    fprintf(stderr, "]");
                }
                if (r5 == 0x012B4980u || elf_in_attr) {
                    fprintf(stderr, "  <== cri_audio CODEC TASK");
                    g_yz_codec_taskset = (uint32_t)ctx->gpr[3];  /* CreateTaskWithAttr r3 = taskset */
                    { static int once = 0; if (!once) { once = 1;
                        fprintf(stderr, "\n[create-fn] CreateTaskWithAttr code=0x%08X CreateTask code=0x%08X CreateTask2 code=0x%08X\n",
                                vm_read32(0x02031624u), vm_read32(0x0203162Cu), vm_read32(0x0203161Cu)); fflush(stderr); } }
                    /* pt35: is a valid SPU ELF actually present at the codec eaElf?
                     * spursTasksetLoadElf DMAs+parses it; if it's missing/garbage,
                     * LoadElf fails -> spursHalt -> dispatch dies after SELECT (the
                     * stuck-running symptom). Expect magic 0x7F454C46 ('\x7fELF'). */
                    uint32_t eaElf = 0x012B4980u;
                    fprintf(stderr, " | eaElf=0x%08X bytes: %08X %08X %08X (magic %s)",
                            eaElf, vm_read32(eaElf), vm_read32(eaElf+4u), vm_read32(eaElf+8u),
                            vm_read32(eaElf) == 0x7F454C46u ? "OK" : "MISSING/BAD");
                }
                /* pt35: dump wid3's scheduling fields AT creation. SPURS @ 0x40197C80:
                 * wklReadyCount1@+0x00, wklCurrentContention@+0x20, wklMaxContention@+0x50
                 * (each u8[16], BE -> word holds wid0..3). If curCont byte for wid3 is
                 * already nonzero here, it's a bad INIT; if 0, it goes 1 later (stuck). */
                { uint32_t sp = 0x40197C80u;
                  fprintf(stderr, " | SPURS curCont@20=0x%08X rc@00=0x%08X maxC@50=0x%08X",
                          vm_read32(sp+0x20u), vm_read32(sp+0x00u), vm_read32(sp+0x50u)); }
                fprintf(stderr, "\n"); fflush(stderr);
                break;
            }
        }
      } }

    /* DIAG (env YZ_TASK_RET, 2026-06-20 pt27): the CRI codec SPURS task is created
     * (CreateTask2WithBinInfo) but never dispatches -- the taskset shows enabled=0,
     * so either the create RETURNS AN ERROR (0x80410902 INVAL / 0x80410911 NULL_PTR)
     * before allocating, or it succeeds but the enable doesn't land. Capture the
     * decisive datum: run the real create here (direct-call idiom, like
     * yz_call_guest_opd) and log its return value + the taskset bitsets right after,
     * one-shot. Then suppress the normal dispatch (we already executed it). */
    { static int rc = -1; static uint32_t rc_ts = 0;
      if (rc < 0) { const char* e = getenv("YZ_TASK_RET"); rc = e ? 1 : 0;
          /* 2026-07-03 s7: optional hex TASKSET filter (YZ_TASK_RET=40199D00)
           * so the one-shot fires on the FAILING create (the pxd IO-service
           * retry loop), not the first successful shader-phase one. "1" or
           * any non-hex value = fire on the first call as before. */
          if (e) { char* end = NULL; unsigned long v = strtoul(e, &end, 16);
                   if (end != e && v > 0xFFFFu) rc_ts = (uint32_t)v; } }
      if (rc) {
        static uint32_t t2c = 0; static int t2ci = 0;
        if (!t2ci) { t2ci = 1; t2c = vm_read32(0x0203161Cu); }
        /* Filtered mode (YZ_TASK_RET=<taskset hex>): NON-INVASIVE per-call
         * snapshot of the sendWorkloadSignal guard inputs at the create's
         * instant (2026-07-03: the signal's rc is DISCARDED by Sony's helper
         * 0x2010C6C -- a guard bail is invisible in the create's rc, and the
         * one-shot inline capture only ever saw the FIRST, healthy create).
         * Guards per libsre 0x200A750: wklEnabled bit (spurs+0xB0),
         * wklState1[wid]==2 (spurs+0x80+wid), spurs+0xD6C==0, wid<max. */
        if (t2c && (target == 0x0203161Cu || target == t2c)
                && rc_ts && (uint32_t)ctx->gpr[3] == rc_ts) {
            static int n = 0;
            if (n < 40) { n++;
                uint32_t ts    = rc_ts;
                uint32_t spurs = vm_read32(ts + 0x64u);
                uint32_t wid   = vm_read32(ts + 0x74u);
                uint32_t st    = (wid < 16u) ? vm_read8(spurs + 0x80u + wid) : 0xFFu;
                fprintf(stderr, "[task-crt] #%d ts=%08X spurs=%08X wid=%u state=%02X "
                        "enabled=%08X sig=%08X d6c=%08X | pend=%08X enb=%08X run=%08X\n",
                        n, ts, spurs, wid, st,
                        vm_read32(spurs + 0xB0u), vm_read32(spurs + 0x70u),
                        vm_read32(spurs + 0xD6Cu),
                        vm_read32(ts + 0x20u), vm_read32(ts + 0x30u), vm_read32(ts + 0x00u));
                fflush(stderr);
            }
            /* fall through: DO NOT run the create inline -- normal dispatch */
        }
        else if (t2c && (target == 0x0203161Cu || target == t2c) && !rc_ts) {
            static int once = 0;
            if (!once) { once = 1;
                uint32_t ts   = (uint32_t)ctx->gpr[3];   /* taskset2 */
                uint32_t tidp = (uint32_t)ctx->gpr[4];   /* taskId out ptr */
                uint32_t enb  = vm_read32(ts + 0x30u);
                yz_ppu_fn cf = yz_lookup_func(t2c);
                if (cf) {
                    ctx->gpr[2] = vm_read32(0x0203161Cu + 4u);   /* libsre TOC */
                    cf(ctx);
                    yz_drain_trampolines(ctx);
                    fprintf(stderr, "[task-ret] CreateTask2 ret=0x%08X taskId=0x%X | "
                            "enabled %08X->%08X pending_ready=%08X ready=%08X signalled=%08X wid=%u\n",
                            (uint32_t)ctx->gpr[3], tidp ? vm_read32(tidp) : 0xFFFFFFFFu, enb,
                            vm_read32(ts + 0x30u), vm_read32(ts + 0x20u),
                            vm_read32(ts + 0x10u), vm_read32(ts + 0x40u), vm_read32(ts + 0x74u));
                    fflush(stderr);
                    g_trampoline_fn = nullptr;
                    return;
                }
            }
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


    /* MEASUREMENT (env YZ_PHASE): GET/PUT at each libgcm segment-recycle reserve
     * entry (func_02103AAC). Decisive test of whether the producer laps the
     * consumer: GET should track PUT (gap small / within one segment). If GET
     * falls a full segment behind over successive reserves, the producer is still
     * lapping. */
    if (target == 0x02103AACu) {
        static int phase = -1;
        if (phase < 0) phase = getenv("YZ_PHASE") ? 1 : 0;
        if (phase) {
            uint32_t get = vm_read32(0x10000044u) & ~3u;
            uint32_t put = vm_read32(0x10000040u) & ~3u;
            static int n = 0;
            if (n < 240) { n++;
                fprintf(stderr, "[reserve #%d] GET=0x%06X PUT=0x%06X\n", n, get, put);
                fflush(stderr); }
        }
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
        if (fn) {
            ctx->gpr[2] = toc;
            /* Same TOC repair as the direct path: a game-range fn reached via an OPD
             * whose TOC word is 0 would run with r2=0 and fault on its first TOC load. */
            if (code < 0x02000000u && g_yz_game_toc && ctx->gpr[2] != g_yz_game_toc)
                ctx->gpr[2] = g_yz_game_toc;
        }
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
