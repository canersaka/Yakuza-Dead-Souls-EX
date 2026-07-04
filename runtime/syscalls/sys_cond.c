/*
 * ps3recomp - Condition variable syscalls (implementation)
 */

#include "sys_cond.h"
#include "../memory/vm.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Cond-layer trace (env YZ_COND_TRACE, 2026-07-02 diag — the CRI boot-stall
 * hunt): logs waits/signals on low-id conds (the CRI dialog/vsync/mixer set)
 * with the guest tid + the associated mutex owner at call time, and flags any
 * signal that BLOCKS acquiring the mutex CS. Real lv2 sys_cond_signal never
 * blocks on the mutex, so every BLOCKED-ON line names a hold-and-wait pair
 * our signal-under-CS emulation introduced. REMOVE when the stall closes. */
static int cond_trace_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("YZ_COND_TRACE"); on = (e && *e) ? 1 : 0; }
    return on;
}
extern uint32_t yz_thread_current_id(void);

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_cond_info g_sys_conds[SYS_COND_MAX];

#ifdef _WIN32
static CRITICAL_SECTION s_cond_table_lock;
static int              s_cond_table_lock_init = 0;
#else
static pthread_mutex_t  s_cond_table_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void cond_table_lock(void)
{
#ifdef _WIN32
    if (!s_cond_table_lock_init) {
        InitializeCriticalSection(&s_cond_table_lock);
        s_cond_table_lock_init = 1;
    }
    EnterCriticalSection(&s_cond_table_lock);
#else
    pthread_mutex_lock(&s_cond_table_lock);
#endif
}

static void cond_table_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_cond_table_lock);
#else
    pthread_mutex_unlock(&s_cond_table_lock);
#endif
}

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

/* ---------------------------------------------------------------------------
 * sys_cond_create
 *
 * r3 = pointer to receive cond ID (u32*)
 * r4 = mutex_id to associate with
 * r5 = pointer to attribute struct
 * -----------------------------------------------------------------------*/
int64_t sys_cond_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t mutex_id    = LV2_ARG_U32(ctx, 1);
    uint32_t attr_addr   = LV2_ARG_PTR(ctx, 2);

    /* Validate the associated mutex */
    if (mutex_id == 0 || mutex_id > SYS_MUTEX_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;
    if (!g_sys_mutexes[mutex_id - 1].active)
        return (int64_t)(int32_t)CELL_ESRCH;

    cond_table_lock();

    int slot = -1;
    for (int i = 0; i < SYS_COND_MAX; i++) {
        if (!g_sys_conds[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        cond_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_cond_info* c = &g_sys_conds[slot];
#ifdef _WIN32
    /* preserve the internal sig lock across slot recycling (a CS must never
     * be re-Initialized while potentially referenced; churn-safety per the
     * lwcond slots) */
    int sig_was_init = c->sig_cs_init;
    CRITICAL_SECTION sig_saved = c->sig_cs;
#else
    int sig_was_init = c->sig_mtx_init;
    pthread_mutex_t sig_saved = c->sig_mtx;
#endif
    memset(c, 0, sizeof(*c));
#ifdef _WIN32
    if (sig_was_init) { c->sig_cs = sig_saved; c->sig_cs_init = 1; }
    else { InitializeCriticalSection(&c->sig_cs); c->sig_cs_init = 1; }
#else
    if (sig_was_init) { c->sig_mtx = sig_saved; c->sig_mtx_init = 1; }
    else { pthread_mutex_init(&c->sig_mtx, NULL); c->sig_mtx_init = 1; }
#endif
    c->active   = 1;
    c->mutex_id = mutex_id;

    /* Read name from attribute if provided */
    if (attr_addr != 0) {
        uint8_t* attr_raw = (uint8_t*)vm_to_host(attr_addr);
        /* name is typically at offset 8 in the cond attr struct */
        memcpy(c->name, attr_raw + 8, 8);
    }

#ifdef _WIN32
    InitializeConditionVariable(&c->cv);
#else
    pthread_cond_init(&c->cv, NULL);
#endif

    uint32_t cond_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, cond_id);
    }

    cond_table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_cond_destroy
 *
 * r3 = cond_id
 * -----------------------------------------------------------------------*/
int64_t sys_cond_destroy(ppu_context* ctx)
{
    uint32_t cond_id = LV2_ARG_U32(ctx, 0);

    if (cond_id == 0 || cond_id > SYS_COND_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    cond_table_lock();

    sys_cond_info* c = &g_sys_conds[cond_id - 1];
    if (!c->active) {
        cond_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

#ifndef _WIN32
    pthread_cond_destroy(&c->cv);
#endif
    /* Windows CONDITION_VARIABLE doesn't need destruction */

    c->active = 0;
    cond_table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_cond_wait
 *
 * r3 = cond_id
 * r4 = timeout_usec (0 = infinite)
 * -----------------------------------------------------------------------*/
int64_t sys_cond_wait(ppu_context* ctx)
{
    uint32_t cond_id    = LV2_ARG_U32(ctx, 0);
    uint64_t timeout_us = LV2_ARG_U64(ctx, 1);

    if (cond_id == 0 || cond_id > SYS_COND_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_cond_info* c = &g_sys_conds[cond_id - 1];
    if (!c->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    uint32_t mutex_id = c->mutex_id;
    if (mutex_id == 0 || mutex_id > SYS_MUTEX_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mutex_info* m = &g_sys_mutexes[mutex_id - 1];
    if (!m->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* Audit item 1c (2026-07-03, user-confirmed; RPCS3 sys_cond.cpp:416-419,
     * 479-481): the caller must currently own the associated mutex. lv2
     * returns CELL_EPERM otherwise instead of touching the mutex/CS state --
     * a non-owner caller releasing/re-acquiring a CS it never entered is UB
     * on Win32 and corrupts whatever the real owner had saved. */
    uint64_t caller_tid = ctx->thread_id;
    if (m->owner_tid != caller_tid) {
        return (int64_t)(int32_t)CELL_EPERM;
    }

    /* The caller must hold the associated mutex. We need to release it
     * atomically with the wait and re-acquire it on wake. */

    int ct = cond_trace_on() && cond_id <= 16;
    if (ct) {
        static long n = 0;
        if (n < 4000) { n++;
            fprintf(stderr, "[cond] t%u WAIT-enter cond=%u mutex=%u owner=t%u cnt=%d\n",
                    yz_thread_current_id(), cond_id, mutex_id,
                    (uint32_t)m->owner_tid, m->lock_count);
            fflush(stderr); }
    }

    /* Save and fully clear ownership info. Audit item 1c (RPCS3
     * sys_cond.cpp:448 lock_count.exchange(0), :587 lock_count.release()):
     * a recursive mutex held N times must release ALL N host-CS entries
     * before parking, not just one -- SleepConditionVariableCS only leaves
     * the CS ONCE, so a recursive holder (lock_count > 1) previously left
     * the CS held N-1 times after "releasing" it, permanently blocking every
     * other thread's EnterCriticalSection. Loop the Leave to match every
     * guest-side Enter recorded by sys_mutex_lock/trylock (sys_mutex.c). */
    uint64_t saved_owner = m->owner_tid;
    int saved_count = m->lock_count;
    m->owner_tid = 0;
    m->lock_count = 0;

#ifdef _WIN32
    DWORD ms = (timeout_us == 0) ? INFINITE : (DWORD)(timeout_us / 1000);
    if (ms == 0 && timeout_us > 0) ms = 1;

    /* RENDEZVOUS (2026-07-03 s8, see sys_cond.h): COMMIT under the internal
     * sig lock BEFORE releasing the guest mutex — from this instant a signal
     * lands in `pending` and cannot be lost, matching lv2's wait-entry
     * enqueue. Then fully release the guest mutex (ALL N recursion levels),
     * park on the CV against the sig lock, consume one pending on wake, and
     * re-acquire the guest mutex. */
    EnterCriticalSection(&c->sig_cs);
    c->committed++;

    for (int i = 0; i < saved_count; i++) {
        LeaveCriticalSection(&m->cs);
    }

    BOOL ok = TRUE;
    ULONGLONG t0 = GetTickCount64();
    while (c->pending == 0) {
        DWORD slice = ms;
        if (ms != INFINITE) {
            ULONGLONG spent = GetTickCount64() - t0;
            if (spent >= ms) { ok = FALSE; break; }
            slice = (DWORD)(ms - spent);
        }
        if (!SleepConditionVariableCS(&c->cv, &c->sig_cs, slice)) {
            if (GetLastError() == ERROR_TIMEOUT && c->pending == 0) { ok = FALSE; break; }
        }
    }
    if (ok)
        c->pending--;
    c->committed--;
    LeaveCriticalSection(&c->sig_cs);

    /* Re-acquire ALL N recursion levels of the guest mutex. */
    for (int i = 0; i < saved_count; i++) {
        EnterCriticalSection(&m->cs);
    }

    /* Restore ownership */
    m->owner_tid = saved_owner;
    m->lock_count = saved_count;

    if (ct) {
        static long n2 = 0;
        if (n2 < 4000) { n2++;
            fprintf(stderr, "[cond] t%u WAIT-exit cond=%u %s\n",
                    yz_thread_current_id(), cond_id,
                    !ok ? "TIMEOUT" : "ok");
            fflush(stderr); }
    }

    if (!ok) {
        return (int64_t)(int32_t)CELL_ETIMEDOUT;
    }
#else
    /* RENDEZVOUS (see Win32 path): commit under the sig lock before the full
     * guest-mutex release; park against the sig lock; consume one pending. */
    pthread_mutex_lock(&c->sig_mtx);
    c->committed++;

    for (int i = 0; i < saved_count; i++) {
        pthread_mutex_unlock(&m->mtx);
    }

    int timed_out = 0;
    if (timeout_us == 0) {
        while (c->pending == 0)
            pthread_cond_wait(&c->cv, &c->sig_mtx);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeout_us / 1000000);
        ts.tv_nsec += (long)((timeout_us % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (c->pending == 0) {
            int rc = pthread_cond_timedwait(&c->cv, &c->sig_mtx, &ts);
            if (rc == ETIMEDOUT && c->pending == 0) { timed_out = 1; break; }
        }
    }
    if (!timed_out)
        c->pending--;
    c->committed--;
    pthread_mutex_unlock(&c->sig_mtx);

    for (int i = 0; i < saved_count; i++) {
        pthread_mutex_lock(&m->mtx);
    }

    /* Restore ownership */
    m->owner_tid = saved_owner;
    m->lock_count = saved_count;

    if (timed_out)
        return (int64_t)(int32_t)CELL_ETIMEDOUT;
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_cond_signal
 *
 * r3 = cond_id
 * -----------------------------------------------------------------------*/
int64_t sys_cond_signal(ppu_context* ctx)
{
    uint32_t cond_id = LV2_ARG_U32(ctx, 0);

    if (cond_id == 0 || cond_id > SYS_COND_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_cond_info* c = &g_sys_conds[cond_id - 1];
    if (!c->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* FAITHFUL lv2 semantics, revision 2 (2026-07-03 s8): real sys_cond_signal
     * NEVER acquires the guest mutex (the 3f1377c fix stands — the former
     * mutex-CS guard caused hold-and-wait). BUT "signal between a waiter's
     * release and park is permitted loss" was WRONG for one window: lv2
     * enqueues the waiter at WAIT-SYSCALL ENTRY, so a signal after that entry
     * is always delivered. The internal sig lock + committed/pending counters
     * reproduce that (held for nanoseconds — no guest-visible hold-and-wait);
     * a signal with no committed waiter is still a faithful discard.
     * Measured root: the CRI staging pump's completion wake vanished in this
     * window = the post-asset-scan boot freeze. */
#ifdef _WIN32
    EnterCriticalSection(&c->sig_cs);
    if (c->committed > c->pending) {
        c->pending++;
        WakeConditionVariable(&c->cv);
    }
    LeaveCriticalSection(&c->sig_cs);
#else
    pthread_mutex_lock(&c->sig_mtx);
    if (c->committed > c->pending) {
        c->pending++;
        pthread_cond_signal(&c->cv);
    }
    pthread_mutex_unlock(&c->sig_mtx);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_cond_signal_all
 *
 * r3 = cond_id
 * -----------------------------------------------------------------------*/
int64_t sys_cond_signal_all(ppu_context* ctx)
{
    uint32_t cond_id = LV2_ARG_U32(ctx, 0);

    if (cond_id == 0 || cond_id > SYS_COND_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_cond_info* c = &g_sys_conds[cond_id - 1];
    if (!c->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* FAITHFUL lv2 semantics, revision 2 (see sys_cond_signal): rendezvous
     * broadcast — serve every committed waiter; never touch the guest mutex. */
#ifdef _WIN32
    EnterCriticalSection(&c->sig_cs);
    c->pending = c->committed;
    WakeAllConditionVariable(&c->cv);
    LeaveCriticalSection(&c->sig_cs);
#else
    pthread_mutex_lock(&c->sig_mtx);
    c->pending = c->committed;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->sig_mtx);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_cond_init(lv2_syscall_table* tbl)
{
    memset(g_sys_conds, 0, sizeof(g_sys_conds));

#ifdef _WIN32
    if (!s_cond_table_lock_init) {
        InitializeCriticalSection(&s_cond_table_lock);
        s_cond_table_lock_init = 1;
    }
#endif

    lv2_syscall_register(tbl, SYS_COND_CREATE,     sys_cond_create);
    lv2_syscall_register(tbl, SYS_COND_DESTROY,     sys_cond_destroy);
    lv2_syscall_register(tbl, SYS_COND_WAIT,        sys_cond_wait);
    lv2_syscall_register(tbl, SYS_COND_SIGNAL,      sys_cond_signal);
    lv2_syscall_register(tbl, SYS_COND_SIGNAL_ALL,  sys_cond_signal_all);
}
