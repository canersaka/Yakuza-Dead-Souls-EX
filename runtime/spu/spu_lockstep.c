/*
 * ps3recomp - YZ_SPU_LOCKSTEP implementation (s42 diagnostic)
 *
 * See spu_lockstep.h for the design summary and the (star)REVIEW DELTAS this
 * implements; scratch/s42_lockstep_design.md (v2) and
 * scratch/s42_lockstep_review.md for the full rationale.
 *
 * Mechanism: one global run token (spu_context* s_holder) shared by every
 * registered SPU host thread, protected by a single mutex+condvar. Members
 * live in a fixed-capacity ring (registration order; tombstoned, never
 * compacted, on unregister -- indices stay stable while any thread might be
 * mid-wait). At quantum expiry (yz_lockstep_tick) the current holder hands
 * the token to the next RUNNABLE member (skipping members mid-block in
 * spu_ch_wait) and blocks until it is granted the token again -- a
 * cooperative round robin, not a scheduler. If no other member is runnable,
 * the holder just re-grants itself (no lock contention beyond the check).
 *
 * Decrementer freeze (review change C): every acquire advances
 * ctx->dec_start_tb by the wall-clock span since this ctx's last release (or
 * registration, if never held before) -- see ls_acquire_locked. This alone
 * implements "the decrementer only counts wall time while its SPU holds the
 * token": the existing RdDec formula in spu_channels.c
 * (`ctx->dec_value - (ppu_timebase_now() - ctx->dec_start_tb)`) needs no
 * changes at all, and stays bit-identical when this module is unarmed
 * (dec_start_tb is written here ONLY while YZ_SPU_LOCKSTEP is armed).
 */
#include "spu_lockstep.h"
#include "spu_context.h"   /* declares ppu_timebase_now(); full spu_context here */

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <intrin.h>
#  define LS_RELAX() _mm_pause()
static SRWLOCK            s_ls_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_ls_cv   = CONDITION_VARIABLE_INIT;
#  define LS_LOCK()       AcquireSRWLockExclusive(&s_ls_lock)
#  define LS_UNLOCK()      ReleaseSRWLockExclusive(&s_ls_lock)
#  define LS_WAIT()        SleepConditionVariableSRW(&s_ls_cv, &s_ls_lock, INFINITE, 0)
#  define LS_BROADCAST()   WakeAllConditionVariable(&s_ls_cv)
#else
#  include <pthread.h>
#  define LS_RELAX() ((void)0)
static pthread_mutex_t s_ls_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_ls_cv   = PTHREAD_COND_INITIALIZER;
#  define LS_LOCK()       pthread_mutex_lock(&s_ls_lock)
#  define LS_UNLOCK()     pthread_mutex_unlock(&s_ls_lock)
#  define LS_WAIT()       pthread_cond_wait(&s_ls_cv, &s_ls_lock)
#  define LS_BROADCAST()  pthread_cond_broadcast(&s_ls_cv)
#endif

volatile int g_yz_lockstep_on = -1;

#define YZ_LOCKSTEP_MAX_SPUS         32
#define YZ_LOCKSTEP_DEFAULT_QUANTUM  65536ull
#define YZ_LOCKSTEP_STARVE_TICKS     10          /* max starvation-watchdog prints */

/* Guest timebase frequency (CBEA v1.02 Section 9.7; PS3 79.8 MHz), duplicated
 * from runtime/syscalls/sys_timer.h's PS3_TIMEBASE_FREQ rather than including
 * that header here (it pulls in the PPU syscall-table/context headers this
 * SPU-only translation unit has no other reason to need). Used only for the
 * >5s-wall starvation-watchdog threshold below. */
#define YZ_LOCKSTEP_TIMEBASE_FREQ    79800000ull

typedef struct {
    spu_context* ctx;
    int active;     /* 0 = free/tombstoned slot */
    int runnable;   /* 0 = mid spu_ch_wait (not eligible to receive the token) */
} yz_ls_slot;

static yz_ls_slot   s_ring[YZ_LOCKSTEP_MAX_SPUS];
static int          s_ring_count = 0;      /* high-water mark of used slots */
static spu_context*  s_holder = 0;          /* current token holder, 0 = none */
static uint64_t      s_holder_acquired_tb = 0;
static uint64_t      s_quantum = YZ_LOCKSTEP_DEFAULT_QUANTUM;
static unsigned long long s_pass_count = 0;
static int           s_watchdog_prints = 0;

static atomic_flag  s_init_claimed = ATOMIC_FLAG_INIT;
static volatile int s_init_complete = 0;

static void ls_arm_now(void)
{
    const char* e = getenv("YZ_SPU_LOCKSTEP");
    int on = (e && *e && *e != '0') ? 1 : 0;
    if (on) {
        const char* qs = getenv("YZ_LOCKSTEP_QUANTUM");
        uint64_t q = (qs && *qs) ? strtoull(qs, NULL, 10) : YZ_LOCKSTEP_DEFAULT_QUANTUM;
        if (q < 1) q = 1;
        s_quantum = q;
        fprintf(stderr,
                "[lockstep] ARMED quantum=%llu (YZ_SPU_LOCKSTEP=1; unset to disable) -- "
                "diagnostic/interventional, default OFF, see scratch/s42_lockstep_design.md\n",
                (unsigned long long)q);
        fflush(stderr);
    }
    g_yz_lockstep_on = on;
}

int yz_lockstep_enabled(void)
{
    if (s_init_complete) return g_yz_lockstep_on;
    if (!atomic_flag_test_and_set_explicit(&s_init_claimed, memory_order_acq_rel)) {
        ls_arm_now();
        s_init_complete = 1;
    } else {
        while (!s_init_complete) LS_RELAX();
    }
    return g_yz_lockstep_on;
}

static int ls_hot(void)
{
    if (g_yz_lockstep_on == 0) return 0;
    if (g_yz_lockstep_on < 0) return yz_lockstep_enabled();
    return 1;
}

/* ---------------------------------------------------------------------------
 * Ring helpers. All callers hold s_ls_lock.
 * -------------------------------------------------------------------------*/
static int ls_find_slot(spu_context* ctx)
{
    int i;
    for (i = 0; i < s_ring_count; i++)
        if (s_ring[i].active && s_ring[i].ctx == ctx) return i;
    return -1;
}

/* Next active+runnable slot strictly after `after` (wrapping), excluding
 * `after` itself. Returns NULL if no other member qualifies. */
static spu_context* ls_next_runnable_after(int after)
{
    int step;
    if (s_ring_count <= 0 || after < 0) return 0;
    for (step = 1; step < s_ring_count; step++) {
        int i = (after + step) % s_ring_count;
        if (s_ring[i].active && s_ring[i].runnable) return s_ring[i].ctx;
    }
    return 0;
}

/* Is any OTHER slot (any state, not just runnable) still active? s_ring_count
 * is a high-water mark that never shrinks on unregister (indices must stay
 * stable while a thread might be mid-wait), so it alone would false-positive
 * the starvation watchdog once a sibling exits late in the boot -- this scans
 * live occupancy instead. */
static int ls_any_other_active(int after)
{
    int i;
    for (i = 0; i < s_ring_count; i++)
        if (i != after && s_ring[i].active) return 1;
    return 0;
}

/* Advances ctx->dec_start_tb by the wall-clock span since ctx's last release
 * (review change C), then makes ctx the holder. Caller holds s_ls_lock. */
static void ls_acquire_locked(spu_context* ctx)
{
    uint64_t now = ppu_timebase_now();
    uint64_t paused = now - ctx->lockstep_release_tb;
    ctx->dec_start_tb += paused;
    s_holder = ctx;
    s_holder_acquired_tb = now;
}

/* Runs the starvation watchdog check, marks ctx's slot per `keep_runnable`,
 * records its release timestamp, and -- if ctx is the current holder --
 * hands the token to the next runnable member (or parks it, s_holder=0, if
 * none exists). Returns 1 if a handoff to ANOTHER member happened (caller
 * must then wait its turn), 0 otherwise. Caller holds s_ls_lock. */
static int ls_release_locked(spu_context* ctx, int keep_runnable)
{
    int idx = ls_find_slot(ctx);
    uint64_t now = ppu_timebase_now();

    if (s_holder == ctx && idx >= 0 && ls_any_other_active(idx)) {
        uint64_t held = now - s_holder_acquired_tb;
        if (held > (YZ_LOCKSTEP_TIMEBASE_FREQ * 5ull) && s_watchdog_prints < YZ_LOCKSTEP_STARVE_TICKS) {
            s_watchdog_prints++;
            fprintf(stderr,
                    "[lockstep] STARVATION token held >5s wall by spu=%X pc=0x%05X "
                    "(pass #%llu, %d/%d prints)\n",
                    ctx->spu_id, ctx->pc & SPU_LS_MASK, (unsigned long long)s_pass_count,
                    s_watchdog_prints, YZ_LOCKSTEP_STARVE_TICKS);
            fflush(stderr);
        }
    }

    if (idx >= 0) s_ring[idx].runnable = keep_runnable;
    ctx->lockstep_release_tb = now;

    if (s_holder != ctx) return 0;   /* defensive: wasn't holder, nothing to hand off */

    {
        spu_context* next = (idx >= 0) ? ls_next_runnable_after(idx) : 0;
        s_holder = next;   /* may be 0: nobody else runnable right now */
        if (next) {
            s_pass_count++;
            if ((s_pass_count % 100000ull) == 0ull) {
                fprintf(stderr, "[lockstep] heartbeat pass #%llu next-holder spu=%X pc=0x%05X\n",
                        (unsigned long long)s_pass_count, next->spu_id, next->pc & SPU_LS_MASK);
                fflush(stderr);
            }
            LS_BROADCAST();
            return 1;
        }
        return 0;
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/
void yz_lockstep_register(spu_context* ctx)
{
    int idx, i;
    if (!yz_lockstep_enabled()) return;   /* also arms + prints the banner */

    LS_LOCK();
    ctx->lockstep_quantum_ctr = 0;
    ctx->lockstep_release_tb = ppu_timebase_now();

    idx = -1;
    for (i = 0; i < s_ring_count; i++) if (!s_ring[i].active) { idx = i; break; }
    if (idx < 0) {
        if (s_ring_count >= YZ_LOCKSTEP_MAX_SPUS) {
            static int wn = 0; if (wn < 4) { wn++;
                fprintf(stderr, "[lockstep] WARNING ring full (%d slots) -- spu=%X runs "
                        "UNGATED (not registered)\n", YZ_LOCKSTEP_MAX_SPUS, ctx->spu_id);
                fflush(stderr); }
            LS_UNLOCK();
            return;   /* fail-open: never block boot on the diagnostic's capacity */
        }
        idx = s_ring_count++;
    }
    s_ring[idx].ctx = ctx; s_ring[idx].active = 1; s_ring[idx].runnable = 1;

    if (s_holder == 0) {
        ls_acquire_locked(ctx);
    } else if (s_holder != ctx) {
        while (s_holder != ctx) LS_WAIT();
        ls_acquire_locked(ctx);
    }
    LS_UNLOCK();
}

void yz_lockstep_unregister(spu_context* ctx)
{
    if (g_yz_lockstep_on != 1) return;
    LS_LOCK();
    {
        int idx = ls_find_slot(ctx);
        if (idx >= 0) {
            if (s_holder == ctx) {
                spu_context* next = ls_next_runnable_after(idx);
                s_ring[idx].active = 0;
                s_holder = next;
                if (next) { s_holder_acquired_tb = ppu_timebase_now(); LS_BROADCAST(); }
            } else {
                s_ring[idx].active = 0;
            }
        }
    }
    LS_UNLOCK();
}

void yz_lockstep_tick(spu_context* ctx)
{
    if (!ls_hot()) return;
    if (++ctx->lockstep_quantum_ctr < s_quantum) return;
    ctx->lockstep_quantum_ctr = 0;

    LS_LOCK();
    if (ls_release_locked(ctx, /*keep_runnable=*/1)) {
        while (s_holder != ctx) LS_WAIT();
    }
    ls_acquire_locked(ctx);   /* re-grants to self with ~0 delta if nothing handed off */
    LS_UNLOCK();
}

void yz_lockstep_block_begin(spu_context* ctx)
{
    if (!ls_hot()) return;
    LS_LOCK();
    ls_release_locked(ctx, /*keep_runnable=*/0);
    LS_UNLOCK();
}

void yz_lockstep_block_end(spu_context* ctx)
{
    if (!ls_hot()) return;
    LS_LOCK();
    { int idx = ls_find_slot(ctx); if (idx >= 0) s_ring[idx].runnable = 1; }
    if (s_holder != 0 && s_holder != ctx) {
        while (s_holder != ctx) LS_WAIT();
    }
    ls_acquire_locked(ctx);
    LS_UNLOCK();
}
