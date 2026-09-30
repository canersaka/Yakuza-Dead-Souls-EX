/* Event flags.
 * Part of cellSpurs.c, which includes its parts once, in order. */

/* ------------------------------------------------------------------------- */
/* Event flags                                                               */

static SyncKey g_event_flags[MAX_EVENT_FLAGS];
/* Exact-write publication snapshot: event bits, SPU pending waiter bits,
 * and the PPU pending-receive byte.  Pending must be part of the snapshot:
 * CLEAR_AUTO setters can publish a task wake while leaving the event bits
 * unchanged. */
static u64 g_event_watch_state[MAX_EVENT_FLAGS];
static void queue_notify_guest_write(u32 ea, u32 size, const u64 mask[2]);
static void jobchain_notify_guest_write(u32 ea, u32 size, int ppu_store,
                                        u32 command_mask, u32 bridge_mask);
static void jobguard_notify_guest_write(u32 ea, u32 size, u64 mask);
static TasksetState* event_waiter_taskset(const CellSpursEventFlag* ef,
                                          u32 slot);

static u64 event_watch_snapshot(const u8* object)
{
    return (u64)rd16(object) |
           ((u64)rd16(object + 0x02) << 16) |
           ((u64)object[0x07] << 32);
}

#define EVENT_WATCH_EVENTS      0x000000000000ffffull
#define EVENT_WATCH_SPU_PENDING 0x00000000ffff0000ull
#define EVENT_WATCH_PPU_PENDING 0x000000ff00000000ull
#define EVENT_WATCH_ALL (EVENT_WATCH_EVENTS | EVENT_WATCH_SPU_PENDING | \
                         EVENT_WATCH_PPU_PENDING)

static void event_watch_refresh_fields_locked(SyncKey* sync, u64 fields)
{
    const u32 index = (u32)(sync - g_event_flags);
    const u64 current = event_watch_snapshot((const u8*)sync->key);
    g_event_watch_state[index] =
        (g_event_watch_state[index] & ~fields) | (current & fields);
}

static u32 guest_write_take_bit64(u64* mask)
{
    u32 bit = 0;
    u64 value = *mask;
    while (!(value & 1u)) {
        value >>= 1;
        ++bit;
    }
    *mask &= *mask - 1u;
    return bit;
}

static void taskset_notify_guest_write(u32 ea, u32 size, u64 mask)
{
    const u64 first = ea;
    const u64 last = first + size;
    while (mask) {
        const u32 i = guest_write_take_bit64(&mask);
        TasksetState* ts = &g_tasksets[i];
        if (!ts->sync.live || !ts->sync.key) continue;
        const u32 object_ea = guest_ea(ts->sync.key);
        const u64 signal_first = (u64)object_ea + 0x40u;
        const u64 signal_last = signal_first + 0x10u;
        if (first >= signal_last || last <= signal_first) continue;
        const u32 byte_first = first <= signal_first ? 0u :
            (u32)(first - signal_first);
        const u32 byte_last = last >= signal_last ? 0x10u :
            (u32)(last - signal_first);
        int wake = 0;
        mx_lock(&ts->sync.mutex);
        const u8* object = (const u8*)ts->sync.key;
        for (u32 byte = byte_first; byte < byte_last; ++byte) {
            const u8 signals = object[0x40u + byte];
            for (u32 lane = 0; lane < 8u; ++lane) {
                if (!(signals & (0x80u >> lane))) continue;
                const u32 task_id = byte * 8u + lane;
                TaskState* task = &ts->tasks[task_id];
                if (!task->thread_valid || task->complete || task->signalled)
                    continue;
                if (image4_intercept_signal_locked(ts, task_id)) {
                    /* The guest latch was already published before this
                     * observer ran.  Native completion consumes the same
                     * depth-one signal, so clear its guest bit exactly as the
                     * task-start path would; never leave a replayable wake. */
                    task_bit(ts->sync.key, 0x40, task_id, 0);
                    continue;
                }
                task->signalled = 1;
                wake = 1;
            }
        }
        if (wake) cv_wake_all(&ts->sync.cond);
        mx_unlock(&ts->sync.mutex);
    }
}

static void event_notify_guest_write(u32 ea, u32 size, const u64 routed[2])
{
    const u64 first = ea;
    const u64 last = first + size;
    for (u32 half = 0; half < 2; ++half) {
        u64 mask = routed[half];
        while (mask) {
            const u32 i = half * 64u + guest_write_take_bit64(&mask);
            SyncKey* sync = &g_event_flags[i];
            if (!sync->live || !sync->key) continue;
            const u32 object_ea = guest_ea(sync->key);
            if (first >= (u64)object_ea + 8u || last <= object_ea) continue;
            TasksetState* wake_tasksets[16] = {0};
            u8 wake_task_ids[16] = {0};
            u32 wake_count = 0;
            mx_lock(&sync->mutex);
            const u8* object = (const u8*)sync->key;
            const u64 previous = g_event_watch_state[i];
            const u64 state = event_watch_snapshot(object);
            u16 new_pending = 0;
            if (state != previous) {
                /* A lifted SPU EventFlagSet publishes completed task slots
                 * by setting pending bits at +0x02.  Waking only the PPU event
                 * condvar would never deliver the corresponding depth-one
                 * task signal, leaving the waiting task blocked until a broad
                 * user-command replay happened to signal every task.
                 * Only newly-pending exact slot edges are dispatched. */
                const u16 pending = (u16)(state >> 16);
                const u16 previous_pending = (u16)(previous >> 16);
                const u16 used = rd16(object + 0x08);
                new_pending =
                    (u16)(pending & (u16)~previous_pending & used);
                g_event_watch_state[i] = state;
                cv_wake_all(&sync->cond);
                for (u32 slot = 0; slot < 16; ++slot) {
                    if (!(new_pending & (0x8000u >> slot))) continue;
                    TasksetState* taskset = event_waiter_taskset(
                        (const CellSpursEventFlag*)object, slot);
                    if (!taskset) continue;
                    wake_tasksets[wake_count] = taskset;
                    wake_task_ids[wake_count] = object[0x50 + slot];
                    ++wake_count;
                }
            }
            mx_unlock(&sync->mutex);
            /* Never nest a taskset mutex below an event-flag mutex.  The
             * guest publication is already complete before the VM-write
             * hook reaches this point, and SendSignal is the SDK's
             * lost-wake-safe depth-one latch. */
            for (u32 wake = 0; wake < wake_count; ++wake)
                _cellSpursSendSignal(
                    (CellSpursTaskset*)wake_tasksets[wake]->sync.key,
                    wake_task_ids[wake]);
        }
    }
}

static _Atomic(uintptr_t) g_guest_write_observer;

static void cellSpursNotifyGuestWriteSource(u32 ea, u32 size, int ppu_store)
{
#if defined(_WIN32)
    const u32 idle_word=atomic_load_explicit(&g_gs_idle_wait_word,memory_order_acquire);
    if(idle_word && (u64)ea<(u64)idle_word+4 && (u64)ea+size>idle_word)
        WakeByAddressSingle(vm_base+idle_word);
#endif
    const uintptr_t observer_bits = atomic_load_explicit(
        &g_guest_write_observer, memory_order_acquire);
    if (observer_bits)
        ((CellSpursGuestWriteObserver)observer_bits)(ea, size);
    if (g_registry_ready != 2 || !vm_base || !size) return;
    GuestWriteTargets targets;
    if (!guest_write_route_lookup(ea, size, &targets)) return;
    if (targets.tasksets)
        taskset_notify_guest_write(ea, size, targets.tasksets);
    if (targets.events[0] || targets.events[1])
        event_notify_guest_write(ea, size, targets.events);
    if (targets.queues[0] || targets.queues[1])
        queue_notify_guest_write(ea, size, targets.queues);
    if (targets.jobguards)
        jobguard_notify_guest_write(ea, size, targets.jobguards);
    if (targets.jobchain_commands || targets.jobchain_bridges)
        jobchain_notify_guest_write(
            ea, size, ppu_store, targets.jobchain_commands,
            targets.jobchain_bridges);
}

void cellSpursSetGuestWriteObserver(CellSpursGuestWriteObserver observer)
{
    atomic_store_explicit(&g_guest_write_observer, (uintptr_t)observer,
                          memory_order_release);
}

void cellSpursNotifyGuestWrite(u32 ea, u32 size)
{
    cellSpursNotifyGuestWriteSource(ea, size, 0);
}

static SyncKey* sync_get(SyncKey* table, u32 count, void* key, int make)
{
    registry_init();
    mx_lock(&g_registry_mutex);
    SyncKey* result = NULL;
    for (u32 i = 0; i < count; ++i)
        if (table[i].live && table[i].key == key) { result = &table[i]; break; }
    if (!result && make) {
        for (u32 i = 0; i < count; ++i)
            if (!table[i].live) { sync_init(&table[i], key); result = &table[i]; break; }
    }
    mx_unlock(&g_registry_mutex);
    return result;
}
static int event_ready(u16 events, u16 mask, u32 mode)
{
    return mode == CELL_SPURS_EVENT_FLAG_AND ? (events & mask) == mask
                                             : (events & mask) != 0;
}

static TasksetState* event_waiter_taskset(const CellSpursEventFlag* ef,
                                          u32 slot)
{
    if (!vm_base || slot >= 16) return NULL;
    const u32 owner_ea = (u32)rd64(ef->bytes + 0x70);
    if (!owner_ea) return NULL;
    void* owner = vm_base + owner_ea;
    if (!ef->bytes[0x0d]) return taskset_find(owner);

    const u32 wid = ef->bytes[0x60 + slot];
    for (u32 i = 0; i < MAX_TASKSETS; ++i) {
        TasksetState* candidate = &g_tasksets[i];
        if (candidate->sync.live && candidate->spurs == owner &&
            candidate->wid_registered && candidate->wid == wid)
            return candidate;
    }
    return NULL;
}

static void event_observe_native_wait(TasksetState* ts, TaskState* task,
                                      u32 candidate_ea)
{
    if (!ts || !task || (candidate_ea & 127u) ||
        !job_guest_range_valid(candidate_ea, 128u)) {
        return;
    }

    CellSpursEventFlag* ef =
        (CellSpursEventFlag*)(vm_base + candidate_ea);
    if (ef->bytes[0x0e] > CELL_SPURS_EVENT_FLAG_ANY2ANY ||
        ef->bytes[0x0f] > CELL_SPURS_EVENT_FLAG_CLEAR_MANUAL) {
        return;
    }

    const u16 used = rd16(ef->bytes + 0x08);
    int matched_waiter = 0;
    for (u32 slot = 0; slot < 16; ++slot) {
        if (!(used & (0x8000u >> slot)) ||
            ef->bytes[0x50 + slot] != task->id)
            continue;
        if (event_waiter_taskset(ef, slot) == ts) {
            matched_waiter = 1;
            break;
        }
    }
    if (!matched_waiter) {
        return;
    }

    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 1);
    if (!sync) {
        return;
    }
    const u32 index = (u32)(sync - g_event_flags);
    TasksetState* wake_tasksets[16] = {0};
    u8 wake_task_ids[16] = {0};
    u32 wake_count = 0;

    mx_lock(&sync->mutex);
    if (!guest_write_route_target_generation(
            GUEST_WRITE_ROUTE_EVENT, index)) {
        guest_write_route_begin_target(GUEST_WRITE_ROUTE_EVENT, index);
        guest_write_route_watch(GUEST_WRITE_ROUTE_EVENT, index,
                                candidate_ea, 8u);
        /* Registration races a setter.  Treat every already-pending slot as
         * an undelivered edge, then snapshot the complete state below. */
        g_event_watch_state[index] =
            event_watch_snapshot(ef->bytes) & ~EVENT_WATCH_SPU_PENDING;
    }

    const u64 previous = g_event_watch_state[index];
    const u64 state = event_watch_snapshot(ef->bytes);
    const u16 pending = (u16)(state >> 16);
    const u16 previous_pending = (u16)(previous >> 16);
    const u16 new_pending =
        (u16)(pending & (u16)~previous_pending & rd16(ef->bytes + 0x08));
    if (state != previous) {
        g_event_watch_state[index] = state;
        cv_wake_all(&sync->cond);
    }
    for (u32 slot = 0; slot < 16; ++slot) {
        if (!(new_pending & (0x8000u >> slot))) continue;
        TasksetState* waiter = event_waiter_taskset(ef, slot);
        if (!waiter) continue;
        wake_tasksets[wake_count] = waiter;
        wake_task_ids[wake_count] = ef->bytes[0x50 + slot];
        ++wake_count;
    }
    mx_unlock(&sync->mutex);

    for (u32 wake = 0; wake < wake_count; ++wake)
        _cellSpursSendSignal(
            (CellSpursTaskset*)wake_tasksets[wake]->sync.key,
            wake_task_ids[wake]);
}
s32 _cellSpursEventFlagInitialize(CellSpurs* spurs, CellSpursTaskset* ts,
                                  CellSpursEventFlag* ef, u32 clear, u32 direction)
{
    if (!ef || (!spurs && !ts)) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(ef, 128)) return CELL_SPURS_TASK_ERROR_ALIGN;
    if (clear > 1 || direction > 3) return CELL_SPURS_TASK_ERROR_INVAL;
    memset(ef->bytes, 0, 128);
    ef->bytes[0x0d] = spurs ? 1 : 0;
    ef->bytes[0x0e] = (u8)direction;
    ef->bytes[0x0f] = (u8)clear;
    ef->bytes[0x0c] = 0xff;
    wr64(ef->bytes + 0x70, guest_ea(spurs ? (void*)spurs : (void*)ts));
    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 1);
    if (!sync) return CELL_SPURS_TASK_ERROR_NOMEM;
    const u32 index = (u32)(sync - g_event_flags);
    guest_write_route_begin_target(GUEST_WRITE_ROUTE_EVENT, index);
    guest_write_route_watch(GUEST_WRITE_ROUTE_EVENT, index,
                            guest_ea(ef), 8u);
    g_event_watch_state[index] = event_watch_snapshot(ef->bytes);
    return CELL_OK;
}
s32 cellSpursEventFlagInitialize(CellSpursTaskset* ts, CellSpursEventFlag* ef,
                                 u32 clear, u32 direction)
{
    return _cellSpursEventFlagInitialize(NULL, ts, ef, clear, direction);
}
s32 cellSpursEventFlagAttachLv2EventQueue(CellSpursEventFlag* ef)
{
    if (!ef) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(ef, 128)) return CELL_SPURS_TASK_ERROR_ALIGN;
    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 0);
    if (!sync) return CELL_SPURS_TASK_ERROR_STAT;
    if (ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_SPU2PPU &&
        ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_ANY2ANY)
        return CELL_SPURS_TASK_ERROR_PERM;
    mx_lock(&g_registry_mutex);
    if (ef->bytes[0x0c] != 0xff) {
        mx_unlock(&g_registry_mutex);
        return CELL_SPURS_TASK_ERROR_STAT;
    }
    u32 port = 16;
    for (; port < 64; ++port) {
        int used = 0;
        for (u32 i = 0; i < MAX_EVENT_FLAGS; ++i) {
            const SyncKey* candidate = &g_event_flags[i];
            if (candidate->live && candidate->key && candidate->key != ef &&
                ((const CellSpursEventFlag*)candidate->key)->bytes[0x0c] == port) {
                used = 1;
                break;
            }
        }
        if (!used) break;
    }
    if (port == 64) {
        mx_unlock(&g_registry_mutex);
        return CELL_SPURS_TASK_ERROR_BUSY;
    }
    const u32 object_id = (u32)(sync - g_event_flags) + 1;
    ef->bytes[0x0c] = (u8)port;
    wr32(ef->bytes + 0x78, object_id);
    wr32(ef->bytes + 0x7c, object_id);
    mx_unlock(&g_registry_mutex);
    return CELL_OK;
}
s32 cellSpursEventFlagDetachLv2EventQueue(CellSpursEventFlag* ef)
{
    if (!ef) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(ef, 128)) return CELL_SPURS_TASK_ERROR_ALIGN;
    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 0);
    if (!sync) return CELL_SPURS_TASK_ERROR_STAT;
    if (ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_SPU2PPU &&
        ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_ANY2ANY)
        return CELL_SPURS_TASK_ERROR_PERM;
    if (ef->bytes[0x0c] == 0xff) return CELL_SPURS_TASK_ERROR_STAT;
    mx_lock(&sync->mutex);
    if (rd16(ef->bytes + 0x04) || ef->bytes[0x07]) {
        mx_unlock(&sync->mutex);
        return CELL_SPURS_TASK_ERROR_BUSY;
    }
    ef->bytes[0x0c] = 0xff;
    wr32(ef->bytes + 0x78, 0);
    wr32(ef->bytes + 0x7c, 0);
    mx_unlock(&sync->mutex);
    return CELL_OK;
}
s32 cellSpursEventFlagSet(CellSpursEventFlag* ef, u16 bits)
{
    if (!ef) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(ef, 128)) return CELL_SPURS_TASK_ERROR_ALIGN;
    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 0);
    if (!sync) return CELL_SPURS_TASK_ERROR_STAT;
    if (ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_PPU2SPU &&
        ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_ANY2ANY)
        return CELL_SPURS_TASK_ERROR_PERM;
    TasksetState* wake_tasksets[16] = {0};
    u8 wake_task_ids[16] = {0};
    u32 wake_count = 0;
    const u32 event_index = (u32)(sync - g_event_flags);
    mx_lock(&sync->mutex);
    /* Serialize the whole read-modify-write against SPU lock-line commits,
     * as the queue paths do.  Waiting tasks clear and re-arm their pending
     * bits (+0x02) with 128-byte line commits; without the lock line the
     * rd16(+0x02) ... wr16(+0x02, old|pending) window resurrects every bit
     * cleared in between and permanently drops those waiters from `used`. */
    spu_lockline_lock();
    u16 events = (u16)(rd16(ef->bytes) | bits);
    u16 used = (u16)(rd16(ef->bytes + 0x08) & ~rd16(ef->bytes + 0x02));
    u16 modes = rd16(ef->bytes + 0x0a);
    u16 pending = 0, consumed = 0;
    for (u32 slot = 0; slot < 16; ++slot) {
        u16 slot_bit = (u16)(0x8000u >> slot);
        if (!(used & slot_bit)) continue;
        u16 mask = rd16(ef->bytes + 0x10 + slot * 2);
        u16 got = (u16)(events & mask);
        if (event_ready(events, mask, (modes & slot_bit) ? 1 : 0)) {
            wr16(ef->bytes + 0x30 + slot * 2, got);
            pending |= slot_bit;
            consumed |= got;
        }
    }
    wr16(ef->bytes + 0x02, (u16)(rd16(ef->bytes + 0x02) | pending));
    /* A PPU waiter registers via ppuWaitMask[0x04] + ppuWaitSlotAndMode[0x06],
     * not the task slot table walked above. Deliver to it explicitly: latch the got
     * bits into its receive slot, raise ppuPendingRecv[0x07], clear the
     * mask, and count the bits consumed so CLEAR_AUTO cannot lose the
     * delivery to a racing task-slot consumer inside the waiter's poll gap. */
    {
        const u16 ppu_mask = rd16(ef->bytes + 0x04);
        if (ppu_mask && !ef->bytes[0x07]) {
            const u32 ppu_slot = ef->bytes[0x06] >> 4;
            const int ppu_mode = ef->bytes[0x06] & 0x0F;
            if (event_ready(events, ppu_mask, ppu_mode)) {
                const u16 got = (u16)(events & ppu_mask);
                wr16(ef->bytes + 0x30 + ppu_slot * 2, got);
                ef->bytes[0x07] = 1;
                wr16(ef->bytes + 0x04, 0);
                consumed |= got;
            }
        }
    }
    if (ef->bytes[0x0f] == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO) events &= (u16)~consumed;
    wr16(ef->bytes, events);
    /* Deliver every pending edge not yet represented by the exact-write
     * bridge, including a lifted-SPU publication that completed just before
     * this HLE setter acquired the lock line.  This closes the
     * writer-before-notifier race without ever replaying an old pending bit. */
    const u16 routed_pending = (u16)(
        rd16(ef->bytes + 0x02) &
        (u16)~(u16)(g_event_watch_state[event_index] >> 16) &
        rd16(ef->bytes + 0x08));
    for (u32 slot = 0; slot < 16; ++slot) {
        if (!(routed_pending & (0x8000u >> slot))) continue;
        TasksetState* taskset = event_waiter_taskset(ef, slot);
        if (taskset) {
            wake_tasksets[wake_count] = taskset;
            wake_task_ids[wake_count] = ef->bytes[0x50 + slot];
            ++wake_count;
        }
    }
    /* Snapshot while the lock line still belongs to this setter.  Refreshing
     * after unlock could absorb a newer lifted-SPU edge before its notifier
     * ran, falsely marking that wake as delivered. */
    event_watch_refresh_fields_locked(sync, EVENT_WATCH_ALL);
    spu_lockline_unlock();
    cv_wake_all(&sync->cond);
    mx_unlock(&sync->mutex);
    /* Do not nest a taskset mutex under the event-flag mutex.  Task code can
     * publish event-flag state while its scheduler owns the taskset mutex. */
    for (u32 i = 0; i < wake_count; ++i)
        _cellSpursSendSignal(
            (CellSpursTaskset*)wake_tasksets[i]->sync.key, wake_task_ids[i]);
    return CELL_OK;
}
static s32 event_wait(CellSpursEventFlag* ef, u16* bits, u32 mode, int try_only)
{
    if (!ef || !bits) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(ef, 128)) return CELL_SPURS_TASK_ERROR_ALIGN;
    if (mode > 1) return CELL_SPURS_TASK_ERROR_INVAL;
    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 0);
    if (!sync) return CELL_SPURS_TASK_ERROR_STAT;
    if (ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_SPU2PPU &&
        ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_ANY2ANY)
        return CELL_SPURS_TASK_ERROR_PERM;
    if (!try_only && ef->bytes[0x0c] == 0xff)
        return CELL_SPURS_TASK_ERROR_STAT;
    const u16 mask = rd16(bits);
    mx_lock(&sync->mutex);
    if (rd16(ef->bytes + 0x04) || ef->bytes[0x07]) {
        mx_unlock(&sync->mutex);
        return CELL_SPURS_TASK_ERROR_BUSY;
    }
    if (event_ready(rd16(ef->bytes), mask, mode)) {
        const u16 got = (u16)(rd16(ef->bytes) & mask);
        wr16(bits, got);
        if (ef->bytes[0x0f] == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO)
            wr16(ef->bytes, (u16)(rd16(ef->bytes) & ~got));
        event_watch_refresh_fields_locked(sync, EVENT_WATCH_EVENTS);
        mx_unlock(&sync->mutex);
        return CELL_OK;
    }
    if (try_only) {
        mx_unlock(&sync->mutex);
        return CELL_SPURS_TASK_ERROR_BUSY;
    }

    u32 slot = 0;
    if (ef->bytes[0x0e] == CELL_SPURS_EVENT_FLAG_ANY2ANY) {
        u16 used = rd16(ef->bytes + 0x08);
        u32 low_slot = 0;
        while (low_slot < 16 && (used & (1u << low_slot))) ++low_slot;
        if (low_slot == 16) {
            mx_unlock(&sync->mutex);
            return CELL_SPURS_TASK_ERROR_BUSY;
        }
        slot = 15 - low_slot;
        /* The PPU waiter records its slot only in ppuWaitSlotAndMode[0x06]; it
         * must not set a bit in spuTaskUsedWaitSlots[0x08]. A phantom used bit
         * with a zero spuTaskWaitMask[0x10+slot*2] makes the guest-side
         * Set routine treat the slot as an always-satisfied task waiter
         * and _cellSpursSendSignal task id 0 on every Set (spurious wakes
         * for an unrelated task), and our own Set's used&~pending scan
         * misroutes deliveries. */
    }
    ef->bytes[0x06] = (u8)((slot << 4) | mode);
    ef->bytes[0x07] = 0;
    wr16(ef->bytes + 0x04, mask);
    atomic_thread_fence(memory_order_release);

    while (!ef->bytes[0x07] && !event_ready(rd16(ef->bytes), mask, mode))
        /* Timed: lifted-SPU tasks set flag bytes via guest memory/MFC
         * atomics without waking this HLE condvar (cv_wait_ms). */
        cv_wait_ms(&sync->cond, &sync->mutex, 2u);

    u16 got;
    if (ef->bytes[0x07]) {
        slot = ef->bytes[0x06] >> 4;
        got = rd16(ef->bytes + 0x30 + slot * 2);
        ef->bytes[0x07] = 0;
    } else {
        got = (u16)(rd16(ef->bytes) & mask);
        if (ef->bytes[0x0f] == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO)
            wr16(ef->bytes, (u16)(rd16(ef->bytes) & ~got));
    }
    wr16(bits, got);
    wr16(ef->bytes + 0x04, 0);
    /* No [0x08] bit was set on entry, so none is cleared here. */
    /* A Set racing our raw-bits exit may have read ppuWaitMask before
     * we zeroed it and completed delivery after, leaving ppuPendingRecv=1
     * with nobody waiting, which would turn every later Wait into BUSY at
     * the entry gate (permanent). We are the only PPU waiter; any pending
     * delivery was ours, so consume the marker on the way out. */
    ef->bytes[0x07] = 0;
    event_watch_refresh_fields_locked(
        sync, EVENT_WATCH_EVENTS | EVENT_WATCH_PPU_PENDING);
    mx_unlock(&sync->mutex);
    return CELL_OK;
}
s32 cellSpursEventFlagWait(CellSpursEventFlag* ef, u16* bits, u32 mode)
{ return event_wait(ef, bits, mode, 0); }
s32 cellSpursEventFlagTryWait(CellSpursEventFlag* ef, u16* bits, u32 mode)
{ return event_wait(ef, bits, mode, 1); }
s32 cellSpursEventFlagClear(CellSpursEventFlag* ef, u16 bits)
{
    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 0);
    if (!sync) return CELL_SPURS_TASK_ERROR_STAT;
    mx_lock(&sync->mutex);
    wr16(ef->bytes, (u16)(rd16(ef->bytes) & ~bits));
    event_watch_refresh_fields_locked(sync, EVENT_WATCH_EVENTS);
    mx_unlock(&sync->mutex);
    return CELL_OK;
}
