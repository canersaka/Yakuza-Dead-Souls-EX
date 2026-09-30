/*
 * ps3recomp - Semaphore syscalls (implementation)
 */

#include "sys_semaphore.h"
#include "sys_timer.h"   /* lv2_usec_deadline: sub-ms timed waits */
#include "../memory/vm.h"
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_semaphore_info g_sys_semaphores[SYS_SEMAPHORE_MAX];

#ifdef _WIN32
static CRITICAL_SECTION s_sem_table_lock;
static int              s_sem_table_lock_init = 0;
#else
static pthread_mutex_t  s_sem_table_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void sem_table_lock(void)
{
#ifdef _WIN32
    if (!s_sem_table_lock_init) {
        InitializeCriticalSection(&s_sem_table_lock);
        s_sem_table_lock_init = 1;
    }
    EnterCriticalSection(&s_sem_table_lock);
#else
    pthread_mutex_lock(&s_sem_table_lock);
#endif
}

static void sem_table_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_sem_table_lock);
#else
    pthread_mutex_unlock(&s_sem_table_lock);
#endif
}

/*
 * A semaphore slot can be recycled immediately after destroy.  Keep every
 * operation pinned to the current slot lifetime so destroy cannot close the
 * host primitive under a waiter/poster and a stale operation cannot cross
 * into a newly-created semaphore that reused the same numeric id.
 */
static sys_semaphore_info* sem_ref_acquire(uint32_t sem_id)
{
    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX) return NULL;
    sem_table_lock();
    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active) {
        sem_table_unlock();
        return NULL;
    }
    s->references++;
    sem_table_unlock();
    return s;
}

static void sem_ref_release(sys_semaphore_info* s)
{
    if (!s) return;
    sem_table_lock();
    if (s->references) s->references--;
    sem_table_unlock();
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
 * sys_semaphore_create
 *
 * r3 = pointer to receive semaphore ID (u32*)
 * r4 = pointer to attribute struct (not read)
 * r5 = initial value
 * r6 = max value
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_create(ppu_context* ctx)
{
    uint32_t id_out_addr  = LV2_ARG_PTR(ctx, 0);
    int32_t  initial      = LV2_ARG_S32(ctx, 2);
    int32_t  max_val      = LV2_ARG_S32(ctx, 3);

    if (max_val <= 0 || initial < 0 || initial > max_val)
        return (int64_t)(int32_t)CELL_EINVAL;

    sem_table_lock();

    int slot = -1;
    for (int i = 0; i < SYS_SEMAPHORE_MAX; i++) {
        if (!g_sys_semaphores[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        sem_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_semaphore_info* s = &g_sys_semaphores[slot];
    memset(s, 0, sizeof(*s));
    s->active    = 1;
    s->value     = initial;
    s->max_value = max_val;

#ifdef _WIN32
    /* The kernel semaphore is a pure wake channel, not a count mirror: it
     * starts at 0 regardless of `initial` (the count lives only in
     * s->value; a waiter checks value before ever sleeping). Cap the
     * handle at LONG_MAX-ish: tokens are bounded by waiters and spurious
     * leftovers are benign (a woken waiter re-checks value and re-sleeps). */
    s->sem_handle = CreateSemaphoreA(NULL, 0, 0x7FFFFFFF, NULL);
    InitializeCriticalSection(&s->value_lock);
    if (s->sem_handle == NULL) {
        s->active = 0;
        sem_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
#else
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cv, NULL);
#endif

    uint32_t sem_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, sem_id);
    }

    sem_table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_destroy
 *
 * r3 = sem_id
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_destroy(ppu_context* ctx)
{
    uint32_t sem_id = LV2_ARG_U32(ctx, 0);

    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sem_table_lock();

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active) {
        sem_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    /* The LV2 contract rejects destruction while the object is in use.  A
     * parked waiter holds a reference for its entire wait, as do concurrent
     * post/trywait/get-value calls. */
    if (s->references != 0 || s->waiters != 0) {
        sem_table_unlock();
        return (int64_t)(int32_t)CELL_EBUSY;
    }

    /* Make the slot unreachable before releasing host resources.  With the
     * table lock held and no references, no operation can still touch them. */
    s->active = 0;

#ifdef _WIN32
    CloseHandle(s->sem_handle);
    DeleteCriticalSection(&s->value_lock);
#else
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mtx);
#endif

    sem_table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_wait
 *
 * r3 = sem_id
 * r4 = timeout_usec (0 = infinite)
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_wait(ppu_context* ctx)
{
    uint32_t sem_id     = LV2_ARG_U32(ctx, 0);
    uint64_t timeout_us = LV2_ARG_U64(ctx, 1);

    /* Clamp the guest timeout to 48 bits so the QPC deadline multiply /
     * ms-DWORD conversion below can't overflow. */
    if (timeout_us > ((1ull << 48) - 1)) timeout_us = (1ull << 48) - 1;

    sys_semaphore_info* s = sem_ref_acquire(sem_id);
    if (!s)
        return (int64_t)(int32_t)CELL_ESRCH;



#ifdef _WIN32
    /* Single source of truth, mirroring the POSIX branch: s->value is the
     * only count, checked and decremented under value_lock before any
     * sleep; the kernel sem is a pure wake channel (post releases
     * min(count, waiters) tokens; spurious or orphaned tokens just cause
     * a benign re-check loop). */
    int64_t deadline = (timeout_us > 0) ? lv2_usec_deadline(timeout_us) : 0;
    for (;;) {
        /* Take a token if available (the only count, under the lock). */
        EnterCriticalSection(&s->value_lock);
        if (s->value > 0) {
            s->value--;
            LeaveCriticalSection(&s->value_lock);
            sem_ref_release(s);
            return CELL_OK;
        }
        if (timeout_us > 0 && lv2_deadline_passed(deadline)) {
            LeaveCriticalSection(&s->value_lock);
            sem_ref_release(s);
            return (int64_t)(int32_t)CELL_ETIMEDOUT;
        }
        s->waiters++;
        LeaveCriticalSection(&s->value_lock);

        /* Sleep on the wake channel. Bounded 1ms slices when timed so the QPC
         * deadline (not the OS timer tick) governs; INFINITE when untimed,
         * since a post always releases a token and a real wake is never
         * missed. */
        DWORD ms = (timeout_us == 0) ? INFINITE : 1;
        WaitForSingleObject(s->sem_handle, ms);

        EnterCriticalSection(&s->value_lock);
        if (s->waiters > 0) s->waiters--;
        LeaveCriticalSection(&s->value_lock);
        /* loop: re-check value */
    }
#else
    pthread_mutex_lock(&s->mtx);

    if (timeout_us == 0) {
        while (s->value <= 0) {
            s->waiters++;
            pthread_cond_wait(&s->cv, &s->mtx);
            s->waiters--;
        }
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeout_us / 1000000);
        ts.tv_nsec += (long)((timeout_us % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (s->value <= 0) {
            s->waiters++;
            int rc = pthread_cond_timedwait(&s->cv, &s->mtx, &ts);
            s->waiters--;
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&s->mtx);
                sem_ref_release(s);
                return (int64_t)(int32_t)CELL_ETIMEDOUT;
            }
        }
    }

    s->value--;
    pthread_mutex_unlock(&s->mtx);
#endif

    sem_ref_release(s);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_trywait
 *
 * r3 = sem_id
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_trywait(ppu_context* ctx)
{
    uint32_t sem_id = LV2_ARG_U32(ctx, 0);

    sys_semaphore_info* s = sem_ref_acquire(sem_id);
    if (!s)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    /* Decide purely on s->value (the handle is a wake channel, not a count
     * mirror). */
    EnterCriticalSection(&s->value_lock);
    if (s->value <= 0) {
        LeaveCriticalSection(&s->value_lock);
        sem_ref_release(s);
        return (int64_t)(int32_t)CELL_EBUSY;
    }
    s->value--;
    LeaveCriticalSection(&s->value_lock);
#else
    pthread_mutex_lock(&s->mtx);
    if (s->value <= 0) {
        pthread_mutex_unlock(&s->mtx);
        sem_ref_release(s);
        return (int64_t)(int32_t)CELL_EBUSY;
    }
    s->value--;
    pthread_mutex_unlock(&s->mtx);
#endif

    sem_ref_release(s);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_post
 *
 * r3 = sem_id
 * r4 = count (number to post)
 * -----------------------------------------------------------------------*/

int64_t sys_semaphore_post(ppu_context* ctx)
{
    uint32_t sem_id = LV2_ARG_U32(ctx, 0);
    int32_t  count  = LV2_ARG_S32(ctx, 1);

    if (count <= 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    sys_semaphore_info* s = sem_ref_acquire(sem_id);
    if (!s)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* Waiters absorb the post first: when count is at least the number of
     * waiting threads, all of them are woken and the semaphore value is
     * incremented by the difference. Only if that resulting value exceeds
     * the maximum are the threads not woken and CELL_EBUSY returned.
     * Comparing value+count against max without crediting waiters would
     * refuse legal posts (and drop every owed wake) whenever count > max
     * while waiters were parked. */
#ifdef _WIN32
    EnterCriticalSection(&s->value_lock);
    int wake = (count < s->waiters) ? count : s->waiters;
    if ((int64_t)s->value + count - wake > (int64_t)s->max_value) {
        LeaveCriticalSection(&s->value_lock);
        sem_ref_release(s);
        return (int64_t)(int32_t)CELL_EBUSY;
    }
    s->value += count;
    /* Wake channel: release exactly enough tokens to cover the parked waiters
     * this post can satisfy (min(count, waiters)). Woken waiters re-check
     * value under the lock; an orphaned token (waiter already left) is benign.
     * A post never skips the release. */
    LeaveCriticalSection(&s->value_lock);

    if (wake > 0)
        ReleaseSemaphore(s->sem_handle, wake, NULL);
#else
    pthread_mutex_lock(&s->mtx);
    /* As in the Win32 branch: waiters absorb the post before the max_value
     * ceiling applies. */
    { int wake = (count < s->waiters) ? count : s->waiters;
      if ((int64_t)s->value + count - wake > (int64_t)s->max_value) {
          pthread_mutex_unlock(&s->mtx);
          sem_ref_release(s);
          return (int64_t)(int32_t)CELL_EBUSY;
      }
      s->value += count;
      /* Wake the waiters this post can satisfy (broadcast is also correct,
       * since they re-check value, but min(count,waiters) signals is
       * precise). */
      for (int i = 0; i < wake; i++) pthread_cond_signal(&s->cv); }
    pthread_mutex_unlock(&s->mtx);
#endif

    sem_ref_release(s);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_get_value
 *
 * r3 = sem_id
 * r4 = pointer to receive value (s32*)
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_get_value(ppu_context* ctx)
{
    uint32_t sem_id   = LV2_ARG_U32(ctx, 0);
    uint32_t out_addr = LV2_ARG_PTR(ctx, 1);

    sys_semaphore_info* s = sem_ref_acquire(sem_id);
    if (!s)
        return (int64_t)(int32_t)CELL_ESRCH;

    int32_t val;
#ifdef _WIN32
    EnterCriticalSection(&s->value_lock);
    val = s->value;
    LeaveCriticalSection(&s->value_lock);
#else
    pthread_mutex_lock(&s->mtx);
    val = s->value;
    pthread_mutex_unlock(&s->mtx);
#endif

    if (out_addr != 0) {
        write_be32(out_addr, (uint32_t)val);
    }

    sem_ref_release(s);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_semaphore_init(lv2_syscall_table* tbl)
{
    memset(g_sys_semaphores, 0, sizeof(g_sys_semaphores));

#ifdef _WIN32
    if (!s_sem_table_lock_init) {
        InitializeCriticalSection(&s_sem_table_lock);
        s_sem_table_lock_init = 1;
    }
#endif

    lv2_syscall_register(tbl, SYS_SEMAPHORE_CREATE,    sys_semaphore_create);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_DESTROY,   sys_semaphore_destroy);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_WAIT,      sys_semaphore_wait);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_TRYWAIT,   sys_semaphore_trywait);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_POST,      sys_semaphore_post);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_GET_VALUE, sys_semaphore_get_value);
}
