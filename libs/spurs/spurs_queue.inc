/* Queues and lock-free queues.
 * Part of cellSpurs.c, which includes its parts once, in order. */

/* ------------------------------------------------------------------------- */
/* Queue and LFQueue                                                         */

typedef struct {
    SyncKey sync;
    u8* buffer;
    void* owner;
    u32 element_size, depth, direction;
    u32 iwl;
    int kind;
    u64 watch_state[2];
} QueueState;
static QueueState g_queues[MAX_QUEUES];

enum {
    QUEUE_KIND_SPURS = 1,
    QUEUE_KIND_LF = 2
};

static QueueState* queue_find(void* key)
{
    for (u32 i = 0; i < MAX_QUEUES; ++i)
        if (g_queues[i].sync.live && g_queues[i].sync.key == key) return &g_queues[i];
    return NULL;
}
static QueueState* queue_make(void* key)
{
    registry_init();
    mx_lock(&g_registry_mutex);
    QueueState* q = queue_find(key);
    if (!q) for (u32 i = 0; i < MAX_QUEUES; ++i) if (!g_queues[i].sync.live) {
        q = &g_queues[i]; memset(q, 0, sizeof(*q)); sync_init(&q->sync, key); break;
    }
    mx_unlock(&g_registry_mutex);
    return q;
}


static TasksetState* lfqueue_waiter_taskset(const QueueState* q, u16 token)
{
    if (!q->iwl) return taskset_find(q->owner);
    const u32 wid = token >> 8;
    for (u32 i = 0; i < MAX_TASKSETS; ++i) {
        TasksetState* ts = &g_tasksets[i];
        if (ts->sync.live && ts->spurs == q->owner && ts->wid == wid)
            return ts;
    }
    return NULL;
}

static void lfqueue_signal_one_waiter(const QueueState* q, const void* object,
                                      u32 wait_list_offset)
{
    const u8* bytes = (const u8*)object;
    if (!rd16(bytes + wait_list_offset)) return;
    for (u32 slot = 0; slot < 15; ++slot) {
        const u16 token = rd16(bytes + wait_list_offset + 2 + slot * 2);
        if (!token) continue;
        const u32 task_id = token & 0xffu;
        TasksetState* ts = lfqueue_waiter_taskset(q, token);
        if (!ts || task_id >= CELL_SPURS_MAX_TASK) continue;
        mx_lock(&ts->sync.mutex);
        TaskState* task = &ts->tasks[task_id];
        /* The signal is latched, so wake the task whether or not it has
         * reached WAITING yet. The SPU consumer publishes its wait token
         * before entering SPURS WAITING; a push in that window that skipped
         * the slot would drop the wake permanently. */
        if (task->thread_valid && !task->complete) {
            task->signalled = 1;
            task_bit(ts->sync.key, 0x40, task_id, 1);
            cv_wake_all(&ts->sync.cond);
            mx_unlock(&ts->sync.mutex);
            return;
        }
        mx_unlock(&ts->sync.mutex);
    }
}

static void queue_notify_range_locked(const void* address, u32 size)
{
    if (!size) return;
    const u32 first = guest_ea(address) & ~127u;
    const u32 last = (guest_ea(address) + size - 1) & ~127u;
    for (u32 line = first; ; line += 128) {
        if (spu_coh_is_reserved(line)) spu_coh_notify_write(line);
        if (line == last) break;
    }
}

static void queue_notify_guest_write(u32 ea, u32 size, const u64 routed[2])
{
    const u64 first = ea;
    const u64 last = first + size;
    for (u32 half = 0; half < 2; ++half) {
        u64 mask = routed[half];
        while (mask) {
            const u32 i = half * 64u + guest_write_take_bit64(&mask);
            QueueState* q = &g_queues[i];
            if (!q->sync.live || !q->sync.key) continue;
            const u64 object_ea = guest_ea(q->sync.key);
            if (first >= object_ea + 16u || last <= object_ea) continue;
            mx_lock(&q->sync.mutex);
            const u8* object = (const u8*)q->sync.key;
            const u64 state0 = rd64(object + 0x00);
            const u64 state1 = rd64(object + 0x08);
            if (state0 != q->watch_state[0] ||
                state1 != q->watch_state[1]) {
                q->watch_state[0] = state0;
                q->watch_state[1] = state1;
                cv_wake_all(&q->sync.cond);
            }
            mx_unlock(&q->sync.mutex);
        }
    }
}

static s32 spurs_queue_init(void* owner, void* spurs, void* taskset,
                            void* object, const void* buffer, u32 size,
                            u32 depth, u32 direction)
{
    if (!owner || !object || !buffer) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(object, 128) || !aligned(buffer, 16))
        return CELL_SPURS_TASK_ERROR_ALIGN;
    if (!size || (size & 15) || !depth || depth >= 0x4000 || direction > 2)
        return CELL_SPURS_TASK_ERROR_INVAL;
    QueueState* q = queue_make(object);
    if (!q) return CELL_SPURS_TASK_ERROR_NOMEM;
    mx_lock(&q->sync.mutex);
    q->buffer = (u8*)buffer;
    q->owner = owner;
    q->element_size = size;
    q->depth = depth;
    q->direction = direction;
    q->iwl = 0;
    q->kind = QUEUE_KIND_SPURS;
    memset(object, 0, 128);
    wr32((u8*)object + 0x00, 0); /* completed consumer pointer */
    wr32((u8*)object + 0x04, 0); /* completed producer pointer */
    wr32((u8*)object + 0x08, size);
    wr32((u8*)object + 0x0c, depth);
    wr64((u8*)object + 0x10, guest_ea(buffer));
    wr32((u8*)object + 0x18, 0xffffffffu);
    wr32((u8*)object + 0x1c, direction);
    wr64((u8*)object + 0x60, guest_ea(taskset));
    wr64((u8*)object + 0x68, guest_ea(spurs));
    q->watch_state[0] = rd64((u8*)object + 0x00);
    q->watch_state[1] = rd64((u8*)object + 0x08);
    {
        const u32 index = (u32)(q - g_queues);
        guest_write_route_begin_target(GUEST_WRITE_ROUTE_QUEUE, index);
        guest_write_route_watch(GUEST_WRITE_ROUTE_QUEUE, index,
                                guest_ea(object), 16u);
    }
    mx_unlock(&q->sync.mutex);
    return CELL_OK;
}

s32 _cellSpursQueueInitialize(CellSpurs* s, CellSpursTaskset* ts, CellSpursQueue* q,
                              const void* b, u32 size, u32 depth, u32 dir)
{
    if (!s && !ts) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    void* spurs = s;
    if (!spurs && ts && vm_base) {
        const u32 ea = (u32)rd64((const u8*)ts + 0x60);
        if (ea) spurs = vm_base + ea;
    }
    return spurs_queue_init(s ? (void*)s : (void*)ts, spurs, ts,
                            q, b, size, depth, dir);
}

static s32 lf_queue_init(void* owner_raw, void* object, const void* buffer,
                         u32 size, u32 depth, u32 direction)
{
    if (!owner_raw || !object || !buffer) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(object, 128) || !aligned(buffer, 16))
        return CELL_SPURS_TASK_ERROR_ALIGN;
    if (!size || size > 0x4000 || (size & 15) || !depth ||
        depth > 0x7fff || direction > 3)
        return CELL_SPURS_TASK_ERROR_INVAL;

    const uintptr_t raw = (uintptr_t)owner_raw;
    const u32 iwl = (u32)(raw & 1);
    void* owner = (void*)(raw & ~(uintptr_t)1);
    QueueState* q = queue_make(object);
    if (!q) return CELL_SPURS_TASK_ERROR_NOMEM;
    mx_lock(&q->sync.mutex);
    q->buffer = (u8*)buffer;
    q->owner = owner;
    q->element_size = size;
    q->depth = depth;
    q->direction = direction;
    q->iwl = iwl;
    q->kind = QUEUE_KIND_LF;

    memset(object, 0, 128);
    wr32((u8*)object + 0x10, size);
    wr32((u8*)object + 0x14, depth);
    wr64((u8*)object + 0x18, guest_ea(buffer) | (direction == 3 ? 1u : 0u));
    memset((u8*)object + 0x20, 0xff, 4);
    wr32((u8*)object + 0x24, direction);
    if (direction == 3) {
        wr32((u8*)object + 0x28, 0xffffffffu);
        wr16((u8*)object + 0x30, 0xffff);
        wr16((u8*)object + 0x50, 0xffff);
    }
    wr64((u8*)object + 0x70, guest_ea(owner) | iwl);
    q->watch_state[0] = rd64((u8*)object + 0x00);
    q->watch_state[1] = rd64((u8*)object + 0x08);
    {
        const u32 index = (u32)(q - g_queues);
        guest_write_route_begin_target(GUEST_WRITE_ROUTE_QUEUE, index);
        guest_write_route_watch(GUEST_WRITE_ROUTE_QUEUE, index,
                                guest_ea(object), 16u);
    }
    mx_unlock(&q->sync.mutex);
    return CELL_OK;
}

s32 _cellSpursLFQueueInitialize(void* owner, CellSpursLFQueue* q, const void* b,
                                u32 size, u32 depth, u32 dir)
{ return lf_queue_init(owner, q, b, size, depth, dir); }

static int spurs_queue_snapshot(const QueueState* q, const void* object,
                                u32* head, u32* tail, u32* count)
{
    s32 raw_head = (s32)rd32((const u8*)object + 0x00);
    s32 raw_tail = (s32)rd32((const u8*)object + 0x04);
    const u32 period = q->depth * 2;
    /* A negative pointer word is the one's-complement "peer parked here"
     * encoding, not a transient reservation; the real value is ~v. Treating
     * it as retry-later would make every push/pop spin forever once an
     * SPU-side peer parked. */
    if (raw_head < 0) raw_head = (s32)~(u32)raw_head;
    if (raw_tail < 0) raw_tail = (s32)~(u32)raw_tail;
    if ((u32)raw_head >= period || (u32)raw_tail >= period) return -1;
    *head = (u32)raw_head;
    *tail = (u32)raw_tail;
    *count = (*tail + period - *head) % period;
    return *count <= q->depth ? 1 : -1;
}

static s32 spurs_queue_push(QueueState* q, void* object, const void* data,
                            int blocking)
{
    if (q->direction != 2) return CELL_SPURS_TASK_ERROR_PERM;
    mx_lock(&q->sync.mutex);
    for (;;) {
        u32 head, tail, count;
        spu_lockline_lock();
        const int state = spurs_queue_snapshot(q, object, &head, &tail, &count);
        if (state < 0) {
            spu_lockline_unlock();
            mx_unlock(&q->sync.mutex);
            return CELL_SPURS_TASK_ERROR_STAT;
        }
        if (state > 0 && count < q->depth) {
            const u32 slot = tail % q->depth;
            memcpy(q->buffer + slot * q->element_size, data, q->element_size);
            atomic_thread_fence(memory_order_release);
            wr32((u8*)object + 0x04, (tail + 1) % (q->depth * 2));
            queue_notify_range_locked(q->buffer + slot * q->element_size,
                                      q->element_size);
            queue_notify_range_locked(object, 128);
            spu_lockline_unlock();
            /* On the empty->non-empty transition, a consumer
             * task that parked itself in the queue object (has-waiter flag
             * at +0x20, task id at +0x21, workload id at +0x41, taskset EA
             * at +0x60) receives _cellSpursSendSignal. The lifted SPU-side
             * queue routine writes that record before entering WAIT_SIGNAL;
             * without the signal the consumer sleeps forever while the
             * producer's memory looks perfectly intact. */
            {
                const u8* obj = (const u8*)object;
                if (count == 0 && obj[0x20]) {
                    const u64 ts_ea = rd64(obj + 0x60);
                    if (ts_ea && vm_base) {
                        _cellSpursSendSignal(
                            (CellSpursTaskset*)(vm_base + (u32)ts_ea),
                            obj[0x21]);
                    }
                }
            }
            cv_wake_all(&q->sync.cond);
            mx_unlock(&q->sync.mutex);
            return CELL_OK;
        }
        spu_lockline_unlock();
        if (!blocking) {
            mx_unlock(&q->sync.mutex);
            return CELL_SPURS_TASK_ERROR_AGAIN;
        }
        /* Timed: the peer side may be lifted SPU code mutating the guest
         * object without an HLE wake (cv_wait_ms). */
        cv_wait_ms(&q->sync.cond, &q->sync.mutex, 2u);
    }
}

static s32 spurs_queue_pop(QueueState* q, void* object, void* data,
                           int peek, int blocking)
{
    if (q->direction != 1) return CELL_SPURS_TASK_ERROR_PERM;
    mx_lock(&q->sync.mutex);
    for (;;) {
        u32 head, tail, count;
        spu_lockline_lock();
        const int state = spurs_queue_snapshot(q, object, &head, &tail, &count);
        if (state < 0) {
            spu_lockline_unlock();
            mx_unlock(&q->sync.mutex);
            return CELL_SPURS_TASK_ERROR_STAT;
        }
        if (state > 0 && count) {
            const u32 slot = head % q->depth;
            atomic_thread_fence(memory_order_acquire);
            memcpy(data, q->buffer + slot * q->element_size, q->element_size);
            if (!peek) {
                wr32((u8*)object + 0x00, (head + 1) % (q->depth * 2));
                queue_notify_range_locked(object, 128);
            }
            spu_lockline_unlock();
            /* Producer wake on the full->not-full edge. The producer waiter
             * record lives at bytes 0x30/0x31/0x51, a separate record from
             * the consumer's 0x20/0x21/0x41 the push side reads. Spurious
             * signals are safe (latched; predicates re-checked). */
            if (!peek) {
                const u8* obj = (const u8*)object;
                if (count == q->depth && obj[0x30]) {
                    const u64 ts_ea = rd64(obj + 0x60);
                    if (ts_ea && vm_base) {
                        _cellSpursSendSignal(
                            (CellSpursTaskset*)(vm_base + (u32)ts_ea),
                            obj[0x31]);
                    }
                }
            }
            cv_wake_all(&q->sync.cond);
            mx_unlock(&q->sync.mutex);
            return CELL_OK;
        }
        spu_lockline_unlock();
        if (!blocking) {
            mx_unlock(&q->sync.mutex);
            return CELL_SPURS_TASK_ERROR_AGAIN;
        }
        /* Timed: the peer side may be lifted SPU code mutating the guest
         * object without an HLE wake (cv_wait_ms). */
        cv_wait_ms(&q->sync.cond, &q->sync.mutex, 2u);
    }
}

s32 cellSpursQueuePushBody(CellSpursQueue* q, const void* d, u32 block)
{
    QueueState* state = queue_find(q);
    if (!q || !d) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!state || state->kind != QUEUE_KIND_SPURS) return CELL_SPURS_TASK_ERROR_STAT;
    return spurs_queue_push(state, q, d, block != 0);
}
s32 cellSpursQueuePopBody(CellSpursQueue* q, void* d, u32 peek, u32 block)
{
    QueueState* state = queue_find(q);
    if (!q || !d) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!state || state->kind != QUEUE_KIND_SPURS) return CELL_SPURS_TASK_ERROR_STAT;
    return spurs_queue_pop(state, q, d, peek != 0, block != 0);
}

static u16 lf_advance(u16 pointer, u32 depth)
{
    return (u16)(((u32)pointer + 1 >= depth * 2) ? 0 : pointer + 1);
}

static s32 lf_queue_push(QueueState* q, void* object, const void* data,
                         int blocking)
{
    if (q->direction == 3) {
        diag("lfqueue-any2any-unsupported", object, q->direction);
        return CELL_SPURS_TASK_ERROR_NOSYS;
    }
    if (q->direction != 2) return CELL_SPURS_TASK_ERROR_PERM;
    mx_lock(&q->sync.mutex);
#if defined(YZ_SPURS_LOCK_ORDER_TEST)
    /* Test-only rendezvous: hold the queue mutex immediately before this
     * path requests the SPU lockline, reproducing the production lock edge. */
    {
        extern void yz_spurs_lock_order_test_queue_mutex_held(void);
        yz_spurs_lock_order_test_queue_mutex_held();
    }
#endif
    for (;;) {
        spu_lockline_lock();
        const u16 consumed = rd16((const u8*)object + 0x00);
        const u16 completed = rd16((const u8*)object + 0x08);
        const u16 reserved = rd16((const u8*)object + 0x0e);
        const u32 period = q->depth * 2;
        const int valid = consumed < period && completed < period &&
                          reserved < period;
        const u32 count = valid ? (reserved + period - consumed) % period : 0;
        if (!valid || count > q->depth) {
            spu_lockline_unlock();
            mx_unlock(&q->sync.mutex);
            return CELL_SPURS_TASK_ERROR_STAT;
        }
        if (reserved == completed && count < q->depth) {
            const u32 slot = reserved % q->depth;
            const u16 next = lf_advance(reserved, q->depth);
            memcpy(q->buffer + slot * q->element_size, data, q->element_size);
            atomic_thread_fence(memory_order_release);
            wr16((u8*)object + 0x08, next);
            wr16((u8*)object + 0x0a, 0);
            wr16((u8*)object + 0x0e, next);
            queue_notify_range_locked(q->buffer + slot * q->element_size,
                                      q->element_size);
            queue_notify_range_locked(object, 128);
            spu_lockline_unlock();
            /* A blocking SPU pop publishes its workload/task token in the
             * consumer wait list before entering SPURS WAITING.  Completing
             * a PPU-to-SPU push wakes exactly one published consumer; the
             * resumed queue routine then claims the now-visible entry and
             * retires its own wait record. */
            lfqueue_signal_one_waiter(q, object, 0x30);
            cv_wake_all(&q->sync.cond);
            mx_unlock(&q->sync.mutex);
            return CELL_OK;
        }
        spu_lockline_unlock();
        if (!blocking) {
            mx_unlock(&q->sync.mutex);
            return CELL_SPURS_TASK_ERROR_AGAIN;
        }
        /* Timed: the peer side may be lifted SPU code mutating the guest
         * object without an HLE wake (cv_wait_ms). */
        cv_wait_ms(&q->sync.cond, &q->sync.mutex, 2u);
    }
}

s32 _cellSpursLFQueuePushBody(CellSpursLFQueue* q, const void* d, u32 block)
{
    QueueState* state = queue_find(q);
    if (!q || !d) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!state || state->kind != QUEUE_KIND_LF) return CELL_SPURS_TASK_ERROR_STAT;
    return lf_queue_push(state, q, d, block != 0);
}
s32 cellSpursQueueAttachLv2EventQueue(CellSpursQueue* q)
{ QueueState* s = queue_find(q); return s && s->kind == QUEUE_KIND_SPURS ? CELL_OK : CELL_SPURS_TASK_ERROR_STAT; }
s32 cellSpursQueueDetachLv2EventQueue(CellSpursQueue* q)
{ QueueState* s = queue_find(q); return s && s->kind == QUEUE_KIND_SPURS ? CELL_OK : CELL_SPURS_TASK_ERROR_STAT; }
s32 cellSpursLFQueueAttachLv2EventQueue(CellSpursLFQueue* q)
{ QueueState* s = queue_find(q); return s && s->kind == QUEUE_KIND_LF ? CELL_OK : CELL_SPURS_TASK_ERROR_STAT; }
s32 cellSpursLFQueueDetachLv2EventQueue(CellSpursLFQueue* q)
{ QueueState* s = queue_find(q); return s && s->kind == QUEUE_KIND_LF ? CELL_OK : CELL_SPURS_TASK_ERROR_STAT; }
s32 cellSpursBarrierInitialize(CellSpursTaskset* ts, CellSpursBarrier* b, u32 total)
{
    if (!ts || !b) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(b, 128)) return CELL_SPURS_TASK_ERROR_ALIGN;
    if (!total || total > 128) return CELL_SPURS_TASK_ERROR_INVAL;
    memset(b->bytes, 0, 128);
    wr32(b->bytes + 0x04, total);
    wr32(b->bytes + 0x34, guest_ea(ts));
    return CELL_OK;
}
