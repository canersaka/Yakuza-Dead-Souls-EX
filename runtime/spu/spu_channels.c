/*
 * ps3recomp - SPU channel + indirect-branch runtime glue
 *
 * Implements the externs the SPU lifter (tools/spu_lifter.py) emits:
 *   - spu_rdch / spu_rchcnt / spu_wrch : SPU channel access. MFC channels are
 *     routed to the DMA engine (spu_dma.h); mailboxes, signal notification,
 *     events and the decrementer use the spu_context channel fields.
 *   - spu_indirect_branch : resolves ctx->pc to a lifted spu_func_* via a
 *     registry that generated code populates by calling spu_recomp_register().
 *
 * The MFC engine state is kept per spu_context here (spu_context.h does not
 * embed one), in a small lazily-populated registry.
 */

#include "spu_dma.h"
#include "../../include/ps3emu/error_codes.h"   /* CELL_EBUSY / CELL_EINVAL: event ack mapping */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#if defined(_MSC_VER)
#  include <intrin.h>
#  define SPU_CPU_RELAX() _mm_pause()
#else
#  include <time.h>
#  define SPU_CPU_RELAX() ((void)0)
#endif

/*
 * Publication acknowledgement for the GCM user callback that stages image-4
 * work after raising the SPU signal.  Shared with the GCM callback code under
 * yakuza/ and the SPU atomics stress test.
 */
volatile long g_yz_ucmd_handler_arg = -1;
volatile long g_yz_ucmd_handler_completed = -1;
volatile long g_yz_ucmd_handler_epoch = 0;
volatile long g_yz_ucmd_handler_completed_epoch = 0;
volatile long g_yz_ucmd_handler_active = 0;


/* Per-thread unwind target: a halted SPU (stop instruction, or an indirect
 * branch into unlifted code) must abort the lifted call chain rather than
 * return into it and spin. The host thread proc sets this with setjmp;
 * spu_halt() longjmps back to it. */
#if defined(_MSC_VER)
__declspec(thread) jmp_buf* g_spu_halt_jmp = 0;
#else
_Thread_local jmp_buf* g_spu_halt_jmp = 0;
#endif

/* Host-call-depth guard restart target + stack base. The lifter emits SPU
 * `brsl`/`bisl` (CALL) as NESTED host calls; a SPURS coroutine/poll yield that
 * tail-branches away from a CALL without ever returning leaks the host frame
 * each iteration, which would overflow the 64MB SPU thread stack. When
 * the host stack grows past a safe threshold, spu_indirect_branch longjmps to
 * g_spu_restart_jmp; the driver (lv2_register.c) re-dispatches from ctx->pc on a
 * fresh stack. All SPU state is in the heap `ctx`, so it survives -- this bounds
 * the host stack the way the SPU's own 256KB LS stack bounds it on hardware. */
#if defined(_MSC_VER)
__declspec(thread) jmp_buf* g_spu_restart_jmp = 0;
__declspec(thread) char*    g_spu_stack_base  = 0;
#else
_Thread_local jmp_buf* g_spu_restart_jmp = 0;
_Thread_local char*    g_spu_stack_base  = 0;
#endif

/* Crash attribution for SPU host threads.  They never set the PPU thread id,
 * so a fault on one would otherwise be reported against the main thread.
 * Called by the crash handler: if this thread carries an SPU context
 * (maintained at every indirect branch), name it. */
void yz_spu_crash_note(void)
{
    spu_context* c = g_spu_cur_ctx;
    if (!c) return;
    fprintf(stderr, "\n[crash] faulting thread is an SPU HOST THREAD: spu=%X img=%d "
            "pc=0x%05X (ignore the tid above -- TLS fallback; any [crash-t1] dump "
            "below is the MAIN thread's state, not the faulter's)",
            c->spu_id, (int)c->image_id, c->pc & SPU_LS_MASK);
    fflush(stderr);
}

/* LS address of the journal consumer's poll cursor: the journal manager sits
 * at LS 0xBC90 in gs_task and keeps its cursor at +0x1C. */
static const uint32_t g_yz_anch_home = 0xBCACu;

static uint32_t yz_ls_w32(const uint8_t* p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

/* The journal consumer's SPU context, published by the MFC engine on the
 * consumer's first poll GET (spu_dma.h).  The PPU-side producer throttle
 * (yz_jrnl_throttle) reads the live poll cursor out of that SPU's LS to bound
 * the head-to-cursor gap.  The cross-thread LS read is racy by design: the
 * value only paces the producer, and every consumer fails open on 0. */
volatile void* g_yz_consumer_ctx;

uint32_t yz_consumer_cursor(void)
{
    spu_context* c = (spu_context*)g_yz_consumer_ctx;
    if (!c) return 0;
    /* The LS bytes at the anchor home only mean "poll cursor" while gs_task
     * (image 0) is resident on that SPU; after a workload switch they are
     * another image's bytes. */
    if (c->image_id != 0) return 0;
    return yz_ls_w32((const uint8_t*)c->ls + g_yz_anch_home);
}

static mfc_engine* mfc_for(spu_context* ctx);
static void mfc_release(mfc_engine* mfc);

/* Tail-call trampoline target (see spu_context.h / SPU_DRAIN). Set by a
 * cross-function tail branch in lifted SPU code; drained iteratively by the
 * enclosing call site or the host-thread driver. */
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = 0;
SPU_THREAD_LOCAL spu_context* g_spu_cur_ctx = 0;

void spu_halt(spu_context* ctx, int status)
{
    ctx->status = (uint32_t)status;
    if (g_spu_halt_jmp)
        longjmp(*g_spu_halt_jmp, 1);
    /* No unwind target (e.g. unit test): fall back to returning. */
}

int spu_native_image_executor(spu_context* ctx, int image_id, uint32_t entry_pc)
{
    jmp_buf halt_jb, restart_jb;
    char stack_base_marker;
    int success;
    if (!ctx) return 0;

    ctx->image_id = image_id;
    ctx->pc = entry_pc & SPU_LS_MASK;
    ctx->status = SPU_STATUS_RUNNING;
    g_spu_halt_jmp = &halt_jb;
    g_spu_restart_jmp = &restart_jb;
    g_spu_stack_base = &stack_base_marker;
    g_spu_trampoline_fn = 0;
    if (setjmp(halt_jb) == 0) {
        (void)setjmp(restart_jb);
        g_spu_trampoline_fn = 0;
        ctx->host_depth = 0;
        spu_indirect_branch(ctx);
        SPU_DRAIN(ctx);
        if (ctx->status == SPU_STATUS_RUNNING)
            ctx->status = SPU_STATUS_STOPPED_BY_STOP;
    }
    success = ctx->status == SPU_STATUS_STOPPED_BY_STOP ||
              ctx->status == SPU_STATUS_STOPPED;
    g_spu_halt_jmp = 0;
    g_spu_restart_jmp = 0;
    g_spu_stack_base = 0;
    g_spu_trampoline_fn = 0;
    return success;
}

/* ===========================================================================
 * Global lock-line lock (GETLLAR/PUTLLC transactions across all SPU host
 * threads). The critical sections are tiny (two 128-byte memcpy + memcmp),
 * but SPURS can make several host SPU threads contend on the same line at
 * once. Use a test-test-and-set lock: only an actual acquisition attempt
 * writes the lock word, while waiters poll it read-only. A plain
 * atomic_flag_test_and_set loop makes every waiter take exclusive ownership
 * of the cache line on every spin. This changes only the host locking
 * algorithm; the process-wide serialization and acquire/release ordering stay
 * identical.
 * ===========================================================================*/
#include <stdatomic.h>   /* MSVC: needs /experimental:c11atomics (set by the
                            runtime CMake flags) */
static atomic_int s_lockline = 0;
void spu_lockline_lock(void)
{
    while (atomic_exchange_explicit(
               &s_lockline, 1, memory_order_acquire)) {
        while (atomic_load_explicit(
                   &s_lockline, memory_order_relaxed))
            SPU_CPU_RELAX();
    }
}
void spu_lockline_unlock(void)
{
    atomic_store_explicit(&s_lockline, 0, memory_order_release);
}

/* Host-core yield for SPU idle-poll loops (the spu_dma.h GETLLAR backoff).
 * level 0 = cpu pause (stay hot, just stop hammering the lock-line lock at
 * full rate); level 1 =
 * hand the core to the OS scheduler for a quantum (still hot when cores are
 * idle -- SwitchToThread returns immediately with no waiter); level 2 = a
 * real 1 ms sleep, the only rung that actually idles the core (laptop
 * heat/throttling; up to ~15 ms wall with the default timer resolution --
 * acceptable reaction latency for an SPU that has been idle this long);
 * level 3 = Sleep(0), which gives any ready host thread a chance without
 * incurring the timer-resolution delay of level 2.
 * Without the backoff the SPURS kernel SPUs each burn nearly a full host core
 * in their GETLLAR poll loops; the pause/yield rungs alone relieve the
 * lock-line contention (the lock rate, not occupancy, is the problem). */
#if defined(_WIN32)
__declspec(dllimport) int __stdcall SwitchToThread(void);
__declspec(dllimport) void __stdcall Sleep(unsigned long ms);
/* Channel-stall wake: Win32 SRWLOCK + CONDITION_VARIABLE (each a single
 * pointer-sized struct; passing &field as the opaque handle is ABI-correct). */
__declspec(dllimport) void __stdcall AcquireSRWLockExclusive(void* lock);
__declspec(dllimport) void __stdcall ReleaseSRWLockExclusive(void* lock);
__declspec(dllimport) void __stdcall WakeAllConditionVariable(void* cv);
__declspec(dllimport) int  __stdcall SleepConditionVariableSRW(void* cv, void* lock,
                                                               unsigned long ms,
                                                               unsigned long flags);
__declspec(dllimport) int  __stdcall WaitOnAddress(
    volatile void* address, void* compare_address,
    size_t address_size, unsigned long milliseconds);
__declspec(dllimport) void __stdcall WakeByAddressAll(void* address);
#else
#include <sched.h>
#include <unistd.h>
#endif

long yz_ucmd_handler_begin(long cause)
{
#if defined(_WIN32)
    _InterlockedIncrement(&g_yz_ucmd_handler_active);
    const long epoch = _InterlockedIncrement(&g_yz_ucmd_handler_epoch);
    _InterlockedExchange(&g_yz_ucmd_handler_arg, cause);
    return epoch;
#else
    ++g_yz_ucmd_handler_active;
    g_yz_ucmd_handler_arg = cause;
    const long epoch = ++g_yz_ucmd_handler_epoch;
    return epoch;
#endif
}

void yz_ucmd_handler_end(long cause, long epoch)
{
#if defined(_WIN32)
    _InterlockedExchange(&g_yz_ucmd_handler_completed, cause);
    long observed = _InterlockedCompareExchange(
        &g_yz_ucmd_handler_completed_epoch, 0, 0);
    while (observed < epoch) {
        const long prior = _InterlockedCompareExchange(
            &g_yz_ucmd_handler_completed_epoch, epoch, observed);
        if (prior == observed) break;
        observed = prior;
    }
    _InterlockedDecrement(&g_yz_ucmd_handler_active);
    WakeByAddressAll(&g_yz_ucmd_handler_active);
#else
    g_yz_ucmd_handler_completed = cause;
    if (g_yz_ucmd_handler_completed_epoch < epoch)
        g_yz_ucmd_handler_completed_epoch = epoch;
    --g_yz_ucmd_handler_active;
#endif
}

long yz_ucmd_wait_for_handler_completion(void)
{
#if defined(_WIN32)
    for (;;) {
        const long active = _InterlockedCompareExchange(
            &g_yz_ucmd_handler_active, 0, 0);
        const long entered = _InterlockedCompareExchange(
            &g_yz_ucmd_handler_epoch, 0, 0);
        const long completed = _InterlockedCompareExchange(
            &g_yz_ucmd_handler_completed_epoch, 0, 0);
        if (!active && completed >= entered &&
            _InterlockedCompareExchange(
                &g_yz_ucmd_handler_active, 0, 0) == 0)
            return entered;
        long observed = active;
        WaitOnAddress(&g_yz_ucmd_handler_active, &observed,
                      sizeof(observed), 0xFFFFFFFFul);
    }
#else
    return g_yz_ucmd_handler_epoch;
#endif
}

int yz_ucmd_handler_snapshot_is_stable(long epoch)
{
#if defined(_WIN32)
    const long active = _InterlockedCompareExchange(
        &g_yz_ucmd_handler_active, 0, 0);
    const long entered = _InterlockedCompareExchange(
        &g_yz_ucmd_handler_epoch, 0, 0);
    const long completed = _InterlockedCompareExchange(
        &g_yz_ucmd_handler_completed_epoch, 0, 0);
    return !active && entered == epoch && completed >= epoch &&
           _InterlockedCompareExchange(
               &g_yz_ucmd_handler_active, 0, 0) == 0;
#else
    return g_yz_ucmd_handler_active == 0 &&
           g_yz_ucmd_handler_epoch == epoch &&
           g_yz_ucmd_handler_completed_epoch >= epoch;
#endif
}

void spu_idle_yield(int level)
{
    if (level == 0) {
        SPU_CPU_RELAX(); SPU_CPU_RELAX(); SPU_CPU_RELAX(); SPU_CPU_RELAX();
        return;
    }
#if defined(_WIN32)
    if (level == 3) Sleep(0);
    else if (level >= 2) Sleep(1);
    else SwitchToThread();
#else
    if (level >= 2) usleep(1000);
    else sched_yield();
#endif
}

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * SPU channel-stall contract
 *
 * The SPU ISA stalls the pipeline on a channel access that cannot complete:
 * `rdch` on an empty read channel waits until data arrives; `wrch` on a full
 * write channel waits until a slot frees. Returning immediately with a
 * stale/architected-default value would steer guest code down control-flow
 * paths hardware never takes. This block implements the stall for the read
 * channels that have a real cross-thread producer (RdInMbox, RdSigNotify1/2,
 * RdEventStat).
 *
 * Threading model: every SPU runs on its own host thread
 * (spu_exec_thread_proc, lv2_register.c); spu_rdch is only ever called from
 * that thread. The producers run on other threads (the PPU via write_snr and
 * vm_write coherence, or a different SPU via the PUTLLC LR-raise), so blocking
 * one SPU host thread never blocks its own waker. Correctness does not depend
 * on the CV signal arriving: the waiter re-polls the predicate every 10 ms, so
 * a missed/racy wake only costs latency (the wake is a latency optimization,
 * and the 10 ms timeout is the missed-wake safety net). The predicate is read
 * fresh each slice (x86 TSO makes a producer's plain store globally visible
 * well within 10 ms), so no lock is needed around the shared state itself.
 *
 * Writes (SPU_WrOutMbox / SPU_WrOutIntrMbox) are deliberately not made blocking;
 * see the comment at those cases in spu_wrch: this runtime has no cross-thread
 * out-mbox drainer, so a block-while-full there would deadlock the SPU host
 * thread.
 *
 * ===========================================================================*/

/* Predicate: is `channel` readable right now (would rdch complete)?
 * Acquire-loads on the cross-thread-shared fields:
 * this predicate is what gates the actual data read (spu_channel_read /
 * the event_status & event_mask test), so it needs to synchronize-with the
 * producer's release store (spu_channel_write / atomic_fetch_or_explicit on
 * event_status), not just be "eventually visible" under TSO. */
static int spu_ch_ready(spu_context* ctx, uint32_t channel)
{
    switch (channel) {
    case SPU_RdInMbox:
        return atomic_load_explicit(&ctx->ch_in_mbox.count, memory_order_acquire) != 0;
    case SPU_RdSigNotify1:
        return atomic_load_explicit(&ctx->ch_sig_notify[0].count, memory_order_acquire) != 0;
    case SPU_RdSigNotify2:
        return atomic_load_explicit(&ctx->ch_sig_notify[1].count, memory_order_acquire) != 0;
    case SPU_RdEventStat:
        return (atomic_load_explicit(&ctx->event_status, memory_order_acquire) & ctx->event_mask) != 0;
    case MFC_RdAtomicStat: {
        mfc_engine* m = mfc_for(ctx);
        const int ready = m->atomic_stat_ready != 0;
        mfc_release(m);
        return ready;
    }
    case MFC_Cmd: {
        mfc_engine* m = mfc_for(ctx);
        const int ready = m->queue_count < MFC_QUEUE_DEPTH;
        mfc_release(m);
        return ready;
    }
    case MFC_RdTagStat: {
        mfc_engine* m = mfc_for(ctx);
        const int ready = m->tag_status_ready != 0;
        mfc_release(m);
        return ready;
    }
    case MFC_WrTagUpdate:
        /* The write channel is acknowledged once the request is accepted;
         * conditional result readiness belongs to RdTagStat, not this count. */
        return 1;
    default:                return 1;   /* non-blocking channels: always "ready" */
    }
}


/* Signal the per-SPU wait CV so a blocked spu_rdch re-checks its predicate. */
void spu_ch_wake(spu_context* ctx)
{
    if (!ctx) return;
#if defined(_WIN32)
    WakeAllConditionVariable(&ctx->ch_wait_cv);
#else
    (void)ctx;
#endif
}

/* Block the calling SPU host thread until `channel` is readable. Never returns
 * a fabricated value -- the caller reads the channel only after this returns.
 * Sets status WAITING_CHANNEL while parked, RUNNING on wake. */
static void spu_ch_wait(spu_context* ctx, uint32_t channel)
{
    if (spu_ch_ready(ctx, channel)) return;   /* fast path: no stall */


    ctx->status = SPU_STATUS_WAITING_CHANNEL;
#if defined(_WIN32)
    {
        while (!spu_ch_ready(ctx, channel)) {
            /* The SRWLOCK only satisfies SleepConditionVariableSRW's API
             * contract; the predicate lives in shared channel state, re-checked
             * under and outside the lock. 10 ms timeout = the missed-wake net. */
            AcquireSRWLockExclusive(&ctx->ch_wait_lock);
            if (!spu_ch_ready(ctx, channel))
                SleepConditionVariableSRW(&ctx->ch_wait_cv, &ctx->ch_wait_lock, 10, 0);
            ReleaseSRWLockExclusive(&ctx->ch_wait_lock);

        }
    }
#else
    {
        while (!spu_ch_ready(ctx, channel)) {
            struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 10 * 1000 * 1000;
            nanosleep(&ts, NULL);
        }
    }
#endif
    ctx->status = SPU_STATUS_RUNNING;
}

/* ===========================================================================
 * PPU<->SPU lock-line coherence.
 * SPU code hammers GETLLAR/PUTLLC on shared management lines while the PPU
 * atomically bumps the same lines
 * (wklReadyCount1, sysSrvMessage, ...). A PPU write that interleaves with the
 * kernel's PUTLLC memcpy is torn/clobbered, so the kernel would never see the
 * bump. We mark every 128-byte line the SPU
 * reserves (GETLLAR) in a bitmap; a PPU write (shims.cpp vm_write*) to a marked
 * line serializes through the lock-line lock so the kernel's PUTLLC memcmp sees
 * the change and re-reads. Scoped to [0x40000000,0x44000000) (SPURS
 * structs + cmd buffer); cmd-buffer lines are never SPU-reserved so they stay
 * unmarked = fast path. Lines are never un-marked (the management area is always
 * reserved; a stale mark only costs an extra locked write to a SPURS struct). */
#define SPU_COH_LO 0x40000000u
#define SPU_COH_HI 0x44000000u
static atomic_uchar s_coh_bitmap[((SPU_COH_HI - SPU_COH_LO) >> 7) / 8];  /* 64 KB, bit per 128B line */

/* Window 2. Reservation loss in the Cell architecture has no address bound, so
 * coherence is not limited to window 1: PPU writes to SPU-reserved lines
 * outside window 1 would take the plain-memcpy path with no lock-line
 * serialization, the same torn-write clobber class window 1 stops. Known
 * resident: the SPURS tasksets (0x63Dxxxxx), PPU-written by CreateTask2 while
 * SPUs PUTLLC-cycle them. Same bitmap+generation machinery, second range.
 * Reservations outside all windows fail open: PPU writes to such lines are
 * not serialized. */
#define SPU_COH2_LO 0x60000000u
#define SPU_COH2_HI 0x68000000u
static atomic_uchar s_coh2_bitmap[((SPU_COH2_HI - SPU_COH2_LO) >> 7) / 8];
static _Atomic uint32_t s_coh2_gen[(SPU_COH2_HI - SPU_COH2_LO) >> 7];

/* Window 3. SPUs GETLLAR-reserve lines in the game's main-RAM structure
 * segment, so PPU writes to them must not take the plain-memcpy path: a torn
 * PPU-store-vs-PUTLLC commit would mix old and new data. Same machinery,
 * third range. */
#define SPU_COH3_LO 0x01000000u
#define SPU_COH3_HI 0x02000000u
static atomic_uchar s_coh3_bitmap[((SPU_COH3_HI - SPU_COH3_LO) >> 7) / 8];
static _Atomic uint32_t s_coh3_gen[(SPU_COH3_HI - SPU_COH3_LO) >> 7];

/* Per-128B-line write generation. Bumped on every PPU coherence write to a
 * reserved line; GETLLAR snapshots it and LR is re-derived at the next GETLLAR
 * by comparison, so a write that lands while no SPU reservation is active (the
 * window between PUTLLC and the next GETLLAR) is not lost. Without it such a
 * wakeup can be missed. */
static _Atomic uint32_t s_coh_gen[(SPU_COH_HI - SPU_COH_LO) >> 7];
uint32_t spu_coh_gen(uint32_t addr)
{
    if (addr - SPU_COH_LO < SPU_COH_HI - SPU_COH_LO)
        return atomic_load_explicit(
            &s_coh_gen[(addr - SPU_COH_LO) >> 7], memory_order_acquire);
    if (addr - SPU_COH2_LO < SPU_COH2_HI - SPU_COH2_LO)
        return atomic_load_explicit(
            &s_coh2_gen[(addr - SPU_COH2_LO) >> 7], memory_order_acquire);
    if (addr - SPU_COH3_LO < SPU_COH3_HI - SPU_COH3_LO)
        return atomic_load_explicit(
            &s_coh3_gen[(addr - SPU_COH3_LO) >> 7], memory_order_acquire);
    return 0;
}

void spu_coh_reserve(uint32_t ea)
{
    if (ea - SPU_COH_LO < SPU_COH_HI - SPU_COH_LO) {
        uint32_t line = (ea - SPU_COH_LO) >> 7;
        atomic_fetch_or_explicit(
            &s_coh_bitmap[line >> 3],
            (unsigned char)(1u << (line & 7)), memory_order_release);
        return;
    }
    if (ea - SPU_COH2_LO < SPU_COH2_HI - SPU_COH2_LO) {
        uint32_t line = (ea - SPU_COH2_LO) >> 7;
        atomic_fetch_or_explicit(
            &s_coh2_bitmap[line >> 3],
            (unsigned char)(1u << (line & 7)), memory_order_release);
        return;
    }
    if (ea - SPU_COH3_LO < SPU_COH3_HI - SPU_COH3_LO) {
        uint32_t line = (ea - SPU_COH3_LO) >> 7;
        atomic_fetch_or_explicit(
            &s_coh3_bitmap[line >> 3],
            (unsigned char)(1u << (line & 7)), memory_order_release);
        return;
    }
    /* Outside every coherence window: fail open. PPU writes to this line are
     * not serialized against the SPU's reservation. */
}
int spu_coh_is_reserved(uint32_t addr)
{
    if (addr - SPU_COH_LO < SPU_COH_HI - SPU_COH_LO) {
        uint32_t line = (addr - SPU_COH_LO) >> 7;
        return (atomic_load_explicit(
                    &s_coh_bitmap[line >> 3], memory_order_acquire)
                >> (line & 7)) & 1u;
    }
    if (addr - SPU_COH2_LO < SPU_COH2_HI - SPU_COH2_LO) {
        uint32_t line = (addr - SPU_COH2_LO) >> 7;
        return (atomic_load_explicit(
                    &s_coh2_bitmap[line >> 3], memory_order_acquire)
                >> (line & 7)) & 1u;
    }
    if (addr - SPU_COH3_LO < SPU_COH3_HI - SPU_COH3_LO) {
        uint32_t line = (addr - SPU_COH3_LO) >> 7;
        return (atomic_load_explicit(
                    &s_coh3_bitmap[line >> 3], memory_order_acquire)
                >> (line & 7)) & 1u;
    }
    return 0;
}

/* ===========================================================================
 * Per-context MFC engine registry
 *
 * Native SPURS maps independently-saved tasks to host contexts. There can be
 * more live task contexts than physical SPUs, so an eight-entry array is not
 * a valid architectural limit. Keep a dynamically-sized, synchronized list:
 * each context always owns isolated tag/reservation/list-stall state, and
 * teardown removes the entry before the context storage is freed.
 * ===========================================================================*/
typedef struct spu_mfc_slot {
    spu_context* ctx;
    mfc_engine   mfc;
    _Atomic uint32_t refs;
    struct spu_mfc_slot* next;
    struct spu_mfc_slot* bucket_next;
} spu_mfc_slot;

/* The global list remains authoritative for diagnostics/coherence. Bucket
 * lookup locks protect the same entries and references; every insertion and
 * removal takes bucket then global, so either lookup mode can run safely. */
typedef struct spu_mfc_bucket {
    spu_mfc_slot* head;
    _Atomic unsigned char lock;
    unsigned char padding[64 - sizeof(void*) - 1];
} spu_mfc_bucket;
_Alignas(64) static spu_mfc_bucket s_mfc_buckets[64];
static _Atomic int s_mfc_sharded_lookup;

void spu_mfc_set_sharded_lookup(int enabled)
{
    atomic_store_explicit(&s_mfc_sharded_lookup, enabled != 0,
                          memory_order_relaxed);
}

static spu_mfc_bucket* mfc_bucket_for(const spu_context* ctx)
{
    uint64_t key = (uint64_t)(uintptr_t)ctx >> 4;
    key ^= key >> 17;
    key *= UINT64_C(0x9E3779B97F4A7C15);
    return &s_mfc_buckets[key >> 58];
}

static void mfc_bucket_lock(spu_mfc_bucket* bucket)
{
    while (atomic_exchange_explicit(&bucket->lock, 1, memory_order_acquire))
        SPU_CPU_RELAX();
}

static void mfc_bucket_unlock(spu_mfc_bucket* bucket)
{
    atomic_store_explicit(&bucket->lock, 0, memory_order_release);
}

static spu_mfc_slot* s_mfc_slots;
static _Atomic unsigned char s_mfc_registry_lock;
static _Atomic int s_mfc_read_spin;

/* Both modes acquire the same lock. Mode changes cannot change ownership or
 * retirement/refcount rules, and are safe while worker threads are active. */
void spu_mfc_set_read_spin(int enabled)
{
    atomic_store_explicit(&s_mfc_read_spin, enabled != 0, memory_order_relaxed);
}

static void mfc_registry_lock(void)
{
    const int read_spin = atomic_load_explicit(&s_mfc_read_spin,
                                               memory_order_relaxed);
    while (atomic_exchange_explicit(&s_mfc_registry_lock, 1,
                                    memory_order_acquire)) {
        do {
            SPU_CPU_RELAX();
        } while (read_spin && atomic_load_explicit(&s_mfc_registry_lock,
                                                   memory_order_relaxed));
    }
}

static void mfc_registry_unlock(void)
{
    atomic_store_explicit(&s_mfc_registry_lock, 0, memory_order_release);
}




static mfc_engine* mfc_for(spu_context* ctx)
{
    spu_mfc_bucket* bucket = mfc_bucket_for(ctx);
    const int sharded = atomic_load_explicit(&s_mfc_sharded_lookup,
                                              memory_order_relaxed);
    if (sharded) mfc_bucket_lock(bucket);
    else mfc_registry_lock();
    if (atomic_load_explicit(&ctx->mfc_retiring, memory_order_acquire)) {
        fprintf(stderr, "[mfc] FATAL: channel access after ctx retirement %p\n",
                (void*)ctx);
        fflush(stderr);
        abort();
    }
    for (spu_mfc_slot* slot = sharded ? bucket->head : s_mfc_slots;
         slot; slot = sharded ? slot->bucket_next : slot->next) {
        if (slot->ctx == ctx) {
            atomic_fetch_add_explicit(&slot->refs, 1, memory_order_relaxed);
            if (sharded) mfc_bucket_unlock(bucket);
            else mfc_registry_unlock();
            return &slot->mfc;
        }
    }
    /* A miss is rare. Release the global-only lookup before taking locks in
     * lifecycle order; recheck because another accessor may have created it. */
    if (!sharded) {
        mfc_registry_unlock();
        mfc_bucket_lock(bucket);
    }
    mfc_registry_lock();
    if (atomic_load_explicit(&ctx->mfc_retiring, memory_order_acquire)) {
        fprintf(stderr, "[mfc] FATAL: channel access after ctx retirement %p\n",
                (void*)ctx);
        fflush(stderr);
        abort();
    }
    for (spu_mfc_slot* found = bucket->head; found; found = found->bucket_next) {
        if (found->ctx == ctx) {
            atomic_fetch_add_explicit(&found->refs, 1, memory_order_relaxed);
            mfc_registry_unlock();
            mfc_bucket_unlock(bucket);
            return &found->mfc;
        }
    }
    spu_mfc_slot* slot = (spu_mfc_slot*)calloc(1, sizeof(*slot));
    if (!slot) {
        mfc_registry_unlock();
        fprintf(stderr,
                "[mfc] FATAL: cannot allocate isolated engine for ctx=%p\n",
                (void*)ctx);
        fflush(stderr);
        abort();
    }
    slot->ctx = ctx;
    atomic_init(&slot->refs, 1);
    mfc_engine_init(&slot->mfc);
    slot->next = s_mfc_slots;
    s_mfc_slots = slot;
    slot->bucket_next = bucket->head;
    bucket->head = slot;
    mfc_registry_unlock();
    mfc_bucket_unlock(bucket);
    return &slot->mfc;
}

static void mfc_release(mfc_engine* mfc)
{
    spu_mfc_slot* slot = (spu_mfc_slot*)((uint8_t*)mfc -
        offsetof(spu_mfc_slot, mfc));
    atomic_fetch_sub_explicit(&slot->refs, 1, memory_order_release);
}

/* No-op if `ctx` never issued an MFC channel operation. */
void spu_mfc_unregister(spu_context* ctx)
{
    if (!ctx) return;
    atomic_store_explicit(&ctx->mfc_retiring, 1, memory_order_release);
    spu_mfc_bucket* bucket = mfc_bucket_for(ctx);
    mfc_bucket_lock(bucket);
    mfc_registry_lock();
    spu_mfc_slot** link = &s_mfc_slots;
    while (*link) {
        spu_mfc_slot* slot = *link;
        if (slot->ctx == ctx) {
            *link = slot->next;
            spu_mfc_slot** bucket_link = &bucket->head;
            while (*bucket_link && *bucket_link != slot)
                bucket_link = &(*bucket_link)->bucket_next;
            if (*bucket_link) *bucket_link = slot->bucket_next;
            mfc_registry_unlock();
            mfc_bucket_unlock(bucket);
            while (atomic_load_explicit(&slot->refs,
                                        memory_order_acquire) != 0)
                SPU_CPU_RELAX();
            memset(&slot->mfc, 0, sizeof(slot->mfc));
            free(slot);
            return;
        }
        link = &slot->next;
    }
    mfc_registry_unlock();
    mfc_bucket_unlock(bucket);
}

/* A task switch flushes the SPU atomic cache.  Ordinary DMA has already been
 * drained by the task API before this seam; only the lock-line reservation
 * needs to be discarded from the persistent host MFC engine. */
void spu_mfc_context_switch(spu_context* ctx)
{
    if (!ctx) return;
    mfc_registry_lock();
    for (spu_mfc_slot* slot = s_mfc_slots; slot; slot = slot->next) {
        if (slot->ctx == ctx) {
            slot->mfc.resv_active = 0;
            slot->mfc.resv_poll_n = 0;
            break;
        }
    }
    mfc_registry_unlock();
}

/* SPU_EVENT_LR (lock-line reservation lost): a write by another processor to a
 * 128-byte line an SPU has GETLLAR-reserved raises event 0x400 on that SPU and
 * invalidates its reservation. SPU code parked in `rdch SPU_RdEventStat` on a
 * reserved line wakes this way when the PPU writes that line. Called from
 * the PPU coherence write path (shims.cpp VM_WRITE_COH) for reserved lines. */
void spu_coh_notify_write(uint32_t ea)
{
    uint32_t line = ea & ~127u;
    if (line - SPU_COH_LO < SPU_COH_HI - SPU_COH_LO)
        atomic_fetch_add_explicit(
            &s_coh_gen[(line - SPU_COH_LO) >> 7], 1,
            memory_order_release); /* generation bump: covers a write between PUTLLC and the next GETLLAR */
    else if (line - SPU_COH2_LO < SPU_COH2_HI - SPU_COH2_LO)
        atomic_fetch_add_explicit(
            &s_coh2_gen[(line - SPU_COH2_LO) >> 7], 1,
            memory_order_release); /* window 2 */
    else if (line - SPU_COH3_LO < SPU_COH3_HI - SPU_COH3_LO)
        atomic_fetch_add_explicit(
            &s_coh3_gen[(line - SPU_COH3_LO) >> 7], 1,
            memory_order_release); /* window 3 (camera/effect block) */
    mfc_registry_lock();
    for (spu_mfc_slot* slot = s_mfc_slots; slot; slot = slot->next) {
        spu_context* c = slot->ctx;
        mfc_engine* m = &slot->mfc;
        if (m->resv_active && (uint32_t)(m->resv_ea & ~127ull) == line) {
            /* Atomic fetch-or, not a plain `|=`. This is a genuine
             * cross-thread write (the caller, shims.cpp VM_WRITE_COH, runs on
             * the PPU-writer thread, not this SPU's own host thread) that
             * races the owning SPU thread's own WrEventAck `&=` below and the
             * MFC engine's same-thread self-raises (spu_dma.h). A plain
             * compound `|=` is a load+store, not one instruction, so a
             * concurrent writer could land between them and lose an update
             * even though spu_lockline_lock already serializes this call
             * against WrEventAck (belt-and-suspenders, not a replacement for
             * that lock). Relaxed: only the bit accumulation itself needs to
             * be atomic here; ordering vs the wake is covered below. */
            atomic_fetch_or_explicit(&c->event_status, 0x400u, memory_order_relaxed);
            m->resv_active = 0;          /* reservation lost */
            /* The event_status store above must be visible before the wake.
             * On x86 TSO the plain store is globally ordered ahead of the
             * WakeAll call's own writes; the blocked RdEventStat waiter also
             * re-polls, so a straggling store is caught within 10 ms regardless. */
            spu_ch_wake(c);
        }
    }
    mfc_registry_unlock();
}

static int channel_is_mfc(uint32_t ch)
{
    switch (ch) {
    case MFC_WrMSSyncReq: case MFC_RdTagMask:  case MFC_LSA:
    case MFC_EAH:         case MFC_EAL:         case MFC_Size:
    case MFC_TagID:       case MFC_Cmd:         case MFC_WrTagMask:
    case MFC_WrTagUpdate: case MFC_RdTagStat:   case MFC_RdListStallStat:
    case MFC_WrListStallAck: case MFC_RdAtomicStat:
        return 1;
    default:
        return 0;
    }
}

/* ===========================================================================
 * Channel write
 * ===========================================================================*/

void spu_wrch(spu_context* ctx, uint32_t channel, u128 value)
{
    uint32_t v = value._u32[0];  /* channel writes use the preferred slot */

    /* MFC parameter channels only stage values in this SPU context.  They do
     * not inspect or mutate the context's mfc_engine, so avoid taking the
     * process-wide MFC registry lock merely to reach mfc_channel_write's
     * corresponding assignments.  MFC_Cmd and the status/list operations
     * still resolve the isolated engine below, preserving command issue,
     * ordering, queue-capacity, and reservation behavior exactly. */
    switch (channel) {
    case MFC_LSA:       ctx->mfc_lsa = v;      return;
    case MFC_EAH:       ctx->mfc_eah = v;      return;
    case MFC_EAL:       ctx->mfc_eal = v;      return;
    case MFC_Size:      ctx->mfc_size = v;     return;
    case MFC_TagID:     ctx->mfc_tag = v & 0x1Fu; return;
    case MFC_WrTagMask: ctx->mfc_tag_mask = v; return;
    default: break;
    }

    if (channel_is_mfc(channel)) {
        if (channel == MFC_Cmd)
            spu_ch_wait(ctx, MFC_Cmd);
        mfc_engine* mfc = mfc_for(ctx);
        mfc_channel_write(mfc, ctx, channel, v);
        mfc_release(mfc);
        return;
    }

    switch (channel) {
    case SPU_WrOutMbox:
        /* Deadlock carve-out: the ISA stalls wrch on a full out-mbox until
         * the PPU reads it. This runtime has no cross-thread out-mbox drainer:
         * the regular out-mbox is read only same-thread (thread-exit status pop
         * in lv2_register.c, and the send_event/eflag routing in SPU_WrOutIntrMbox
         * below), never by an independent PPU reader thread (no sys_spu_thread_
         * read_mb is implemented/registered). Blocking while full would
         * therefore deadlock the sole SPU host thread, so this write stays
         * non-blocking (overwrite). Depth is 1 and a same-thread double-write
         * overwrites rather than stalls, a known fidelity gap. */
        spu_channel_write(&ctx->ch_out_mbox, v);       break;
    case SPU_WrOutIntrMbox:
        /* Same deadlock carve-out as SPU_WrOutMbox: the interrupt mbox has
         * no drainer at all in this runtime (spu_channel_write below is the only
         * toucher of ch_out_intr_mbox), so block-while-full is a guaranteed hang.
         * Stays non-blocking. */
        /* sys_spu_thread_send_event / throw_event delivery.
         * code = v>>24; code<64 = send_event (acks the result into the SPU
         * InMbox), 64..127 = throw_event (no ack); both pop data
         * from the OutMbox (written just before) and deliver
         * {SYS_SPU_THREAD_EVENT_USER_KEY, spu_id, (spup<<32)|(v&0xFFFFFF), data}
         * to the event queue bound to the group's port (connect_event_all_
         * threads / cellSpursQueueAttachLv2EventQueue). The SPU-side
         * SpursQueue push wakes a blocking PPU popper (parked in
         * sys_event_queue_receive inside cellSpursQueuePopBody) by throwing on
         * the queue's attached port (v=0x53000000 -> port 0x13); dropping it
         * would leave that PPU thread blocked forever. */
        { uint32_t code = v >> 24;
          if (code < 128 && ctx->ch_out_mbox.count) {
              extern int yz_throw_latch_add(uint32_t qid, uint64_t src,
                                            uint64_t d1, uint64_t d2, uint64_t d3);
              uint32_t spup = code & 63;
              extern uint32_t spu_group_spup_queue(uint32_t group_id, uint32_t spup);
              extern int sys_event_queue_push_by_id(uint32_t queue_id, uint64_t source,
                                                    uint64_t d1, uint64_t d2, uint64_t d3);
              uint32_t qid = spu_group_spup_queue(ctx->spu_group_id, spup);
              if (qid) {
                  uint32_t data = spu_channel_read(&ctx->ch_out_mbox);
                  int rc = sys_event_queue_push_by_id(qid,
                              0xFFFFFFFF53505501ULL /* SYS_SPU_THREAD_EVENT_USER_KEY */,
                              ctx->spu_id,
                              ((uint64_t)spup << 32) | (v & 0xFFFFFFu),
                              data);
                  /* The ack reflects the push result, not a hardcoded
                   * CELL_OK: the result of the queue send is written into the
                   * SPU's InMbox for sys_spu_thread_send_event, so a
                   * full/dead destination queue is visible to the guest
                   * instead of being silently reported as delivered. The push
                   * helper's only failure mode past this point (qid already
                   * resolved to a live queue by spu_group_spup_queue above) is
                   * a full ring buffer (event_queue_push returns -1 there);
                   * sys_event_port_send maps that same -1 to CELL_EBUSY, so
                   * the ack reuses it. */
                  if (code < 64) {                     /* send_event acks the SPU */
                      spu_channel_write(&ctx->ch_in_mbox,
                          rc == 0 ? 0u /* CELL_OK */ : (uint32_t)(int32_t)CELL_EBUSY);
                      spu_ch_wake(ctx);   /* in-mbox now readable (same-thread ack, harmless) */
                  }
                  /* throw_event (code>=64) has no ack path by protocol; on
                   * real HW a full destination queue means the event is
                   * simply lost, and the game only survives because its
                   * consumer keeps up. Our timing widens the full-queue
                   * window, so latch the failed throw and redeliver when the
                   * queue drains (vblank-tick flush). The event was thrown;
                   * only its delivery timing shifts. A full latch drops it. */
                  if (code >= 64 && rc != 0)
                      (void)yz_throw_latch_add(qid,
                                  0xFFFFFFFF53505501ULL, ctx->spu_id,
                                  ((uint64_t)spup << 32) | (v & 0xFFFFFFu), data);
                  break;
              }
          }
          /* sys_event_flag set_bit protocol (codes 128 strict / 192 impatient):
           * the SPU sets bit (v&0xFFFFFF) of the lv2 event flag whose id it
           * wrote to OutMbox just before. A SPURS taskset exit handler fires
           * code 192 as its task-exit completion doorbell; dropping it would
           * leave the waiter on that event flag blocked. Strict form acks the
           * setter's return code into the SPU InMbox; impatient form doesn't
           * ack. */
          if ((code == 128 || code == 192) && ctx->ch_out_mbox.count) {
              extern int64_t sys_event_flag_set_by_id(uint32_t flag_id, uint64_t bitpat);
              uint32_t bit  = v & 0xFFFFFFu;
              uint32_t fid  = spu_channel_read(&ctx->ch_out_mbox);
              int64_t rc = (bit < 64)
                  ? sys_event_flag_set_by_id(fid, 1ull << bit)
                  : (int64_t)(int32_t)CELL_EINVAL;
              if (code == 128) {
                  spu_channel_write(&ctx->ch_in_mbox, (uint32_t)rc);
                  spu_ch_wake(ctx);   /* in-mbox now readable (same-thread ack) */
              }
              break;
          }
        }
        spu_channel_write(&ctx->ch_out_intr_mbox, v);
        break;
    case SPU_WrDec:
        /* Decrementer write (CBEA). Re-anchors the count: the written value
         * becomes the new count and the guest timebase at this instant becomes
         * the new zero point. Also re-arms counting if a prior WrEventAck had
         * frozen it (the ack-while-Tm-disabled stop rule, see SPU_WrEventAck
         * below), since a wrch to SPU_WrDec starts the decrementer.
         *
         * Not implemented here: the decrementer event (status bit 0x20, Tm, on an
         * MSb 0->1 transition), including the immediate-signal case where the
         * written value's own MSb is already 1 while the previous count's MSb
         * was 0. No lifted image has a `wrch SPU_WrDec` site and only one
         * WrEventMask and one WrEventAck site exist in total, so no guest code
         * currently arms or consumes Tm. This is a known remaining gap:
         * implementing the edge-detector would require persisting the
         * pre-write MSb across writes/reads with no live case to validate it
         * against, so it is left for when a real Tm consumer appears. */
        ctx->dec_value    = v;
        ctx->dec_start_tb = ppu_timebase_now();
        ctx->dec_running  = 1;
        break;
    case SPU_WrEventMask:
        /* A mask write can newly enable an already-pending event, making
         * (event_status & event_mask) satisfy a blocked RdEventStat. Same-thread
         * with any such reader (the SPU can't be both here and parked in rdch),
         * so the wake is moot in practice, but issued for producer completeness.
         * (WrEventAck below only clears status bits, so it can never satisfy the
         * predicate and gets no wake.) */
        ctx->event_mask = v;
        spu_ch_wake(ctx);
        break;
    case SPU_WrEventAck:
        /* Serialize this read-modify-write against the PPU coherence write
         * (spu_coh_notify_write, |= SPU_EVENT_LR) and the GETLLAR LR-raise
         * (spu_dma.h), both of which set event_status under
         * spu_lockline_lock. A bare `&= ~v` here could read event_status, then have
         * a concurrent `|= 0x400` land, then write back the stale value -> the LR
         * edge is dropped and the idle SPURS kernel never wakes.
         * atomic_fetch_and_explicit, not a plain `&=`, for the same reason
         * spu_coh_notify_write's write is atomic above: the lock keeps
         * ordering correct, the atomic RMW keeps the operation itself a single
         * indivisible op (defense in depth, not a replacement for the lock). */
        spu_lockline_lock();
        atomic_fetch_and_explicit(&ctx->event_status, ~v, memory_order_relaxed);
        spu_lockline_unlock();
        /* Decrementer stop rule (CBEA): acking the Tm event (bit 0x20) while
         * Tm is disabled in the event mask stops the decrementer; it must not
         * stop if Tm is enabled at ack time. The freeze is inert today (no
         * WrEventAck site acks 0x20) but is implemented faithfully since it
         * composes for free with the SPU_WrDec re-arm above. */
        if ((v & 0x20u) && !(ctx->event_mask & 0x20u))
            ctx->dec_running = 0;
        break;
    case SPU_WrSRR0:         ctx->srr0 = v;                                 break;
    default:
        /* Unknown/unhandled channel write: loud once per channel, then
         * ignored (a no-op on the SPU side). */
        {
            static uint8_t seen[128];
            if (channel < 128 && !seen[channel]) {
                seen[channel] = 1;
                fprintf(stderr, "[ch] UNIMPLEMENTED wrch ch=%u pc=0x%05X val=0x%08X (continuing)\n",
                        channel, ctx->pc & SPU_LS_MASK, v);
                fflush(stderr);
            }
        }
        break;
    }
}

void spu_mfc_issue_compact(spu_context* ctx,
                           uint32_t lsa, uint32_t eah, uint32_t eal,
                           uint32_t size, uint32_t tag, uint32_t cmd,
                           const uint32_t channel_pcs[6])
{

    ctx->mfc_lsa = lsa;
    ctx->mfc_eah = eah;
    ctx->mfc_eal = eal;
    ctx->mfc_size = size;
    ctx->mfc_tag = tag & 0x1Fu;

    spu_ch_wait(ctx, MFC_Cmd);
    mfc_engine* const mfc = mfc_for(ctx);
    mfc_channel_write(mfc, ctx, MFC_Cmd, cmd);
    mfc_release(mfc);
}

uint64_t spu_mfc_read_atomic_status_compact(spu_context* ctx)
{
    mfc_engine* const mfc = mfc_for(ctx);
    if (!mfc->atomic_stat_ready) {
        mfc_release(mfc);
        return 0;
    }
    const uint32_t status =
        mfc_channel_read(mfc, ctx, MFC_RdAtomicStat);
    mfc_release(mfc);
    return (1ull << 32) | status;
}

/* ===========================================================================
 * Channel read (returns value in the preferred word slot)
 * ===========================================================================*/
u128 spu_rdch(spu_context* ctx, uint32_t channel)
{
    uint32_t v = 0;

    if (channel_is_mfc(channel)) {
        if (channel == MFC_RdAtomicStat || channel == MFC_RdTagStat)
            spu_ch_wait(ctx, channel);
        mfc_engine* mfc = mfc_for(ctx);
        v = mfc_channel_read(mfc, ctx, channel);
        mfc_release(mfc);
        return spu_make_preferred_u32(v);
    }

    switch (channel) {
    case SPU_RdInMbox:
        /* rdch on an empty in-mbox waits (producer = PPU-side mailbox write
         * or the same-thread send_event ack). */
        spu_ch_wait(ctx, SPU_RdInMbox);
        v = spu_channel_read(&ctx->ch_in_mbox);     break;
    case SPU_RdSigNotify1:
        /* Wait while empty (producer = sys_spu_thread_write_snr, PPU thread). */
        spu_ch_wait(ctx, SPU_RdSigNotify1);
        /* The read-value-then-clear-count pair must be mutually exclusive
         * with the writer's check-count-then-mutate-value pair (OR-mode
         * accumulate in lv2_register.c sys_spu_thread_write_snr_handler), not
         * just individually atomic; see the long comment at that handler.
         * Same global lock-line spinlock that handler takes; the critical
         * section is one plain load + one atomic store (spu_channel_read),
         * the same tiny-section shape as every other spu_lockline_lock user
         * in this file. */
        spu_lockline_lock();
        v = spu_channel_read(&ctx->ch_sig_notify[0]);
        spu_lockline_unlock();
        break;
    case SPU_RdSigNotify2:
        spu_ch_wait(ctx, SPU_RdSigNotify2);
        spu_lockline_lock();
        v = spu_channel_read(&ctx->ch_sig_notify[1]);
        spu_lockline_unlock();
        break;
    case SPU_RdDec:
        /* Decrementer read (CBEA). Nonblocking; successive reads can return
         * the same value, which holds here too when called back-to-back within
         * the same guest timebase tick.
         * Overflow-safe: elapsed ticks can exceed dec_value (the count wraps
         * past 0, which is architecturally correct: it is a free-running
         * counter), so the subtraction is done in 32-bit wrapping arithmetic
         * exactly like a real 32-bit decrementer register. When dec_running is
         * 0 (frozen by WrEventAck while Tm was disabled) the count does not
         * advance. */
        {
            uint64_t elapsed = ctx->dec_running
                              ? (ppu_timebase_now() - ctx->dec_start_tb)
                              : 0;
            v = ctx->dec_value - (uint32_t)elapsed;
        }
        break;
    case SPU_RdEventMask:   v = ctx->event_mask;                        break;
    case SPU_RdEventStat:
        /* The architected stall: rdch RdEventStat waits until an enabled
         * event is pending, i.e. (event_status & event_mask) != 0. Every
         * event_status producer (spu_coh_notify_write's LR raise, the spu_dma.h
         * GETLLAR/PUTLLC LR + SN raises) calls spu_ch_wake(ctx), and the
         * 10 ms re-poll is the safety net if a producer path is missed. */
        spu_ch_wait(ctx, SPU_RdEventStat);
        /* The channel read returns the pending event register ANDed with the
         * event mask. The lifter's bisled emission already tests
         * `(event_status & event_mask)` (spu_lifter.py); the runtime channel-read
         * path is consistent with that, so code that masks an event to defer it
         * does not still observe it here. */
        /* Acquire-load, not a volatile cast: this is the actual channel value
         * the guest observes, so it must synchronize-with whichever
         * producer's release/atomic-RMW store set the bit(s) being read
         * (spu_ch_wait's predicate just above used the same acquire load to
         * decide the wait was over; re-reading here instead of reusing that
         * result is correct either way since both are acquire loads of the
         * same atomic object). */
        v = atomic_load_explicit(&ctx->event_status, memory_order_acquire) & ctx->event_mask;
        break;
    case SPU_RdMachStat:    v = (ctx->status == SPU_STATUS_RUNNING) ? 1 : 0; break;
    case SPU_RdSRR0:        v = ctx->srr0;                              break;
    default:
        /* Unknown/unimplemented read channel. Loud once-per-channel; the
         * ISA would stall on an unimplemented read channel (zero capacity), but
         * we have no producer to ever satisfy it, so we continue with 0 rather
         * than hang. */
        {
            static uint8_t seen[128];
            if (channel < 128 && !seen[channel]) {
                seen[channel] = 1;
                fprintf(stderr, "[ch] UNIMPLEMENTED rdch ch=%u pc=0x%05X -> 0 (continuing)\n",
                        channel, ctx->pc & SPU_LS_MASK);
                fflush(stderr);
            }
        }
        v = 0;
        break;
    }
    return spu_make_preferred_u32(v);
}

void spu_task_wait_all_dma(spu_context* ctx)
{
    if (!ctx) return;
    const uint32_t saved_tag_mask = ctx->mfc_tag_mask;
    spu_wrch(ctx, MFC_WrTagMask, spu_make_preferred_u32(UINT32_MAX));
    spu_wrch(ctx, MFC_WrTagUpdate, spu_make_preferred_u32(2u));
    (void)spu_rdch(ctx, MFC_RdTagStat);
    spu_wrch(ctx, MFC_WrTagMask, spu_make_preferred_u32(saved_tag_mask));
}

/* ===========================================================================
 * Channel count (rchcnt) -- how many entries can be read/written right now
 * ===========================================================================*/
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel)
{
    switch (channel) {
    case SPU_RdEventStat:
        /* With no enabled events pending, rchcnt returns zero, so rchcnt(0) is
         * (pending & mask) ? 1 : 0, matching the masking SPU_RdEventStat's
         * read applies (see spu_rdch above) and what the lifter's bisled
         * emission assumes. */
        /* Acquire-load: rchcnt is itself the busy-wait predicate for
         * `while (!rchcnt(ch));` idioms, so it needs the same synchronizes-with
         * guarantee as spu_ch_ready's identical test, not just eventual TSO
         * visibility. */
        return (atomic_load_explicit(&ctx->event_status, memory_order_acquire) & ctx->event_mask) ? 1u : 0u;
    case SPU_RdInMbox:       return atomic_load_explicit(&ctx->ch_in_mbox.count, memory_order_acquire); /* readable */
    case SPU_WrOutMbox:      return SPU_MBOX_DEPTH - ctx->ch_out_mbox.count; /* free slots (same-thread only) */
    case SPU_WrOutIntrMbox:  return SPU_INTR_MBOX_DEPTH - ctx->ch_out_intr_mbox.count; /* same-thread only */
    case SPU_RdSigNotify1:   return atomic_load_explicit(&ctx->ch_sig_notify[0].count, memory_order_acquire);
    case SPU_RdSigNotify2:   return atomic_load_explicit(&ctx->ch_sig_notify[1].count, memory_order_acquire);
    case MFC_Cmd: {
        mfc_engine* m = mfc_for(ctx);
        const u32 count = MFC_QUEUE_DEPTH - m->queue_count;
        mfc_release(m);
        return count;
    }
    case MFC_RdTagStat: {
        mfc_engine* m = mfc_for(ctx);
        const u32 count = m->tag_status_ready ? 1u : 0u;
        mfc_release(m);
        return count;
    }
    case MFC_WrTagUpdate:    return 1u;
    case MFC_RdAtomicStat: {
        mfc_engine* m = mfc_for(ctx);
        const u32 count = m->atomic_stat_ready ? 1u : 0u;
        mfc_release(m);
        return count;
    }
    case MFC_RdListStallStat:
        /* Nonzero (readable) iff at least one tag is currently stalled. The
         * count is collapsed to the architected 0/1 rchcnt result, matching
         * every other channel's rchcnt encoding in this function; an
         * un-stalled list reads 0 (the generic `default: return 1` would be
         * wrong for the common "nothing stalled" case). */
        {
            mfc_engine* m = mfc_for(ctx);
            const u32 count = m->stall_mask ? 1u : 0u;
            mfc_release(m);
            return count;
        }
    default:
        /* An unimplemented channel has zero capacity on the ISA, so rchcnt
         * reads 0; reporting 1 would falsely tell guest code the channel was
         * ready/drainable. Loud once-per-channel.
         *
         * Exception: the MFC-class channels without an explicit rchcnt case
         * here (WrTagMask=22, WrTagUpdate=23, WrListStallAck=26,
         * RdAtomicStat=27, ...) are implemented (routed through the MFC
         * engine in rdch/wrch); they merely lack a bespoke count. They keep
         * the ready(=1) reply rather than falsely reporting zero capacity for
         * a live channel. */
        if (channel_is_mfc(channel))
            return 1;
        {
            static uint8_t seen[128];
            if (channel < 128 && !seen[channel]) {
                seen[channel] = 1;
                fprintf(stderr, "[rchcnt] UNIMPLEMENTED ch=%u pc=0x%05X -> 0\n",
                        channel, ctx->pc & SPU_LS_MASK);
                fflush(stderr);
            }
        }
        return 0;
    }
}

/* ===========================================================================
 * Indirect-branch dispatch + function registry
 * ===========================================================================*/
typedef void (*spu_fn)(spu_context*);

typedef struct {
    uint32_t addr;
    spu_fn   fn;
    int      image_id;   /* which recompiled image this function belongs to */
} spu_reg_entry;

/* All EBOOT task images + kernel/service/policy and the observed fixed-base
 * job relocations exceed 524288 lifted functions. A registry that is too small
 * drops the later images, so keep a power-of-two tier of headroom for
 * additional relocations; this table is static process memory and stays
 * cheap. */
#define SPU_FN_REGISTRY_MAX 1048576
static spu_reg_entry s_registry[SPU_FN_REGISTRY_MAX];
static uint32_t s_registry_count = 0;

/* Image currently being registered. SPURS images (kernel/policy/job) overlap in
 * LS, so each registers under a distinct id via spu_begin_image() before calling
 * its (prefixed) spu_recomp_register(). Single-image callers leave it 0. */
static int s_reg_image = 0;
void spu_begin_image(int image_id) { s_reg_image = image_id; }

void spu_register_function(uint32_t addr, spu_fn fn)
{
    if (s_registry_count < SPU_FN_REGISTRY_MAX) {
        s_registry[s_registry_count].addr = addr;
        s_registry[s_registry_count].fn = fn;
        s_registry[s_registry_count].image_id = s_reg_image;
        s_registry_count++;
    } else {
        /* Never silent: an overflow would drop whole images (whichever
         * registers last). */
        static int warned = 0;
        if (!warned) { warned = 1;
            fprintf(stderr, "[SPU] FATAL: function registry FULL (%u) -- raise "
                    "SPU_FN_REGISTRY_MAX; refusing a partial registry\n",
                    s_registry_count);
            fflush(stderr); }
        abort();
    }
}

/* Lifted job binaries (the job module itself is image 13).  They are only
 * entered from the job module, never as a foreign owner of another image's
 * LS address. */
static int spu_image_is_job_binary(int image_id)
{
    return image_id == 14 || image_id == 15 ||
           (image_id >= 17 && image_id <= 41) ||
           (image_id >= 44 && image_id <= 48);
}

/* Shared exact/wildcard decision, applied identically after either the sorted
 * index binary search or the O(n) fallback scan below have found (a) an exact
 * (addr,image_id) hit -- returned immediately by the caller before this is
 * even reached -- or (b) the best wildcard candidate. Kept as one function so
 * the guard's behavior can't drift between the fast and fallback paths. */
static spu_fn spu_lookup_apply_job_guard(uint32_t addr, int image_id,
                                          spu_fn wildcard, int wildcard_img)
{
    /* Jobchain-family wildcard guard. A query from the module or any of its
     * lifted job images at a job-span address (>= 0x4880, past the module's
     * end; kernel-yield targets are all below 0xA00 and stay wildcard-served
     * by design) that misses its exact image would run another image's code
     * at the job site (e.g. a job loaded at LS 0x4C00 would run another
     * image's function at that address through the wildcard and never do its
     * real work).
     * Fail loud instead (NULL -> unknown-branch halt); the job binaries are
     * lifted at both observed slot bases so a legitimate dispatch always has
     * an exact match. */
    if ((image_id == 13 || spu_image_is_job_binary(image_id))
            && addr >= 0x4880u && wildcard) {
        static int wl = 0; if (wl < 32) { wl++;
            fprintf(stderr, "[job-wildcard] query img=%d addr=0x%05X exact-miss; wildcard img=%d "
                    "REFUSED (would run wrong image's code)\n",
                    image_id, addr, wildcard_img);
            fflush(stderr); }
        return NULL;
    }
    /* Kernel-context wildcard guard. Contexts tagged as the SPURS kernel
     * (image 16, adopted at lv2 group start) resolve only against the
     * kernel's own registrations: a wild branch from kernel code that fell to
     * the image-0 wildcard would execute another image's code and
     * mis-attribute the failure. A kernel-tagged query that misses exact is a
     * bug by definition. Fail loud. */
    if (image_id == 16 && wildcard) {
        static int kl = 0; if (kl < 32) { kl++;
            fprintf(stderr, "[kern-wildcard] KERNEL ctx query addr=0x%05X exact-miss; wildcard img=%d %s\n",
                    addr, wildcard_img,
                    "REFUSED (wild kernel branch, failing loud)");
            fflush(stderr); }
        return NULL;
    }
    return wildcard;
}

/* ===========================================================================
 * spu_lookup fast index. The registry holds O(10^5) entries and every SPU
 * computed branch (bi/bisl/brasl-return) queries it; with ~150k+ functions
 * registered, a plain forward scan would be the hottest O(n) cost in the
 * whole dispatch loop. Instead the lookup uses a copy of the registry sorted
 * by addr (ties broken by original registration order, so the exact/wildcard
 * tie-break below matches a forward scan), binary-searched to the first
 * entry with a matching addr, then scanned across just the (small, bounded
 * by how many images alias that LS address) run of same-addr entries:
 * O(log n + k) instead of O(n).
 *
 * Registration is append-only and, in this runtime, complete (all
 * spu_begin_image()/spu_recomp_register*() calls in main()) before any SPU
 * host thread starts calling spu_lookup (threads are spawned later, from
 * sys_spu_thread_group_start), so the index only needs to be built
 * once, with no reader racing the build. It's built lazily on the first
 * lookup after registration (rather than eagerly per spu_register_function
 * call) so the O(n log n) sort cost is paid once, not once per registration
 * in a ~150k-call registration burst.
 *
 * Thread safety: builds are serialized by a spinlock (same atomic_flag
 * pattern as the throw latch below); the built array is published through an
 * atomic pointer + count so a lookup on another thread either sees the
 * fully-built index or the prior (possibly NULL) one, never a partial write.
 * A rebuilt index is never freed (only ever superseded) -- registration is
 * rare (once at boot in practice) and a stale reader could still be scanning
 * it, so the bounded, one-time leak is the safe trade against a use-after-
 * free. If a build ever fails (OOM), spu_lookup transparently falls back to
 * the O(n) scan, so correctness never depends on the index. */
typedef struct {
    uint32_t addr;
    uint32_t order;    /* original registration index; preserves the linear
                        * scan's "first registered wins" tie-break */
    spu_fn   fn;
    int      image_id;
} spu_idx_entry;

static int spu_idx_cmp(const void* pa, const void* pb)
{
    const spu_idx_entry* a = (const spu_idx_entry*)pa;
    const spu_idx_entry* b = (const spu_idx_entry*)pb;
    if (a->addr != b->addr) return (a->addr < b->addr) ? -1 : 1;
    return (a->order < b->order) ? -1 : (a->order > b->order ? 1 : 0);
}

static atomic_flag              s_index_build_lock = ATOMIC_FLAG_INIT;
static _Atomic(spu_idx_entry*)  s_index = 0;         /* published sorted snapshot */
static _Atomic(uint32_t)        s_index_count = 0;   /* entries in that snapshot */
static uint32_t                 s_index_built_from = 0; /* lock-protected */

static void spu_lookup_rebuild_index(void)
{
    while (atomic_flag_test_and_set_explicit(&s_index_build_lock, memory_order_acquire))
        SPU_CPU_RELAX();
    uint32_t n = s_registry_count;
    if (n != s_index_built_from) {
        spu_idx_entry* idx = (spu_idx_entry*)malloc((n ? (size_t)n : 1) * sizeof(spu_idx_entry));
        if (idx) {
            for (uint32_t i = 0; i < n; i++) {
                idx[i].addr = s_registry[i].addr;
                idx[i].fn = s_registry[i].fn;
                idx[i].image_id = s_registry[i].image_id;
                idx[i].order = i;
            }
            qsort(idx, n, sizeof(spu_idx_entry), spu_idx_cmp);
            /* Publishing the count before the pointer isn't safe (a reader
             * could load the new pointer with the old count); publish the
             * pointer last with a release store so a reader that observes the
             * new pointer also observes fully-initialized entries and the
             * matching count (read together below, count first, pointer
             * second, matching this store order in reverse; see
             * spu_lookup). */
            atomic_store_explicit(&s_index_count, n, memory_order_relaxed);
            atomic_store_explicit(&s_index, idx, memory_order_release);
            s_index_built_from = n;
        }
        /* malloc failure: leave whatever is already published (possibly
         * NULL) in place; spu_lookup's fallback path covers this. */
    }
    atomic_flag_clear_explicit(&s_index_build_lock, memory_order_release);
}

/* serve_img (may be NULL): which image's registration actually served the
 * lookup: image_id itself on an exact hit, the wildcard owner's id on a
 * substituted serve. Feeds adopt-on-serve in spu_indirect_branch so the
 * context is tagged with the code it truly runs. */
static spu_fn spu_lookup(uint32_t addr, int image_id, int* serve_img)
{
    if (serve_img) *serve_img = image_id;
    spu_idx_entry* idx = atomic_load_explicit(&s_index, memory_order_acquire);
    uint32_t idx_n = atomic_load_explicit(&s_index_count, memory_order_acquire);
    if (!idx || idx_n != s_registry_count) {
        spu_lookup_rebuild_index();
        idx = atomic_load_explicit(&s_index, memory_order_acquire);
        idx_n = atomic_load_explicit(&s_index_count, memory_order_acquire);
    }
    if (idx && idx_n == s_registry_count) {
        /* Binary search: first index with addr >= target. */
        uint32_t lo = 0, hi = idx_n;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (idx[mid].addr < addr) lo = mid + 1; else hi = mid;
        }
        spu_fn wildcard = NULL;
        int wildcard_img = -1;
        for (uint32_t i = lo; i < idx_n && idx[i].addr == addr; i++) {
            if (idx[i].image_id == image_id) {
                return idx[i].fn; /* exact (incl 0==0) */
            }
            /* Kernel entries (16) are universal servers: every context may
             * branch into the resident scheduler (workload exits at 0x290/
             * 0x838). The kernel guard above still refuses img-0 substitution
             * for kernel contexts. */
            if ((idx[i].image_id == 0 || idx[i].image_id == 16 || image_id == 0) && !wildcard) {
                wildcard = idx[i].fn;
                wildcard_img = idx[i].image_id;
            }
        }
        if (serve_img && wildcard) *serve_img = wildcard_img;
        return spu_lookup_apply_job_guard(addr, image_id, wildcard, wildcard_img);
    }
    /* Fallback (index unavailable, e.g. OOM on the very first build): a
     * linear O(n) scan. Correctness never depends on the index existing. */
    spu_fn wildcard = NULL;
    int wildcard_img = -1;
    for (uint32_t i = 0; i < s_registry_count; i++) {
        if (s_registry[i].addr != addr) continue;
        if (s_registry[i].image_id == image_id) return s_registry[i].fn; /* exact (incl 0==0) */
        if ((s_registry[i].image_id == 0 || s_registry[i].image_id == 16 || image_id == 0) && !wildcard) {
            wildcard = s_registry[i].fn;
            wildcard_img = s_registry[i].image_id;
        }
    }
    if (serve_img && wildcard) *serve_img = wildcard_img;
    return spu_lookup_apply_job_guard(addr, image_id, wildcard, wildcard_img);
}

/* Does a lifted function exist at this LS address (any image)? Lets the
 * lv2 layer decide between real SPU execution and PPU fallbacks. */
int spu_have_function(uint32_t addr)
{
    return spu_lookup(addr, 0, NULL) != NULL;
}

/* ===========================================================================
 * throw_event loss latch.
 *
 * SPU WrOutIntrMbox codes 64-127 (throw_event) are fire-and-forget by
 * protocol: no ack channel exists, so a full destination lv2 queue loses the
 * event with no guest-visible signal. Real HW tolerates this because its
 * consumers keep pace; our host timing widens the full-queue window. Failed
 * throws are latched here and redelivered from the vblank tick once the
 * queue has drained; the throw happened, only delivery timing shifts.
 * Small fixed ring; overflow drops the event. Writers = SPU threads,
 * flusher = the vblank thread; serialized by a spinlock. */
#define YZ_THROW_LAT_MAX 16
typedef struct { uint32_t qid; uint64_t src, d1, d2, d3; int used; } yz_throw_lat;
static yz_throw_lat g_throw_lat[YZ_THROW_LAT_MAX];
static atomic_flag  g_throw_lat_lock = ATOMIC_FLAG_INIT;

int yz_throw_latch_add(uint32_t qid, uint64_t src,
                       uint64_t d1, uint64_t d2, uint64_t d3)
{
    int ok = 0;
    while (atomic_flag_test_and_set_explicit(&g_throw_lat_lock, memory_order_acquire))
        SPU_CPU_RELAX();
    for (int i = 0; i < YZ_THROW_LAT_MAX; i++)
        if (!g_throw_lat[i].used) {
            g_throw_lat[i].qid = qid; g_throw_lat[i].src = src;
            g_throw_lat[i].d1 = d1; g_throw_lat[i].d2 = d2; g_throw_lat[i].d3 = d3;
            g_throw_lat[i].used = 1; ok = 1; break;
        }
    atomic_flag_clear_explicit(&g_throw_lat_lock, memory_order_release);
    return ok;
}

/* Called from the vblank tick (~16 ms cadence). Retries every latched throw
 * in order; entries that still fail stay latched. */
void yz_throw_retry_flush(void)
{
    extern int sys_event_queue_push_by_id(uint32_t queue_id, uint64_t source,
                                          uint64_t d1, uint64_t d2, uint64_t d3);
    while (atomic_flag_test_and_set_explicit(&g_throw_lat_lock, memory_order_acquire))
        SPU_CPU_RELAX();
    for (int i = 0; i < YZ_THROW_LAT_MAX; i++) {
        if (!g_throw_lat[i].used) continue;
        int rc = sys_event_queue_push_by_id(g_throw_lat[i].qid, g_throw_lat[i].src,
                                            g_throw_lat[i].d1, g_throw_lat[i].d2,
                                            g_throw_lat[i].d3);
        if (rc == 0)
            g_throw_lat[i].used = 0;
    }
    atomic_flag_clear_explicit(&g_throw_lat_lock, memory_order_release);
}

/* Restore ctx->image_id after a nested call. The lifted brsl/bisl emissions
 * bracket every nested host call with
 *     { int32_t _si = ctx->image_id;  <call + SPU_DRAIN + depth-->;
 *       spu_img_restore(ctx, _si); }
 * so any image adopted inside the call (dispatcher serve, foreign-resident
 * adoption, drain tail-chain hops) is scoped to the call and cannot leak
 * into the caller's continuation. longjmp unwinds (restack/halt) discard the
 * bracket locals with their frames; those paths set image_id explicitly. */
void spu_img_restore(spu_context* ctx, int32_t saved_img)
{
    ctx->image_id = saved_img;
}


void spu_indirect_branch(spu_context* ctx)
{
    /* Native SPURS task syscall entry. Contexts without the callback go
     * through the ordinary dispatcher. */
    if ((ctx->pc & SPU_LS_MASK) == 0x0A70u && ctx->native_spurs_syscall) {
        int native_result = ctx->native_spurs_syscall(
            ctx, ctx->native_spurs_opaque);
        if (native_result > 0) {
            ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
            /* Match an SPU `bi $lr` return.  When the lifted caller's host
             * frame is still live, its call bracket owns the continuation
             * and will publish it after the nested drain unwinds.  Dispatching
             * r0 here as well executes the callee epilogue twice (and, for a
             * blocking task syscall, leaks one guest stack frame per wake).
             * A top-level/restacked resume has no such host caller, so only
             * that case must re-enter through the indirect dispatcher. */
            g_spu_trampoline_fn =
                spu_return_needs_dispatch(ctx) ? spu_indirect_branch : 0;
        } else if (native_result < 0) {
            spu_halt(ctx, SPU_STATUS_STOPPED_BY_STOP);
        } else {
            spu_halt(ctx, SPU_STATUS_STOPPED_BY_HALT);
        }
        return;
    }

    /* Host-call-depth guard: a never-returning SPU coroutine/poll
     * yield leaks a host frame per iteration because
     * the lifter models SPU `brsl`/`bisl` as nested host calls. Once the host
     * stack passes the threshold, unwind to the driver and re-dispatch from
     * ctx->pc on a fresh stack (SPU state is all in heap `ctx`). Stack grows
     * down, so base - sp = depth; 32MB of the 64MB SPU thread stack. Legitimate
     * SPU recursion is bounded by the 256KB LS, far below this. */
    if (g_spu_restart_jmp && g_spu_stack_base) {
        char probe;
        if ((size_t)(g_spu_stack_base - &probe) > (size_t)(32u * 1024u * 1024u)) {
            g_spu_trampoline_fn = 0;
            longjmp(*g_spu_restart_jmp, 1);
        }
    }

    /* Keep the per-thread context pointer live for crash attribution. */
    g_spu_cur_ctx = ctx;

    int serve_img = ctx->image_id;
    spu_fn fn = spu_lookup(ctx->pc, ctx->image_id, &serve_img);

    if (fn) {
        /* Adopt-on-serve: tag the context with the image whose registration
         * actually serves this dispatch (kernel universal serves, image-0
         * wildcards), so attribution is true while the foreign code runs; a
         * wild branch inside kernel-served code reports image 16, not
         * whatever the context last wore. Safe because
         * the caller's bracket (spu_img_restore) restores its image on the
         * host return. */
        if (serve_img != ctx->image_id)
            ctx->image_id = serve_img;
        fn(ctx);
        return;
    }
    /* Reverse image switch (task -> resident SPURS runtime). A running SPURS
     * task calls back into the resident taskset policy/runtime, which lives at
     * low LS under a different image. The scoped lookup then misses a
     * perfectly-lifted function and we would halt on a legitimate cross-image
     * call.
     * The SPU LS is one shared address space; if exactly one other lifted
     * image owns this LS address, dispatch it and adopt its image_id (the
     * forward policy->task switch restores the task image when the runtime
     * branches back to LS>=0x3000). Genuine overlap (>1 foreign owner, the
     * kernel/service/policy 0xA00+ aliasing) stays ambiguous -> falls through
     * to the halt; a truly missing/garbage target has 0 owners -> also halts,
     * so real dispatch bugs are not masked. */
    {
        uint32_t la = ctx->pc & SPU_LS_MASK;
        int foreign = 0, foreign_img = 0; spu_fn ffn = 0;
        /* Job binaries are only ever entered from the job module: exclude
         * them as foreign owners for every other context so their addresses
         * can't push an exactly-one adoption into ambiguity. */
        const int from_jobworld = ctx->image_id == 13 ||
                                  spu_image_is_job_binary(ctx->image_id);
        for (uint32_t i = 0; i < s_registry_count; i++)
            if (s_registry[i].addr == la && s_registry[i].image_id != ctx->image_id) {
                if (!from_jobworld &&
                    spu_image_is_job_binary(s_registry[i].image_id))
                    continue;
                foreign++; foreign_img = s_registry[i].image_id; ffn = s_registry[i].fn;
            }
        /* Kernel-context refusal (keeps the kernel guard from being bypassed):
         * a kernel-tagged context (image 16) whose branch missed every kernel
         * registration must not silently adopt another image's code here.
         * A kernel query landing here is a wild branch by definition; fall
         * through to the loud halt. */
        if (foreign == 1 && ffn && ctx->image_id == 16) {
            static int kf = 0; if (kf < 16) { kf++;
                fprintf(stderr, "[spu-ximg] KERNEL ctx branch LS 0x%05X owned only by image %d "
                        "REFUSED (wild kernel branch, failing loud)\n", la, foreign_img);
                fflush(stderr); }
            foreign = 0; ffn = 0;
        }
        if (foreign == 1 && ffn) {
            ctx->image_id = foreign_img;
            ffn(ctx);
            return;
        }
    }
    {
        static int unknown_logged = 0;
        if (unknown_logged < 6) {
            ++unknown_logged;
            fprintf(stderr, "[SPU] unknown branch pc=0x%08X LS 0x%05X image=%d spu=%X lr=0x%05X\n",
                    ctx->pc, ctx->pc & SPU_LS_MASK, ctx->image_id, ctx->spu_id,
                    ctx->gpr[0]._u32[0] & SPU_LS_MASK);
            fflush(stderr);
        }
    }
    spu_halt(ctx, SPU_STATUS_STOPPED_BY_HALT);   /* unwinds; does not return */
}

#ifdef __cplusplus
}
#endif
