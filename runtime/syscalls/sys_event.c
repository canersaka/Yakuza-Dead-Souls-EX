/*
 * ps3recomp - Event queue and event flag syscalls (implementation)
 */

#include "sys_event.h"
#include "sys_mutex.h"   /* SYS_SYNC_FIFO / SYS_SYNC_PRIORITY (audit sec.5 item 3, queue-create validation) */
#include "../memory/vm.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* t1-unblock diagnostic (env YZ_T1_UNBLOCK, 2026-07-04, DIAGNOSTIC ONLY --
 * see docs/FLAGS.md; NOT a shipping fix, LESSONS #13). Companion to the
 * sys_semaphore.c lever of the same name -- see that file for the rationale.
 * Makes t1's sys_event_queue_receive return immediately (CELL_OK, zeroed
 * event regs) instead of parking when the queue is empty, so t1 can barrel
 * through its whole SPURS-wait chain to scope how deep it goes. REMOVE with
 * the t1-wedge frontier. */
static int t1_unblock_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("YZ_T1_UNBLOCK"); on = (e && *e) ? 1 : 0; }
    return on;
}
extern uint32_t yz_thread_current_id(void);

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_event_queue_info g_sys_event_queues[SYS_EVENT_QUEUE_MAX];
sys_event_port_info  g_sys_event_ports[SYS_EVENT_PORT_MAX];
sys_event_flag_info  g_sys_event_flags[SYS_EVENT_FLAG_MAX];

/* Table lock for allocation */
#ifdef _WIN32
static CRITICAL_SECTION s_evt_table_lock;
static int              s_evt_table_lock_init = 0;
#else
static pthread_mutex_t  s_evt_table_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void evt_table_lock(void)
{
#ifdef _WIN32
    if (!s_evt_table_lock_init) {
        InitializeCriticalSection(&s_evt_table_lock);
        s_evt_table_lock_init = 1;
    }
    EnterCriticalSection(&s_evt_table_lock);
#else
    pthread_mutex_lock(&s_evt_table_lock);
#endif
}

static void evt_table_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_evt_table_lock);
#else
    pthread_mutex_unlock(&s_evt_table_lock);
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

static uint64_t bswap64(uint64_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    return ((v >> 56) & 0xFFULL) |
           ((v >> 40) & 0xFF00ULL) |
           ((v >> 24) & 0xFF0000ULL) |
           ((v >>  8) & 0xFF000000ULL) |
           ((v <<  8) & 0xFF00000000ULL) |
           ((v << 24) & 0xFF0000000000ULL) |
           ((v << 40) & 0xFF000000000000ULL) |
           ((v << 56) & 0xFF00000000000000ULL);
#else
    return v;
#endif
}

/* =========================================================================
 * EVENT QUEUES
 * ========================================================================= */

/* ---------------------------------------------------------------------------
 * sys_event_queue_create
 *
 * r3 = pointer to receive queue ID (u32*)
 * r4 = pointer to attribute struct
 * r5 = key (u64)
 * r6 = size (s32)
 * -----------------------------------------------------------------------*/
int64_t sys_event_queue_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr   = LV2_ARG_PTR(ctx, 1);
    uint64_t key         = LV2_ARG_U64(ctx, 2);
    int32_t  size        = LV2_ARG_S32(ctx, 3);

    /* Audit sec.5 item 3 (2026-07-03, user-confirmed; RPCS3
     * sys_event.cpp:229-247): size must be in [1,127] -- EINVAL, not a
     * silent clamp to the max. Protocol/type are read from the attribute
     * struct up front (before any slot is allocated, matching RPCS3's
     * validate-then-create order) and must be one of the documented values,
     * else EINVAL. */
    if (size <= 0 || size > SYS_EVENT_QUEUE_BUF_MAX)
        return (int64_t)(int32_t)CELL_EINVAL;

    uint32_t protocol = SYS_SYNC_FIFO;
    uint32_t qtype     = SYS_PPU_QUEUE;
    uint8_t  name_buf[8];
    memset(name_buf, 0, sizeof(name_buf));

    if (attr_addr != 0) {
        uint8_t* attr_raw = (uint8_t*)vm_to_host(attr_addr);
        uint32_t proto_be;
        memcpy(&proto_be, attr_raw, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        proto_be = ((proto_be >> 24) & 0xFF) | ((proto_be >> 8) & 0xFF00) |
                   ((proto_be << 8) & 0xFF0000) | ((proto_be << 24) & 0xFF000000u);
#endif
        protocol = proto_be;

        uint32_t type_be;
        memcpy(&type_be, attr_raw + 4, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        type_be = ((type_be >> 24) & 0xFF) | ((type_be >> 8) & 0xFF00) |
                  ((type_be << 8) & 0xFF0000) | ((type_be << 24) & 0xFF000000u);
#endif
        qtype = type_be;

        memcpy(name_buf, attr_raw + 8, 8);
    }

    if (protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_PRIORITY)
        return (int64_t)(int32_t)CELL_EINVAL;
    if (qtype != SYS_PPU_QUEUE && qtype != SYS_SPU_QUEUE)
        return (int64_t)(int32_t)CELL_EINVAL;

    evt_table_lock();

    int slot = -1;
    for (int i = 0; i < SYS_EVENT_QUEUE_MAX; i++) {
        if (!g_sys_event_queues[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_event_queue_info* q = &g_sys_event_queues[slot];
    memset(q, 0, sizeof(*q));
    q->active   = 1;
    q->key      = key;
    q->capacity = size;
    q->head     = 0;
    q->tail     = 0;
    q->count    = 0;
    q->protocol = protocol;
    q->type     = (int32_t)qtype;
    memcpy(q->name, name_buf, 8);

#ifdef _WIN32
    InitializeCriticalSection(&q->lock);
    InitializeConditionVariable(&q->not_empty);
#else
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
#endif

    uint32_t queue_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, queue_id);
    }

    evt_table_unlock();
    return CELL_OK;
}

int64_t sys_event_queue_destroy(ppu_context* ctx)
{
    uint32_t queue_id = LV2_ARG_U32(ctx, 0);
    int32_t  mode      = LV2_ARG_S32(ctx, 1);

    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* Audit sec.5 item 4 (2026-07-03, user-confirmed; RPCS3
     * sys_event.cpp:273): mode must be 0 or SYS_EVENT_QUEUE_DESTROY_FORCE. */
    if (mode && mode != SYS_EVENT_QUEUE_DESTROY_FORCE)
        return (int64_t)(int32_t)CELL_EINVAL;

    evt_table_lock();

    sys_event_queue_info* q = &g_sys_event_queues[queue_id - 1];
    if (!q->active) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    /* Mark inactive and WAKE any blocked receivers so they observe !active and
     * return ESRCH (or ECANCELED under FORCE, see below) instead of hanging
     * forever on `not_empty` (lost-wakeup / teardown deadlock). Do it under
     * the per-queue lock so the wake can't race a receiver entering the wait.
     * Deliberately do NOT Delete/destroy the CRITICAL_SECTION / condition
     * variable here: a receiver may still be unwinding out of
     * SleepConditionVariableCS and needs them valid (destroying them under a
     * live waiter is UB). The slot's primitives are re-Initialized (after a
     * memset) when the slot is reused by sys_event_queue_create, so leaking
     * them is benign on this bounded, reused 128-slot table.
     *
     * Audit sec.5 item 4: without FORCE, a queue with parked waiters must
     * fail with CELL_EBUSY and NOT be torn down (RPCS3 sys_event.cpp:273
     * checks `!mode && head` on the internal wait queue -- our `waiters`
     * counter is the equivalent). With FORCE, waiters are woken and made to
     * observe force_destroyed so they return CELL_ECANCELED instead of the
     * generic CELL_ESRCH. */
#ifdef _WIN32
    EnterCriticalSection(&q->lock);
    if (!mode && q->waiters > 0) {
        LeaveCriticalSection(&q->lock);
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_EBUSY;
    }
    q->active = 0;
    if (mode == SYS_EVENT_QUEUE_DESTROY_FORCE) q->force_destroyed = 1;
    WakeAllConditionVariable(&q->not_empty);
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
    if (!mode && q->waiters > 0) {
        pthread_mutex_unlock(&q->lock);
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_EBUSY;
    }
    q->active = 0;
    if (mode == SYS_EVENT_QUEUE_DESTROY_FORCE) q->force_destroyed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
#endif

    evt_table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_event_queue_receive
 *
 * r3 = queue_id
 * r4 = pointer to sys_event_t in guest memory
 * r5 = timeout_usec (0 = infinite)
 * -----------------------------------------------------------------------*/
int64_t sys_event_queue_receive(ppu_context* ctx)
{
    uint32_t queue_id    = LV2_ARG_U32(ctx, 0);
    uint32_t event_addr  = LV2_ARG_PTR(ctx, 1);
    uint64_t timeout_us  = LV2_ARG_U64(ctx, 2);

    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_queue_info* q = &g_sys_event_queues[queue_id - 1];
    if (!q->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* YZ_T1_UNBLOCK: if t1 would block here (queue empty), return CELL_OK
     * immediately with a zeroed event instead of parking. Cap logging at
     * ~100 lines but let the forcing continue unbounded. */
    if (t1_unblock_on() && yz_thread_current_id() == 1) {
        int would_block;
#ifdef _WIN32
        EnterCriticalSection(&q->lock);
        would_block = (q->count == 0);
        LeaveCriticalSection(&q->lock);
#else
        pthread_mutex_lock(&q->lock);
        would_block = (q->count == 0);
        pthread_mutex_unlock(&q->lock);
#endif
        if (would_block) {
            static long n = 0; long c = ++n;
            if (c <= 100) {
                fprintf(stderr, "[t1-unblock] eq_recv q=%u forced\n", queue_id);
                fflush(stderr);
            }
            ctx->gpr[4] = 0;
            ctx->gpr[5] = 0;
            ctx->gpr[6] = 0;
            ctx->gpr[7] = 0;
            if (event_addr != 0) {
                uint64_t* out = (uint64_t*)vm_to_host(event_addr);
                out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
            }
            return CELL_OK;
        }
    }

#ifdef _WIN32
    EnterCriticalSection(&q->lock);
    q->waiters++;   /* audit sec.5 item 4: destroy's EBUSY check needs this */

    if (timeout_us == 0) {
        while (q->count == 0 && q->active) {
            SleepConditionVariableCS(&q->not_empty, &q->lock, INFINITE);
        }
    } else {
        DWORD ms = (DWORD)(timeout_us / 1000);
        if (ms == 0) ms = 1;
        while (q->count == 0 && q->active) {
            if (!SleepConditionVariableCS(&q->not_empty, &q->lock, ms)) {
                if (GetLastError() == ERROR_TIMEOUT) {
                    q->waiters--;
                    LeaveCriticalSection(&q->lock);
                    return (int64_t)(int32_t)CELL_ETIMEDOUT;
                }
            }
        }
    }

    q->waiters--;

    if (!q->active || q->count == 0) {
        /* Audit sec.5 item 4: a FORCE destroy wakes waiters with
         * CELL_ECANCELED (RPCS3 sys_event.cpp:377/388); a plain teardown
         * that raced ahead of this waiter still reports CELL_ESRCH. */
        int64_t rc = q->force_destroyed ? (int64_t)(int32_t)CELL_ECANCELED
                                         : (int64_t)(int32_t)CELL_ESRCH;
        LeaveCriticalSection(&q->lock);
        return rc;
    }

    sys_event_t evt = q->buffer[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
    q->waiters++;

    if (timeout_us == 0) {
        while (q->count == 0 && q->active) {
            pthread_cond_wait(&q->not_empty, &q->lock);
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
        while (q->count == 0 && q->active) {
            int rc = pthread_cond_timedwait(&q->not_empty, &q->lock, &ts);
            if (rc == ETIMEDOUT) {
                q->waiters--;
                pthread_mutex_unlock(&q->lock);
                return (int64_t)(int32_t)CELL_ETIMEDOUT;
            }
        }
    }

    q->waiters--;

    if (!q->active || q->count == 0) {
        int64_t rc = q->force_destroyed ? (int64_t)(int32_t)CELL_ECANCELED
                                         : (int64_t)(int32_t)CELL_ESRCH;
        pthread_mutex_unlock(&q->lock);
        return rc;
    }

    sys_event_t evt = q->buffer[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    pthread_mutex_unlock(&q->lock);
#endif

    /* The lv2 ABI returns the event in GPRs r4-r7; the pointer arg is IGNORED
     * (RPCS3 names it `dummy_event` and writes the result to ppu.gpr[4..7],
     * sys_event.cpp:499). Sony's libgcm interrupt thread reads these registers
     * to decode the vblank/flip cause -- if they are not set it sees stale
     * garbage and never dispatches the game's handler (the render loop then
     * never advances). Set the registers; also keep the legacy buffer write
     * for any caller that reads the (non-standard) out-pointer. */
    ctx->gpr[4] = evt.source;
    ctx->gpr[5] = evt.data1;
    ctx->gpr[6] = evt.data2;
    ctx->gpr[7] = evt.data3;

    /* Legacy: also write event to guest memory in big-endian (harmless). */
    if (event_addr != 0) {
        uint64_t* out = (uint64_t*)vm_to_host(event_addr);
        out[0] = bswap64(evt.source);
        out[1] = bswap64(evt.data1);
        out[2] = bswap64(evt.data2);
        out[3] = bswap64(evt.data3);
    }

    return CELL_OK;
}

int64_t sys_event_queue_tryreceive(ppu_context* ctx)
{
    uint32_t queue_id   = LV2_ARG_U32(ctx, 0);
    uint32_t event_addr = LV2_ARG_PTR(ctx, 1);
    int32_t  max_count  = LV2_ARG_S32(ctx, 2);
    uint32_t count_addr = LV2_ARG_PTR(ctx, 3);

    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_queue_info* q = &g_sys_event_queues[queue_id - 1];
    if (!q->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (max_count <= 0) max_count = 1;

    int32_t received = 0;

#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif

    while (received < max_count && q->count > 0) {
        sys_event_t evt = q->buffer[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;

        if (event_addr != 0) {
            uint64_t* out = (uint64_t*)vm_to_host(event_addr + (uint32_t)(received * 32));
            out[0] = bswap64(evt.source);
            out[1] = bswap64(evt.data1);
            out[2] = bswap64(evt.data2);
            out[3] = bswap64(evt.data3);
        }
        received++;
    }

#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif

    if (count_addr != 0) {
        write_be32(count_addr, (uint32_t)received);
    }

    return CELL_OK;
}

int64_t sys_event_queue_drain(ppu_context* ctx)
{
    uint32_t queue_id = LV2_ARG_U32(ctx, 0);

    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_queue_info* q = &g_sys_event_queues[queue_id - 1];
    if (!q->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    EnterCriticalSection(&q->lock);
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    pthread_mutex_unlock(&q->lock);
#endif

    return CELL_OK;
}

/* =========================================================================
 * EVENT PORTS
 * ========================================================================= */

/* Helper to enqueue an event into a queue */
static int event_queue_push(sys_event_queue_info* q, const sys_event_t* evt)
{
#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif

    if (q->count >= q->capacity) {
#ifdef _WIN32
        LeaveCriticalSection(&q->lock);
#else
        pthread_mutex_unlock(&q->lock);
#endif
        return -1; /* full */
    }

    q->buffer[q->tail] = *evt;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

#ifdef _WIN32
    WakeConditionVariable(&q->not_empty);
    LeaveCriticalSection(&q->lock);
#else
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
#endif

    return 0;
}

int64_t sys_event_port_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    int32_t  port_type   = LV2_ARG_S32(ctx, 1);
    uint64_t name        = LV2_ARG_U64(ctx, 2);

    evt_table_lock();

    int slot = -1;
    for (int i = 0; i < SYS_EVENT_PORT_MAX; i++) {
        if (!g_sys_event_ports[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_event_port_info* p = &g_sys_event_ports[slot];
    memset(p, 0, sizeof(*p));
    p->active = 1;
    p->type   = port_type;
    p->name   = name;
    p->connected_queue = 0;

    uint32_t port_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, port_id);
    }

    evt_table_unlock();
    return CELL_OK;
}

int64_t sys_event_port_destroy(ppu_context* ctx)
{
    uint32_t port_id = LV2_ARG_U32(ctx, 0);

    if (port_id == 0 || port_id > SYS_EVENT_PORT_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    evt_table_lock();

    sys_event_port_info* p = &g_sys_event_ports[port_id - 1];
    if (!p->active) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    p->active = 0;
    evt_table_unlock();
    return CELL_OK;
}

int64_t sys_event_port_connect_local(ppu_context* ctx)
{
    uint32_t port_id  = LV2_ARG_U32(ctx, 0);
    uint32_t queue_id = LV2_ARG_U32(ctx, 1);

    if (port_id == 0 || port_id > SYS_EVENT_PORT_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;
    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    evt_table_lock();

    sys_event_port_info* p = &g_sys_event_ports[port_id - 1];
    if (!p->active) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (!g_sys_event_queues[queue_id - 1].active) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (p->connected_queue != 0) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_EISCONN;
    }

    p->connected_queue = (int32_t)queue_id;
    evt_table_unlock();
    return CELL_OK;
}

int64_t sys_event_port_disconnect(ppu_context* ctx)
{
    uint32_t port_id = LV2_ARG_U32(ctx, 0);

    if (port_id == 0 || port_id > SYS_EVENT_PORT_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    evt_table_lock();

    sys_event_port_info* p = &g_sys_event_ports[port_id - 1];
    if (!p->active) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (p->connected_queue == 0) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_ENOTCONN;
    }

    p->connected_queue = 0;
    evt_table_unlock();
    return CELL_OK;
}

/* Public helper for non-syscall callers: push an event into a queue by
 * ID. Returns 0 on success, -1 if the queue is unknown/inactive or full. */
int sys_event_queue_push_by_id(uint32_t queue_id,
                               uint64_t source, uint64_t data1,
                               uint64_t data2,  uint64_t data3)
{
    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX) return -1;
    sys_event_queue_info* q = &g_sys_event_queues[queue_id - 1];
    if (!q->active) return -1;
    sys_event_t evt;
    evt.source = source;
    evt.data1  = data1;
    evt.data2  = data2;
    evt.data3  = data3;
    return event_queue_push(q, &evt);
}

/* Public helper: resolve an event queue by its ipc_key (as registered at
 * sys_event_queue_create). Returns the queue_id (1-based) or 0 if none. Used by
 * cellAudio to route the audio-period notify event to the game's queue. */
unsigned int sys_event_find_queue_by_key(unsigned long long key)
{
    if (key == 0) return 0;
    for (int i = 0; i < SYS_EVENT_QUEUE_MAX; i++) {
        if (g_sys_event_queues[i].active && g_sys_event_queues[i].key == key)
            return (unsigned int)(i + 1);
    }
    return 0;
}

int64_t sys_event_port_send(ppu_context* ctx)
{
    uint32_t port_id = LV2_ARG_U32(ctx, 0);
    uint64_t data1   = LV2_ARG_U64(ctx, 1);
    uint64_t data2   = LV2_ARG_U64(ctx, 2);
    uint64_t data3   = LV2_ARG_U64(ctx, 3);

    if (port_id == 0 || port_id > SYS_EVENT_PORT_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_port_info* p = &g_sys_event_ports[port_id - 1];
    if (!p->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (p->connected_queue == 0)
        return (int64_t)(int32_t)CELL_ENOTCONN;

    int32_t qidx = p->connected_queue;
    if (qidx <= 0 || qidx > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_queue_info* q = &g_sys_event_queues[qidx - 1];
    if (!q->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* Audit sec.5 item 2 (2026-07-03, user-confirmed; RPCS3
     * sys_event.cpp:789): an unnamed port (name == 0, the common case --
     * sys_event_port_create's `name` arg is usually 0 and assigned only by
     * callers wanting a fixed identity) must synthesize its source as
     * (pid << 32) | port_id, not send a bare 0. Our process always reports
     * pid 1 (sys_process_getpid, lv2_register.c). Receivers that switch on
     * `source` (rather than blindly trusting whichever port fired) got 0
     * from every unnamed port and could not tell them apart. */
    sys_event_t evt;
    evt.source = p->name ? p->name : ((uint64_t)1 << 32) | (uint64_t)port_id;
    evt.data1  = data1;
    evt.data2  = data2;
    evt.data3  = data3;

    if (event_queue_push(q, &evt) < 0) {
        return (int64_t)(int32_t)CELL_EBUSY;
    }

    return CELL_OK;
}

/* =========================================================================
 * EVENT FLAGS
 * ========================================================================= */

int64_t sys_event_flag_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr   = LV2_ARG_PTR(ctx, 1);
    uint64_t init_pattern = LV2_ARG_U64(ctx, 2);

    evt_table_lock();

    int slot = -1;
    for (int i = 0; i < SYS_EVENT_FLAG_MAX; i++) {
        if (!g_sys_event_flags[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_event_flag_info* f = &g_sys_event_flags[slot];
    memset(f, 0, sizeof(*f));
    f->active  = 1;
    f->pattern = init_pattern;

    if (attr_addr != 0) {
        uint8_t* attr_raw = (uint8_t*)vm_to_host(attr_addr);
        uint32_t proto_be, type_be;
        memcpy(&proto_be, attr_raw, 4);
        memcpy(&type_be, attr_raw + 4, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        proto_be = ((proto_be >> 24) & 0xFF) | ((proto_be >> 8) & 0xFF00) |
                   ((proto_be << 8) & 0xFF0000) | ((proto_be << 24) & 0xFF000000u);
        type_be  = ((type_be >> 24) & 0xFF)  | ((type_be >> 8) & 0xFF00) |
                   ((type_be << 8) & 0xFF0000) | ((type_be << 24) & 0xFF000000u);
#endif
        f->protocol = proto_be;
        f->type     = type_be;
        memcpy(f->name, attr_raw + 8, 8);
    }

#ifdef _WIN32
    InitializeCriticalSection(&f->lock);
    InitializeConditionVariable(&f->cv);
#else
    pthread_mutex_init(&f->lock, NULL);
    pthread_cond_init(&f->cv, NULL);
#endif

    uint32_t flag_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, flag_id);
    }

    evt_table_unlock();
    return CELL_OK;
}

int64_t sys_event_flag_destroy(ppu_context* ctx)
{
    uint32_t flag_id = LV2_ARG_U32(ctx, 0);

    if (flag_id == 0 || flag_id > SYS_EVENT_FLAG_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    evt_table_lock();

    sys_event_flag_info* f = &g_sys_event_flags[flag_id - 1];
    if (!f->active) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

#ifdef _WIN32
    DeleteCriticalSection(&f->lock);
#else
    pthread_cond_destroy(&f->cv);
    pthread_mutex_destroy(&f->lock);
#endif

    f->active = 0;
    evt_table_unlock();
    return CELL_OK;
}

/* Check if wait condition is met */
static int flag_check(uint64_t pattern, uint64_t bitpat, uint32_t mode)
{
    if (mode & SYS_EVENT_FLAG_WAIT_AND) {
        return (pattern & bitpat) == bitpat;
    } else {
        /* OR mode */
        return (pattern & bitpat) != 0;
    }
}

/* ---------------------------------------------------------------------------
 * sys_event_flag_wait
 *
 * r3 = flag_id
 * r4 = bitpattern (u64)
 * r5 = mode (AND/OR + clear flags)
 * r6 = pointer to receive result pattern (u64*)
 * r7 = timeout_usec (0 = infinite)
 * -----------------------------------------------------------------------*/
int64_t sys_event_flag_wait(ppu_context* ctx)
{
    uint32_t flag_id    = LV2_ARG_U32(ctx, 0);
    uint64_t bitpat     = LV2_ARG_U64(ctx, 1);
    uint32_t mode       = LV2_ARG_U32(ctx, 2);
    uint32_t result_addr = LV2_ARG_PTR(ctx, 3);
    uint64_t timeout_us = LV2_ARG_U64(ctx, 4);

    if (flag_id == 0 || flag_id > SYS_EVENT_FLAG_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_flag_info* f = &g_sys_event_flags[flag_id - 1];
    if (!f->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (bitpat == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

#ifdef _WIN32
    EnterCriticalSection(&f->lock);

    if (timeout_us == 0) {
        while (!flag_check(f->pattern, bitpat, mode) && f->active) {
            SleepConditionVariableCS(&f->cv, &f->lock, INFINITE);
        }
    } else {
        DWORD ms = (DWORD)(timeout_us / 1000);
        if (ms == 0) ms = 1;
        while (!flag_check(f->pattern, bitpat, mode) && f->active) {
            if (!SleepConditionVariableCS(&f->cv, &f->lock, ms)) {
                if (GetLastError() == ERROR_TIMEOUT) {
                    /* Write current pattern even on timeout */
                    if (result_addr != 0) {
                        uint64_t* out = (uint64_t*)vm_to_host(result_addr);
                        *out = bswap64(f->pattern);
                    }
                    LeaveCriticalSection(&f->lock);
                    return (int64_t)(int32_t)CELL_ETIMEDOUT;
                }
            }
        }
    }

    uint64_t result = f->pattern;

    /* Clear matched bits if requested */
    if (mode & SYS_EVENT_FLAG_WAIT_CLEAR) {
        f->pattern &= ~bitpat;
    } else if (mode & SYS_EVENT_FLAG_WAIT_CLEAR_ALL) {
        f->pattern = 0;
    }

    if (result_addr != 0) {
        uint64_t* out = (uint64_t*)vm_to_host(result_addr);
        *out = bswap64(result);
    }

    LeaveCriticalSection(&f->lock);
#else
    pthread_mutex_lock(&f->lock);

    if (timeout_us == 0) {
        while (!flag_check(f->pattern, bitpat, mode) && f->active) {
            pthread_cond_wait(&f->cv, &f->lock);
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
        while (!flag_check(f->pattern, bitpat, mode) && f->active) {
            int rc = pthread_cond_timedwait(&f->cv, &f->lock, &ts);
            if (rc == ETIMEDOUT) {
                if (result_addr != 0) {
                    uint64_t* out = (uint64_t*)vm_to_host(result_addr);
                    *out = bswap64(f->pattern);
                }
                pthread_mutex_unlock(&f->lock);
                return (int64_t)(int32_t)CELL_ETIMEDOUT;
            }
        }
    }

    uint64_t result = f->pattern;

    if (mode & SYS_EVENT_FLAG_WAIT_CLEAR) {
        f->pattern &= ~bitpat;
    } else if (mode & SYS_EVENT_FLAG_WAIT_CLEAR_ALL) {
        f->pattern = 0;
    }

    if (result_addr != 0) {
        uint64_t* out = (uint64_t*)vm_to_host(result_addr);
        *out = bswap64(result);
    }

    pthread_mutex_unlock(&f->lock);
#endif

    return CELL_OK;
}

int64_t sys_event_flag_trywait(ppu_context* ctx)
{
    uint32_t flag_id     = LV2_ARG_U32(ctx, 0);
    uint64_t bitpat      = LV2_ARG_U64(ctx, 1);
    uint32_t mode        = LV2_ARG_U32(ctx, 2);
    uint32_t result_addr = LV2_ARG_PTR(ctx, 3);

    if (flag_id == 0 || flag_id > SYS_EVENT_FLAG_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_flag_info* f = &g_sys_event_flags[flag_id - 1];
    if (!f->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    EnterCriticalSection(&f->lock);
#else
    pthread_mutex_lock(&f->lock);
#endif

    if (!flag_check(f->pattern, bitpat, mode)) {
        if (result_addr != 0) {
            uint64_t* out = (uint64_t*)vm_to_host(result_addr);
            *out = bswap64(f->pattern);
        }
#ifdef _WIN32
        LeaveCriticalSection(&f->lock);
#else
        pthread_mutex_unlock(&f->lock);
#endif
        return (int64_t)(int32_t)CELL_EBUSY;
    }

    uint64_t result = f->pattern;

    if (mode & SYS_EVENT_FLAG_WAIT_CLEAR) {
        f->pattern &= ~bitpat;
    } else if (mode & SYS_EVENT_FLAG_WAIT_CLEAR_ALL) {
        f->pattern = 0;
    }

    if (result_addr != 0) {
        uint64_t* out = (uint64_t*)vm_to_host(result_addr);
        *out = bswap64(result);
    }

#ifdef _WIN32
    LeaveCriticalSection(&f->lock);
#else
    pthread_mutex_unlock(&f->lock);
#endif

    return CELL_OK;
}

/* Internal by-id setter: shared by the PPU syscall and the SPU
 * WrOutIntrMbox set_bit protocol (codes 128/192 -- RPCS3 SPUThread.cpp
 * :6090-6160: an SPU sets an lv2 event-flag bit by writing the flag id to
 * OutMbox and (0x80|0xC0)<<24 | bit to OutIntrMbox; Sony's SPURS taskset
 * exit handler uses the impatient form as its completion doorbell). */
int64_t sys_event_flag_set_by_id(uint32_t flag_id, uint64_t bitpat)
{
    if (flag_id == 0 || flag_id > SYS_EVENT_FLAG_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_flag_info* f = &g_sys_event_flags[flag_id - 1];
    if (!f->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    EnterCriticalSection(&f->lock);
    f->pattern |= bitpat;
    WakeAllConditionVariable(&f->cv);
    LeaveCriticalSection(&f->lock);
#else
    pthread_mutex_lock(&f->lock);
    f->pattern |= bitpat;
    pthread_cond_broadcast(&f->cv);
    pthread_mutex_unlock(&f->lock);
#endif

    return CELL_OK;
}

int64_t sys_event_flag_set(ppu_context* ctx)
{
    uint32_t flag_id = LV2_ARG_U32(ctx, 0);
    uint64_t bitpat  = LV2_ARG_U64(ctx, 1);

    return sys_event_flag_set_by_id(flag_id, bitpat);
}

int64_t sys_event_flag_clear(ppu_context* ctx)
{
    uint32_t flag_id = LV2_ARG_U32(ctx, 0);
    uint64_t bitpat  = LV2_ARG_U64(ctx, 1);

    if (flag_id == 0 || flag_id > SYS_EVENT_FLAG_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_flag_info* f = &g_sys_event_flags[flag_id - 1];
    if (!f->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    EnterCriticalSection(&f->lock);
    f->pattern &= bitpat;  /* PS3 clear: AND with bitpat (clear bits that are 0 in bitpat) */
    LeaveCriticalSection(&f->lock);
#else
    pthread_mutex_lock(&f->lock);
    f->pattern &= bitpat;
    pthread_mutex_unlock(&f->lock);
#endif

    return CELL_OK;
}

int64_t sys_event_flag_get(ppu_context* ctx)
{
    uint32_t flag_id  = LV2_ARG_U32(ctx, 0);
    uint32_t out_addr = LV2_ARG_PTR(ctx, 1);

    if (flag_id == 0 || flag_id > SYS_EVENT_FLAG_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_flag_info* f = &g_sys_event_flags[flag_id - 1];
    if (!f->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    uint64_t pattern;
#ifdef _WIN32
    EnterCriticalSection(&f->lock);
    pattern = f->pattern;
    LeaveCriticalSection(&f->lock);
#else
    pthread_mutex_lock(&f->lock);
    pattern = f->pattern;
    pthread_mutex_unlock(&f->lock);
#endif

    if (out_addr != 0) {
        uint64_t* out = (uint64_t*)vm_to_host(out_addr);
        *out = bswap64(pattern);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_event_init(lv2_syscall_table* tbl)
{
    memset(g_sys_event_queues, 0, sizeof(g_sys_event_queues));
    memset(g_sys_event_ports,  0, sizeof(g_sys_event_ports));
    memset(g_sys_event_flags,  0, sizeof(g_sys_event_flags));

#ifdef _WIN32
    if (!s_evt_table_lock_init) {
        InitializeCriticalSection(&s_evt_table_lock);
        s_evt_table_lock_init = 1;
    }
#endif

    /* Event queues */
    lv2_syscall_register(tbl, SYS_EVENT_QUEUE_CREATE,      sys_event_queue_create);
    lv2_syscall_register(tbl, SYS_EVENT_QUEUE_DESTROY,     sys_event_queue_destroy);
    lv2_syscall_register(tbl, SYS_EVENT_QUEUE_RECEIVE,     sys_event_queue_receive);
    lv2_syscall_register(tbl, SYS_EVENT_QUEUE_TRYRECEIVE,  sys_event_queue_tryreceive);
    lv2_syscall_register(tbl, SYS_EVENT_QUEUE_DRAIN,       sys_event_queue_drain);

    /* Event ports */
    lv2_syscall_register(tbl, SYS_EVENT_PORT_CREATE,        sys_event_port_create);
    lv2_syscall_register(tbl, SYS_EVENT_PORT_DESTROY,       sys_event_port_destroy);
    lv2_syscall_register(tbl, SYS_EVENT_PORT_CONNECT_LOCAL, sys_event_port_connect_local);
    lv2_syscall_register(tbl, SYS_EVENT_PORT_DISCONNECT,    sys_event_port_disconnect);
    lv2_syscall_register(tbl, SYS_EVENT_PORT_SEND,          sys_event_port_send);

    /* Event flags */
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_CREATE,   sys_event_flag_create);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_DESTROY,  sys_event_flag_destroy);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_WAIT,     sys_event_flag_wait);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_TRYWAIT,  sys_event_flag_trywait);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_SET,      sys_event_flag_set);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_CLEAR,    sys_event_flag_clear);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_GET,      sys_event_flag_get);
}
