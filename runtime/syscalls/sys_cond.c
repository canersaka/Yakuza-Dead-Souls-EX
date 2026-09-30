/*
 * ps3recomp - Condition variable syscalls (implementation)
 */

#include "sys_cond.h"
#include "sys_timer.h"   /* lv2_usec_deadline: sub-ms timed waits */
#include "../memory/vm.h"
#include "../ppu/ppu_memory.h"
#include <string.h>


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

/* Committed-waiter tid registry (see sys_cond.h). All three
 * helpers require the cond's sig lock to be held by the caller. */
static void cond_waiter_add(sys_cond_info* c, uint64_t tid)
{
    for (int i = 0; i < SYS_COND_WAITER_TIDS_MAX; i++) {
        if (c->waiter_tids[i] == 0) { c->waiter_tids[i] = tid; return; }
    }
    /* Table full: waiter stays untracked (bounded, documented in sys_cond.h). */
}

static void cond_waiter_remove(sys_cond_info* c, uint64_t tid)
{
    for (int i = 0; i < SYS_COND_WAITER_TIDS_MAX; i++) {
        if (c->waiter_tids[i] == tid) { c->waiter_tids[i] = 0; return; }
    }
}

static int cond_waiter_present(const sys_cond_info* c, uint64_t tid)
{
    for (int i = 0; i < SYS_COND_WAITER_TIDS_MAX; i++) {
        if (c->waiter_tids[i] == tid) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * sys_cond_create
 *
 * r3 = pointer to receive cond ID (u32*)
 * r4 = mutex_id to associate with
 * r5 = pointer to attribute struct (not read)
 * -----------------------------------------------------------------------*/
int64_t sys_cond_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t mutex_id    = LV2_ARG_U32(ctx, 1);

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
     * be re-Initialized while potentially referenced) */
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

    /* Count this cond against its mutex so sys_mutex_destroy can return EPERM
     * while associated conds exist. Serialized against other cond
     * create/destroy by the cond table lock held here. */
    g_sys_mutexes[mutex_id - 1].cond_count++;

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

    /* EBUSY when a PPU thread is waiting on this cond. Destroying anyway
     * strands the parked waiter forever, and the slot's later recycling lets
     * the stale waiter drive the new cond's committed/pending counters
     * negative, after which every signal on the recycled cond is dropped. */
    if (c->committed > 0) {
        cond_table_unlock();
        return (int64_t)(int32_t)CELL_EBUSY;
    }

#ifndef _WIN32
    pthread_cond_destroy(&c->cv);
#endif
    /* Windows CONDITION_VARIABLE doesn't need destruction */

    /* Release this cond's claim on its mutex (see sys_cond_create). Guarded
     * against a mutex slot that was itself already destroyed/recycled out
     * from under us. */
    if (c->mutex_id >= 1 && c->mutex_id <= SYS_MUTEX_MAX) {
        sys_mutex_info* m = &g_sys_mutexes[c->mutex_id - 1];
        if (m->active && m->cond_count > 0)
            m->cond_count--;
    }

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

    /* Cap very large guest timeouts so the microsecond-to-millisecond and
     * timespec conversions below stay in range. */
    if (timeout_us > ((1ull << 48) - 1)) timeout_us = (1ull << 48) - 1;

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

    /* The caller must currently own the associated mutex. lv2 returns
     * CELL_EPERM otherwise instead of touching the mutex/CS state: a
     * non-owner caller releasing/re-acquiring a CS it never entered is UB on
     * Win32 and corrupts whatever the real owner had saved. */
    uint64_t caller_tid = ctx->thread_id;
    if (m->owner_tid != caller_tid) {
        return (int64_t)(int32_t)CELL_EPERM;
    }

    /* The caller must hold the associated mutex. We need to release it
     * atomically with the wait and re-acquire it on wake. */


    /* Save and fully clear ownership info. A recursive mutex held N times
     * releases all N host-CS entries before parking, not just one:
     * SleepConditionVariableCS only leaves the CS once, so a recursive holder
     * (lock_count > 1) would otherwise keep the CS held N-1 times after
     * "releasing" it, permanently blocking every other thread's
     * EnterCriticalSection. Loop the Leave to match every guest-side Enter
     * recorded by sys_mutex_lock/trylock (sys_mutex.c). */
    uint64_t saved_owner = m->owner_tid;
    int saved_count = m->lock_count;
    m->owner_tid = 0;
    m->lock_count = 0;

#ifdef _WIN32
    /* Never let a converted ms value land on 0xFFFFFFFF (INFINITE); cap at
     * 0xFFFFFFFE. */
    DWORD ms;
    if (timeout_us == 0) {
        ms = INFINITE;
    } else {
        uint64_t ms64 = timeout_us / 1000;
        ms = (ms64 > 0xFFFFFFFEull) ? 0xFFFFFFFEu : (DWORD)ms64;
    }

    /* Rendezvous (see sys_cond.h): commit under the internal sig lock before
     * releasing the guest mutex. From this instant a signal lands in
     * `pending` and cannot be lost, matching lv2's wait-entry enqueue. Then
     * fully release the guest mutex (all N recursion levels), park on the CV
     * against the sig lock, consume one pending on wake, and re-acquire the
     * guest mutex. */
    EnterCriticalSection(&c->sig_cs);
    c->committed++;
    cond_waiter_add(c, caller_tid);   /* visible to signal_to */

    for (int i = 0; i < saved_count; i++) {
        LeaveCriticalSection(&m->cs);
    }

    BOOL ok = TRUE;
    if (timeout_us > 0 && timeout_us < 1000) {
        /* Sub-ms timed wait: safe to poll (rather than park on the CV) because
         * a signal commits to c->pending under sig_cs, so it cannot be lost
         * while we yield. A floored-to-1ms CV wait would round up to the OS
         * timer tick and inflate micro-timeouts ~50x. */
        int64_t deadline = lv2_usec_deadline(timeout_us);
        while (c->pending == 0) {
            if (lv2_deadline_passed(deadline)) { ok = FALSE; break; }
            LeaveCriticalSection(&c->sig_cs);
            SwitchToThread();
            EnterCriticalSection(&c->sig_cs);
        }
    } else {
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
    }
    if (ok)
        c->pending--;
    c->committed--;
    cond_waiter_remove(c, caller_tid);
    LeaveCriticalSection(&c->sig_cs);

    /* Re-acquire all N recursion levels of the guest mutex. */
    for (int i = 0; i < saved_count; i++) {
        EnterCriticalSection(&m->cs);
    }

    /* Restore ownership */
    m->owner_tid = saved_owner;
    m->lock_count = saved_count;


    if (!ok) {
        return (int64_t)(int32_t)CELL_ETIMEDOUT;
    }
#else
    /* Rendezvous (see Win32 path): commit under the sig lock before the full
     * guest-mutex release; park against the sig lock; consume one pending. */
    pthread_mutex_lock(&c->sig_mtx);
    c->committed++;
    cond_waiter_add(c, caller_tid);   /* visible to signal_to */

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
    cond_waiter_remove(c, caller_tid);
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


    /* Matches lv2 semantics: sys_cond_signal never acquires the guest mutex
     * (a mutex-CS guard would cause hold-and-wait). lv2 enqueues the waiter
     * at wait-syscall entry, so a signal after that entry is always
     * delivered, including in the window between the waiter releasing the
     * mutex and parking. The internal sig lock + committed/pending counters
     * reproduce that (held for nanoseconds, no guest-visible hold-and-wait);
     * a signal with no committed waiter is discarded, as on lv2. */
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


    /* Rendezvous broadcast (see sys_cond_signal): serve every committed
     * waiter; never touch the guest mutex. */
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
 * sys_cond_signal_to (syscall 110)
 *
 * r3 = cond_id
 * r4 = ppu_thread_id (target)
 *
 * Returns CELL_OK when the signal is delivered; ESRCH for an un-issued cond
 * ID; EPERM when the specified PPU thread is not waiting on the condition
 * variable. Membership comes from the waiter-tid registry the wait path
 * maintains under the sig lock, so the check is race-free against wait
 * entry/exit.
 *
 * Delivery approximation (documented deviation): the rendezvous core wakes
 * committed waiters through a shared pending counter, so with multiple
 * simultaneous waiters an arbitrary committed waiter may consume the signal
 * instead of the targeted one (real lv2 wakes exactly the target). The
 * validation/error contract and the delivery count are exact; single-waiter
 * behavior (the common signal_to idiom) is exact. Exact targeting would need
 * a per-thread directed-wake channel.
 * -----------------------------------------------------------------------*/
int64_t sys_cond_signal_to(ppu_context* ctx)
{
    uint32_t cond_id   = LV2_ARG_U32(ctx, 0);
    uint64_t target_tid = LV2_ARG_U64(ctx, 1);

    if (cond_id == 0 || cond_id > SYS_COND_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_cond_info* c = &g_sys_conds[cond_id - 1];
    if (!c->active)
        return (int64_t)(int32_t)CELL_ESRCH;


#ifdef _WIN32
    EnterCriticalSection(&c->sig_cs);
    if (!cond_waiter_present(c, target_tid)) {
        LeaveCriticalSection(&c->sig_cs);
        return (int64_t)(int32_t)CELL_EPERM;
    }
    if (c->committed > c->pending) {
        c->pending++;
        /* Broadcast so the target is guaranteed to wake and re-check; extra
         * wakes are benign (waiters loop on pending and re-sleep). */
        WakeAllConditionVariable(&c->cv);
    }
    /* committed == pending: every committed waiter (incl. the target) already
     * has a wake owed -- the signal is already satisfied; CELL_OK. */
    LeaveCriticalSection(&c->sig_cs);
#else
    pthread_mutex_lock(&c->sig_mtx);
    if (!cond_waiter_present(c, target_tid)) {
        pthread_mutex_unlock(&c->sig_mtx);
        return (int64_t)(int32_t)CELL_EPERM;
    }
    if (c->committed > c->pending) {
        c->pending++;
        pthread_cond_broadcast(&c->cv);
    }
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
    lv2_syscall_register(tbl, SYS_COND_SIGNAL_TO,   sys_cond_signal_to);
}
