/*
 * ps3recomp - Timer and time syscalls (implementation)
 */

#include "sys_timer.h"
#include "sys_event.h"
#include "../memory/vm.h"
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")   /* timeBeginPeriod */
#endif

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_timer_info g_sys_timers[SYS_TIMER_MAX];

/* Per-slot state parallel to g_sys_timers, kept here rather than in
 * sys_timer_info: base_time (absolute, timer_now_usec() clock) and whether
 * sys_timer_start armed this slot as one-shot (period == 0). Reset when a
 * slot is (re)created. */
static uint64_t s_timer_base_time[SYS_TIMER_MAX];
static int      s_timer_one_shot[SYS_TIMER_MAX];

#ifdef _WIN32
static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_init = 0;

static void ensure_qpc_init(void)
{
    if (!s_qpc_init) {
        QueryPerformanceFrequency(&s_qpc_freq);
        s_qpc_init = 1;
    }
}

/* Pin the system timer resolution to 1ms the first time this module actually
 * uses a waitable timer, so sys_timer_usleep's precision doesn't depend on
 * the runner having already called timeBeginPeriod(1) (the runner does this
 * too, for scheduler-wide effect, but this module should be correct
 * standalone, e.g. for another consumer of ps3recomp_runtime). Idempotent
 * one-shot; timeBeginPeriod is a process-wide, refcounted request so a
 * second call from the runner is harmless. */
static int s_time_period_init = 0;
static void ensure_time_period_init(void)
{
    if (!s_time_period_init) {
        timeBeginPeriod(1);
        s_time_period_init = 1;
    }
}

#endif

/* ---------------------------------------------------------------------------
 * PPU timebase (mftb/mftbu) -- the guest clock.
 *
 * One global monotonic counter scaled to the PS3 timebase (79.8 MHz),
 * anchored at first use. Every guest timing loop (pacers, throttles,
 * profilers) reads it, so it must be a real clock rather than a per-read
 * increment. Overflow-safe split multiply (rem < qpf, so
 * rem * 79.8e6 < ~8e14 << 2^63).
 * -----------------------------------------------------------------------*/
uint64_t ppu_timebase_now(void)
{
#ifdef _WIN32
    static LONGLONG t0 = 0;
    ensure_qpc_init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!t0) t0 = now.QuadPart;   /* benign race: same anchor either way */
    uint64_t d = (uint64_t)(now.QuadPart - t0);
    uint64_t q = (uint64_t)s_qpc_freq.QuadPart;
    return (d / q) * PS3_TIMEBASE_FREQ + (d % q) * PS3_TIMEBASE_FREQ / q;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * PS3_TIMEBASE_FREQ +
           (uint64_t)ts.tv_nsec * PS3_TIMEBASE_FREQ / 1000000000ull;
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

static void write_be64(uint32_t addr, uint64_t val)
{
    uint64_t* p = (uint64_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 56) & 0xFFULL) |
          ((val >> 40) & 0xFF00ULL) |
          ((val >> 24) & 0xFF0000ULL) |
          ((val >>  8) & 0xFF000000ULL) |
          ((val <<  8) & 0xFF00000000ULL) |
          ((val << 24) & 0xFF0000000000ULL) |
          ((val << 40) & 0xFF000000000000ULL) |
          ((val << 56) & 0xFF00000000000000ULL);
#endif
    *p = val;
}

/* ---------------------------------------------------------------------------
 * sys_timer_usleep
 *
 * r3 = microseconds
 * -----------------------------------------------------------------------*/
static int64_t sys_timer_usleep_impl(ppu_context* ctx)
{
    uint64_t usec = LV2_ARG_U64(ctx, 0);

#ifdef _WIN32
    ensure_time_period_init();

    /* Use high-resolution sleep via waitable timer for better precision.
     * Prefer CreateWaitableTimerExW with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
     * (Win10 1803+): it gives sub-millisecond timer resolution without
     * needing the process-wide timeBeginPeriod(1) for this wait. Falls back
     * to the plain CreateWaitableTimerW path on older systems where the
     * HIGH_RESOLUTION flag is rejected (CreateWaitableTimerExW returns NULL:
     * pre-1803 Windows 10, or Windows 7/8.x). */
    if (usec >= 1000) {
        HANDLE timer = CreateWaitableTimerExW(NULL, NULL,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) {
            timer = CreateWaitableTimerW(NULL, TRUE, NULL);
        }
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -((LONGLONG)usec * 10); /* 100ns units, negative = relative */
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
            CloseHandle(timer);
        } else {
            Sleep((DWORD)(usec / 1000));
        }
    } else if (usec > 0) {
        /* Sub-millisecond sleep. A waitable timer / Sleep rounds up to the
         * ~0.5-1 ms scheduler quantum, which is too coarse: guest code polls
         * with 30/250 us sleeps waiting for the RSX GET pointer to advance,
         * and these must not collapse to a single yield. Busy-wait to the
         * precise QPC deadline. Give a sibling host thread one scheduling opportunity
         * before the precision tail, then use processor pauses while checking the
         * deadline. Repeating SwitchToThread on every QPC check can reschedule the
         * caller for milliseconds under SPURS contention, multiplying a 30/250 us
         * guest poll far beyond its requested duration. */
        ensure_qpc_init();
        LARGE_INTEGER qpc_start, qpc_now;
        QueryPerformanceCounter(&qpc_start);
        const int64_t qpc_deadline = qpc_start.QuadPart +
            (int64_t)((usec * (uint64_t)s_qpc_freq.QuadPart) / 1000000ULL);
        SwitchToThread();              /* one handoff to the awaited producer */
        do {
            YieldProcessor();          /* precise tail without repeated reschedules */
            QueryPerformanceCounter(&qpc_now);
        } while (qpc_now.QuadPart < qpc_deadline);
    }
#else
    if (usec > 0) {
        struct timespec ts;
        ts.tv_sec  = (time_t)(usec / 1000000);
        ts.tv_nsec = (long)((usec % 1000000) * 1000);
        nanosleep(&ts, NULL);
    }
#endif

    return CELL_OK;
}

int64_t sys_timer_usleep(ppu_context* ctx)
{
    return sys_timer_usleep_impl(ctx);
}

/* ---------------------------------------------------------------------------
 * sys_timer_sleep
 *
 * r3 = seconds
 * -----------------------------------------------------------------------*/
int64_t sys_timer_sleep(ppu_context* ctx)
{
    uint32_t sec = LV2_ARG_U32(ctx, 0);

#ifdef _WIN32
    Sleep(sec * 1000);
#else
    sleep(sec);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_time_get_current_time
 *
 * r3 = pointer to receive seconds (u64*)
 * r4 = pointer to receive nanoseconds (u64*)
 * -----------------------------------------------------------------------*/
int64_t sys_time_get_current_time(ppu_context* ctx)
{
    uint32_t sec_addr  = LV2_ARG_PTR(ctx, 0);
    uint32_t nsec_addr = LV2_ARG_PTR(ctx, 1);

    uint64_t sec, nsec;

#ifdef _WIN32
    /* Report calendar time: anchor once to the real wall clock
     * (GetSystemTimeAsFileTime + a QPC anchor pair) and add the QPC delta,
     * rather than exposing time since the QPC epoch (host uptime), which a
     * guest reading a real timestamp would see as hours since this PC
     * booted, not seconds since 1970. */
    static int           s_anchor_init = 0;
    static uint64_t      s_anchor_epoch_ns = 0;  /* Unix epoch ns at anchor */
    static LARGE_INTEGER s_anchor_qpc;

    ensure_qpc_init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    {
        if (!s_anchor_init) {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            uint64_t ft100ns = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            /* 100ns units since 1601-01-01 -> since 1970-01-01, then to
             * nanoseconds. */
            s_anchor_epoch_ns = (ft100ns - 116444736000000000ULL) * 100ULL;
            s_anchor_qpc = now;
            s_anchor_init = 1;
        }

        uint64_t d = (uint64_t)(now.QuadPart - s_anchor_qpc.QuadPart);
        uint64_t q = (uint64_t)s_qpc_freq.QuadPart;
        uint64_t delta_ns = (d / q) * 1000000000ULL + (d % q) * 1000000000ULL / q;
        uint64_t time_ns  = s_anchor_epoch_ns + delta_ns;
        sec  = time_ns / 1000000000ULL;
        nsec = time_ns % 1000000000ULL;
    }
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    sec  = (uint64_t)ts.tv_sec;
    nsec = (uint64_t)ts.tv_nsec;
#endif

    if (sec_addr != 0) {
        write_be64(sec_addr, sec);
    }
    if (nsec_addr != 0) {
        write_be64(nsec_addr, nsec);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_time_get_timebase_frequency
 *
 * Returns the PS3 timebase frequency in r3.
 * -----------------------------------------------------------------------*/
int64_t sys_time_get_timebase_frequency(ppu_context* ctx)
{
    (void)ctx;
    return (int64_t)PS3_TIMEBASE_FREQ;
}

/* ---------------------------------------------------------------------------
 * Periodic timer thread
 * -----------------------------------------------------------------------*/

/* Forward declaration */
static void timer_send_event(sys_timer_info* t);

/* sys_timer base/expiry times and the event data3 stamp are defined in system
 * time: microseconds since boot, the clock the guest reads through
 * sys_time_get_system_time, not Unix-epoch wall time. A wall-epoch clock
 * (~1.75e15 us) would make every guest-supplied absolute base compare as
 * "already past", firing one-shots and periodic-absolute timers immediately.
 * The runner registers its boot-anchored guest clock here at startup;
 * standalone runtime builds fall back to a local first-call anchor in the
 * same domain. */
uint64_t (*g_lv2_system_time_us)(void) = 0;

#ifdef _WIN32
static uint64_t timer_now_usec(void)
{
    if (g_lv2_system_time_us)
        return g_lv2_system_time_us();

    static int           anchor_init = 0;
    static LARGE_INTEGER anchor_qpc;

    ensure_qpc_init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (!anchor_init) {
        anchor_qpc = now;
        anchor_init = 1;
    }

    uint64_t d = (uint64_t)(now.QuadPart - anchor_qpc.QuadPart);
    uint64_t q = (uint64_t)s_qpc_freq.QuadPart;
    return (d / q) * 1000000ULL + (d % q) * 1000000ULL / q;
}
#else
static uint64_t timer_now_usec(void)
{
    if (g_lv2_system_time_us)
        return g_lv2_system_time_us();

    static int      anchor_init = 0;
    static uint64_t anchor_us = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
    if (!anchor_init) { anchor_us = us; anchor_init = 1; }
    return us - anchor_us;
}
#endif

#ifdef _WIN32
static DWORD WINAPI timer_thread_proc(LPVOID param)
{
    sys_timer_info* t = (sys_timer_info*)param;
    int slot = (int)(t - g_sys_timers);
    uint64_t base = s_timer_base_time[slot];
    int one_shot  = s_timer_one_shot[slot];

    /* One-shot / first-periodic-fire base_time wait: wait until base_time is
     * reached, or fire immediately if it's already past. stop_event still
     * cancels the wait. */
    if (base != 0) {
        uint64_t now = timer_now_usec();
        if (base > now) {
            uint64_t delta_ms64 = (base - now) / 1000;
            DWORD wait_ms = (delta_ms64 > 0xFFFFFFFEull) ? 0xFFFFFFFEu : (DWORD)delta_ms64;
            if (wait_ms == 0) wait_ms = 1;
            if (WaitForSingleObject(t->stop_event, wait_ms) == WAIT_OBJECT_0)
                return 0; /* stop signaled before base_time */
        }
    }

    if (!t->running) return 0;

    if (one_shot) {
        timer_send_event(t);
        t->running = 0;
        return 0;
    }

    while (t->running) {
        DWORD ms = (DWORD)(t->period_usec / 1000);
        if (ms == 0) ms = 1;

        DWORD result = WaitForSingleObject(t->stop_event, ms);
        if (result == WAIT_OBJECT_0) {
            break; /* stop signaled */
        }

        if (t->running) {
            timer_send_event(t);
        }
    }

    return 0;
}
#else
static void* timer_thread_proc(void* param)
{
    sys_timer_info* t = (sys_timer_info*)param;
    int slot = (int)(t - g_sys_timers);
    uint64_t base = s_timer_base_time[slot];
    int one_shot  = s_timer_one_shot[slot];

    pthread_mutex_lock(&t->mtx);

    if (base != 0) {
        uint64_t now = timer_now_usec();
        if (base > now) {
            uint64_t delta = base - now;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += (time_t)(delta / 1000000);
            ts.tv_nsec += (long)((delta % 1000000) * 1000);
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            while (!t->stop_flag) {
                int rc = pthread_cond_timedwait(&t->cv, &t->mtx, &ts);
                if (rc == ETIMEDOUT) break;
            }
        }
    }

    if (t->stop_flag) {
        pthread_mutex_unlock(&t->mtx);
        return NULL;
    }

    if (one_shot) {
        pthread_mutex_unlock(&t->mtx);
        timer_send_event(t);
        t->running = 0;
        return NULL;
    }

    while (!t->stop_flag) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(t->period_usec / 1000000);
        ts.tv_nsec += (long)((t->period_usec % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        int rc = pthread_cond_timedwait(&t->cv, &t->mtx, &ts);
        if (t->stop_flag) break;

        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&t->mtx);
            timer_send_event(t);
            pthread_mutex_lock(&t->mtx);
        }
    }
    pthread_mutex_unlock(&t->mtx);

    return NULL;
}
#endif

static void timer_send_event(sys_timer_info* t)
{
    if (t->event_queue_id <= 0 || t->event_queue_id > SYS_EVENT_QUEUE_MAX)
        return;

    sys_event_queue_info* q = &g_sys_event_queues[t->event_queue_id - 1];
    if (!q->active)
        return;

    /* data2 is the connect-time argument and data3 is the system time at
     * expiry (timer_now_usec(), the system-time clock described above),
     * i.e. system_time_t at the moment of firing. */
    sys_event_t evt;
    evt.source = t->source;
    evt.data1  = t->data1;
    evt.data2  = t->data2;
    evt.data3  = timer_now_usec();

    /* Push event (ignore if queue full) */
#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif

    if (q->count < q->capacity) {
        q->buffer[q->tail] = evt;
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;
#ifdef _WIN32
        WakeConditionVariable(&q->not_empty);
#else
        pthread_cond_signal(&q->not_empty);
#endif
    }

#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif
}

/* ---------------------------------------------------------------------------
 * sys_timer_create
 *
 * r3 = pointer to receive timer ID (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_timer_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);

    int slot = -1;
    for (int i = 0; i < SYS_TIMER_MAX; i++) {
        if (!g_sys_timers[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_timer_info* t = &g_sys_timers[slot];
    memset(t, 0, sizeof(*t));
    t->active  = 1;
    t->running = 0;
    t->event_queue_id = 0;
    s_timer_base_time[slot] = 0;
    s_timer_one_shot[slot]  = 0;

#ifdef _WIN32
    t->stop_event    = NULL;
    t->thread_handle = NULL;
#else
    pthread_mutex_init(&t->mtx, NULL);
    pthread_cond_init(&t->cv, NULL);
    t->stop_flag = 0;
#endif

    uint32_t timer_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, timer_id);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_destroy
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_destroy(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* A timer still connected to an event queue must be disconnected first;
     * destroy returns CELL_EISCONN rather than silently tearing down the
     * connection. */
    if (t->event_queue_id != 0)
        return (int64_t)(int32_t)CELL_EISCONN;

    /* Stop if running */
    if (t->running) {
        t->running = 0;
#ifdef _WIN32
        if (t->stop_event) SetEvent(t->stop_event);
        if (t->thread_handle) {
            WaitForSingleObject(t->thread_handle, 3000);
            CloseHandle(t->thread_handle);
        }
        if (t->stop_event) CloseHandle(t->stop_event);
#else
        pthread_mutex_lock(&t->mtx);
        t->stop_flag = 1;
        pthread_cond_signal(&t->cv);
        pthread_mutex_unlock(&t->mtx);
        pthread_join(t->thread, NULL);
        pthread_mutex_destroy(&t->mtx);
        pthread_cond_destroy(&t->cv);
#endif
    }

    t->active = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_connect_event_queue
 *
 * r3 = timer_id
 * r4 = queue_id
 * r5 = name (source in the delivered event; SYS_TIMER_EVENT_NO_NAME
 *      substitutes PID<<32|timer_id)
 * r6 = data1
 * r7 = data2
 *
 * The connect-time data2 (r7) is delivered verbatim in every fired event.
 * NO_NAME source substitution is not implemented: t->source is stored and
 * emitted raw.
 * -----------------------------------------------------------------------*/
int64_t sys_timer_connect_event_queue(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);
    uint32_t queue_id = LV2_ARG_U32(ctx, 1);
    uint64_t source   = LV2_ARG_U64(ctx, 2);
    uint64_t data1    = LV2_ARG_U64(ctx, 3);
    uint64_t data2    = LV2_ARG_U64(ctx, 4);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    t->event_queue_id = (int32_t)queue_id;
    t->source         = source;
    t->data1          = data1;
    t->data2          = data2;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_disconnect_event_queue
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_disconnect_event_queue(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    t->event_queue_id = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_start
 *
 * r3 = timer_id
 * r4 = base_time (absolute start time, 0 = now)
 * r5 = period_usec
 * -----------------------------------------------------------------------*/
int64_t sys_timer_start(ppu_context* ctx)
{
    uint32_t timer_id    = LV2_ARG_U32(ctx, 0);
    uint64_t base_time   = LV2_ARG_U64(ctx, 1);
    uint64_t period      = LV2_ARG_U64(ctx, 2);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (t->running)
        return (int64_t)(int32_t)CELL_EBUSY;

    if (period != 0 && period < 100)
        return (int64_t)(int32_t)CELL_EINVAL;

    /* period == 0 is a one-shot timer, not an error: it fires exactly once
     * when base_time is reached (immediately if base_time is already past;
     * no error is returned in that case) and does not re-arm. */

    int slot = (int)(timer_id - 1);
    s_timer_base_time[slot] = base_time;
    s_timer_one_shot[slot]  = (period == 0) ? 1 : 0;

    t->period_usec = period;
    t->running     = 1;

#ifdef _WIN32
    t->stop_event    = CreateEventA(NULL, TRUE, FALSE, NULL);
    t->thread_handle = CreateThread(NULL, 0, timer_thread_proc, t, 0, NULL);
#else
    t->stop_flag = 0;
    pthread_create(&t->thread, NULL, timer_thread_proc, t);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_stop
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_stop(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (!t->running)
        return CELL_OK;

    t->running = 0;

#ifdef _WIN32
    if (t->stop_event) SetEvent(t->stop_event);
    if (t->thread_handle) {
        WaitForSingleObject(t->thread_handle, 3000);
        CloseHandle(t->thread_handle);
        t->thread_handle = NULL;
    }
    if (t->stop_event) {
        CloseHandle(t->stop_event);
        t->stop_event = NULL;
    }
#else
    pthread_mutex_lock(&t->mtx);
    t->stop_flag = 1;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->mtx);
    pthread_join(t->thread, NULL);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 *
 * The syscall numbers below are the real, non-colliding lv2 values (see
 * lv2_syscall_table.h).
 * -----------------------------------------------------------------------*/
void sys_timer_init(lv2_syscall_table* tbl)
{
    memset(g_sys_timers, 0, sizeof(g_sys_timers));

#ifdef _WIN32
    ensure_qpc_init();
#endif

    lv2_syscall_register(tbl, SYS_TIMER_CREATE,                   sys_timer_create);
    lv2_syscall_register(tbl, SYS_TIMER_DESTROY,                  sys_timer_destroy);
    lv2_syscall_register(tbl, SYS_TIMER_START,                    sys_timer_start);
    lv2_syscall_register(tbl, SYS_TIMER_STOP,                     sys_timer_stop);
    lv2_syscall_register(tbl, SYS_TIMER_CONNECT_EVENT_QUEUE,      sys_timer_connect_event_queue);
    lv2_syscall_register(tbl, SYS_TIMER_DISCONNECT_EVENT_QUEUE,   sys_timer_disconnect_event_queue);

    lv2_syscall_register(tbl, SYS_TIME_GET_TIMEBASE_FREQUENCY, sys_time_get_timebase_frequency);
    lv2_syscall_register(tbl, SYS_TIMER_USLEEP,            sys_timer_usleep);
    lv2_syscall_register(tbl, SYS_TIMER_SLEEP,             sys_timer_sleep);
    lv2_syscall_register(tbl, SYS_TIME_GET_CURRENT_TIME,   sys_time_get_current_time);
}
