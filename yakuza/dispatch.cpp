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

/* ===========================================================================
 * mwPly (CRI Sofdec player) dynamic-ABI probe (env YZ_MWPLY_PROBE, 2026-07-04)
 * See scratch/MWPLY_RESOLVE.md for the resolution recipe. Three resolved
 * addresses, all reached only via indirect (bctr) dispatch:
 *   func_00F4D0A8 = mwPlyIsNextFrmReady (the poll gate t1 spins on)
 *   func_00F4DA90 = mwPlyGetFrm         (decoded video frame)
 *   func_00F48E48 = mwPlyGetAudioPcmData_PS3 (audio PCM pull)
 * Diagnostic-only; gated OFF by default (band-aid hygiene: env-gated,
 * default-off, kill-switched). Also implements
 * YZ_MWPLY_FORCEREADY (separate flag, default OFF): skip the real
 * IsNextFrmReady call and force ctx->gpr[3]=1, to see whether the player then
 * advances into calling the getters at all.
 * =========================================================================*/
#define YZ_MWPLY_ISREADY   0x00F4D0A8u
#define YZ_MWPLY_GETFRM    0x00F4DA90u
#define YZ_MWPLY_GETAUDIO  0x00F48E48u

static bool yz_mwply_probe_target(uint32_t target)
{
    static int en = -1;
    if (en < 0) en = getenv("YZ_MWPLY_PROBE") ? 1 : 0;
    if (!en) return false;
    return target == YZ_MWPLY_ISREADY || target == YZ_MWPLY_GETFRM ||
           target == YZ_MWPLY_GETAUDIO;
}

static const char* yz_mwply_name(uint32_t target)
{
    if (target == YZ_MWPLY_ISREADY)  return "mwPlyIsNextFrmReady";
    if (target == YZ_MWPLY_GETFRM)   return "mwPlyGetFrm";
    if (target == YZ_MWPLY_GETAUDIO) return "mwPlyGetAudioPcmData_PS3";
    return "?";
}

/* Volume-bounded gate shared by all three probe sites: first 40 calls, then
 * 1-in-500 (a tight poll must not flood/serialize the guest). */
static bool yz_mwply_should_log(long* counter)
{
    long c = ++(*counter);
    return c <= 40 || (c % 500) == 0;
}

/* Dump `len` bytes at guest EA `ea` as hex words, guest-BE (as the game wrote
 * them), guarded by addr_readable so a garbage pointer can't fault the probe. */
static void yz_mwply_dump_buf(const char* tag, uint32_t ea, uint32_t len)
{
    if (!ea) { fprintf(stderr, "  %s=NULL\n", tag); return; }
    if (!addr_readable(ea)) {
        fprintf(stderr, "  %s=0x%08X (UNREADABLE, skipped)\n", tag, ea);
        return;
    }
    fprintf(stderr, "  %s=0x%08X:", tag, ea);
    for (uint32_t o = 0; o < len; o += 4)
        fprintf(stderr, " %08X", vm_read32(ea + o));
    fprintf(stderr, "\n");
}

extern "C" void yz_drain_trampolines(ppu_context* ctx);

/* Entry point called from ps3_indirect_call for the three probe targets
 * instead of the normal g_trampoline_fn hand-off. We call the real function
 * SYNCHRONOUSLY here (rather than via the trampoline slot) so we can log its
 * return value and out-params right after -- safe because these are leaf-ish
 * CRI player calls, not long tail-chains that would grow the host stack. */
static void yz_mwply_probe_dispatch(uint32_t target, yz_ppu_fn fn, ppu_context* ctx)
{
    static long counters[3] = {0, 0, 0};
    int idx = (target == YZ_MWPLY_ISREADY) ? 0 : (target == YZ_MWPLY_GETFRM) ? 1 : 2;
    bool log_this = yz_mwply_should_log(&counters[idx]);
    const char* nm = yz_mwply_name(target);
    unsigned tid = yz_thread_current_id();

    uint64_t r3 = ctx->gpr[3], r4 = ctx->gpr[4], r5 = ctx->gpr[5];
    uint64_t r6 = ctx->gpr[6], r7 = ctx->gpr[7], r8 = ctx->gpr[8];

    if (log_this) {
        fprintf(stderr, "[mwply] %s tid=%u r3=0x%llX r4=0x%llX r5=0x%llX r6=0x%llX r7=0x%llX r8=0x%llX\n",
                nm, tid, (unsigned long long)r3, (unsigned long long)r4,
                (unsigned long long)r5, (unsigned long long)r6,
                (unsigned long long)r7, (unsigned long long)r8);
        fflush(stderr);
    }

    static int force_ready = -1;
    if (force_ready < 0) force_ready = getenv("YZ_MWPLY_FORCEREADY") ? 1 : 0;

    if (target == YZ_MWPLY_ISREADY && force_ready) {
        /* Skip the real poll test; force "ready" (nonzero) so we can see
         * whether the player advances to calling the getters at all. */
        ctx->gpr[3] = 1;
        if (log_this) {
            fprintf(stderr, "  [FORCEREADY] skipped real call, forced ret=1\n");
            fflush(stderr);
        }
    } else {
        fn(ctx);
        yz_drain_trampolines(ctx);
    }

    if (log_this) {
        fprintf(stderr, "  -> ret ctx->gpr[3]=0x%llX\n", (unsigned long long)ctx->gpr[3]);
        if (target == YZ_MWPLY_GETFRM) {
            /* r4 = small out-struct (init'd to -1 sentinel per static read);
             * r5 = ~0xA8-byte out-struct memset to 0 then filled. */
            yz_mwply_dump_buf("r4(small-struct)", (uint32_t)r4, 0x20);
            yz_mwply_dump_buf("r5(frame-struct,0xA8)", (uint32_t)r5, 0xA8);
        } else if (target == YZ_MWPLY_GETAUDIO) {
            /* r4/r5/r6 = the out pointers per the static ABI read (count/ptr
             * fields observed at entry); dump ~0x40 bytes at each candidate. */
            yz_mwply_dump_buf("r4", (uint32_t)r4, 0x40);
            yz_mwply_dump_buf("r5", (uint32_t)r5, 0x40);
            yz_mwply_dump_buf("r6", (uint32_t)r6, 0x40);
        }
        fflush(stderr);
    }
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

/* T1-ONLY non-suspending PC/LR sampler (env YZ_T1SAMPLE, 2026-07-04, the
 * sem-4-rendezvous-then-silent-spin frontier). g_yz_last_targets below is
 * shared by every guest thread, so it's useless once other threads (t7/t10/
 * t13-24) keep calling indirectly while t1 alone goes quiet -- their hits
 * drown t1's. These are written ONLY when yz_thread_current_id()==1 (one
 * branch, no lock, no suspend -- a diagnostic must never perturb the thread
 * it measures) and read by a low-rate timer thread in
 * main.cpp. g_yz_t1_sample_seq increments on every t1 hop, so the printer
 * can tell "moved since last read" from "still parked at the same site". */
extern "C" volatile uint32_t g_yz_t1_last_target = 0;   /* ctr at t1's last bctr(l) */
extern "C" volatile uint32_t g_yz_t1_last_lr     = 0;   /* lr at t1's last trampoline hop */
extern "C" volatile long     g_yz_t1_sample_seq  = 0;
/* s25: t1's last hop target as a raw HOST fn ptr, written UNGATED (one TLS
 * read + store per hop; no guest resolution — that linear scan stays gated
 * behind YZ_T1SAMPLE). Feeds the park-rel fast tier's SPIN witness: a t1
 * orbiting the gcm progress-throttle usleep loop hops forever (seq climbs)
 * but lands on the same target — it makes no flush call in that loop
 * (stopper_drain_re.md Q1), so it is exactly as unable to drain its own
 * journal as a frozen t1. Measured: rides s25ride4-6 ground at 3-5 s/flip
 * because the seq-frozen witness never fired against the spin. */
extern "C" volatile void*    g_yz_t1_last_tf     = 0;

/* s21 HOP CENSUS: size the emission-rework payoff before building it. Every
 * trampoline hop passes this guard; only register-target (bctr-class) calls
 * pass ps3_indirect_call. total - indirect = hops whose target was STATICALLY
 * KNOWN at lift time and trampolined purely for stack discipline -- the
 * fraction the switch/goto + direct-call emission eliminates. Printed once
 * per 10M hops. Counters are per-process, racy-increment tolerable (census). */
extern "C" unsigned long long g_yz_hops_total = 0, g_yz_hops_indirect = 0;

extern "C" void yz_tramp_guard(void* tf, void* ctxv)
{
    ppu_context* ctx0 = (ppu_context*)ctxv;
    if ((++g_yz_hops_total & 0x9FFFFFull) == 0) /* ~every 10.5M */
        fprintf(stderr, "[hops] total=%llu indirect=%llu static=%llu (%.1f%% static)\n",
                g_yz_hops_total, g_yz_hops_indirect,
                g_yz_hops_total - g_yz_hops_indirect,
                100.0 * (double)(g_yz_hops_total - g_yz_hops_indirect) / (double)g_yz_hops_total);
    unsigned _ri = g_yz_tramp_idx++ & 255;
    g_yz_tramp_ring[_ri] = tf;
    g_yz_tramp_r31[_ri]  = ctx0->gpr[31];
    g_yz_tramp_r1[_ri]   = ctx0->gpr[1];
    g_yz_tramp_lr[_ri]   = ctx0->lr;     /* = caller's return addr (names the caller) */

    /* YZ_T1SAMPLE (see globals above): covers the same-chunk direct-tail-branch
     * hop (g_trampoline_fn = func_X; return;) which ps3_indirect_call never
     * sees. tf is a host fn ptr here, not a guest addr -- resolve is left to
     * the printer/tools (yz_guest_of_host runs in this TU). */
    /* s21 PERF: this block exists only to feed the YZ_T1SAMPLE diagnostic, but
     * it ran UNGATED -- and yz_guest_of_host is a LINEAR SCAN over all ~57k
     * lifted functions, executed on EVERY t1 trampoline hop. Profiled as the
     * main thread's dominant cost (yz_tramp_guard+0xA5,
     * scratch/hot_threads_id.md). Gate it on the flag it serves. */
    if (yz_thread_current_id() == 1) {
        g_yz_t1_last_tf = tf;            /* ungated spin-witness feed (s25) */
        g_yz_t1_sample_seq++;
        static int t1s = -1; if (t1s < 0) t1s = getenv("YZ_T1SAMPLE") ? 1 : 0;
        if (t1s) {
            uint32_t g = yz_guest_of_host(tf);
            if (g) g_yz_t1_last_target = g;
            g_yz_t1_last_lr = (uint32_t)ctx0->lr;
        }
    }

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

    /* mwPly PROBE, entry-args-only path (env YZ_MWPLY_PROBE, 2026-07-04): this
     * trampoline hop covers BOTH the bctr-indirect case (already fully wrapped
     * in ps3_indirect_call, which returns before setting g_trampoline_fn for
     * these targets, so it does NOT reach here) and the same-chunk direct
     * tail-branch case (e.g. func_00F48E48, reached via
     * `g_trampoline_fn = func_00F48E48; return;` inside another lifted
     * function's fallthrough -- ps3_indirect_call never runs for that path).
     * DRAIN_TRAMPOLINE captures the callee into a local before this guard runs
     * and calls it unconditionally right after, so we cannot wrap the return
     * here without editing generated code -- entry args only. */
    { static int en3 = -1; if (en3 < 0) en3 = getenv("YZ_MWPLY_PROBE") ? 1 : 0;
      if (en3) {
        static void* mwfn[3]; static long c3[3]; static int init3 = 0;
        if (!init3) { init3 = 1;
            mwfn[0] = (void*)yz_lookup_func(0x00F4D0A8u);   /* mwPlyIsNextFrmReady */
            mwfn[1] = (void*)yz_lookup_func(0x00F4DA90u);   /* mwPlyGetFrm */
            mwfn[2] = (void*)yz_lookup_func(0x00F48E48u); } /* mwPlyGetAudioPcmData_PS3 */
        static const char* mwnm[3] = { "mwPlyIsNextFrmReady", "mwPlyGetFrm", "mwPlyGetAudioPcmData_PS3" };
        for (int i = 0; i < 3; i++) if (tf == mwfn[i] && mwfn[i]) {
            long c = ++c3[i];
            if (c <= 40 || (c % 500) == 0)
                fprintf(stderr, "[mwply-tramp] %s (direct-tail-branch hop) tid=%u r3=0x%llX r4=0x%llX r5=0x%llX r6=0x%llX\n",
                        mwnm[i], yz_thread_current_id(),
                        (unsigned long long)ctx0->gpr[3], (unsigned long long)ctx0->gpr[4],
                        (unsigned long long)ctx0->gpr[5], (unsigned long long)ctx0->gpr[6]);
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

/* Defined in main.cpp with plain C++ linkage (shared by the crash handler and
 * the stall watchdog). Declared here at FILE SCOPE, outside any extern "C"
 * context -- a local `extern` forward-decl nested inside ps3_indirect_call
 * (itself `extern "C"`) gets C linkage in MSVC and fails to link against the
 * C++-mangled definition. */
void yz_dump_guest_state(const ppu_context* gc, const char* tag);

/* Public helper from runtime/syscalls/sys_event.c (declared here rather than
 * pulling in sys_event.h, matching this file's existing forward-decl style
 * for cross-module calls). Used only by the YZ_EVFLAG_FORCE fallback below. */
extern "C" int sys_event_queue_push_by_id(uint32_t queue_id,
                                           uint64_t source, uint64_t data1,
                                           uint64_t data2,  uint64_t data3);

extern "C" void ps3_indirect_call(ppu_context* ctx)
{
    g_yz_hops_indirect++;                     /* s21 hop census (see yz_tramp_guard) */
    uint32_t target = (uint32_t)ctx->ctr;
    g_yz_last_targets[g_yz_last_idx++ & 15] = target;
    if (yz_thread_current_id() == 1) {
        g_yz_t1_last_target = target;
        g_yz_t1_sample_seq++;
        /* s25 spin-witness feed (see g_yz_t1_last_tf in this file): guest
         * addr cast to the same slot the tramp path fills with host ptrs —
         * only ever compared for equality, and the two spaces can't collide. */
        extern volatile void* g_yz_t1_last_tf;
        g_yz_t1_last_tf = (void*)(uintptr_t)target;

        /* s24 (env YZ_UPDCB): the master per-object update handler
         * func_00D1E838 [0xD1E838,0xD1FC38) dispatches ~27 callbacks via bctrl
         * per main-loop iteration; the oracle completes every iteration in
         * ~40-65 ms while ours never returns from iteration #2 (chain-probe
         * census, scratch/s24cp1.err). Logging every bctrl issued FROM inside
         * it names the callback that never returns (= the last line printed).
         * Cap 400 lines (2-3 iterations' worth). */
        { static int ucb = -1; static int ucb_n = 0;
          if (ucb < 0) { ucb = getenv("YZ_UPDCB") ? 1 : 0;
              if (ucb) fprintf(stderr, "[updcb] ARMED: t1 bctrl-from-func_00D1E838 log\n"); }
          if (ucb && ucb_n < 400) {
              uint32_t l = (uint32_t)ctx->lr;
              if (l >= 0x00D1E838u && l < 0x00D1FC38u) {
                  ucb_n++;
                  fprintf(stderr, "[updcb] #%d target=0x%08X lr=0x%08X r3=0x%llX r4=0x%llX\n",
                          ucb_n, target, l,
                          (unsigned long long)ctx->gpr[3],
                          (unsigned long long)ctx->gpr[4]);
                  if (ucb_n == 400) fprintf(stderr, "[updcb] cap reached\n");
                  fflush(stderr);
              }
          }
        }
    }

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
          }
          /* s23: armed banner (a zero-hit boot without one is PLAUSIBLE, not
           * MEASURED -- this bit us on the 00DDDA6C hook). NB per the SIGCALL
           * comment below, a bctrl may carry the OPD address rather than the
           * code address: hook BOTH (find the OPD EA whose word0 = the code
           * addr) or expect false negatives. */
          if (hn) { fprintf(stderr, "[hook] armed:"); for (int i = 0; i < hn; i++)
                        fprintf(stderr, " 0x%08X", hk[i]);
                    fprintf(stderr, "\n"); fflush(stderr); } }
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

    /* SPURS event-flag WAIT/SET watch (env YZ_EVFLAG_WATCH, 2026-07-04, diag —
     * REMOVE when the t1-wedge frontier closes). Measured t1 parked
     * forever in func_02015C2C (cellSpursEventFlagWait's poll body, entered via
     * func_02015F74) and the release test on object 0x63D61720 did NOT wake it
     * (scratch/adx_release_test2.err) -- so either the object, the waited
     * bitmask, or both are the wrong guess. This logs the REAL wait args t1
     * passes to func_02015F74 (object EA / mode / bitmask / the object's
     * current flag-bits at entry) and every cellSpursEventFlagSet call's
     * (object EA, set-bits, calling tid) so we can see whether a producer ever
     * targets t1's real object at all.
     *
     * ABI (read from the lifted bodies, recomp_prx/libsre_recomp_000.cpp):
     *   func_02015F74(eventFlag ea=r3, mode=r4, mask=r5) -- entry is a
     *     3-instruction trampoline (`gpr6=1; goto func_02015C2C`) that tail-
     *     hops into the real poll body func_02015C2C with the SAME r3/r4/r5
     *     (r3->r11/r9, r4->r31, r5 compared against 1 at loc_02015C88) and an
     *     implicit "wait" flag (r6=1) distinguishing it from the non-waiting
     *     entry func_02015F7C (r6=0, same body). The object's mode/type byte
     *     lives at [ea+0xE]; the CAS'd flag-bits FIELD is the 8-byte word at
     *     ea+0x0 itself (loc_02015CC4: gpr29 = (uint32_t)gpr9, and gpr9==gpr3
     *     ==the object ea unmodified -- the ldarx/stdcx pair at loc_02015D18/
     *     loc_02015D2C operates on *(u64*)ea, NOT ea+8). Log a 64-bit read at
     *     ea+0 as "current bits" (this also matches func_02016010's own CAS
     *     loop below, same ea+0 word).
     *   func_02016010(eventFlag ea=r3, bits=r4) -- cellSpursEventFlagSet; its
     *     ldarx/stdcx CAS loop (loc_020160F4/loc_020161E4) targets gpr27 =
     *     (uint32_t)gpr9 = (uint32_t)gpr3 = the SAME ea+0 word, confirming the
     *     two functions share one object layout. Confirmed against
     *     scratch/libsre_lle_map.txt (both names) and the ADX_SPURS_EVENTFLAG_*
     *     defines in libs/filesystem/cellFs.c (which independently arrived at
     *     the same two addresses). */
    { static int ew = -1; if (ew < 0) ew = getenv("YZ_EVFLAG_WATCH") ? 1 : 0;
      if (ew) {
          /* addr_readable() above only covers main-mem [0x10000,0x10000000) and
           * the stack region [0xD0000000,0xE0000000) -- it does NOT know about
           * the sys_memory-allocate heap (observed live at 0x40000000+, e.g.
           * "[sys_memory] allocate -> 0x40000000" early in every boot log),
           * which vm.h commits on demand (vm_commit, ppu/memory/vm.h ~157) and
           * is real, backed, readable guest memory. A local, WIDER guard here
           * (not touching the shared addr_readable, which other probes rely on
           * for its narrower main-mem/stack-only semantics) avoids a false
           * "EA UNREADABLE" on a perfectly valid SPURS-heap object. */
          auto evflag_ea_ok = [](uint32_t a) {
              return (a >= 0x00010000u && a < 0xE0000000u);
          };
          if (target == 0x02015F74u && yz_thread_current_id() == 1) {
              static long n = 0; long c = ++n;
              if (c <= 10 || (c % 97) == 0) {
                  uint32_t ea   = (uint32_t)ctx->gpr[3];
                  uint32_t mode = (uint32_t)ctx->gpr[4];
                  uint32_t mask = (uint32_t)ctx->gpr[5];
                  bool ok = evflag_ea_ok(ea);
                  uint64_t bits = ok ? vm_read64(ea) : ~0ull;
                  uint32_t type = ok ? vm_read8(ea + 0xEu) : 0xFFu;
                  fprintf(stderr, "[evflag-wait] #%ld tid=1 ea=0x%08X mode=0x%X mask=0x%08X "
                          "cur_bits=0x%016llX type@0xE=0x%02X %s lr=0x%08llX\n",
                          c, ea, mode, mask, (unsigned long long)bits, type,
                          ok ? "" : "(EA UNREADABLE)",
                          (unsigned long long)ctx->lr);
                  fflush(stderr);

                  /* YZ_EVFLAG_BT (2026-07-04, diag -- DIAGNOSIS TASK, remove with
                   * the t1-wedge frontier): the immediate ctx->lr here is 0 (this
                   * hook fires INSIDE the trampoline hop func_02015F74 -> the real
                   * poll body func_02015C2C, and the trampoline is reached via
                   * bctr, not bl -- so ctx->lr is whatever the trampoline itself
                   * had, not the GAME caller). Walk the guest PPC64 back-chain
                   * (r1 -> *(r1)=caller sp, saved LR at sp+0x10 per frame) to
                   * name the real game function(s) that called into this wait.
                   * Reuses the crash handler's yz_dump_guest_state walker
                   * (extern from main.cpp) -- only for ea==0x4019C680 (the
                   * measured wedge object) and only the first few hits, to keep
                   * output bounded (must not perturb the guest and must stay
                   * env-gated/default-off). */
                  { static int bt = -1; if (bt < 0) bt = getenv("YZ_EVFLAG_BT") ? 1 : 0;
                    static long btn = 0;
                    if (bt && ea == 0x4019C680u && btn < 5) {
                        btn++;
                        yz_dump_guest_state(ctx, "evflag-bt");
                    } }
              }
          } else if (target == 0x02016010u) {
              static long n = 0; long c = ++n;
              if (c <= 10 || (c % 97) == 0) {
                  uint32_t ea  = (uint32_t)ctx->gpr[3];
                  uint32_t set = (uint32_t)ctx->gpr[4];
                  fprintf(stderr, "[evflag-set] #%ld tid=%u ea=0x%08X set_bits=0x%08X lr=0x%08llX\n",
                          c, yz_thread_current_id(), ea, set, (unsigned long long)ctx->lr);
                  fflush(stderr);
              }
          }
      } }

    /* SPURS event-flag FORCE (env YZ_EVFLAG_FORCE, 2026-07-04, DIAGNOSTIC ONLY --
     * see docs/FLAGS.md; NOT a shipping fix, must stay env-gated and
     * default-off with a kill-switch). Purpose: t1 deadlocks
     * forever in cellSpursEventFlagWait on IWL object ea=0x4019C680, waiting for
     * mask 0x1; the owning SPU workload never signals it. This forces the wait
     * to succeed so we can observe what boot wall comes NEXT.
     *
     * Injection point: func_02015F74's entry (target==0x02015F74u), i.e. BEFORE
     * the trampoline hops into the real poll body func_02015C2C. func_02015C2C's
     * CAS loop (loc_02015D18/loc_02015D2C) operates on the FULL 64-bit word at
     * ea+0x0 (ldarx/stdcx, not a narrower access), and extracts the `events`
     * struct field (CellSpursEventFlag, RPCS3 cellSpurs.h:890, be_t<u16> at
     * ea+0x00) as the TOP 16 bits of that 64-bit big-endian word: loc_02015D48
     * does `ppc_rldicl(gpr9, 48, 48)` = rotate-left-48 (=rotate-right-16) then
     * keep bits[48:63], which un-rotates the original bits[0:15] into the low
     * 16 bits -- i.e. `(bits >> 48) & 0xFFFF` in a host uint64_t holding the
     * BE-loaded word. So writing the guest BE u16 at ea+0x0 (vm_write16, which
     * does the host->guest byteswap itself) is exactly the field the CAS
     * fast-path re-checks; no need to touch ea+0x2 (a different sub-field also
     * covered by the same 8-byte CAS but unrelated to the mask test at
     * loc_02015D48/loc_02015D7C).
     *
     * We fire this on EVERY entry (not gated to first-N like the watch above)
     * since t1 may re-wait after this one is satisfied and hit a sibling
     * event-flag -- each such wait must also be force-satisfied to keep
     * scoping how far the boot advances. Scoped to ea==0x4019C680 only (the
     * ONE measured wedge object; do not generalize to "always satisfy any
     * wait", which would erase the very information this experiment wants). */
    { static int ef = -1; if (ef < 0) ef = getenv("YZ_EVFLAG_FORCE") ? 1 : 0;
      if (ef && target == 0x02015F74u && yz_thread_current_id() == 1) {
          uint32_t ea = (uint32_t)ctx->gpr[3];
          if (ea == 0x4019C680u) {
              vm_write16(ea + 0x0u, 0x0001u);
              static long n = 0; long c = ++n;
              if (c <= 20 || (c % 97) == 0) {
                  fprintf(stderr, "[evflag-force] #%ld tid=1 ea=0x%08X wrote events=0x0001 "
                          "(pre-CAS force) lr=0x%08llX\n",
                          c, ea, (unsigned long long)ctx->lr);
                  fflush(stderr);
              }

              /* Fallback (env YZ_EVFLAG_FORCE, same flag): if the CAS write above
               * did NOT stop t1 from parking (i.e. the poll body already read the
               * old bits before this hook ran, or it fell through into the
               * blocking sys_event_queue_receive at syscall 0x82 -- see
               * func_02015C2C loc_02015E3C/loc_02015ECC), also push the event
               * queue directly so a receiver blocked in the syscall wakes up.
               * eventQueueId lives at ea+0x7C (RPCS3 cellSpurs.h:915,
               * be_t<u32>); read it guest-endian via vm_read32. source/data*
               * are dummy nonzero payload -- the CAS-based wait re-checks the
               * bits itself on wake, it doesn't trust the event payload. */
              uint32_t qid = vm_read32(ea + 0x7Cu);
              if (qid != 0) {
                  sys_event_queue_push_by_id(qid, 0x4556464Cu /*"EVFL"*/, 1, 0, 0);
              }
          }
      } }

    /* SPURS event-flag LIFECYCLE watch (env YZ_EVFLAG_LIFECYCLE, 2026-07-04, diag --
     * DIAGNOSIS TASK, remove with the t1-wedge frontier). Complements YZ_EVFLAG_WATCH
     * (which only sees Wait/Set). This hooks the CREATE/ATTACH side so we can name the
     * workload (wid) that OWNS a given event-flag object -- the producer-chain
     * question for the t1 wedge on ea=0x4019C680.
     *
     * ABI (read from the lifted bodies, recomp_prx/libsre_recomp_000.cpp):
     *   func_02015758 = _cellSpursEventFlagInitialize(eventFlag ea=r3, taskset/spurs=r4,
     *     direction=r5, clearMode=r6, ...=r7): gpr29=r3(ea), gpr30=r4(taskset/spurs ptr),
     *     gpr31=r5(direction: 0=clears via CAS same as Set, else compared -- this is
     *     CELL_SPURS_EVENT_FLAG_CLEAR_AUTO/MANUAL), gpr27=r6, gpr28=r7. It reads
     *     *(r4+0x74) at loc_02015830 -- taskset+0x74 = the owning wid field,
     *     so THIS is how we read off the owning wid when r4 is a taskset (not NULL/IWL).
     *     Writes mode/type bytes to ea+0xC/0xD/0xE/0xF (ea+0xE done at loc_0201586C:
     *     type = (r27<1) ? ... ; the direction-derived type constant).
     *   func_020158C4 = cellSpursEventFlagAttachLv2EventQueue(eventFlag ea=r3): gpr30=r3;
     *     reads ea+0xE (mode/type byte) and, on the isIwl path (type==3, loc_02015978+),
     *     reads ea+0x74 as an EA and calls through it (func_0200B244) -- so ea+0x74 on an
     *     ATTACHED flag is a live pointer/id, not a static wid; only USEFUL at INIT time
     *     (before attach rewrites it) do we get taskset+0x74=wid. Log both the INIT-time
     *     wid-read and the ATTACH-time ea+0xE type byte + ea+0x74 raw word so we can tell
     *     isIwl (type==3, taskset==NULL passed to Initialize) from taskset-owned (type!=3,
     *     wid = *(taskset+0x74)). */
    { static int el = -1; if (el < 0) el = getenv("YZ_EVFLAG_LIFECYCLE") ? 1 : 0;
      if (el) {
          auto ok = [](uint32_t a) { return (a >= 0x00010000u && a < 0xE0000000u); };
          if (target == 0x02015758u) {
              static long n = 0; long c = ++n;
              uint32_t ea      = (uint32_t)ctx->gpr[3];
              uint32_t taskset = (uint32_t)ctx->gpr[4];
              uint32_t dir     = (uint32_t)ctx->gpr[5];
              bool tsOk = ok(taskset);
              uint32_t wid = (tsOk && taskset != 0) ? vm_read32(taskset + 0x74u) : 0xFFFFFFFFu;
              fprintf(stderr, "[evflag-init] #%ld tid=%u ea=0x%08X taskset=0x%08X dir=0x%X "
                      "%s wid(taskset+0x74)=%s0x%08X lr=0x%08llX\n",
                      c, yz_thread_current_id(), ea, taskset, dir,
                      (taskset == 0) ? "(NULL taskset => IWL)" : "",
                      (taskset == 0) ? "N/A=" : "",
                      wid, (unsigned long long)ctx->lr);
              fflush(stderr);
          } else if (target == 0x020158C4u) {
              static long n = 0; long c = ++n;
              uint32_t ea = (uint32_t)ctx->gpr[3];
              bool eaOk = ok(ea);
              uint32_t type = eaOk ? vm_read8(ea + 0xEu) : 0xFFu;
              uint32_t f74  = eaOk ? vm_read32(ea + 0x74u) : 0xFFFFFFFFu;
              fprintf(stderr, "[evflag-attach] #%ld tid=%u ea=0x%08X type@0xE=0x%02X "
                      "raw@0x74=0x%08X lr=0x%08llX\n",
                      c, yz_thread_current_id(), ea, type, f74, (unsigned long long)ctx->lr);
              fflush(stderr);
          } else if (target == 0x02015AA4u) {
              static long n = 0; long c = ++n;
              uint32_t ea = (uint32_t)ctx->gpr[3];
              fprintf(stderr, "[evflag-detach] #%ld tid=%u ea=0x%08X lr=0x%08llX\n",
                      c, yz_thread_current_id(), ea, (unsigned long long)ctx->lr);
              fflush(stderr);
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
        /* s40b: the "TEMP DEBUG (SPURS bring-up)" trace above was never stripped
         * and is UNGATED — measured firing thousands of times/min on t10's
         * memcpy loop in the current boot phase, two stderr writes per import
         * call on the hot dispatch path (LESSONS #6c: the logging floor is an
         * instrument). Now env-gated OFF; YZ_LLECALL_TRACE=1 restores it. */
        static int lle_trace_on = -1;
        if (lle_trace_on < 0) lle_trace_on = getenv("YZ_LLECALL_TRACE") ? 1 : 0;
        int trace = lle_trace_on && target != YZ_GCM_CB_FAKE_KEY &&
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

    /* DYNAMIC PROBE (env YZ_MWPLY_PROBE, 2026-07-04): live-ABI instrumentation
     * for the CRI Sofdec mwPly player (see scratch/MWPLY_RESOLVE.md). Covers
     * the genuinely-indirect (bctr through the function table) call path with
     * FULL entry+exit+out-param logging. NOTE: at least one of the three
     * (func_00F48E48) is ALSO reached via a same-chunk direct tail-branch
     * (`g_trampoline_fn = func_00F48E48; return;` inside another lifted
     * function) which never reaches ps3_indirect_call at all -- that path is
     * covered separately (entry-args-only) in yz_tramp_guard below, since the
     * DRAIN_TRAMPOLINE macro captures the callee into a local before any hook
     * runs and calls it unconditionally, so a post-call wrapper can't be
     * spliced in there without editing generated code. */
    if (yz_mwply_probe_target(target)) {
        yz_mwply_probe_dispatch(target, fn, ctx);
        return;
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
