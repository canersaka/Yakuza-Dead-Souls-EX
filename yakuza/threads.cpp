/*
 * PPU thread runtime: one host thread per guest thread.
 *
 * The game imports sys_ppu_thread_create/_exit/_get_id/_once from
 * sysPrxForUser and issues yield/join/set_priority/get_priority/
 * get_stack_information as direct syscalls (43/44/47/48/49), so creation
 * lives here as ctx-aware import overrides and the rest as syscall
 * handlers registered over the runtime defaults (shims.cpp).
 *
 * Semantics follow the CELL OS ABI as exercised by this game and
 * cross-checked against RPCS3's sys_ppu_thread behavior (reimplemented,
 * not copied):
 *   - create: r3 = guest u64* that receives the thread id, r4 = entry
 *     function DESCRIPTOR address (OPD: code + TOC), r5 = arg (new
 *     thread's r3), r6 = priority, r7 = stack size, r8 = flags,
 *     r9 = name. The sysPrxForUser-level create starts the thread.
 *   - every thread needs its own stack (carved from the committed stack
 *     region, top-down so it can't collide with vm_stack_allocate's
 *     bottom-up main/callback stacks) and its own TLS block (same layout
 *     the sys_initialize_tls override builds for the main thread).
 */

#include "ppu_recomp.h"
#include "yakuza_runner.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" uint8_t* vm_base;

/* TLS template, exported by main.cpp's ELF loader */
extern "C" uint64_t yz_tls_vaddr, yz_tls_filesz, yz_tls_memsz;

/* Guest layout */
#define THR_STACK_TOP     0xDFF00000u  /* grows down; vm_init committed it */
#define THR_STACK_MIN     0x00010000u
#define THR_STACK_DEFAULT 0x00100000u
#define THR_TLS_BASE      0x0FE10000u  /* above the main thread's TLS block */
#define THR_TLS_END       0x0FF00000u

#define MAX_GUEST_THREADS 64

typedef struct {
    int          in_use;
    int          started;
    uint32_t     tid;
    HANDLE       handle;
    ppu_context* ctx;          /* live guest context (host-stack local in thr_proc) */
    int32_t      priority;
    uint32_t     stack_base;
    uint32_t     stack_size;
    uint64_t     exit_code;
    char         name[28];
    /* Batch fixes (RPCS3 sys_ppu_thread.cpp): detach state (:286-287 re-detach
     * guard) and the single-joiner guard (:195-211). */
    int          detached;
    int          joiner;       /* tid currently joined on this thread, 0 = none */
} thr_rec;

static thr_rec          s_threads[MAX_GUEST_THREADS];
static CRITICAL_SECTION s_thr_cs;
static int              s_thr_cs_init = 0;
static uint32_t         s_stack_cursor = THR_STACK_TOP;
static uint32_t         s_tls_cursor   = THR_TLS_BASE;
static uint32_t         s_next_tid     = 2;   /* 1 = main thread */

static thread_local uint32_t s_cur_tid = 1;

static void thr_lock(void)
{
    if (!s_thr_cs_init) {  /* main thread initializes before any spawn */
        InitializeCriticalSection(&s_thr_cs);
        s_thr_cs_init = 1;
    }
    EnterCriticalSection(&s_thr_cs);
}

static void thr_unlock(void) { LeaveCriticalSection(&s_thr_cs); }

static thr_rec* thr_find(uint32_t tid)
{
    for (int i = 0; i < MAX_GUEST_THREADS; i++) {
        if (s_threads[i].in_use && s_threads[i].tid == tid)
            return &s_threads[i];
    }
    return NULL;
}

extern "C" uint32_t yz_thread_current_id(void) { return s_cur_tid; }

/* Authoritative live guest context for a thread (NULL if not a running guest
 * thread). The stall dump reads guest PC/GPRs from this instead of walking the
 * host stack -- a blocked thread parked in an lv2 wait still holds the syscall
 * args in its GPRs, so this names what it is waiting on without guessing. */
extern "C" void* yz_thread_context(uint32_t tid)
{
    thr_lock();
    thr_rec* t = thr_find(tid);
    void* c = t ? (void*)t->ctx : NULL;
    thr_unlock();
    return c;
}

/* ---- lv2 wait recorder (blocker #21 diagnostic) ---------------------------
 * shims.cpp's central lv2_syscall records the in-flight syscall per guest
 * thread here (before it dispatches, which may block), and clears it on
 * return. A stall dump then reads, for every blocked thread, EXACTLY which
 * syscall it is parked in + the object-id args + how long -- the authoritative
 * "what is this thread waiting on" the host-stack walk can only guess at. */
typedef struct { uint32_t num; uint64_t a3, a4, a5; DWORD since; int active; } wait_slot;
static wait_slot s_wait[256];

extern "C" void yz_wait_enter(uint32_t num, uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint32_t tid = s_cur_tid;
    if (tid >= 256) return;
    s_wait[tid].num   = num;
    s_wait[tid].a3    = a3;
    s_wait[tid].a4    = a4;
    s_wait[tid].a5    = a5;
    s_wait[tid].since = GetTickCount();
    s_wait[tid].active = 1;   /* cleared by yz_wait_exit on return */
}

extern "C" void yz_wait_exit(void)
{
    uint32_t tid = s_cur_tid;
    if (tid < 256) s_wait[tid].active = 0;
}

/* Returns 1 + fills the out-params if thread `tid` is currently inside a
 * syscall (i.e. blocked / mid-call); 0 if it is running guest code. */
extern "C" int yz_wait_get(uint32_t tid, uint32_t* num, uint64_t* a3,
                           uint64_t* a4, uint64_t* a5, uint32_t* held_ms)
{
    if (tid >= 256 || !s_wait[tid].active) return 0;
    if (num)     *num     = s_wait[tid].num;
    if (a3)      *a3      = s_wait[tid].a3;
    if (a4)      *a4      = s_wait[tid].a4;
    if (a5)      *a5      = s_wait[tid].a5;
    if (held_ms) *held_ms = GetTickCount() - s_wait[tid].since;
    return 1;
}

/* TEMP DIAG (blocker #21 producer hunt): snapshot all live guest threads
 * (tid/name/handle) and invoke cb for each OUTSIDE the lock, so the callback may
 * safely suspend+walk each thread's host stack without risking a lock deadlock. */
extern "C" void yz_for_each_thread(void (*cb)(uint32_t tid, const char* name, void* handle))
{
    struct snap_t { uint32_t tid; char name[28]; HANDLE h; } snap[MAX_GUEST_THREADS];
    int n = 0;
    thr_lock();
    for (int i = 0; i < MAX_GUEST_THREADS; i++)
        if (s_threads[i].in_use && s_threads[i].started && s_threads[i].handle) {
            snap[n].tid = s_threads[i].tid;
            strncpy(snap[n].name, s_threads[i].name, sizeof(snap[n].name) - 1);
            snap[n].name[sizeof(snap[n].name) - 1] = 0;
            snap[n].h = s_threads[i].handle;
            n++;
        }
    thr_unlock();
    for (int i = 0; i < n; i++) cb(snap[i].tid, snap[i].name, (void*)snap[i].h);
}

/* s29 [t11fate] (probe spec scratch/s29_t11_fate.md): raw registry dump
 * BYPASSING the enumeration filter above -- the +150s dump showed EVERY
 * tid>=11 thread absent while no implemented exit path clears the filter
 * fields, so either the registry lost them or the filter hides them. Prints
 * every slot that was ever used, so the two cases separate in one boot. */
extern "C" void yz_thread_registry_raw(void)
{
    thr_lock();
    for (int i = 0; i < MAX_GUEST_THREADS; i++) {
        const thr_rec* t = &s_threads[i];
        if (!t->in_use && !t->tid) continue;   /* never used */
        fprintf(stderr, "[t11fate] slot%02d in_use=%d started=%d handle=%p tid=%u \"%s\"\n",
                i, t->in_use, t->started, (void*)t->handle, t->tid, t->name);
    }
    thr_unlock();
    fflush(stderr);
}

static int yz_lv2_prio_to_win(int32_t prio);   /* defined below */
static int yz_thread_prio_on(void);            /* defined below */

/* s37 PROTOTYPE A (bounded-concurrency proof): pin every GUEST PPU thread (the
 * thr_proc-spawned threads + the registered main thread) to a small host-core
 * mask, so at most popcount(mask) PPU threads run truly-parallel. Models the
 * Cell's single PPU (2 SMT) against our one-host-thread-per-guest-thread runtime.
 * SPU host threads (lv2_register.c) and the render/window/RSX host threads
 * (import_overrides.cpp/main.cpp) are created outside this file, so they stay
 * UNPINNED (free on all cores). Env YZ_PPU_AFFINITY = the host affinity mask
 * (base-0 parse: '3' = cores {0,1}, '1' = core {0}, '0xF' = 4 cores). Default
 * OFF (mask 0 -> no pinning) so the working boot is untouched. */
static DWORD_PTR yz_ppu_affinity_mask(void)
{
    static long long m = -1;   /* -1 = not yet read */
    if (m == -1) {
        const char* e = getenv("YZ_PPU_AFFINITY");
        m = e ? strtoll(e, NULL, 0) : 0;   /* 0 = disabled */
        fprintf(stderr, "[ppu-aff] ARMED (%s): guest PPU threads %s (mask=0x%llX)\n",
                e ? "YZ_PPU_AFFINITY" : "off",
                (e && m) ? "PINNED" : "unpinned", (unsigned long long)m);
        fflush(stderr);
    }
    return (DWORD_PTR)m;
}

/* ---------------------------------------------------------------------------
 * s37 PROTOTYPES B/C + CONTROLS: bounded-concurrency admission gate.
 *
 * Thesis under test: our runtime runs every guest PPU thread truly-parallel on
 * its own host core, vs the Cell's ONE PPU (2 SMT lanes) time-sliced by lv2
 * PRIORITY. The CRI scenario.bin read-completion is a HW-calibrated
 * producer/consumer/deliver handshake that our over-parallelism shreds.
 *
 * A guest PPU thread HOLDS a slot while running guest code and VACATES it across
 * every lv2 syscall (release before the dispatch that may block, re-acquire
 * after) and at thread start/exit. Re-acquire is the arbitration point. A thread
 * blocked in a wait is parked INSIDE the dispatch with its slot vacated, so it
 * never occupies a slot while sleeping -> no deadlock from blocked high-prio
 * threads. Modes (env, checked once in yz_threads_init, all default OFF):
 *   YZ_PPU_CAP=N    Proto B  : priority-BLIND cap of N (FIFO admission).
 *   YZ_PPU_PRIO=N   Proto C  : faithful top-N by lv2 priority (lowest number =
 *                              highest priority runs).
 *   YZ_PPU_WRONG=N  control  : top-N by INVERTED priority (admit LOWEST prio).
 *   YZ_PPU_DELAY=1  control  : NO cap/priority; just SwitchToThread() at the same
 *                              sites (pure-latency, to separate timing from
 *                              mechanism -- broader than the refuted YZ_CRI_YIELD).
 * default cap = 2 (the PS3's 2 SMT lanes) unless N given.
 */
enum { GATE_OFF = 0, GATE_CAP, GATE_PRIO, GATE_WRONG, GATE_DELAY };

static int  g_gate_mode = GATE_OFF;
static int  g_gate_cap  = 2;

static CRITICAL_SECTION   g_gate_cs;
static CONDITION_VARIABLE  g_gate_cv;
static int                g_gate_admitted = 0;
static volatile long      g_gate_ticket   = 0;

typedef struct { int active; int prio; long ticket; } gate_waiter;
static gate_waiter s_gate_waiters[MAX_GUEST_THREADS * 2];

static thread_local int s_gate_held = 0;   /* does THIS thread hold a slot? */

/* Parse env + init primitives. Called once from yz_threads_init (single-threaded,
 * before any worker spawns). */
static void yz_gate_init(void)
{
    InitializeCriticalSection(&g_gate_cs);
    InitializeConditionVariable(&g_gate_cv);

    const char* e;
    if ((e = getenv("YZ_PPU_CAP")))        { g_gate_mode = GATE_CAP;   g_gate_cap = atoi(e); }
    else if ((e = getenv("YZ_PPU_PRIO")))  { g_gate_mode = GATE_PRIO;  g_gate_cap = atoi(e); }
    else if ((e = getenv("YZ_PPU_WRONG"))) { g_gate_mode = GATE_WRONG; g_gate_cap = atoi(e); }
    else if ((e = getenv("YZ_PPU_DELAY"))) { g_gate_mode = GATE_DELAY; }
    if (g_gate_cap < 1) g_gate_cap = 2;

    const char* mn = g_gate_mode == GATE_CAP  ? "CAP(blind)"
                   : g_gate_mode == GATE_PRIO ? "PRIO(faithful)"
                   : g_gate_mode == GATE_WRONG? "WRONG(inverted-ctrl)"
                   : g_gate_mode == GATE_DELAY? "DELAY(pure-latency-ctrl)"
                   : "off";
    fprintf(stderr, "[ppu-gate] ARMED: mode=%s cap=%d\n", mn, g_gate_cap);
    fflush(stderr);
}

/* Called when this thread is about to run guest code and must occupy a slot. */
extern "C" void yz_gate_acquire(void)
{
    if (g_gate_mode == GATE_OFF || g_gate_mode == GATE_DELAY) return;
    if (s_gate_held) return;                       /* already holding */

    int myprio = 1001;
    thr_lock();
    if (thr_rec* t = thr_find(s_cur_tid)) myprio = t->priority;
    thr_unlock();

    long myticket = InterlockedIncrement(&g_gate_ticket);

    EnterCriticalSection(&g_gate_cs);
    /* register as a waiter */
    int wi = -1;
    for (int i = 0; i < (int)(sizeof(s_gate_waiters)/sizeof(s_gate_waiters[0])); i++)
        if (!s_gate_waiters[i].active) { wi = i; break; }
    if (wi >= 0) { s_gate_waiters[wi].active = 1; s_gate_waiters[wi].prio = myprio;
                   s_gate_waiters[wi].ticket = myticket; }

    for (;;) {
        int ahead = 0;   /* how many OTHER waiters get a slot before me */
        for (int i = 0; i < (int)(sizeof(s_gate_waiters)/sizeof(s_gate_waiters[0])); i++) {
            if (i == wi || !s_gate_waiters[i].active) continue;
            const gate_waiter* w = &s_gate_waiters[i];
            int wins;
            if (g_gate_mode == GATE_CAP)
                wins = (w->ticket < myticket);                 /* blind FIFO */
            else {
                int wp = (g_gate_mode == GATE_WRONG) ? -w->prio : w->prio;
                int mp = (g_gate_mode == GATE_WRONG) ? -myprio  : myprio;
                wins = (wp < mp) || (wp == mp && w->ticket < myticket);
            }
            if (wins) ahead++;
        }
        if (g_gate_admitted + ahead < g_gate_cap) {
            g_gate_admitted++;
            if (wi >= 0) s_gate_waiters[wi].active = 0;
            break;
        }
        SleepConditionVariableCS(&g_gate_cv, &g_gate_cs, INFINITE);
    }
    LeaveCriticalSection(&g_gate_cs);
    s_gate_held = 1;
}

/* Called when this thread stops running guest code (about to block in a syscall,
 * or exiting). Vacates its slot and wakes the arbitration. */
extern "C" void yz_gate_release(void)
{
    if (g_gate_mode == GATE_OFF || g_gate_mode == GATE_DELAY) return;
    if (!s_gate_held) return;
    EnterCriticalSection(&g_gate_cs);
    if (g_gate_admitted > 0) g_gate_admitted--;
    WakeAllConditionVariable(&g_gate_cv);
    LeaveCriticalSection(&g_gate_cs);
    s_gate_held = 0;
}

/* The lv2 syscall funnel (shims.cpp) brackets its (possibly blocking) dispatch
 * with these: release the slot before, re-acquire after. For the DELAY control
 * this is a pure SwitchToThread() at the same rate, no admission logic. */
extern "C" void yz_gate_syscall_release(void)
{
    if (g_gate_mode == GATE_DELAY) { SwitchToThread(); return; }
    yz_gate_release();
}
extern "C" void yz_gate_syscall_acquire(void)
{
    if (g_gate_mode == GATE_DELAY) return;
    yz_gate_acquire();
}

/* Register the main thread so join/priority/stack-info work on id 1. */
extern "C" void yz_threads_init(uint32_t main_stack_base, uint32_t main_stack_size)
{
    thr_lock();
    thr_rec* t = &s_threads[0];
    memset(t, 0, sizeof(*t));
    t->in_use     = 1;
    t->started    = 1;
    t->tid        = 1;
    t->priority   = 1001;   /* lv2 default main-thread priority (est.) */
    t->stack_base = main_stack_base;
    t->stack_size = main_stack_size;
    strcpy(t->name, "main");
    thr_unlock();

    /* s36: the MAIN thread is the CRI-FS completion PRODUCER (runs the driver
     * pump func_00EEF740 + the register/signal func_00F00580, MEASURED tid=1),
     * while the CONSUMER is cri_dlg (tid=11, lv2 prio 800). On HW cri_dlg (higher
     * priority) preempts main to consume the completion; on our flat scheduler it
     * doesn't -> the s36 completion race. The main thread is created outside the
     * import path, so apply its priority here (prio 1001 -> BELOW_NORMAL) so the
     * mapped CRI threads (800 -> NORMAL) can preempt it, as on HW. Env-gated. */
    if (yz_thread_prio_on())
        SetThreadPriority(GetCurrentThread(), yz_lv2_prio_to_win(1001));

    /* s37 PROTOTYPE A: pin the main guest PPU thread to the bounded core mask. */
    if (DWORD_PTR am = yz_ppu_affinity_mask())
        SetThreadAffinityMask(GetCurrentThread(), am);

    /* s37 PROTOTYPES B/C: init the admission gate (single-threaded here, before
     * any worker spawns) and give the main guest PPU thread its starting slot. */
    yz_gate_init();
    yz_gate_acquire();
}

/* Build a TLS block with the same layout sys_initialize_tls uses for the
 * main thread: 0x30-byte zeroed system area, TLS image copy, zero fill;
 * r13 = block + 0x30 + 0x7000. Returns the r13 value, 0 on failure. */
static uint64_t thr_alloc_tls(void)
{
    uint32_t need = 0x30 + (uint32_t)yz_tls_memsz;
    need = (need + 0xFF) & ~0xFFu;

    thr_lock();
    uint32_t block = s_tls_cursor;
    if (block + need > THR_TLS_END) {
        thr_unlock();
        return 0;
    }
    s_tls_cursor += need;
    thr_unlock();

    memset(vm_base + block, 0, need);
    if (yz_tls_filesz)
        memcpy(vm_base + block + 0x30, vm_base + (uint32_t)yz_tls_vaddr,
               (size_t)yz_tls_filesz);
    return (uint64_t)block + 0x30 + 0x7000;
}

typedef struct {
    uint32_t opd_addr;
    uint64_t arg;
    uint32_t sp;
    uint64_t r13;
    uint32_t tid;
    int32_t  priority;
} thr_start_params;

/* s36 (2026-07-12, user lead): the game assigns lv2 thread priorities (0=highest
 * .. 3071=lowest; e.g. _gcm_intr=1, cri_dlg=800, cri_adxm_idle=1500) and lv2
 * schedules strictly by them. Our runtime creates one host thread per guest
 * thread and previously DROPPED the priority (never SetThreadPriority'd) -> every
 * PPU thread ran at Windows NORMAL, so a HW-calibrated producer/consumer handshake
 * (the scenario.bin CRI-FS read-completion, s36) becomes a timing race. Map the
 * lv2 priority onto Windows' coarse thread-priority levels, preserving the game's
 * relative ordering, and apply it. Faithful direction (honors the game's own
 * priorities); RPCS3 respects lv2 priority in its PPU scheduler (Utilities/
 * Thread.cpp set_native_priority uses BELOW/NORMAL/ABOVE), we have no scheduler so
 * this coarse host mapping is the available approximation. Env-gated YZ_THREAD_PRIO
 * (default OFF) for a clean A/B until validated; kill-switch by design. */
static int yz_lv2_prio_to_win(int32_t prio)
{
    if (prio <  150) return THREAD_PRIORITY_HIGHEST;      /* +2 */
    if (prio <  450) return THREAD_PRIORITY_ABOVE_NORMAL; /* +1 */
    if (prio <  900) return THREAD_PRIORITY_NORMAL;       /*  0 */
    if (prio < 1200) return THREAD_PRIORITY_BELOW_NORMAL; /* -1 */
    return THREAD_PRIORITY_LOWEST;                        /* -2 */
}

static int yz_thread_prio_on(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("YZ_THREAD_PRIO") ? 1 : 0;
        fprintf(stderr, "[thr-prio] ARMED (%s): lv2->win priority mapping %s\n",
                on ? "YZ_THREAD_PRIO" : "off", on ? "APPLIED" : "disabled");
        fflush(stderr);
    }
    return on;
}

static DWORD WINAPI thr_proc(LPVOID pv)
{
    thr_start_params p = *(thr_start_params*)pv;
    free(pv);

    s_cur_tid = p.tid;

    /* s36: honor the guest thread priority (env-gated, see yz_lv2_prio_to_win). */
    if (yz_thread_prio_on())
        SetThreadPriority(GetCurrentThread(), yz_lv2_prio_to_win(p.priority));

    /* s37 PROTOTYPE A: pin this guest PPU thread to the bounded core mask. */
    if (DWORD_PTR am = yz_ppu_affinity_mask())
        SetThreadAffinityMask(GetCurrentThread(), am);

    /* s37 PROTOTYPES B/C: take a slot before running any guest code. */
    yz_gate_acquire();

    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.thread_id = p.tid;   /* lv2 mutex/cond ownership keys on this; 0 caused false EDEADLK */
    ctx.gpr[1]  = p.sp;
    ctx.gpr[3]  = p.arg;
    ctx.gpr[13] = p.r13;
    vm_write64(p.sp, 0);  /* null back-chain */
    g_yz_cur_ctx = &ctx;  /* crash handler walks this thread's back-chain */

    /* Publish this thread's live context so a stall dump can read its guest
     * PC/GPRs directly (authoritative) instead of guessing from a host-stack
     * walk -- the cross-thread g_yz_cur_ctx is thread_local and unreadable. */
    thr_lock();
    if (thr_rec* ts = thr_find(p.tid)) ts->ctx = &ctx;
    thr_unlock();

    fprintf(stderr, "[thread %u] start opd=0x%08X arg=0x%llX sp=0x%08X\n",
            p.tid, p.opd_addr, (unsigned long long)p.arg, p.sp);

    /* resolves code+TOC from the descriptor, dispatches, drains trampolines */
    yz_call_guest_opd(p.opd_addr, &ctx);

    /* s37 PROTOTYPES B/C: guest code done, vacate the slot permanently. */
    yz_gate_release();

    thr_lock();
    thr_rec* t = thr_find(p.tid);
    if (t) {
        if (!t->exit_code) t->exit_code = ctx.gpr[3];
        t->ctx = NULL;     /* ctx local is about to go out of scope */
    }
    thr_unlock();

    fprintf(stderr, "[thread %u] exited r3=0x%llX\n",
            p.tid, (unsigned long long)ctx.gpr[3]);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Import overrides (referenced from gen_imports.py OVERRIDES)
 * -----------------------------------------------------------------------*/

extern "C" void yz_ovr_sys_ppu_thread_create(ppu_context* ctx)
{
    uint32_t id_out   = (uint32_t)ctx->gpr[3];
    uint32_t opd_addr = (uint32_t)ctx->gpr[4];
    uint64_t arg      = ctx->gpr[5];
    int32_t  priority = (int32_t)ctx->gpr[6];
    uint32_t stacksz  = (uint32_t)ctx->gpr[7];
    uint64_t flags    = ctx->gpr[8];
    uint32_t name_ea  = (uint32_t)ctx->gpr[9];

    /* Priority/flags validation (RPCS3 sys_ppu_thread.cpp:336,487,492-495).
     * s23 REGRESSION BISECT: LOG-ONLY until the values the game passes are
     * measured (an enforcement in this batch wedged the boot at 3 flips). */
    if (priority < 0 || priority > 3071 || (flags & 3) == 3) {
        static int n = 0; if (n < 8) { n++;
            fprintf(stderr, "[thr] create would-reject: prio=%lld flags=0x%llX (log-only)\n",
                    (long long)priority, (unsigned long long)flags); fflush(stderr); }
    }

    if (stacksz < THR_STACK_MIN) stacksz = THR_STACK_DEFAULT;
    stacksz = (stacksz + 0xFFFF) & ~0xFFFFu;

    thr_lock();
    thr_rec* t = NULL;
    for (int i = 0; i < MAX_GUEST_THREADS; i++) {
        if (!s_threads[i].in_use) { t = &s_threads[i]; break; }
    }
    if (!t) {
        thr_unlock();
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010001; /* CELL_EAGAIN */
        return;
    }
    memset(t, 0, sizeof(*t));
    t->in_use     = 1;
    t->tid        = s_next_tid++;
    t->priority   = priority;
    s_stack_cursor -= stacksz;
    t->stack_base  = s_stack_cursor;
    t->stack_size  = stacksz;
    thr_unlock();

    if (name_ea) {
        for (int i = 0; i < 27; i++) {
            t->name[i] = (char)vm_read8(name_ea + (uint32_t)i);
            if (!t->name[i]) break;
        }
    }

    uint64_t r13 = thr_alloc_tls();
    if (!r13) {
        fprintf(stderr, "[thread] TLS allocation failed for \"%s\"\n", t->name);
        thr_lock(); t->in_use = 0; thr_unlock();
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010004; /* CELL_ENOMEM */
        return;
    }

    fprintf(stderr, "[thread] create \"%s\" tid=%u opd=0x%08X prio=%d "
            "stack=0x%08X+0x%X flags=0x%llX\n",
            t->name, t->tid, opd_addr, priority, t->stack_base, stacksz,
            (unsigned long long)flags);

    /* Interrupt threads (flag 0x2) are started by interrupt plumbing we
     * don't model; create the record but never run it. */
    if (!(flags & 0x2)) {
        thr_start_params* p = (thr_start_params*)malloc(sizeof(*p));
        p->opd_addr = opd_addr;
        p->arg      = arg;
        p->sp       = (t->stack_base + stacksz - 0x100) & ~0xFu;
        p->r13      = r13;
        p->tid      = t->tid;
        p->priority = priority;
        HANDLE h = CreateThread(NULL, 0, thr_proc, p, 0, NULL);
        if (!h) {
            fprintf(stderr, "[thread] CreateThread failed (%lu)\n", GetLastError());
            free(p);
            thr_lock(); t->in_use = 0; thr_unlock();
            ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010004;
            return;
        }
        t->handle  = h;
        t->started = 1;
    } else {
        fprintf(stderr, "[thread] \"%s\" is an interrupt thread - not started\n",
                t->name);
    }

    if (id_out)
        vm_write64(id_out, (uint64_t)t->tid);
    ctx->gpr[3] = 0;
}

extern "C" void yz_ovr_sys_ppu_thread_exit(ppu_context* ctx)
{
    uint64_t code = ctx->gpr[3];
    thr_lock();
    thr_rec* t = thr_find(s_cur_tid);
    if (t)
        t->exit_code = code;
    thr_unlock();
    fprintf(stderr, "[thread %u] sys_ppu_thread_exit(0x%llX)\n",
            s_cur_tid, (unsigned long long)code);
    /* Does not return. For the main thread this still only ends the thread,
     * matching the console: the process lives while workers run. */
    ExitThread(0);
}

/* sys_ppu_thread_once(once_ctl*, init_opd): run the initializer exactly
 * once; CONCURRENT CALLERS BLOCK until it completes (pthread_once
 * semantics — a caller returning early would use whatever the initializer
 * builds before it's finished). The initializer runs under the once lock:
 * the critical section is recursive, so an initializer that itself calls
 * once() doesn't deadlock. The flag is a guest s32: 0 = not yet, 1 = done.
 * The init function takes no arguments and runs on the calling thread
 * (volatile regs and r2 are caller-saved around import calls). */
static CRITICAL_SECTION s_once_cs;
static int              s_once_cs_init = 0;

extern "C" void yz_ovr_sys_ppu_thread_once(ppu_context* ctx)
{
    uint32_t once_ea = (uint32_t)ctx->gpr[3];
    uint32_t opd_ea  = (uint32_t)ctx->gpr[4];

    if (!s_once_cs_init) {  /* first call happens before any thread spawns */
        InitializeCriticalSection(&s_once_cs);
        s_once_cs_init = 1;
    }

    EnterCriticalSection(&s_once_cs);
    if (vm_read32(once_ea) == 0) {
        fprintf(stderr, "[thread %u] sys_ppu_thread_once: running init "
                "opd=0x%08X (ctl=0x%08X)\n", s_cur_tid, opd_ea, once_ea);
        if (opd_ea)
            yz_call_guest_opd(opd_ea, ctx);
        vm_write32(once_ea, 1);
    }
    LeaveCriticalSection(&s_once_cs);
    ctx->gpr[3] = 0;
}

/* ---------------------------------------------------------------------------
 * Direct syscall handlers (registered over the runtime defaults by
 * shims.cpp: 43 yield, 44 join, 47/48 priority, 49 stack information)
 * -----------------------------------------------------------------------*/

extern "C" int64_t yz_sc_thread_yield(ppu_context* ctx)
{
    (void)ctx;
    SwitchToThread();
    return 0;
}

extern "C" int64_t yz_sc_thread_join(ppu_context* ctx)
{
    uint32_t tid    = (uint32_t)ctx->gpr[3];
    uint32_t out_ea = (uint32_t)ctx->gpr[4];

    /* Self-join guard (RPCS3 sys_ppu_thread.cpp:188-191): a thread can never
     * complete waiting on itself. */
    if (tid == s_cur_tid)
        return (int64_t)(int32_t)0x80010008; /* CELL_EDEADLK */

    thr_lock();
    thr_rec* t = thr_find(tid);
    if (!t) {
        thr_unlock();
        return (int64_t)(int32_t)0x80010005; /* CELL_ESRCH */
    }
    if (t->detached) {
        thr_unlock();
        return (int64_t)(int32_t)0x80010002; /* CELL_EINVAL: not joinable */
    }
    /* Double-join guard (RPCS3 sys_ppu_thread.cpp:195-211): only one joiner
     * at a time. */
    if (t->joiner) {
        thr_unlock();
        return (int64_t)(int32_t)0x80010002; /* CELL_EINVAL */
    }
    t->joiner = (int)s_cur_tid;
    HANDLE h = t->handle;
    thr_unlock();

    if (h) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    thr_lock();
    if (out_ea)
        vm_write64(out_ea, t->exit_code);
    t->in_use = 0;
    t->joiner = 0;
    thr_unlock();
    return 0;
}

/* sys_ppu_thread_detach: routed here (not the runtime's standalone table) so
 * ids line up with threads created through the sysPrxForUser import
 * overrides -- see the file header + shims.cpp registration comment. */
extern "C" int64_t yz_sc_thread_detach(ppu_context* ctx)
{
    uint32_t tid = (uint32_t)ctx->gpr[3];

    thr_lock();
    thr_rec* t = thr_find(tid);
    if (!t) {
        thr_unlock();
        return (int64_t)(int32_t)0x80010005; /* CELL_ESRCH */
    }
    if (t->detached) {
        thr_unlock();
        return (int64_t)(int32_t)0x80010002; /* CELL_EINVAL: RPCS3 sys_ppu_thread.cpp:286-287 */
    }
    t->detached = 1;
    thr_unlock();
    return 0;
}

/* sys_ppu_thread_get_join_state: reports whether the CALLING thread is still
 * joinable. Out-param convention mirrors sys_ppu_thread.c's
 * sys_ppu_thread_get_join_state (277-380 range covers the sibling
 * join/detach out-EA writes this copies). */
extern "C" int64_t yz_sc_thread_get_join_state(ppu_context* ctx)
{
    uint32_t out_ea = (uint32_t)ctx->gpr[3];

    thr_lock();
    thr_rec* t = thr_find(s_cur_tid);
    int32_t joinable = (t && !t->detached) ? 1 : 0;
    thr_unlock();

    if (out_ea)
        vm_write32(out_ea, (uint32_t)joinable);
    return 0;
}

extern "C" int64_t yz_sc_thread_set_priority(ppu_context* ctx)
{
    uint32_t tid  = (uint32_t)ctx->gpr[3];
    int32_t  prio = (int32_t)ctx->gpr[4];

    /* Priority validation (RPCS3 sys_ppu_thread.cpp:336,487). */
    if (prio < 0 || prio > 3071)
        return (int64_t)(int32_t)0x80010002; /* CELL_EINVAL */

    thr_lock();
    thr_rec* t = thr_find(tid);
    if (t)
        t->priority = prio;
    thr_unlock();
    return t ? 0 : (int64_t)(int32_t)0x80010005;
}

extern "C" int64_t yz_sc_thread_get_priority(ppu_context* ctx)
{
    uint32_t tid    = (uint32_t)ctx->gpr[3];
    uint32_t out_ea = (uint32_t)ctx->gpr[4];
    thr_lock();
    thr_rec* t = thr_find(tid);
    int32_t prio = t ? t->priority : 0;
    thr_unlock();
    if (!t)
        return (int64_t)(int32_t)0x80010005;
    if (out_ea)
        vm_write32(out_ea, (uint32_t)prio);
    return 0;
}

extern "C" int64_t yz_sc_thread_get_stack_information(ppu_context* ctx)
{
    uint32_t out_ea = (uint32_t)ctx->gpr[3];
    thr_lock();
    thr_rec* t = thr_find(s_cur_tid);
    uint32_t base = t ? t->stack_base : 0;
    uint32_t size = t ? t->stack_size : 0;
    thr_unlock();
    if (out_ea) {
        vm_write32(out_ea, base);
        vm_write32(out_ea + 4, size);
    }
    return 0;
}
