/*
 * ps3recomp - sysPrxForUser HLE implementation
 *
 * lwmutex is backed by a host CRITICAL_SECTION (pthread mutex on POSIX) and
 * lwcond by a CONDITION_VARIABLE (pthread cond on POSIX); the process, string
 * and heap helpers are thin wrappers over the C runtime.
 */

#include "sysPrxForUser.h"
#include "../../runtime/ppu/ppu_guest_write.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal: Lightweight mutex table
 * -----------------------------------------------------------------------*/
#define MAX_LWMUTEX 256

typedef struct {
    int in_use;
    int recursive;
    /* Destroy/recreate churn safety: titles may destroy and recreate an
     * lwmutex while other threads are locking it.
     * cs_init: the host CS is initialized once per slot and never deleted or
     * re-initialized (Delete/re-Init while another thread sits in Enter is
     * UB that corrupts the CS and causes random forever-blocks).
     * gen: bumped on destroy; lockers revalidate {in_use, gen} after Enter
     * and back out with ESRCH if the lwmutex died/recycled under them.
     * owner_tid: lets destroy refuse (EBUSY) whenever the lwmutex is held,
     * including held by the destroyer itself. */
    int cs_init;
    u32 gen;
    unsigned long owner_tid;
#ifdef _WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t mtx;
#endif
} LwMutexSlot;

static LwMutexSlot s_lwmutex[MAX_LWMUTEX];
static u32 s_lwmutex_next = 0;

/* Guards slot allocation in the create paths now that guest threads are
 * real host threads. Lock/unlock/signal stay lock-free: they only touch
 * the slot the caller already owns. */
#ifdef _WIN32
static SRWLOCK s_slot_lock = SRWLOCK_INIT;
static void slot_lock(void)   { AcquireSRWLockExclusive(&s_slot_lock); }
static void slot_unlock(void) { ReleaseSRWLockExclusive(&s_slot_lock); }
#else
static pthread_mutex_t s_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static void slot_lock(void)   { pthread_mutex_lock(&s_slot_lock); }
static void slot_unlock(void) { pthread_mutex_unlock(&s_slot_lock); }
#endif

/* Destroy all lwmutex slots and clear the table (lwcond slots are untouched). */
void sys_lwmutex_reset_all(void)
{
    for (u32 i = 0; i < MAX_LWMUTEX; i++) {
        if (s_lwmutex[i].in_use) {
#ifdef _WIN32
            DeleteCriticalSection(&s_lwmutex[i].cs);
#else
            pthread_mutex_destroy(&s_lwmutex[i].mtx);
#endif
        }
    }
    memset(s_lwmutex, 0, sizeof(s_lwmutex));
    s_lwmutex_next = 0;
}


/* ---------------------------------------------------------------------------
 * Internal: Lightweight cond table
 * -----------------------------------------------------------------------*/
#define MAX_LWCOND 256

typedef struct {
    int in_use;
    int cv_init;    /* CV initialized once per slot, never re-init/destroyed
                     * (re-init or destroy with a stale waiter parked is UB;
                     * same churn-safety scheme as LwMutexSlot) */
    u32 lwmutex_id; /* index into s_lwmutex */
    /* Rendezvous state: real lv2's waiter commits (under the lwmutex) before
     * releasing it, and a signal aimed at a committed waiter is held by the
     * kernel even if the waiter hasn't parked yet. A bare Win32 CV drops that
     * signal (edge-triggered), which can leave the waiter asleep forever.
     * `committed`/`pending` under the internal sig lock reproduce lv2's
     * semantics: held signal for committed waiters, discard when none. The
     * signaler never touches the guest lwmutex, to avoid hold-and-wait. */
    int committed;  /* waiters that entered wait, not yet returned */
    int pending;    /* signals held for committed waiters */
#ifdef _WIN32
    CONDITION_VARIABLE cv;
    CRITICAL_SECTION sig_cs;
    int sig_cs_init;
#else
    pthread_cond_t cv;
    pthread_mutex_t sig_mtx;
    int sig_mtx_init;
#endif
} LwCondSlot;

static LwCondSlot s_lwcond[MAX_LWCOND];
static u32 s_lwcond_next = 0;

/* ---------------------------------------------------------------------------
 * Internal: Heap table
 * -----------------------------------------------------------------------*/
#define MAX_HEAPS 16

typedef struct {
    int in_use;
    u32 id;
} HeapSlot;

static HeapSlot s_heaps[MAX_HEAPS];
static u32 s_next_heap_id = 1;

/* ---------------------------------------------------------------------------
 * Process management
 * -----------------------------------------------------------------------*/

void sys_process_exit(s32 exitcode)
{
    printf("[sysPrxForUser] sys_process_exit(code=%d)\n", exitcode);
    exit(exitcode);
}

s32 sys_process_getpid(void)
{
    return 1001;
}

s32 sys_process_get_number_of_object(u32 object_type, u32* count)
{
    (void)object_type;
    if (count) *count = 0;
    return CELL_OK;
}

s32 sys_process_is_spu_lock_line_reservation_address(u32 addr, u64 flags)
{
    (void)addr; (void)flags;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * String/memory functions
 * -----------------------------------------------------------------------*/

s32 _sys_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[PS3] ");
    int ret = vprintf(fmt, ap);
    va_end(ap);
    return ret;
}

s32 _sys_sprintf(char* buf, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsprintf(buf, fmt, ap);
    va_end(ap);
    return ret;
}

s32 _sys_snprintf(char* buf, u32 size, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

s32 _sys_strlen(const char* str)
{
    if (!str) return 0;
    return (s32)strlen(str);
}

char* _sys_strncpy(char* dst, const char* src, u32 size)
{
    /* returns dst, like libc; callers chain these returns. */
    if (!dst || !src) return dst;
    return strncpy(dst, src, size);
}

char* _sys_strcat(char* dst, const char* src)
{
    /* returns dst; callers pass the return of strcat straight into the
     * next string call. */
    if (!dst || !src) return dst;
    return strcat(dst, src);
}

s32 _sys_strcmp(const char* s1, const char* s2)
{
    if (!s1 || !s2) return -1;
    return strcmp(s1, s2);
}

s32 _sys_strncmp(const char* s1, const char* s2, u32 max)
{
    /* returns exactly -1/0/1 */
    if (!s1 || !s2) return -1;
    int r = strncmp(s1, s2, max);
    return (r > 0) - (r < 0);
}

char* _sys_strcpy(char* dst, const char* src)
{
    /* returns dst */
    if (!dst || !src) return dst;
    return strcpy(dst, src);
}

char* _sys_strncat(char* dst, const char* src, u32 max)
{
    /* appends at most max chars then terminates; returns dst */
    if (!dst || !src) return dst;
    return strncat(dst, src, max);
}

/* Both are CELL_OK no-ops (SPURS-heavy titles boot with that); a guest
 * va_list is not interpretable by a host vararg call anyway. */
s32 _sys_vprintf(const char* fmt, u32 va_guest)
{
    (void)fmt; (void)va_guest;
    return CELL_OK;
}

s32 _sys_vsnprintf(char* buf, u32 size, const char* fmt, u32 va_guest)
{
    (void)fmt; (void)va_guest;
    if (buf && size) buf[0] = '\0';
    return CELL_OK;
}

void* _sys_memset(void* dst, s32 val, u32 size)
{
    extern u8* vm_base;
    const uintptr_t host = (uintptr_t)dst;
    const uintptr_t base = (uintptr_t)vm_base;
    void* result = memset(dst, val, size);
    if (dst && vm_base && host >= base && host - base <= UINT32_MAX &&
        (uint64_t)(host - base) + size <= 0x100000000ull)
        vm_native_spurs_notify_write((u32)(host - base), size);
    return result;
}

void* _sys_memcpy(void* dst, const void* src, u32 size)
{
    extern u8* vm_base;
    const uintptr_t host = (uintptr_t)dst;
    const uintptr_t base = (uintptr_t)vm_base;
    void* result = memcpy(dst, src, size);
    if (dst && vm_base && host >= base && host - base <= UINT32_MAX &&
        (uint64_t)(host - base) + size <= 0x100000000ull)
        vm_native_spurs_notify_write((u32)(host - base), size);
    return result;
}

s32 _sys_memcmp(const void* s1, const void* s2, u32 size)
{
    return memcmp(s1, s2, size);
}

s32 _sys_toupper(s32 c) { return toupper(c); }
s32 _sys_tolower(s32 c) { return tolower(c); }

/* ---------------------------------------------------------------------------
 * Lightweight mutex
 * -----------------------------------------------------------------------*/

extern u8* vm_base;  /* for guest-address diagnostics in the boot log */
#define YZ_GUEST_ADDR(p) ((u32)((u8*)(p) - vm_base))

/* Guest structs arrive as raw pointers into big-endian guest RAM (the import
 * bridges do no marshaling), so multi-byte guest fields must be decoded. */
static inline u32 guest_be32(u32 v)
{
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) |
           ((v << 8) & 0x00FF0000u) | (v << 24);
}

/* lv2 lwmutex owner-word sentinels. On real lv2 the lock state lives in the
 * guest struct itself: lock_var = {owner, waiter} (SDK sys/synchronization.h),
 * owner = FREE when unlocked, a thread id when held, and DEAD once destroyed;
 * userland lock classifies lock-after-destroy via the DEAD sentinel instead of
 * silently re-registering. Create stores FREE, destroy stores DEAD into
 * owner, and lock on DEAD returns CELL_EINVAL.
 * Titles may cycle destroy->create on one lwmutex; a concurrent locker landing
 * in the gap must not reach lwmutex_lazy_register (sleep_queue==0 after a
 * plain zeroing destroy), which would silently acquire a fresh phantom slot
 * while create then allocated a different one, losing mutual exclusion.
 * The owner word is stored guest-BE (guest_be32 is its own inverse). */
#define YZ_LWMUTEX_FREE 0xFFFFFFFFu
#define YZ_LWMUTEX_DEAD 0xFFFFFFFEu

static inline u32 lwmutex_owner_get(const sys_lwmutex_t_hle* m)
{
    u32 raw;
    memcpy(&raw, &m->lock_var, 4);      /* owner = first BE u32 of lock_var */
    return guest_be32(raw);
}

static inline void lwmutex_owner_set(sys_lwmutex_t_hle* m, u32 owner)
{
    u32 raw = guest_be32(owner);
    memcpy((void*)&m->lock_var, &raw, 4);
}

/* Shared guard for lock/trylock: a destroyed lwmutex (owner sentinel DEAD)
 * must fail loudly, never lazily re-register. Returns nonzero when the call
 * must bail with CELL_EINVAL. */
static int lwmutex_dead_check(sys_lwmutex_t_hle* m, const char* who)
{
    if (m->sleep_queue != 0 || lwmutex_owner_get(m) != YZ_LWMUTEX_DEAD)
        return 0;
    {
        static long n = 0;               /* log cap only; a race just skews log volume */
        long k = ++n;
        if (k <= 24 || (k & (k - 1)) == 0) {
            fprintf(stderr, "[lwm] %s on DESTROYED lwmutex guest=0x%08X -> EINVAL "
                    "(resurrection prevented; #%ld)\n",
                    who, YZ_GUEST_ADDR(m), k);
            fflush(stderr);
        }
    }
    return 1;
}

/* Slot-allocation core: caller must hold slot_lock. Inits a host lock for the
 * lwmutex and writes its 1-based slot id into sleep_queue. */
static s32 lwmutex_register_locked(sys_lwmutex_t_hle* lwmutex, const sys_lwmutex_attribute_t* attr)
{
    u32 idx = s_lwmutex_next;
    for (u32 i = 0; i < MAX_LWMUTEX; i++) {
        u32 slot = (idx + i) % MAX_LWMUTEX;
        if (!s_lwmutex[slot].in_use) {
            LwMutexSlot* m = &s_lwmutex[slot];
            m->in_use = 1;
            m->owner_tid = 0;
            /* attr lives in guest memory: decode the BE flags word (a raw host
             * read of BE 0x10 sees 0x10000000). */
            m->recursive = (attr && (guest_be32(attr->recursive) & SYS_SYNC_RECURSIVE)) ? 1 : 0;

            /* Init the host lock once per slot lifetime; recycled slots reuse
             * the live CS (re-Init over a CS another thread may still be
             * blocked on is UB -- see the struct comment). */
#ifdef _WIN32
            if (!m->cs_init) { InitializeCriticalSection(&m->cs); m->cs_init = 1; }
#else
            if (!m->cs_init) {
                pthread_mutexattr_t mattr;
                pthread_mutexattr_init(&mattr);
                pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
                pthread_mutex_init(&m->mtx, &mattr);
                pthread_mutexattr_destroy(&mattr);
                m->cs_init = 1;
            }
#endif

            memset(lwmutex, 0, sizeof(*lwmutex));
            lwmutex->sleep_queue = slot + 1; /* 1-based ID */
            lwmutex_owner_set(lwmutex, YZ_LWMUTEX_FREE); /* lv2 create stores FREE */
            s_lwmutex_next = (slot + 1) % MAX_LWMUTEX;
            return CELL_OK;
        }
    }
    return CELL_EAGAIN;
}

s32 sys_lwmutex_create(sys_lwmutex_t_hle* lwmutex, const sys_lwmutex_attribute_t* attr)
{
    if (!lwmutex)
        return CELL_EFAULT;

    slot_lock();
    s32 rc = lwmutex_register_locked(lwmutex, attr);
    slot_unlock();
    return rc;
}

/* Lazily register a statically/inline-initialized lwmutex (sleep_queue==0) that
 * never went through create. Double-checked under slot_lock: such lwmutexes live
 * in main RAM, not the thread stack, so they may be shared; two threads racing
 * the first lock must not allocate two slots or stomp each other. */
static s32 lwmutex_lazy_register(sys_lwmutex_t_hle* lwmutex)
{
    slot_lock();
    s32 rc = CELL_OK;
    if (lwmutex->sleep_queue == 0) {        /* re-check under lock (TOCTOU-safe) */
        /* The static initializer stored the SYS_SYNC_* flags in the guest struct's
         * attribute word; capture it before register_locked memsets the struct,
         * or a SYS_SYNC_RECURSIVE lwmutex silently registers non-recursive. Kept
         * raw (guest-BE) -- register_locked decodes it like a create-path attr. */
        sys_lwmutex_attribute_t a;
        a.protocol = 0;
        a.recursive = lwmutex->attribute;
        memcpy(a.name, "lazylwm", 8);
        rc = lwmutex_register_locked(lwmutex, &a);
    }
    slot_unlock();
    return rc;
}

s32 sys_lwmutex_lock(sys_lwmutex_t_hle* lwmutex, u64 timeout)
{
    if (!lwmutex) return CELL_EFAULT;

    /* Destroyed lwmutex (owner sentinel DEAD): real lv2 fails this lock;
     * lazy-register must never resurrect it. */
    if (lwmutex_dead_check(lwmutex, "lock"))
        return CELL_EINVAL;

    /* Statically/inline-initialized lwmutex that never passed through create
     * (PS3 lwmutexes are user-space; the kernel sleep queue is allocated lazily),
     * which would otherwise fail forever with ESRCH. lwmutex_lazy_register is
     * TOCTOU-safe under slot_lock. */
    if (lwmutex->sleep_queue == 0) {
        s32 rc = lwmutex_lazy_register(lwmutex);
        if (rc != CELL_OK) return rc;
    }

    u32 slot = lwmutex->sleep_queue - 1;
    if (slot >= MAX_LWMUTEX || !s_lwmutex[slot].in_use) {
#ifdef _WIN32
        printf("[sysPrxForUser] sys_lwmutex_lock FAIL guest=0x%08X "
               "sleep_queue=0x%08X (%s) [host tid %lu] -> ESRCH\n",
               YZ_GUEST_ADDR(lwmutex), lwmutex->sleep_queue,
               slot >= MAX_LWMUTEX ? "bad slot" : "slot not in use",
               GetCurrentThreadId());
#else
        printf("[sysPrxForUser] sys_lwmutex_lock FAIL guest=0x%08X "
               "sleep_queue=0x%08X (%s) -> ESRCH\n",
               YZ_GUEST_ADDR(lwmutex), lwmutex->sleep_queue,
               slot >= MAX_LWMUTEX ? "bad slot" : "slot not in use");
#endif
        return CELL_ESRCH;
    }
    u32 gen_snap = s_lwmutex[slot].gen;

    /* A NOT_RECURSIVE lwmutex relocked by its owner returns CELL_EDEADLK.
     * The host lock is recursive, so without this check the relock would
     * succeed and recursion depth would grow unbounded. Safe before
     * the acquire: owner_tid can only equal this thread if WE hold it. */
    {
#ifdef _WIN32
        unsigned long self_tid = (unsigned long)GetCurrentThreadId();
#else
        unsigned long self_tid = (unsigned long)(uintptr_t)pthread_self();
#endif
        if (!s_lwmutex[slot].recursive &&
            s_lwmutex[slot].owner_tid == self_tid) {
            static int n = 0; if (n < 8) { n++;
                fprintf(stderr, "[lwm] non-recursive self-relock guest=0x%08X -> EDEADLK\n",
                        YZ_GUEST_ADDR(lwmutex)); fflush(stderr); }
            return CELL_EDEADLK;
        }
    }

#ifdef _WIN32
    if (timeout == 0) {
        EnterCriticalSection(&s_lwmutex[slot].cs);
    } else {
        /* A finite timeout (microseconds) returns ETIMEDOUT. A CS has no
         * timed acquire; poll the try form against a tick deadline. */
        ULONGLONG t0 = GetTickCount64();
        u64 ms = timeout / 1000; if (ms == 0) ms = 1;
        while (!TryEnterCriticalSection(&s_lwmutex[slot].cs)) {
            if (GetTickCount64() - t0 >= ms)
                return CELL_ETIMEDOUT;
            SwitchToThread();
        }
    }
#else
    if (timeout == 0) {
        pthread_mutex_lock(&s_lwmutex[slot].mtx);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeout / 1000000);
        ts.tv_nsec += (long)((timeout % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        if (pthread_mutex_timedlock(&s_lwmutex[slot].mtx, &ts) != 0)
            return CELL_ETIMEDOUT;
    }
#endif

    /* Revalidate after acquiring: the lwmutex may have been destroyed (and
     * the slot even recycled for a different lwmutex) while we were blocked
     * in Enter. Real lv2 returns ESRCH to a locker of a destroyed lwmutex. */
    if (!s_lwmutex[slot].in_use || s_lwmutex[slot].gen != gen_snap) {
#ifdef _WIN32
        LeaveCriticalSection(&s_lwmutex[slot].cs);
#else
        pthread_mutex_unlock(&s_lwmutex[slot].mtx);
#endif
        { static int n = 0; if (n < 40) { n++;
            fprintf(stderr, "[lwm] lock lost race with destroy guest=0x%08X slot=%u -> ESRCH\n",
                    YZ_GUEST_ADDR(lwmutex), slot); fflush(stderr); } }
        return CELL_ESRCH;
    }

#ifdef _WIN32
    s_lwmutex[slot].owner_tid = (unsigned long)GetCurrentThreadId();
#else
    s_lwmutex[slot].owner_tid = (unsigned long)(uintptr_t)pthread_self();
#endif
    lwmutex->lock_var = 1;
    lwmutex->recursive_count++;
    return CELL_OK;
}

s32 sys_lwmutex_trylock(sys_lwmutex_t_hle* lwmutex)
{
    if (!lwmutex) return CELL_EFAULT;

    if (lwmutex_dead_check(lwmutex, "trylock"))
        return CELL_EINVAL;

    if (lwmutex->sleep_queue == 0) {   /* lazily register static-init lwmutex (see lock) */
        s32 rc = lwmutex_lazy_register(lwmutex);
        if (rc != CELL_OK) return rc;
    }

    u32 slot = lwmutex->sleep_queue - 1;
    if (slot >= MAX_LWMUTEX || !s_lwmutex[slot].in_use)
        return CELL_ESRCH;
    u32 gen_snap = s_lwmutex[slot].gen;

#ifdef _WIN32
    if (!TryEnterCriticalSection(&s_lwmutex[slot].cs))
        return CELL_EBUSY;
#else
    if (pthread_mutex_trylock(&s_lwmutex[slot].mtx) != 0)
        return CELL_EBUSY;
#endif

    /* Destroyed/recycled while we raced in (see sys_lwmutex_lock). */
    if (!s_lwmutex[slot].in_use || s_lwmutex[slot].gen != gen_snap) {
#ifdef _WIN32
        LeaveCriticalSection(&s_lwmutex[slot].cs);
#else
        pthread_mutex_unlock(&s_lwmutex[slot].mtx);
#endif
        return CELL_ESRCH;
    }

#ifdef _WIN32
    s_lwmutex[slot].owner_tid = (unsigned long)GetCurrentThreadId();
#else
    s_lwmutex[slot].owner_tid = (unsigned long)(uintptr_t)pthread_self();
#endif
    lwmutex->lock_var = 1;
    lwmutex->recursive_count++;
    return CELL_OK;
}

s32 sys_lwmutex_unlock(sys_lwmutex_t_hle* lwmutex)
{
    if (!lwmutex) return CELL_EFAULT;

    u32 slot = lwmutex->sleep_queue - 1;
    if (slot >= MAX_LWMUTEX || !s_lwmutex[slot].in_use)
        return CELL_ESRCH;

    /* Only the owning thread may unlock (EPERM). This also guards the
     * recursive_count 0 -> 0xFFFFFFFF underflow that would permanently pin
     * owner_tid (making destroy return EBUSY forever) and the Win32 UB of
     * LeaveCriticalSection from a non-owner. */
    {
#ifdef _WIN32
        unsigned long self_tid = (unsigned long)GetCurrentThreadId();
#else
        unsigned long self_tid = (unsigned long)(uintptr_t)pthread_self();
#endif
        if (s_lwmutex[slot].owner_tid != self_tid ||
            lwmutex->recursive_count == 0) {
            static int n = 0; if (n < 8) { n++;
                fprintf(stderr, "[lwm] unlock by non-owner guest=0x%08X "
                        "(owner=%lu self=%lu rc=%d) -> EPERM\n",
                        YZ_GUEST_ADDR(lwmutex), s_lwmutex[slot].owner_tid,
                        self_tid, lwmutex->recursive_count); fflush(stderr); }
            return CELL_EPERM;
        }
    }

    lwmutex->recursive_count--;
    if (lwmutex->recursive_count == 0) {
        lwmutex->lock_var = 0;
        s_lwmutex[slot].owner_tid = 0;   /* full release: destroy may proceed */
    }

#ifdef _WIN32
    LeaveCriticalSection(&s_lwmutex[slot].cs);
#else
    pthread_mutex_unlock(&s_lwmutex[slot].mtx);
#endif
    return CELL_OK;
}

s32 sys_lwmutex_destroy(sys_lwmutex_t_hle* lwmutex)
{
    if (!lwmutex) return CELL_EFAULT;

    slot_lock();   /* serialize vs create's slot recycling */
    u32 slot = lwmutex->sleep_queue - 1;
    if (slot >= MAX_LWMUTEX || !s_lwmutex[slot].in_use) {
        slot_unlock();
        printf("[sysPrxForUser] sys_lwmutex_destroy -> ESRCH\n");
        return CELL_ESRCH;   /* already destroyed / never created */
    }

    /* lv2 refuses to destroy a held lwmutex (CELL_EBUSY) - games rely on
     * this when tearing down a heap another thread is still allocating
     * from: the EBUSY keeps the lock (and the heap) alive. owner_tid covers
     * all holders including the destroyer itself (a TryEnter check passes
     * for a self-held recursive CS). */
    if (s_lwmutex[slot].owner_tid != 0) {
        slot_unlock();
        printf("[sysPrxForUser] sys_lwmutex_destroy -> EBUSY\n");
        return CELL_EBUSY;
    }
#ifdef _WIN32
    if (!TryEnterCriticalSection(&s_lwmutex[slot].cs)) {
        /* a locker is mid-Enter (pre-owner-write) */
        slot_unlock();
        printf("[sysPrxForUser] sys_lwmutex_destroy -> EBUSY\n");
        return CELL_EBUSY;
    }
    /* Mark dead while holding the CS, so any locker blocked in Enter wakes
     * into the revalidation path and backs out with ESRCH. The CS itself is
     * never deleted: a concurrent locker may still be sitting in Enter, and
     * DeleteCriticalSection under it is UB (random forever-blocks at
     * whatever lock next contended). The slot's CS stays initialized and is
     * reused by the next create. */
    s_lwmutex[slot].in_use = 0;
    s_lwmutex[slot].gen++;
    LeaveCriticalSection(&s_lwmutex[slot].cs);
#else
    if (pthread_mutex_trylock(&s_lwmutex[slot].mtx) != 0) {
        slot_unlock();
        return CELL_EBUSY;
    }
    s_lwmutex[slot].in_use = 0;
    s_lwmutex[slot].gen++;
    pthread_mutex_unlock(&s_lwmutex[slot].mtx);
#endif
    slot_unlock();

    /* Publish the lv2 DEAD sentinel instead of a bare zero struct: a lock()
     * racing this destroy (or landing before the next create) must classify
     * the mutex as deleted (CELL_EINVAL), not fall into
     * lwmutex_lazy_register and silently resurrect it on a phantom slot. */
    memset(lwmutex, 0, sizeof(*lwmutex));
    lwmutex_owner_set(lwmutex, YZ_LWMUTEX_DEAD);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Lightweight condition variable
 * -----------------------------------------------------------------------*/

s32 sys_lwcond_create(sys_lwcond_t_hle* lwcond, sys_lwmutex_t_hle* lwmutex,
                      const sys_lwcond_attribute_t* attr)
{
    (void)attr;

    if (!lwcond || !lwmutex)
        return CELL_EFAULT;

    slot_lock();
    u32 idx = s_lwcond_next;
    for (u32 i = 0; i < MAX_LWCOND; i++) {
        u32 slot = (idx + i) % MAX_LWCOND;
        if (!s_lwcond[slot].in_use) {
            LwCondSlot* c = &s_lwcond[slot];
            c->in_use = 1;
            c->lwmutex_id = lwmutex->sleep_queue - 1;

#ifdef _WIN32
            if (!c->cv_init) { InitializeConditionVariable(&c->cv); c->cv_init = 1; }
            if (!c->sig_cs_init) { InitializeCriticalSection(&c->sig_cs); c->sig_cs_init = 1; }
#else
            if (!c->cv_init) { pthread_cond_init(&c->cv, NULL); c->cv_init = 1; }
            if (!c->sig_mtx_init) { pthread_mutex_init(&c->sig_mtx, NULL); c->sig_mtx_init = 1; }
#endif
            c->committed = 0;
            c->pending = 0;

            lwcond->lwcond_queue = slot + 1;
            s_lwcond_next = (slot + 1) % MAX_LWCOND;
            slot_unlock();
            return CELL_OK;
        }
    }
    slot_unlock();
    return CELL_EAGAIN;
}

s32 sys_lwcond_signal(sys_lwcond_t_hle* lwcond)
{
    if (!lwcond) return CELL_EFAULT;

    u32 slot = lwcond->lwcond_queue - 1;
    if (slot >= MAX_LWCOND || !s_lwcond[slot].in_use)
        return CELL_ESRCH;

    /* Rendezvous semantics (see LwCondSlot): a signal is held for a committed
     * waiter (even one not yet parked in the CV) and discarded when no
     * committed waiter remains unserved, as on lv2. */
#ifdef _WIN32
    EnterCriticalSection(&s_lwcond[slot].sig_cs);
    if (s_lwcond[slot].committed > s_lwcond[slot].pending) {
        s_lwcond[slot].pending++;
        WakeConditionVariable(&s_lwcond[slot].cv);
    }
    LeaveCriticalSection(&s_lwcond[slot].sig_cs);
#else
    pthread_mutex_lock(&s_lwcond[slot].sig_mtx);
    if (s_lwcond[slot].committed > s_lwcond[slot].pending) {
        s_lwcond[slot].pending++;
        pthread_cond_signal(&s_lwcond[slot].cv);
    }
    pthread_mutex_unlock(&s_lwcond[slot].sig_mtx);
#endif
    return CELL_OK;
}

s32 sys_lwcond_signal_all(sys_lwcond_t_hle* lwcond)
{
    if (!lwcond) return CELL_EFAULT;

    u32 slot = lwcond->lwcond_queue - 1;
    if (slot >= MAX_LWCOND || !s_lwcond[slot].in_use)
        return CELL_ESRCH;

#ifdef _WIN32
    EnterCriticalSection(&s_lwcond[slot].sig_cs);
    s_lwcond[slot].pending = s_lwcond[slot].committed;
    WakeAllConditionVariable(&s_lwcond[slot].cv);
    LeaveCriticalSection(&s_lwcond[slot].sig_cs);
#else
    pthread_mutex_lock(&s_lwcond[slot].sig_mtx);
    s_lwcond[slot].pending = s_lwcond[slot].committed;
    pthread_cond_broadcast(&s_lwcond[slot].cv);
    pthread_mutex_unlock(&s_lwcond[slot].sig_mtx);
#endif
    return CELL_OK;
}

s32 sys_lwcond_wait(sys_lwcond_t_hle* lwcond, u64 timeout)
{
    if (!lwcond) return CELL_EFAULT;

    u32 cslot = lwcond->lwcond_queue - 1;
    if (cslot >= MAX_LWCOND || !s_lwcond[cslot].in_use)
        return CELL_ESRCH;

    u32 mslot = s_lwcond[cslot].lwmutex_id;
    if (mslot >= MAX_LWMUTEX || !s_lwmutex[mslot].in_use)
        return CELL_ESRCH;
    /* Snapshot the generation now and revalidate after the wake-side
     * re-acquire: a waiter parked here across a destroy+create cycle would
     * otherwise re-enter the recycled slot's CS and stamp owner_tid over an
     * unrelated live lwmutex. */
    u32 mgen_snap = s_lwmutex[mslot].gen;

    /* Rendezvous protocol (see LwCondSlot): commit under the internal sig
     * lock before releasing the guest lwmutex; after this instant a signal
     * cannot be lost (it lands in `pending`). Then release the guest mutex,
     * park on the CV against the sig lock, and on wake consume one pending.
     * The guest mutex is re-acquired before returning (lwcond_wait's
     * contract), with the same bookkeeping as lock/unlock. */
    s32 rc_out = CELL_OK;
#ifdef _WIN32
    EnterCriticalSection(&s_lwcond[cslot].sig_cs);
    s_lwcond[cslot].committed++;

    /* release ONE level of the guest lwmutex (mirror sys_lwmutex_unlock) */
    s_lwmutex[mslot].owner_tid = 0;
    LeaveCriticalSection(&s_lwmutex[mslot].cs);

    DWORD deadline_ms = (timeout == 0) ? INFINITE : (DWORD)(timeout / 1000);
    /* A sub-ms timeout floored to 0 would return ETIMEDOUT without ever
     * waiting, and a huge timeout could collide with INFINITE. */
    if (timeout != 0 && deadline_ms == 0) deadline_ms = 1;
    if (timeout != 0 && deadline_ms == INFINITE) deadline_ms = INFINITE - 1;
    ULONGLONG t0 = GetTickCount64();
    while (s_lwcond[cslot].pending == 0) {
        DWORD ms = deadline_ms;
        if (deadline_ms != INFINITE) {
            ULONGLONG spent = GetTickCount64() - t0;
            if (spent >= deadline_ms) { rc_out = CELL_ETIMEDOUT; break; }
            ms = (DWORD)(deadline_ms - spent);
        }
        if (!SleepConditionVariableCS(&s_lwcond[cslot].cv,
                                      &s_lwcond[cslot].sig_cs, ms)) {
            if (GetLastError() == ERROR_TIMEOUT) {
                if (s_lwcond[cslot].pending == 0) { rc_out = CELL_ETIMEDOUT; break; }
            } else if (s_lwcond[cslot].pending == 0) {
                rc_out = CELL_EFAULT; break;
            }
        }
    }
    if (rc_out == CELL_OK)
        s_lwcond[cslot].pending--;
    s_lwcond[cslot].committed--;
    /* Timeout exit consumed no pending: keep the invariant pending <=
     * committed so a signal that landed in the exit window cannot strand and
     * suppress the next legitimate signal. */
    if (s_lwcond[cslot].pending > s_lwcond[cslot].committed)
        s_lwcond[cslot].pending = s_lwcond[cslot].committed;
    LeaveCriticalSection(&s_lwcond[cslot].sig_cs);

    /* re-acquire the guest lwmutex (mirror sys_lwmutex_lock) */
    EnterCriticalSection(&s_lwmutex[mslot].cs);
    if (!s_lwmutex[mslot].in_use || s_lwmutex[mslot].gen != mgen_snap) {
        LeaveCriticalSection(&s_lwmutex[mslot].cs);
        { static int n = 0; if (n < 8) { n++;
            fprintf(stderr, "[lwc] wait re-acquire lost lwmutex to destroy "
                    "(slot=%u) -> ESRCH\n", mslot); fflush(stderr); } }
        return CELL_ESRCH;
    }
    s_lwmutex[mslot].owner_tid = (unsigned long)GetCurrentThreadId();
#else
    pthread_mutex_lock(&s_lwcond[cslot].sig_mtx);
    s_lwcond[cslot].committed++;
    s_lwmutex[mslot].owner_tid = 0;
    pthread_mutex_unlock(&s_lwmutex[mslot].mtx);

    if (timeout == 0) {
        while (s_lwcond[cslot].pending == 0)
            pthread_cond_wait(&s_lwcond[cslot].cv, &s_lwcond[cslot].sig_mtx);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(timeout / 1000000);
        ts.tv_nsec += (long)((timeout % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (s_lwcond[cslot].pending == 0) {
            int rc = pthread_cond_timedwait(&s_lwcond[cslot].cv,
                                            &s_lwcond[cslot].sig_mtx, &ts);
            if (rc == 110 /* ETIMEDOUT */ && s_lwcond[cslot].pending == 0) {
                rc_out = CELL_ETIMEDOUT;
                break;
            }
        }
    }
    if (rc_out == CELL_OK)
        s_lwcond[cslot].pending--;
    s_lwcond[cslot].committed--;
    if (s_lwcond[cslot].pending > s_lwcond[cslot].committed)
        s_lwcond[cslot].pending = s_lwcond[cslot].committed;
    pthread_mutex_unlock(&s_lwcond[cslot].sig_mtx);

    pthread_mutex_lock(&s_lwmutex[mslot].mtx);
    if (!s_lwmutex[mslot].in_use || s_lwmutex[mslot].gen != mgen_snap) {
        pthread_mutex_unlock(&s_lwmutex[mslot].mtx);
        return CELL_ESRCH;
    }
    s_lwmutex[mslot].owner_tid = (unsigned long)(uintptr_t)pthread_self();
#endif

    return rc_out;
}

s32 sys_lwcond_destroy(sys_lwcond_t_hle* lwcond)
{
    if (!lwcond) return CELL_EFAULT;

    slot_lock();
    u32 slot = lwcond->lwcond_queue - 1;
    if (slot < MAX_LWCOND && s_lwcond[slot].in_use) {
        /* The CV object itself is kept initialized for slot reuse: destroying
         * it with a stale waiter parked is UB (churn-safety). */
        s_lwcond[slot].in_use = 0;
    }
    slot_unlock();

    memset(lwcond, 0, sizeof(*lwcond));
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Heap management (wraps malloc)
 * -----------------------------------------------------------------------*/

s32 sys_heap_create_heap(sys_heap_t* heap, u32 start_addr, u32 size,
                          u32 flags, void* alloc_func, void* free_func)
{
    (void)start_addr; (void)size; (void)flags;
    (void)alloc_func; (void)free_func;

    if (!heap) return CELL_EFAULT;

    for (int i = 0; i < MAX_HEAPS; i++) {
        if (!s_heaps[i].in_use) {
            s_heaps[i].in_use = 1;
            s_heaps[i].id = s_next_heap_id++;
            *heap = s_heaps[i].id;
            return CELL_OK;
        }
    }
    return CELL_ENOMEM;
}

s32 sys_heap_destroy_heap(sys_heap_t heap)
{
    for (int i = 0; i < MAX_HEAPS; i++) {
        if (s_heaps[i].in_use && s_heaps[i].id == heap) {
            s_heaps[i].in_use = 0;
            return CELL_OK;
        }
    }
    return CELL_ESRCH;
}

void* sys_heap_malloc(sys_heap_t heap, u32 size)
{
    (void)heap;
    return malloc(size);
}

s32 sys_heap_free(sys_heap_t heap, void* ptr)
{
    (void)heap;
    free(ptr);
    return CELL_OK;
}

void* sys_heap_memalign(sys_heap_t heap, u32 align, u32 size)
{
    (void)heap;
#ifdef _WIN32
    return _aligned_malloc(size, align);
#else
    void* ptr = NULL;
    if (posix_memalign(&ptr, align, size) != 0)
        return NULL;
    return ptr;
#endif
}

/* ---------------------------------------------------------------------------
 * PRX utilities
 * -----------------------------------------------------------------------*/

s32 sys_prx_exitspawn_with_level(void)
{
    return CELL_OK;
}

s32 sys_prx_get_module_id_by_name(const char* name, u64 flags, u32* id)
{
    /* The ABI is (name, flags, pOpt): the 3rd arg is an optional attribute
     * struct, legally NULL and ignored here, not an out-param; the module id
     * comes back in the return value (positive) or a negative error. */
    (void)flags;
    (void)id;
    printf("[sysPrxForUser] sys_prx_get_module_id_by_name('%s')\n",
           name ? name : "(null)");

    /* This game only ever loads pxd_shader (the ogrez_shader_ps3 family);
     * any other name is unknown -- no registry lookup needed. */
    if (name && strncmp(name, "ogrez_shader_ps3", 16) == 0)
        return (s32)0x23000001;

    return CELL_ESRCH;
}

/* ---------------------------------------------------------------------------
 * Random number generation
 *
 * Used by many games for seeding RNG, crypto operations, UUID generation.
 * We use the host OS PRNG for quality random data.
 * -----------------------------------------------------------------------*/

s32 sys_get_random_number(void* buf, u64 size)
{
    if (!buf || size == 0) return CELL_EFAULT;

#ifdef _WIN32
    /* Use BCryptGenRandom on Windows */
    #include <bcrypt.h>
    NTSTATUS status = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)size,
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        /* Fallback: fill with simple pseudo-random */
        u8* p = (u8*)buf;
        for (u64 i = 0; i < size; i++)
            p[i] = (u8)(rand() & 0xFF);
    }
#else
    /* Use /dev/urandom on Unix */
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        fread(buf, 1, (size_t)size, f);
        fclose(f);
    } else {
        u8* p = (u8*)buf;
        for (u64 i = 0; i < size; i++)
            p[i] = (u8)(rand() & 0xFF);
    }
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Console I/O (debug)
 * -----------------------------------------------------------------------*/

s32 console_putc(s32 ch)
{
    fputc(ch, stderr);
    return CELL_OK;
}

s32 console_getc(void)
{
    return -1; /* no input */
}

s32 console_write(const void* buf, u32 len)
{
    if (buf && len > 0)
        fwrite(buf, 1, len, stderr);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Process info
 * -----------------------------------------------------------------------*/

s32 sys_process_get_paramsfo(void* buf)
{
    /* _sys_process_get_paramsfo fills a fixed 0x40-byte buffer (the firmware
     * ABI; callers allocate exactly 0x40 on the stack -- e.g. libgcm's
     * _cellGcmInitBody zeroes 0x40 then calls this, with its saved non-volatile
     * registers immediately after the buffer). Writing more would overrun
     * the caller's frame and smash its saved r26-r31 -> the caller restores 0
     * and faults. Zeroing 0x40 yields a minimal/empty param.sfo (callers
     * strncmp the title id and fall through to the default path). */
    if (!buf) return CELL_EFAULT;
    memset(buf, 0, 0x40);
    return CELL_OK;
}
