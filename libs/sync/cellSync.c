/*
 * ps3recomp - cellSync HLE implementation
 *
 * The mutex operates on the guest's big-endian ticket word in place and
 * serializes against SPU lock-line reservations; the other primitives use
 * host C11 atomics.
 */

#include "cellSync.h"
#include "ps3emu/endian.h"
#include "../../runtime/ppu/ppu_memory.h"
#include <string.h>

/* Yield hint for spin-wait loops */
#ifdef _WIN32
#include <windows.h>
#define SYNC_YIELD() SwitchToThread()
#else
#include <sched.h>
#define SYNC_YIELD() sched_yield()
#endif

/* =========================================================================
 * Mutex
 * =====================================================================*/

static void sync_mutex_coherence(CellSyncMutex* mutex);

s32 cellSyncMutexInitialize(CellSyncMutex* mutex)
{
    if (!mutex)
        return CELL_SYNC_ERROR_NULL_POINTER;

    atomic_store_explicit(&mutex->tickets, 0, memory_order_release);
    /* Kill any outstanding SPU line reservation after the write, as
     * Lock/TryLock/Unlock do. An SPU holding a GETLLAR reservation on this
     * line across a PPU re-init could otherwise commit a stale PUTLLC over
     * the fresh ticket word (the same class as the ticket freeze described
     * below). */
    sync_mutex_coherence(mutex);
    return CELL_OK;
}

static void sync_mutex_coherence(CellSyncMutex* mutex)
{
    if (!vm_base || (uint8_t*)mutex < vm_base) return;
    uint32_t ea = (uint32_t)((uint8_t*)mutex - vm_base) & ~127u;
    if (spu_coh_is_reserved(ea)) spu_coh_notify_write(ea);
}

/* ---- PPU RMW vs SPU lock-line commits: mutual exclusion ------------------
 * Without this, the ticket lock can freeze: an SPU context holds an active
 * reservation on this very line while `serving` stays one behind `next`, and
 * every PPU TryLock returns BUSY.
 *
 * An SPU PUTLLC commits the whole 128-byte line from its local snapshot, under
 * spu_lockline_lock, after a memcmp against that snapshot (runtime/spu/
 * spu_dma.h). A PPU read-modify-write performed outside that lock can land
 * between the SPU's memcmp and its memcpy, and the memcpy then restores the
 * pre-write bytes, silently erasing the PPU update. For a ticket lock the
 * erased update is the unlock, so `serving` never advances again and every
 * later TryLock returns BUSY forever.
 *
 * The runtime models this correctly for lifted PPU atomics
 * (yakuza/shims.cpp ppu_res_stwcx/ppu_res_stdcx: commit under spu_lockline_lock
 * when the line is SPU-reserved, then notify). cellSync's HLE would otherwise
 * use bare C11 atomics. These helpers apply the same discipline; the
 * check-then-lock shape mirrors ppu_res_stwcx exactly (if no SPU holds a
 * reservation on the line, no PUTLLC for it can be in flight, and a
 * reservation taken later either snapshots our new value or fails its own
 * memcmp). */

static int sync_line_lock(const void* p, uint32_t* ea_out)
{
    if (!vm_base || (const uint8_t*)p < vm_base) return 0;
    uint32_t ea = (uint32_t)((const uint8_t*)p - vm_base) & ~127u;
    if (!spu_coh_is_reserved(ea)) return 0;
    *ea_out = ea;
    spu_lockline_lock();
    return 1;
}

static void sync_line_unlock(int locked, uint32_t ea, int wrote)
{
    if (!locked) return;
    if (wrote) spu_coh_notify_write(ea);
    spu_lockline_unlock();
}

s32 cellSyncMutexLock(CellSyncMutex* mutex)
{
    if (!mutex)
        return CELL_SYNC_ERROR_NULL_POINTER;

    unsigned int raw, word, desired, ticket;
    int spins = 0;

    raw = atomic_load_explicit(&mutex->tickets, memory_order_acquire);
    for (;;) {
        word = ps3_bswap32(raw);
        ticket = word & 0xffffu;
        if ((u16)(ticket - (word >> 16)) == 0xffffu)
            return CELL_SYNC_ERROR_BUSY;
        desired = ps3_bswap32((word & 0xffff0000u) | (u16)(ticket + 1));
        {
            uint32_t line_ea = 0;
            int locked = sync_line_lock(mutex, &line_ea);
            int ok = atomic_compare_exchange_weak_explicit(&mutex->tickets, &raw, desired,
                                                           memory_order_acq_rel,
                                                           memory_order_acquire);
            sync_line_unlock(locked, line_ea, ok);
            if (ok) break;
        }
    }
    sync_mutex_coherence(mutex);
    for (;;) {
        word = ps3_bswap32(atomic_load_explicit(&mutex->tickets, memory_order_acquire));
        if ((word >> 16) == (u16)ticket) return CELL_OK;
        if (++spins > 1000) {
            SYNC_YIELD();
            spins = 0;
        }
    }
}

s32 cellSyncMutexTryLock(CellSyncMutex* mutex)
{
    if (!mutex)
        return CELL_SYNC_ERROR_NULL_POINTER;

    unsigned raw = atomic_load_explicit(&mutex->tickets, memory_order_acquire);
    for (;;) {
        unsigned word = ps3_bswap32(raw);
        unsigned current = word >> 16;
        unsigned next = word & 0xffffu;
        if (current != next) {
            return CELL_SYNC_ERROR_BUSY;
        }
        unsigned desired = ps3_bswap32((current << 16) | (u16)(next + 1));
        uint32_t line_ea = 0;
        int locked = sync_line_lock(mutex, &line_ea);
        if (!atomic_compare_exchange_weak_explicit(&mutex->tickets, &raw, desired,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            sync_line_unlock(locked, line_ea, 0);
            continue;
        }
        sync_line_unlock(locked, line_ea, 1);
        if (!locked) sync_mutex_coherence(mutex);
        return CELL_OK;
    }
}

s32 cellSyncMutexUnlock(CellSyncMutex* mutex)
{
    if (!mutex)
        return CELL_SYNC_ERROR_NULL_POINTER;

    unsigned raw = atomic_load_explicit(&mutex->tickets, memory_order_acquire);
    for (;;) {
        unsigned word = ps3_bswap32(raw);
        unsigned current = word >> 16;
        unsigned next = word & 0xffffu;
        if (current == next) return CELL_SYNC_ERROR_STAT;
        unsigned desired = ps3_bswap32(((u16)(current + 1) << 16) | next);
        /* Hold the SPU lock-line lock across the CAS: an SPU PUTLLC that has
         * already passed its memcmp would otherwise restore the pre-unlock
         * bytes right after we commit, and `serving` would never advance
         * again. */
        uint32_t line_ea = 0;
        int locked = sync_line_lock(mutex, &line_ea);
        if (atomic_compare_exchange_weak_explicit(&mutex->tickets, &raw, desired,
                                                  memory_order_release,
                                                  memory_order_acquire)) {
            sync_line_unlock(locked, line_ea, 1);
            if (!locked) sync_mutex_coherence(mutex);
            return CELL_OK;
        }
        sync_line_unlock(locked, line_ea, 0);
    }
}

/* =========================================================================
 * Barrier
 * =====================================================================*/

s32 cellSyncBarrierInitialize(CellSyncBarrier* barrier, u16 totalCount)
{
    if (!barrier)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (totalCount == 0)
        return CELL_SYNC_ERROR_INVAL;

    atomic_store(&barrier->arrived, 0);
    barrier->total = totalCount;
    atomic_store(&barrier->phase, 0);

    return CELL_OK;
}

s32 cellSyncBarrierNotify(CellSyncBarrier* barrier)
{
    if (!barrier)
        return CELL_SYNC_ERROR_NULL_POINTER;

    unsigned int old_arrived = atomic_fetch_add(&barrier->arrived, 1);

    /* If we're the last to arrive, advance the phase and reset */
    if (old_arrived + 1 >= barrier->total) {
        atomic_store(&barrier->arrived, 0);
        atomic_fetch_add(&barrier->phase, 1);
    }

    return CELL_OK;
}

s32 cellSyncBarrierTryNotify(CellSyncBarrier* barrier)
{
    /* Same as notify for this implementation */
    return cellSyncBarrierNotify(barrier);
}

s32 cellSyncBarrierWait(CellSyncBarrier* barrier)
{
    if (!barrier)
        return CELL_SYNC_ERROR_NULL_POINTER;

    unsigned int phase = atomic_load(&barrier->phase);
    int spins = 0;

    /* Wait until the phase changes (all have arrived) */
    while (atomic_load(&barrier->phase) == phase) {
        if (++spins > 1000) {
            SYNC_YIELD();
            spins = 0;
        }
    }

    return CELL_OK;
}

s32 cellSyncBarrierTryWait(CellSyncBarrier* barrier)
{
    if (!barrier)
        return CELL_SYNC_ERROR_NULL_POINTER;

    /* Check if all have arrived (count == 0 means reset happened) */
    if (atomic_load(&barrier->arrived) != 0)
        return CELL_SYNC_ERROR_BUSY;

    return CELL_OK;
}

/* =========================================================================
 * Reader-Writer Memory
 * =====================================================================*/

s32 cellSyncRwmInitialize(CellSyncRwm* rwm, void* buffer, u32 size)
{
    if (!rwm || !buffer)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (size == 0)
        return CELL_SYNC_ERROR_INVAL;

    atomic_store(&rwm->readers, 0);
    atomic_store(&rwm->writer, 0);
    rwm->size = size;
    rwm->buffer = buffer;

    return CELL_OK;
}

s32 cellSyncRwmRead(CellSyncRwm* rwm, void* dst)
{
    if (!rwm || !dst)
        return CELL_SYNC_ERROR_NULL_POINTER;

    int spins = 0;

    /* Wait until no writer is active */
    while (atomic_load(&rwm->writer)) {
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }

    atomic_fetch_add(&rwm->readers, 1);

    /* Double-check no writer started */
    while (atomic_load(&rwm->writer)) {
        atomic_fetch_sub(&rwm->readers, 1);
        while (atomic_load(&rwm->writer)) {
            if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
        }
        atomic_fetch_add(&rwm->readers, 1);
    }

    memcpy(dst, rwm->buffer, rwm->size);
    atomic_fetch_sub(&rwm->readers, 1);

    return CELL_OK;
}

s32 cellSyncRwmTryRead(CellSyncRwm* rwm, void* dst)
{
    if (!rwm || !dst)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (atomic_load(&rwm->writer))
        return CELL_SYNC_ERROR_BUSY;

    atomic_fetch_add(&rwm->readers, 1);

    if (atomic_load(&rwm->writer)) {
        atomic_fetch_sub(&rwm->readers, 1);
        return CELL_SYNC_ERROR_BUSY;
    }

    memcpy(dst, rwm->buffer, rwm->size);
    atomic_fetch_sub(&rwm->readers, 1);

    return CELL_OK;
}

s32 cellSyncRwmWrite(CellSyncRwm* rwm, const void* src)
{
    if (!rwm || !src)
        return CELL_SYNC_ERROR_NULL_POINTER;

    unsigned int expected;
    int spins = 0;

    /* Acquire write lock */
    for (;;) {
        expected = 0;
        if (atomic_compare_exchange_weak(&rwm->writer, &expected, 1))
            break;
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }

    /* Wait for all readers to finish */
    spins = 0;
    while (atomic_load(&rwm->readers) > 0) {
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }

    memcpy(rwm->buffer, src, rwm->size);
    atomic_store(&rwm->writer, 0);

    return CELL_OK;
}

s32 cellSyncRwmTryWrite(CellSyncRwm* rwm, const void* src)
{
    if (!rwm || !src)
        return CELL_SYNC_ERROR_NULL_POINTER;

    unsigned int expected = 0;
    if (!atomic_compare_exchange_strong(&rwm->writer, &expected, 1))
        return CELL_SYNC_ERROR_BUSY;

    if (atomic_load(&rwm->readers) > 0) {
        atomic_store(&rwm->writer, 0);
        return CELL_SYNC_ERROR_BUSY;
    }

    memcpy(rwm->buffer, src, rwm->size);
    atomic_store(&rwm->writer, 0);

    return CELL_OK;
}

/* =========================================================================
 * Bounded Queue
 * =====================================================================*/

static void queue_spinlock_acquire(atomic_uint* lock)
{
    unsigned int expected;
    int spins = 0;
    for (;;) {
        expected = 0;
        if (atomic_compare_exchange_weak(lock, &expected, 1))
            return;
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }
}

static void queue_spinlock_release(atomic_uint* lock)
{
    atomic_store(lock, 0);
}

s32 cellSyncQueueInitialize(CellSyncQueue* queue, void* buffer,
                            u32 size, u32 depth)
{
    if (!queue || !buffer)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (size == 0 || depth == 0 || depth > CELL_SYNC_QUEUE_MAX_DEPTH)
        return CELL_SYNC_ERROR_INVAL;

    atomic_store(&queue->head, 0);
    atomic_store(&queue->tail, 0);
    atomic_store(&queue->count, 0);
    atomic_store(&queue->lock, 0);
    queue->depth = depth;
    queue->elemSize = size;
    queue->buffer = (u8*)buffer;

    memset(buffer, 0, (size_t)size * depth);

    return CELL_OK;
}

s32 cellSyncQueuePush(CellSyncQueue* queue, const void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    int spins = 0;

    /* Spin until there's space */
    while (atomic_load(&queue->count) >= queue->depth) {
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }

    queue_spinlock_acquire(&queue->lock);

    if (atomic_load(&queue->count) >= queue->depth) {
        queue_spinlock_release(&queue->lock);
        return cellSyncQueuePush(queue, data); /* retry */
    }

    u32 tail = atomic_load(&queue->tail);
    memcpy(queue->buffer + (size_t)tail * queue->elemSize,
           data, queue->elemSize);
    atomic_store(&queue->tail, (tail + 1) % queue->depth);
    atomic_fetch_add(&queue->count, 1);

    queue_spinlock_release(&queue->lock);
    return CELL_OK;
}

s32 cellSyncQueueTryPush(CellSyncQueue* queue, const void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (atomic_load(&queue->count) >= queue->depth)
        return CELL_SYNC_ERROR_OVERFLOW;

    unsigned int expected = 0;
    if (!atomic_compare_exchange_strong(&queue->lock, &expected, 1))
        return CELL_SYNC_ERROR_BUSY;

    if (atomic_load(&queue->count) >= queue->depth) {
        atomic_store(&queue->lock, 0);
        return CELL_SYNC_ERROR_OVERFLOW;
    }

    u32 tail = atomic_load(&queue->tail);
    memcpy(queue->buffer + (size_t)tail * queue->elemSize,
           data, queue->elemSize);
    atomic_store(&queue->tail, (tail + 1) % queue->depth);
    atomic_fetch_add(&queue->count, 1);

    atomic_store(&queue->lock, 0);
    return CELL_OK;
}

s32 cellSyncQueuePop(CellSyncQueue* queue, void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    int spins = 0;

    while (atomic_load(&queue->count) == 0) {
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }

    queue_spinlock_acquire(&queue->lock);

    if (atomic_load(&queue->count) == 0) {
        queue_spinlock_release(&queue->lock);
        return cellSyncQueuePop(queue, data); /* retry */
    }

    u32 head = atomic_load(&queue->head);
    memcpy(data, queue->buffer + (size_t)head * queue->elemSize,
           queue->elemSize);
    atomic_store(&queue->head, (head + 1) % queue->depth);
    atomic_fetch_sub(&queue->count, 1);

    queue_spinlock_release(&queue->lock);
    return CELL_OK;
}

s32 cellSyncQueueTryPop(CellSyncQueue* queue, void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (atomic_load(&queue->count) == 0)
        return CELL_SYNC_ERROR_EMPTY;

    unsigned int expected = 0;
    if (!atomic_compare_exchange_strong(&queue->lock, &expected, 1))
        return CELL_SYNC_ERROR_BUSY;

    if (atomic_load(&queue->count) == 0) {
        atomic_store(&queue->lock, 0);
        return CELL_SYNC_ERROR_EMPTY;
    }

    u32 head = atomic_load(&queue->head);
    memcpy(data, queue->buffer + (size_t)head * queue->elemSize,
           queue->elemSize);
    atomic_store(&queue->head, (head + 1) % queue->depth);
    atomic_fetch_sub(&queue->count, 1);

    atomic_store(&queue->lock, 0);
    return CELL_OK;
}

s32 cellSyncQueuePeek(CellSyncQueue* queue, void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (atomic_load(&queue->count) == 0)
        return CELL_SYNC_ERROR_EMPTY;

    queue_spinlock_acquire(&queue->lock);

    if (atomic_load(&queue->count) == 0) {
        queue_spinlock_release(&queue->lock);
        return CELL_SYNC_ERROR_EMPTY;
    }

    u32 head = atomic_load(&queue->head);
    memcpy(data, queue->buffer + (size_t)head * queue->elemSize,
           queue->elemSize);

    queue_spinlock_release(&queue->lock);
    return CELL_OK;
}

s32 cellSyncQueueSize(CellSyncQueue* queue, u32* size)
{
    if (!queue || !size)
        return CELL_SYNC_ERROR_NULL_POINTER;

    *size = atomic_load(&queue->count);
    return CELL_OK;
}

s32 cellSyncQueueClear(CellSyncQueue* queue)
{
    if (!queue)
        return CELL_SYNC_ERROR_NULL_POINTER;

    queue_spinlock_acquire(&queue->lock);
    atomic_store(&queue->head, 0);
    atomic_store(&queue->tail, 0);
    atomic_store(&queue->count, 0);
    queue_spinlock_release(&queue->lock);

    return CELL_OK;
}

/* =========================================================================
 * Lock-Free Queue
 *
 * Uses the same spinlock approach as bounded queue for simplicity.
 * A true lock-free implementation would use multi-word CAS.
 * =====================================================================*/

s32 cellSyncLFQueueInitialize(CellSyncLFQueue* queue, void* buffer,
                               u32 size, u32 depth, u32 direction,
                               void* eaSignal)
{
    (void)eaSignal;

    if (!queue || !buffer)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (size == 0 || depth == 0)
        return CELL_SYNC_ERROR_INVAL;

    atomic_store(&queue->head, 0);
    atomic_store(&queue->tail, 0);
    atomic_store(&queue->count, 0);
    atomic_store(&queue->lock, 0);
    queue->depth = depth;
    queue->elemSize = size;
    queue->buffer = (u8*)buffer;
    queue->direction = direction;

    memset(buffer, 0, (size_t)size * depth);
    return CELL_OK;
}

s32 cellSyncLFQueuePush(CellSyncLFQueue* queue, const void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    int spins = 0;
    while (atomic_load(&queue->count) >= queue->depth) {
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }

    queue_spinlock_acquire(&queue->lock);

    if (atomic_load(&queue->count) >= queue->depth) {
        queue_spinlock_release(&queue->lock);
        return cellSyncLFQueuePush(queue, data);
    }

    u32 tail = atomic_load(&queue->tail);
    memcpy(queue->buffer + (size_t)tail * queue->elemSize,
           data, queue->elemSize);
    atomic_store(&queue->tail, (tail + 1) % queue->depth);
    atomic_fetch_add(&queue->count, 1);

    queue_spinlock_release(&queue->lock);
    return CELL_OK;
}

s32 cellSyncLFQueueTryPush(CellSyncLFQueue* queue, const void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (atomic_load(&queue->count) >= queue->depth)
        return CELL_SYNC_ERROR_OVERFLOW;

    unsigned int expected = 0;
    if (!atomic_compare_exchange_strong(&queue->lock, &expected, 1))
        return CELL_SYNC_ERROR_BUSY;

    if (atomic_load(&queue->count) >= queue->depth) {
        atomic_store(&queue->lock, 0);
        return CELL_SYNC_ERROR_OVERFLOW;
    }

    u32 tail = atomic_load(&queue->tail);
    memcpy(queue->buffer + (size_t)tail * queue->elemSize,
           data, queue->elemSize);
    atomic_store(&queue->tail, (tail + 1) % queue->depth);
    atomic_fetch_add(&queue->count, 1);

    atomic_store(&queue->lock, 0);
    return CELL_OK;
}

s32 cellSyncLFQueuePop(CellSyncLFQueue* queue, void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    int spins = 0;
    while (atomic_load(&queue->count) == 0) {
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }

    queue_spinlock_acquire(&queue->lock);

    if (atomic_load(&queue->count) == 0) {
        queue_spinlock_release(&queue->lock);
        return cellSyncLFQueuePop(queue, data);
    }

    u32 head = atomic_load(&queue->head);
    memcpy(data, queue->buffer + (size_t)head * queue->elemSize,
           queue->elemSize);
    atomic_store(&queue->head, (head + 1) % queue->depth);
    atomic_fetch_sub(&queue->count, 1);

    queue_spinlock_release(&queue->lock);
    return CELL_OK;
}

s32 cellSyncLFQueueTryPop(CellSyncLFQueue* queue, void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (atomic_load(&queue->count) == 0)
        return CELL_SYNC_ERROR_EMPTY;

    unsigned int expected = 0;
    if (!atomic_compare_exchange_strong(&queue->lock, &expected, 1))
        return CELL_SYNC_ERROR_BUSY;

    if (atomic_load(&queue->count) == 0) {
        atomic_store(&queue->lock, 0);
        return CELL_SYNC_ERROR_EMPTY;
    }

    u32 head = atomic_load(&queue->head);
    memcpy(data, queue->buffer + (size_t)head * queue->elemSize,
           queue->elemSize);
    atomic_store(&queue->head, (head + 1) % queue->depth);
    atomic_fetch_sub(&queue->count, 1);

    atomic_store(&queue->lock, 0);
    return CELL_OK;
}

s32 cellSyncLFQueueGetDirection(const CellSyncLFQueue* queue, u32* dir)
{
    if (!queue || !dir)
        return CELL_SYNC_ERROR_NULL_POINTER;

    *dir = queue->direction;
    return CELL_OK;
}

s32 cellSyncLFQueueDepth(const CellSyncLFQueue* queue, u32* depth)
{
    if (!queue || !depth)
        return CELL_SYNC_ERROR_NULL_POINTER;

    *depth = queue->depth;
    return CELL_OK;
}

s32 cellSyncLFQueueSize(CellSyncLFQueue* queue, u32* size)
{
    if (!queue || !size)
        return CELL_SYNC_ERROR_NULL_POINTER;

    *size = atomic_load(&queue->count);
    return CELL_OK;
}

s32 cellSyncLFQueueClear(CellSyncLFQueue* queue)
{
    if (!queue)
        return CELL_SYNC_ERROR_NULL_POINTER;

    queue_spinlock_acquire(&queue->lock);
    atomic_store(&queue->head, 0);
    atomic_store(&queue->tail, 0);
    atomic_store(&queue->count, 0);
    queue_spinlock_release(&queue->lock);

    return CELL_OK;
}
