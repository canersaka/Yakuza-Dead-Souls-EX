/*
 * ps3recomp - SPU DMA engine (MFC)
 *
 * The Memory Flow Controller (MFC) handles DMA transfers between an SPU's
 * 256 KB local store and the main memory (EA space).  Each SPU has a 16-entry
 * MFC command queue.
 *
 * Supported operations:
 *   - DMA get/put  (local store <-> main memory)
 *   - DMA list commands (scatter/gather)
 *   - Tag group management and synchronization
 *   - Barrier and fence
 */

#ifndef SPU_DMA_H
#define SPU_DMA_H

#include "spu_context.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <excpt.h>   /* __try/__except for guarding DMA against bad EAs */
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* MFC queue depth */
#define MFC_QUEUE_DEPTH     16

/* Maximum DMA transfer size per element */
#define MFC_MAX_DMA_SIZE    (16 * 1024)

/* Sequence lock around the GCM user-command handler (spu_channels.c): the
 * image-4 report-record fetch below waits for the handler to finish staging. */
long yz_ucmd_wait_for_handler_completion(void);
int yz_ucmd_handler_snapshot_is_stable(long epoch);
long yz_ucmd_handler_begin(long cause);
void yz_ucmd_handler_end(long cause, long epoch);

/* ---------------------------------------------------------------------------
 * MFC command entry
 * -----------------------------------------------------------------------*/
typedef struct mfc_cmd {
    uint32_t lsa;       /* Local store address */
    uint64_t ea;        /* EA for ordinary DMA; EAH plus list-LS pointer for list DMA */
    uint32_t size;      /* Transfer size in bytes */
    uint32_t tag;       /* Tag group ID (0-31) */
    uint32_t cmd;       /* Command opcode (MFC_PUT_CMD, MFC_GET_CMD, etc.) */
} mfc_cmd;

/* ---------------------------------------------------------------------------
 * MFC DMA engine state
 * -----------------------------------------------------------------------*/
typedef struct mfc_engine {
    mfc_cmd     queue[MFC_QUEUE_DEPTH];
    uint32_t    queue_count;

    /* Per-tag completion tracking (bitmask: bit N = tag N completed) */
    uint32_t    tag_completed;
    uint16_t    tag_outstanding[32];

    /* Deferred same-tag commands. A fenced command waits here while an older
     * list command with the same tag is stalled; later commands on that tag
     * queue behind the fence. */
    uint32_t    deferred_count;
    uint8_t     draining_deferred;

    /* One-entry tag-status result channel. */
    uint8_t     tag_update_pending;
    uint8_t     tag_status_ready;
    uint8_t     tag_update_type;
    uint32_t    tag_update_mask;

    /* Lock-line reservation (GETLLAR/PUTLLC). A 128-byte snapshot of the
     * line is taken under the global lock-line lock; PUTLLC commits only
     * if main memory still matches it (compare-and-swap semantics). */
    uint64_t    resv_ea;          /* line EA (128-aligned), 0 = none */
    uint8_t     resv_data[128];
    int         resv_active;
    uint32_t    resv_gen;         /* line write-generation at GETLLAR (LR re-derivation) */
    uint32_t    resv_poll_n;      /* consecutive same-line GETLLARs with an unchanged
                                   * write-generation = an idle reservation poll loop;
                                   * drives the host-yield backoff (see MFC_GETLLAR_CMD) */
    uint32_t    resv_cas_idle_ea; /* last PUTLLC line used by contention backoff */
    uint32_t    resv_cas_idle_n;  /* consecutive failed/no-change PUTLLCs on that line */
    uint32_t    atomic_stat;      /* value waiting in MFC_RdAtomicStat */
    uint32_t    atomic_stat_ready;/* 1 while that single channel entry is readable */

    /* DMA-list stall-and-notify.
     * A stall-and-notify list element (bit 15 of the element's
     * size-and-flags halfword) requests that list processing be suspended
     * after that element completes, the Sn event (event bit 0x2) be raised,
     * and the list wait for a matching MFC_WrListStallAck before continuing.
     * The resume contract: on hitting a stalled element the engine records
     * {tag, the EA of the next element, the LS cursor, and the remaining list
     * byte count} and returns without processing the rest;
     * MFC_WrListStallAck takes a tag number (not an element index), clears
     * that tag's stall bit, and re-submits the remainder from the saved
     * cursor. RdListStallStat reports a per-tag bitmask (`rotl(1, tag)`), not
     * an element index.
     *
     * One in-flight stall per tag (32 tags) is enough to be faithful: real
     * hardware processes the MFC queue in order per tag group, so a second
     * list on the same tag cannot be issued by well-formed guest code before
     * the first stall is acked (our synchronous engine also has no queue
     * depth beyond 1 in-flight list transfer at a time). */
    uint32_t    stall_mask;                     /* bit T set = tag T list transfer is stalled */
    uint32_t    stall_list_lsa[32];              /* LS address of the (unread) list, per tag */
    uint64_t    stall_ea_base[32];               /* ea_base passed to mfc_do_list_transfer, per tag */
    uint32_t    stall_next_elem[32];             /* index of the NEXT (unprocessed) list element */
    uint32_t    stall_num_elements[32];          /* total element count of the list, per tag */
    uint32_t    stall_cur_lsa[32];               /* LS landing cursor to resume writing/reading at */
    uint32_t    stall_base_cmd[32];              /* base GET/PUT command (list bit already stripped) */
} mfc_engine;

/* Host scheduling backoff for a guest CAS retry loop. A PUTLLC that changes
 * bytes is productive and resets the ladder. Repeated failures or successful
 * same-value commits on one line are contention/idle churn: preserve every
 * architectural attempt, but yield only after the lock has been released so
 * the thread owning useful work can run. Unlike lock parking, this never
 * sleeps a lock hand-off or penalizes the first retry. */
static inline int mfc_putllc_backoff(
    mfc_engine* mfc, uint32_t line_ea, int made_progress)
{
    if (mfc->resv_cas_idle_ea != line_ea) {
        mfc->resv_cas_idle_ea = line_ea;
        mfc->resv_cas_idle_n = 0;
    }
    if (made_progress) {
        mfc->resv_cas_idle_n = 0;
        return -1;
    }
    if (mfc->resv_cas_idle_n != UINT32_MAX)
        mfc->resv_cas_idle_n++;
    if (mfc->resv_cas_idle_n <= 16u)
        return -1;
    return mfc->resv_cas_idle_n > 256u ? 3 : 0;
}

/* Single process-wide lock serializing lock-line transactions across all
 * SPU host threads (defined in spu_channels.c). */
void spu_lockline_lock(void);
void spu_lockline_unlock(void);
void spu_lockline_unlock_then_notify_guest_write(
    int notify, uint32_t ea, uint32_t size);
/* Host-core yield for SPU idle-poll loops (defined in spu_channels.c):
 * level 0 = cpu pause, level 1 = same-core scheduler yield, level 2 =
 * 1 ms sleep, level 3 = process-wide zero-duration scheduler yield. */
void spu_idle_yield(int level);

/* External: pointer to host mapping of PS3 main memory.
 * Must be set by the VM manager before any DMA. */
extern uint8_t* vm_base;

/* ---------------------------------------------------------------------------
 * Initialization
 * -----------------------------------------------------------------------*/
static inline void mfc_engine_init(mfc_engine* mfc)
{
    memset(mfc, 0, sizeof(*mfc));
    mfc->tag_completed = 0xFFFFFFFF; /* all tags idle = completed */
}

static inline void mfc_publish_atomic_status(mfc_engine* mfc, uint32_t status)
{
    mfc->atomic_stat = status;
    mfc->atomic_stat_ready = 1;
}

static inline int mfc_tag_update_satisfied(const mfc_engine* mfc)
{
    uint32_t completed = mfc->tag_completed & mfc->tag_update_mask;
    switch (mfc->tag_update_type) {
    case 0: return 1;
    case 1: return completed != 0;
    case 2: return completed == mfc->tag_update_mask;
    default: return 1;
    }
}

static inline void mfc_refresh_tag_status(mfc_engine* mfc, spu_context* spu)
{
    if (!mfc->tag_update_pending || !mfc_tag_update_satisfied(mfc))
        return;
    spu->mfc_tag_status = mfc->tag_completed & mfc->tag_update_mask;
    mfc->tag_update_pending = 0;
    mfc->tag_status_ready = 1;
    spu_ch_wake(spu);
}

static inline int mfc_tag_accept(mfc_engine* mfc, uint32_t tag)
{
    uint32_t bit = 1u << tag;
    if (mfc->queue_count >= MFC_QUEUE_DEPTH || mfc->tag_outstanding[tag] == UINT16_MAX)
        return -1;
    mfc->tag_outstanding[tag]++;
    mfc->queue_count++;
    mfc->tag_completed &= ~bit;
    return 0;
}

static inline void mfc_tag_finish(mfc_engine* mfc, spu_context* spu,
                                  uint32_t tag)
{
    uint32_t bit = 1u << tag;
    if (mfc->tag_outstanding[tag] != 0) {
        mfc->tag_outstanding[tag]--;
        if (mfc->queue_count != 0)
            mfc->queue_count--;
    }
    if (mfc->tag_outstanding[tag] == 0)
        mfc->tag_completed |= bit;
    else
        mfc->tag_completed &= ~bit;
    mfc_refresh_tag_status(mfc, spu);
    spu_ch_wake(spu);
}

static inline int mfc_has_deferred_tag(const mfc_engine* mfc, uint32_t tag)
{
    for (uint32_t i = 0; i < mfc->deferred_count; i++)
        if (mfc->queue[i].tag == tag)
            return 1;
    return 0;
}


static inline int mfc_defer_command(mfc_engine* mfc, uint32_t lsa, uint64_t ea,
                                    uint32_t size, uint32_t tag, uint32_t cmd)
{
    if (mfc->deferred_count >= MFC_QUEUE_DEPTH)
        return -1;
    mfc_cmd* q = &mfc->queue[mfc->deferred_count++];
    q->lsa = lsa;
    q->ea = ea;
    q->size = size;
    q->tag = tag;
    q->cmd = cmd;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Core DMA transfer (synchronous for recompiled code)
 *
 * In real hardware this would be asynchronous.  For a static recompiler the
 * DMA is executed immediately; the tag status is updated after completion.
 * -----------------------------------------------------------------------*/

static inline int mfc_is_get(uint32_t cmd)
{
    return (cmd & 0x40) != 0; /* GET family has bit 6 set */
}

static inline int mfc_is_put(uint32_t cmd)
{
    return (cmd & 0x20) != 0 && !mfc_is_get(cmd);
}

static inline int mfc_is_list(uint32_t cmd)
{
    return (cmd & 0x04) != 0; /* list variants have bit 2 set */
}

static inline int mfc_is_fence(uint32_t cmd)
{
    return (cmd & 0x02) != 0; /* fence variants have bit 1 set */
}


static inline int mfc_uses_tag(uint32_t cmd)
{
    switch (cmd) {
    case MFC_PUT_CMD:  case MFC_PUTB_CMD:  case MFC_PUTF_CMD:
    case MFC_GET_CMD:  case MFC_GETB_CMD:  case MFC_GETF_CMD:
    case MFC_PUTL_CMD: case MFC_PUTLB_CMD: case MFC_PUTLF_CMD:
    case MFC_GETL_CMD: case MFC_GETLB_CMD: case MFC_GETLF_CMD:
    case MFC_SNDSIG_CMD:
        return 1;
    default:
        return 0;
    }
}


/*
 * Execute a single DMA transfer between local store and main memory.
 * Returns 0 on success, -1 on error.
 */
static inline int mfc_do_transfer(spu_context* spu, uint32_t lsa, uint64_t ea,
                                   uint32_t size, uint32_t cmd)
{
    /* Zero is an architecturally valid no-op transfer size. It still retires
     * its command and participates in tag completion. */
    if (size > MFC_MAX_DMA_SIZE)
        return -1;
    if (size == 0)
        return 0;

    /* Mask LSA to local store range */
    lsa &= SPU_LS_MASK;


    /* An MFC transfer may not cross the 256 KiB local-store boundary.  Masking
     * the start address alone is insufficient: a large transfer near the end
     * of LS would otherwise overwrite the spu_context fields that immediately
     * follow ls[] (including the per-SPU wait condition variable). */
    if (size > SPU_LS_SIZE - lsa) {
        static unsigned long oob_n = 0;
        const unsigned long n = ++oob_n;
        if (n <= 32 || (n & 0xFFu) == 0) {
            fprintf(stderr,
                    "[spu-dma] skip LS overflow n=%lu spu=%X img=%d "
                    "pc=0x%05X cmd=0x%02X lsa=0x%05X size=0x%X\n",
                    n, spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK,
                    cmd, lsa, size);
            fflush(stderr);
        }
        /*
         * The recompiled caller retries a negative channel result, so an
         * error here would turn one malformed command into a permanent SPU
         * busy loop.  Retire the invalid transfer as a no-op instead, like
         * the guarded-EA case below.
         */
        return 0;
    }

    uint8_t* ls_ptr = &spu->ls[lsa];
    uint8_t* ea_ptr = vm_base + (uint32_t)ea; /* PS3 uses 32-bit effective addresses for SPU DMA */

    /* The 4 GB guest space is reserved but only specific regions are committed
     * (main RAM, stacks, on-demand allocations). A DMA with a bad EA (e.g. the
     * SPURS GUID-trace buffer when tracing is unconfigured) points into
     * reserved-but-uncommitted memory, so `vm_base + ea` is a wild pointer and
     * the memcpy segfaults the host. Guard the copy: on an access violation,
     * log + skip the transfer (a benign no-op) instead of crashing. */
#ifdef _WIN32
    __try {
#endif
        if (mfc_is_get(cmd)) {
            /* GET: main memory -> local store. */
            memcpy(ls_ptr, ea_ptr, size);
            if (spu->image_id == 4 && size == 0x40u) {
                uint32_t rea = (uint32_t)ea;
                if (rea >= 0x42452880u && rea < 0x424529E0u) {
                    /*
                     * The GCM callback raises the SPU signal before
                     * publishing this record.  Order every image-4 record
                     * fetch after callback completion, then re-copy. Checking
                     * only for value==0 would miss the stale-nonzero form
                     * where record N-1 remains while N is staged.
                     */
#ifdef _WIN32
                    /* Treat the callback epochs as a sequence lock.  A
                     * callback can begin after the first completion check,
                     * so a single wait followed by memcpy still admits a
                     * half-old/half-new record.  Retry until the copy was
                     * entirely outside a callback update. */
                    long stable_epoch;
                    do {
                        stable_epoch =
                            yz_ucmd_wait_for_handler_completion();
                        memcpy(ls_ptr, ea_ptr, size);
                    } while (!yz_ucmd_handler_snapshot_is_stable(
                        stable_epoch));
#else
                    memcpy(ls_ptr, ea_ptr, size);
#endif
                }
            }
        } else if (mfc_is_put(cmd)) {
            /* PUT: local store -> main memory */
            /* GETLLAR snapshots a line under the lockline lock, so an unlocked
             * payload copy here could produce torn mixed-writer 128-byte
             * snapshots under contention. Every other writer path (GETLLAR/
             * PUTLLC/PUTLLUC) copies under the lock; do the same when the span
             * overlaps any reserved line, and keep the lock-free copy for
             * unreserved spans (bulk asset PUTs are large and hot).
             * Residual: a line reserved after the span check but during an
             * unlocked copy can still observe a tear, a far narrower window
             * than an unlocked whole-copy. */
            {
                extern int  spu_coh_is_reserved(uint32_t);
                extern void spu_coh_notify_write(uint32_t);
                extern void spu_lockline_lock(void);
                extern void spu_lockline_unlock(void);
                int span_reserved = 0;
                uint32_t a0 = (uint32_t)ea & ~127u;
                uint32_t a1 = size ? (((uint32_t)ea + size - 1u) & ~127u) : a0;
                if (size != 0)
                    for (uint32_t a = a0; ; a += 128u) {
                        if (spu_coh_is_reserved(a)) { span_reserved = 1; break; }
                        if (a == a1) break;
                    }
                if (span_reserved) {
                    /* copy + generation bump + peer-reservation kill as one
                     * critical section (spu_coh_notify_write expects the lock
                     * held, the same contract as the PUTLLC commit path). */
                    spu_lockline_lock();
                    memcpy(ea_ptr, ls_ptr, size);
                    for (uint32_t a = a0; ; a += 128u) {
                        if (spu_coh_is_reserved(a)) spu_coh_notify_write(a);
                        if (a == a1) break;
                    }
                    spu_lockline_unlock();
                } else {
                    memcpy(ea_ptr, ls_ptr, size);
                    /* Unreserved case: any line that turns out reserved still
                     * gets its generation bump (the fast-path staleness
                     * guard). */
                    if (size != 0)
                        for (uint32_t a = a0; ; a += 128u) {
                            if (spu_coh_is_reserved(a)) {
                                spu_lockline_lock();
                                spu_coh_notify_write(a);
                                spu_lockline_unlock();
                            }
                            if (a == a1) break;
                        }
                }
            }
            /* Verify-after-write for any PUT covering the decode label
             * 0x10200FE0.  A publish of that label can fail to land (a
             * concurrent line restorer can overwrite it); read the word back
             * and, on mismatch, log and write it again.  One compare on a
             * rare EA. */
            {
                uint32_t vea = (uint32_t)ea;
                if (vea <= 0x10200FE0u && vea + size > 0x10200FE0u && size <= 0x80u) {
                    uint32_t off = 0x10200FE0u - vea;
                    uint32_t want_, got_;
                    memcpy(&want_, ls_ptr + off, 4);
                    memcpy(&got_,  vm_base + 0x10200FE0u, 4);
                    if (want_ != got_) {
                        fprintf(stderr, "[put-verify] LABEL PUT DID NOT LAND: wrote=%02X%02X%02X%02X "
                                "readback=%02X%02X%02X%02X spu=%X pc=0x%05X; rewriting\n",
                                ls_ptr[off], ls_ptr[off+1], ls_ptr[off+2], ls_ptr[off+3],
                                ((uint8_t*)&got_)[0], ((uint8_t*)&got_)[1],
                                ((uint8_t*)&got_)[2], ((uint8_t*)&got_)[3],
                                spu->spu_id, spu->pc & SPU_LS_MASK);
                        fflush(stderr);
                        memcpy(vm_base + 0x10200FE0u, ls_ptr + off, 4);
                    }
                }
            }
        }
        if (mfc_is_put(cmd)) {
            extern void cellSpursNotifyGuestWrite(uint32_t, uint32_t);
            cellSpursNotifyGuestWrite((uint32_t)ea, size);
        }
#ifdef _WIN32
    } __except (GetExceptionCode() == 0xC0000005u /* EXCEPTION_ACCESS_VIOLATION */
                ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        static int w = 0; if (w < 24) { w++;
            fprintf(stderr, "[spu-dma] SKIP faulting %s ea=0x%08X size=%u lsa=0x%05X "
                    "spu=%X image=%d pc=0x%05X "
                    "(reserved/uncommitted guest mem -- e.g. unconfigured SPURS trace buffer)\n",
                    mfc_is_get(cmd) ? "GET" : "PUT", (uint32_t)ea, size, lsa,
                    spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK);
            fflush(stderr); }
        return 0;   /* benign skip */
    }
#endif

    return 0;
}

/* SPU_EVENT_SN (stall-and-notify tag event, event bit 0x2). Raised the first
 * time any tag transitions into the stalled set, i.e. edge-triggered on
 * 0->nonzero stall_mask, not re-raised per stalled tag. */
#define SPU_EVENT_SN 0x2u

/*
 * Execute a DMA list command (scatter/gather), starting at element `start_elem`
 * of a list of `num_elements` total (both 0 unless resuming a
 * stalled list; see mfc_channel_write's MFC_WrListStallAck case below).
 * The list resides in the SPU's local store at `list_lsa`; `cur_lsa` is the
 * running LS landing cursor (the caller's mfc_lsa on a fresh start, or the
 * saved cursor on resume). `base_cmd` is the GET/PUT opcode with the list bit
 * already stripped.
 *
 * Returns 0 on normal completion, 1 if the list stalled on a stall-and-notify
 * element (the resume state is kept in mfc->stall_*[tag] rather than a
 * re-queued mfc_cmd since our DMA is synchronous), or a negative
 * mfc_do_transfer error.
 */
static inline int mfc_do_list_transfer_from(mfc_engine* mfc, spu_context* spu,
                                             uint32_t tag, uint32_t list_lsa,
                                             uint64_t ea_base, uint32_t base_cmd,
                                             uint32_t start_elem, uint32_t num_elements,
                                             uint32_t cur_lsa)
{
    for (uint32_t i = start_elem; i < num_elements; i++) {
        uint32_t elem_lsa = (list_lsa + i * 8) & SPU_LS_MASK;

        /* Read list element from local store (big-endian): the first word
         * holds the stall-and-notify flag in its most significant bit and the
         * transfer size in its low 15 bits; the second word is the low 32 bits
         * of the effective address. */
        uint32_t size_and_flags = spu_ls_read32(spu, elem_lsa);
        uint32_t eal = spu_ls_read32(spu, elem_lsa + 4);

        uint32_t xfer_size = size_and_flags & 0x7FFF; /* low 15 bits */
        int stall_notify = (size_and_flags >> 31) & 1; /* byte0 bit7, not bit15 */

        uint64_t ea = (ea_base & 0xFFFFFFFF00000000ull) | eal;

        /* For a sub-quadword element, the transfer LSA takes its low nibble
         * from the element EA. After every nonzero element the running LSA
         * advances to the next quadword boundary. */
        uint32_t transfer_lsa = cur_lsa;
        if (xfer_size != 0 && xfer_size < 16)
            transfer_lsa = (cur_lsa & ~0xFu) | (eal & 0xFu);

        int rc = mfc_do_transfer(spu, transfer_lsa, ea, xfer_size, base_cmd);
        if (rc != 0)
            return rc;

        if (xfer_size != 0)
            cur_lsa = (transfer_lsa + xfer_size + 15u) & ~15u;

        /* A stall bit on the final element is unsupported and ignored. */
        if (stall_notify && i + 1u < num_elements) {
            /* Suspend the list after this element (the transfer above already
             * ran: the stalling element's own transfer completes; only the
             * rest of the list is held). Record enough to resume from
             * element i+1 on MFC_WrListStallAck, and raise Sn (edge-triggered
             * on the first tag to stall). */
            uint32_t bit = 1u << (tag & 0x1Fu);
            if (!mfc->stall_mask) {
                /* event_status is _Atomic uint32_t (runtime/spu/spu_context.h);
                 * atomic_fetch_or_explicit, not a plain `|=`, for consistency
                 * with the field's other writers (this one is same-thread, the
                 * issuing SPU's own list transfer, so relaxed is enough; no
                 * cross-thread ordering is being established here). */
                atomic_fetch_or_explicit(&spu->event_status, SPU_EVENT_SN, memory_order_relaxed);
                spu_ch_wake(spu);   /* wake a blocked RdEventStat */
            }
            mfc->stall_mask |= bit;
            mfc->stall_list_lsa[tag]     = list_lsa;
            mfc->stall_ea_base[tag]      = ea_base;
            mfc->stall_next_elem[tag]    = i + 1u;
            mfc->stall_num_elements[tag] = num_elements;
            mfc->stall_cur_lsa[tag]      = cur_lsa;
            mfc->stall_base_cmd[tag]     = base_cmd;
            return 1;   /* stalled: the caller must not mark the tag completed */
        }
    }

    return 0;
}

/* Fresh (non-resuming) list transfer used by mfc_submit; routes through
 * mfc_do_list_transfer_from starting at element 0. */
static inline int mfc_do_list_transfer(mfc_engine* mfc, spu_context* spu,
                                        uint32_t tag, uint32_t transfer_lsa,
                                        uint64_t list_addr, uint32_t list_size,
                                        uint32_t cmd)
{
    if (list_size > MFC_MAX_DMA_SIZE || (list_size & 7u) != 0)
        return -1;

    /* list_size is in bytes; each element is 8 bytes */
    uint32_t num_elements = list_size / 8;
    uint32_t base_cmd = cmd & ~0x04u; /* strip the 'list' bit to get base GET/PUT */
    /* For a list command, MFC_EAL is not an effective address: it is the
     * local-store pointer to the list. MFC_LSA remains the transfer cursor,
     * and MFC_EAH supplies the high half of every element's effective address. */
    uint32_t list_lsa = (uint32_t)list_addr & SPU_LS_MASK;
    uint64_t ea_base = list_addr & 0xFFFFFFFF00000000ull;

    /* The LS landing accumulates across elements, but the staging MFC_LSA
     * register itself is not mutated. */
    uint32_t cur_lsa = transfer_lsa;

    return mfc_do_list_transfer_from(mfc, spu, tag, list_lsa, ea_base, base_cmd,
                                      0, num_elements, cur_lsa);
}

/*
 * Submit and immediately execute an MFC command.
 * This is the main entry point called when the SPU writes to MFC_Cmd.
 */
static inline int mfc_submit(mfc_engine* mfc, spu_context* spu, uint32_t cmd)
{
    uint32_t lsa  = spu->mfc_lsa;
    uint64_t ea   = ((uint64_t)spu->mfc_eah << 32) | spu->mfc_eal;
    uint32_t size = spu->mfc_size;
    uint32_t tag  = spu->mfc_tag & 0x1F;
    int tagged = mfc_uses_tag(cmd);
    int rc = 0;


    if (tagged && !mfc->draining_deferred && mfc_tag_accept(mfc, tag) != 0)
        return -1;

    /* A fence cannot pass an older stalled command with the same tag. Once a
     * fence is queued, later commands on that tag preserve issue order behind
     * it until the stalled command retires. */
    if (tagged && !mfc->draining_deferred &&
        ((mfc_is_fence(cmd) && (mfc->stall_mask & (1u << tag))) ||
         mfc_has_deferred_tag(mfc, tag))) {
        if (mfc_defer_command(mfc, lsa, ea, size, tag, cmd) == 0)
            return 0;
        mfc_tag_finish(mfc, spu, tag);
        return -1;
    }

    /* Handle sync/barrier commands */
    if (cmd == MFC_BARRIER_CMD || cmd == MFC_EIEIO_CMD || cmd == MFC_SYNC_CMD) {
        /* For synchronous emulation these are no-ops; all prior DMAs
         * have already completed. */
        if (tagged) mfc_tag_finish(mfc, spu, tag);
        return 0;
    }

    /* Record the context that consumes the command journal: the first
     * image-0 GET from the journal arena [0x41F00000,0x42200000).  Readers of
     * g_yz_consumer_ctx must accept it being unset. */
    if (!g_yz_consumer_ctx && spu->image_id == 0 && mfc_is_get(cmd)
        && ea >= 0x41F00000ull && ea < 0x42200000ull) {
        g_yz_consumer_ctx = (volatile void*)spu;
    }



    /* Lock-line atomics: a 128-byte line, EA implicitly 128-aligned, the
     * MFC_Size register is ignored. Status lands in MFC_RdAtomicStat. */
    if (cmd == MFC_GETLLAR_CMD || cmd == MFC_PUTLLC_CMD ||
        cmd == MFC_PUTLLUC_CMD || cmd == MFC_PUTQLLUC_CMD) {
        int post_unlock_idle_level = -1;
        /* Sony's SPURS kernel/system-service tags lock-line atomic EAs with
         * bit 31 (a memory coherency/cache attribute the Cell MFC strips when
         * resolving the real address). The SPURS management area lives at the
         * instance base (e.g. 0x40197C80); the SPU does GETLLAR/PUTLLC against
         * base|0x80000000. We model one backing store for the line, so clear
         * the attribute bit to reach it. Scoped to lock-line only: regular DMA
         * already uses the clean base, and legit VRAM DMA (0xC0000000+) must
         * keep bit 31. With the bit cleared, the masked EA hits the real
         * instance and the SPURS CAS handshake advances. */
        ea &= ~0x80000000ull;
        /* Lock-line atomics against guest [0,0x10000) complete against a
         * private zero-initialised backing store.  That page is uncommitted
         * here but mapped on the console, where such a CAS completes; a guest
         * CAS retry loop at EA 0 must terminate rather than fault or spin.
         * The backing is not arbitrated across SPU threads: nothing
         * legitimate lives there, stray traffic only needs to terminate. */
        if ((uint32_t)(ea & ~127ull) < 0x10000u) {
            static uint8_t s_lowmem[0x10000];
            const uint32_t loff = (uint32_t)ea & 0xFFFFu & ~127u;
            uint8_t* lls = &spu->ls[lsa & SPU_LS_MASK & ~127u];
            if (cmd == MFC_GETLLAR_CMD) {
                memcpy(lls, s_lowmem + loff, 128);
                memcpy(mfc->resv_data, s_lowmem + loff, 128);
                mfc->resv_active = 1;
                mfc->resv_ea = (ea & ~127ull);
                mfc_publish_atomic_status(mfc, MFC_GETLLAR_SUCCESS);
            } else if (cmd == MFC_PUTLLC_CMD) {
                if (mfc->resv_active && mfc->resv_ea == (ea & ~127ull)
                    && memcmp(s_lowmem + loff, mfc->resv_data, 128) == 0) {
                    memcpy(s_lowmem + loff, lls, 128);
                    mfc->resv_active = 0;
                    mfc_publish_atomic_status(mfc, MFC_PUTLLC_SUCCESS);
                } else {
                    mfc_publish_atomic_status(mfc, MFC_PUTLLC_FAILURE);
                }
            } else {
                memcpy(s_lowmem + loff, lls, 128);
                mfc_publish_atomic_status(mfc, MFC_PUTLLUC_SUCCESS);
            }
            if (tagged) mfc_tag_finish(mfc, spu, tag);
            return 0;
        }
        uint8_t* line = vm_base + ((uint32_t)ea & ~127u);
        uint8_t* ls_ptr = &spu->ls[lsa & SPU_LS_MASK & ~127u];
        /* Lock-free GETLLAR idle poll. SPURS kernels poll their management
         * lines with GETLLAR, and taking the process-wide lock-line lock for
         * every poll is expensive. When this context re-GETLLARs the same line and
         * the line's write generation has not moved, the cached reservation
         * copy still equals memory (every writer bumps the generation: PPU
         * VM_WRITE_COH/stwcx/stdcx, SPU PUTLLC commits, and plain PUTs into
         * reserved lines), so serve the cached copy without the lock. Reaction
         * latency to a real write stays one generation-check deep; LR wake edge
         * semantics are identical (cached copy == memory while the generation
         * holds). A peer's notify bumps the generation before clearing
         * resv_active, so a stale resv_active read here still fails the
         * generation check. */
        if (cmd == MFC_GETLLAR_CMD) {
            uint64_t le = ea & ~127ull;
            extern uint32_t spu_coh_gen(uint32_t);
            /* Whitelist: host-side bulk writers (HLE _sys_memset/_sys_memcpy,
             * file reads) write guest memory without bumping the generation,
             * so a general cached-serve can go stale. Until
             * those paths sweep the coherence bitmap, serve cache-fast only for
             * ranges whose writers notify the generation map.  The SPURS
             * management poll line is written through CAS/VM_WRITE_COH.  The
             * command journal is produced by lifted VM stores and SPU PUTs,
             * both of which notify the same generation map.  Keeping this as
             * an exact range avoids making unrelated file/bulk-write memory
             * cacheable. */
            const int coherent_poll_line =
                le == 0x40197C80ull ||
                (le >= 0x41F00000ull && le < 0x42110000ull);
            if (coherent_poll_line && mfc->resv_active && mfc->resv_ea == le
                && spu_coh_gen((uint32_t)le) == mfc->resv_gen) {
                memcpy(ls_ptr, mfc->resv_data, 128);
                mfc->resv_poll_n++;
                if (mfc->resv_poll_n > 16)
                    spu_idle_yield(mfc->resv_poll_n > (1u << 20) ? 2
                                 : mfc->resv_poll_n > 256       ? 1 : 0);
                {
                  if ((uint32_t)le == 0x40197C80u
                      && (mfc->resv_data[0x70] | mfc->resv_data[0x71])) {
                      /* Same-thread self-raise of LR; relaxed order is enough. */
                      atomic_fetch_or_explicit(&spu->event_status, 0x400u, memory_order_relaxed); spu_ch_wake(spu); } }
                mfc_publish_atomic_status(mfc, MFC_GETLLAR_SUCCESS);
                if (tagged) mfc_tag_finish(mfc, spu, tag);
                return 0;
            }
        }
        int notify_spurs_after_unlock = 0;
        spu_lockline_lock();
        switch (cmd) {
        case MFC_GETLLAR_CMD: {
            extern void spu_coh_reserve(uint32_t);   /* mark this line for PPU-write coherence */
            spu_coh_reserve((uint32_t)(ea & ~127ull));
            memcpy(ls_ptr, line, 128);
            memcpy(mfc->resv_data, line, 128);
            /* LR re-derivation: if the line's write-generation advanced since
             * this context last reserved it, a cross-processor write was missed
             * while we held no reservation -> raise SPU_EVENT_LR so the SPURS
             * idle kernel wakes instead of losing the wakeup. */
            { extern uint32_t spu_coh_gen(uint32_t);
              uint64_t le = ea & ~127ull; uint32_t g = spu_coh_gen((uint32_t)le);
              if (mfc->resv_ea == le && g != mfc->resv_gen) {
                  /* Same-thread self-raise of LR; relaxed order is enough. */
                      atomic_fetch_or_explicit(&spu->event_status, 0x400u, memory_order_relaxed); spu_ch_wake(spu); }
              /* Idle-poll backoff.
               * The SPURS kernels poll their management lines with GETLLAR at
               * full host speed and every poll takes the process-wide
               * lock-line spinlock, so the PPU threads' atomics contend
               * against the spinning SPU threads.
               * Polling continues; but when the same line is re-read
               * and its write-generation has not moved, arrange to yield the
               * host core with escalation after releasing the process-wide
               * lock-line lock. Yielding here while still owning that lock
               * can starve every SPU/PPU reservation user behind a sleeping
               * poller. Any observed change (or a different line) resets the
               * ladder, so reaction latency to real writes stays one poll
               * deep. */
              if (mfc->resv_ea == le && g == mfc->resv_gen) {
                  mfc->resv_poll_n++;
                  /* Rung 3 (real sleep) only after about a million unchanged
                   * polls, a long quiet stretch: each wake from a real sleep
                   * costs up to ~15 ms at default timer resolution. */
                  if (mfc->resv_poll_n > 16)
                      post_unlock_idle_level =
                          mfc->resv_poll_n > (1u << 20) ? 2
                        : mfc->resv_poll_n > 256        ? 1 : 0;
              } else {
                  mfc->resv_poll_n = 0;
              }
              mfc->resv_ea  = le;
              mfc->resv_gen = g; }
            mfc->resv_active = 1;
            mfc_publish_atomic_status(mfc, MFC_GETLLAR_SUCCESS);
            /* Lost-wakeup handling. The SPURS idle kernel backs off
             * (host-yield) between GETLLAR polls of its management line, and
             * the PPU-side wklSignal/readyCount write that submits a workload
             * does not raise the backed-off SPU's SPU_EVENT_LR (that write
             * bypasses the coherence write-generation path). The transient
             * signal can land in a poll gap and the kernel would never
             * re-enter workload selection. So on a GETLLAR of the management
             * line that still carries a pending workload signal
             * (wklSignal1 at +0x70 != 0), raise LR so the kernel re-enters
             * selection and runs the system service. It self-limits: once the
             * workload is dispatched the kernel is busy, not idle-polling. */
            {
              if ((uint32_t)(ea & ~127ull) == 0x40197C80u
                  && (line[0x70] | line[0x71])) {
                  /* Same-thread self-raise of LR; relaxed order is enough. */
                      atomic_fetch_or_explicit(&spu->event_status, 0x400u, memory_order_relaxed); spu_ch_wake(spu); } }
            break; }
        case MFC_PUTLLC_CMD:
            const int putllc_success =
                mfc->resv_active && mfc->resv_ea == (ea & ~127ull) &&
                memcmp(line, mfc->resv_data, 128) == 0;
            if (putllc_success) {
                const int made_progress =
                    memcmp(mfc->resv_data, ls_ptr, 128) != 0;
                memcpy(line, ls_ptr, 128);
                /* A committing PUTLLC is a line write: bump the coherence
                 * generation and kill peer reservations (CBEA reservation-lost;
                 * required for the lock-free GETLLAR fast path: a peer polling
                 * its cached copy must observe this commit). Clear our own
                 * reservation first so the notify loop does not raise a
                 * spurious self-LR (HW: a successful PUTLLC consumes the
                 * reservation, it does not "lose" it). Already under the lock. */
                mfc->resv_active = 0;
                { extern void spu_coh_notify_write(uint32_t);
                  spu_coh_notify_write((uint32_t)(ea & ~127ull)); }
                mfc_publish_atomic_status(mfc, MFC_PUTLLC_SUCCESS);
                post_unlock_idle_level = mfc_putllc_backoff(
                    mfc, (uint32_t)(ea & ~127ull), made_progress);
            } else {
                mfc_publish_atomic_status(mfc, MFC_PUTLLC_FAILURE);
                post_unlock_idle_level = mfc_putllc_backoff(
                    mfc, (uint32_t)(ea & ~127ull), 0);
            }
            mfc->resv_active = 0;
            break;
        default: /* PUTLLUC / PUTQLLUC: unconditional 128-byte line store. Per HW
                  * this invalidates every reservation on the line
                  * (this SPU + all others) and raises SPU_EVENT_LR, else another SPU's
                  * pending PUTLLC can spuriously succeed against a now-stale snapshot. */
            memcpy(line, ls_ptr, 128);
            mfc->resv_cas_idle_n = 0;
            { extern void spu_coh_notify_write(uint32_t);
              spu_coh_notify_write((uint32_t)(ea & ~127ull)); }  /* clears resv_active + raises LR for all matching ctxs */
            mfc_publish_atomic_status(mfc, MFC_PUTLLUC_SUCCESS);
            break;
        }
        notify_spurs_after_unlock =
            (cmd == MFC_PUTLLC_CMD &&
             mfc->atomic_stat == MFC_PUTLLC_SUCCESS) ||
            (cmd != MFC_GETLLAR_CMD && cmd != MFC_PUTLLC_CMD);
        /* Do not enter SPURS while the global reservation-line lock is held;
         * the helper releases it before notifying. */
        spu_lockline_unlock_then_notify_guest_write(
            notify_spurs_after_unlock, (uint32_t)(ea & ~127ull), 128);
        if (post_unlock_idle_level >= 0)
            spu_idle_yield(post_unlock_idle_level);
        if (tagged) mfc_tag_finish(mfc, spu, tag);
        return 0;
    }


    /* Execute the transfer */
    if (mfc_is_list(cmd)) {
        rc = mfc_do_list_transfer(mfc, spu, tag, lsa, ea, size, cmd);
    } else {
        rc = mfc_do_transfer(spu, lsa, ea, size, cmd);
    }

    /* Mark tag as completed, except when the list stalled (rc==1): the
     * transfer is genuinely incomplete until MFC_WrListStallAck resumes and
     * finishes it, so RdTagStat/WrTagUpdate must not report this tag done
     * yet (a stalled list is not completed, it is paused). */
    if (rc != 1) {
        /* Command retirement and fault reporting are independent. A retired
         * command must not leave an ALL tag-status request pending forever. */
        if (tagged) mfc_tag_finish(mfc, spu, tag);
    }

    return (rc == 1) ? 0 : rc;   /* stall is not a transfer error */
}

/* ---------------------------------------------------------------------------
 * Tag group synchronization
 * -----------------------------------------------------------------------*/

/* Tag update types (values written to MFC_WrTagUpdate) */
#define MFC_TAG_UPDATE_IMMEDIATE  0  /* return status immediately */
#define MFC_TAG_UPDATE_ANY        1  /* wait for any tag in mask */
#define MFC_TAG_UPDATE_ALL        2  /* wait for all tags in mask */

/*
 * Poll for tag completion.
 * update_type: 0 = immediate, 1 = any, 2 = all. ANY and ALL leave the
 * one-entry result channel pending until their completion condition is met.
 */
static inline void mfc_request_tag_status(mfc_engine* mfc, spu_context* spu,
                                          uint32_t tag_mask, uint32_t update_type)
{
    mfc->tag_update_mask = tag_mask;
    mfc->tag_update_type = (uint8_t)update_type;
    mfc->tag_status_ready = 0;
    mfc->tag_update_pending = 1;
    mfc_refresh_tag_status(mfc, spu);
}

static inline void mfc_drain_deferred_tag(mfc_engine* mfc, spu_context* spu,
                                           uint32_t tag)
{
    while (!(mfc->stall_mask & (1u << tag))) {
        uint32_t index = mfc->deferred_count;
        for (uint32_t i = 0; i < mfc->deferred_count; i++) {
            if (mfc->queue[i].tag == tag) {
                index = i;
                break;
            }
        }
        if (index == mfc->deferred_count)
            break;

        mfc_cmd q = mfc->queue[index];
        for (uint32_t i = index + 1; i < mfc->deferred_count; i++)
            mfc->queue[i - 1] = mfc->queue[i];
        mfc->deferred_count--;
        memset(&mfc->queue[mfc->deferred_count], 0, sizeof(mfc->queue[0]));

        uint32_t saved_lsa = spu->mfc_lsa;
        uint32_t saved_eah = spu->mfc_eah;
        uint32_t saved_eal = spu->mfc_eal;
        uint32_t saved_size = spu->mfc_size;
        uint32_t saved_tag = spu->mfc_tag;
        spu->mfc_lsa = q.lsa;
        spu->mfc_eah = (uint32_t)(q.ea >> 32);
        spu->mfc_eal = (uint32_t)q.ea;
        spu->mfc_size = q.size;
        spu->mfc_tag = q.tag;
        mfc->draining_deferred = 1;
        (void)mfc_submit(mfc, spu, q.cmd);
        mfc->draining_deferred = 0;
        spu->mfc_lsa = saved_lsa;
        spu->mfc_eah = saved_eah;
        spu->mfc_eal = saved_eal;
        spu->mfc_size = saved_size;
        spu->mfc_tag = saved_tag;
    }
}

/* ---------------------------------------------------------------------------
 * Convenience: process an SPU channel write for MFC-related channels.
 * -----------------------------------------------------------------------*/
static inline void mfc_channel_write(mfc_engine* mfc, spu_context* spu,
                                      uint32_t channel, uint32_t value)
{
    switch (channel) {
    case MFC_LSA:
        spu->mfc_lsa = value;
        break;
    case MFC_EAH:
        spu->mfc_eah = value;
        break;
    case MFC_EAL:
        spu->mfc_eal = value;
        break;
    case MFC_Size:
        spu->mfc_size = value;
        break;
    case MFC_TagID:
        spu->mfc_tag = value & 0x1F;
        break;
    case MFC_Cmd:
        mfc_submit(mfc, spu, value);
        break;
    case MFC_WrTagMask:
        spu->mfc_tag_mask = value;
        break;
    case MFC_WrTagUpdate:
        mfc_request_tag_status(mfc, spu, spu->mfc_tag_mask, value);
        break;
    case MFC_WrListStallAck:
        /* The written value's low 5 bits select a tag (not an element index;
         * matches RdListStallStat's per-tag bitmask below), whose stalled list
         * transfer resumes from the next unprocessed element. Our DMA model is
         * synchronous, so "resume" means finish the rest of the list right
         * here rather than re-queuing an async command; the guest-visible
         * contract (RdTagStat only reports the tag completed once the whole
         * list, including the part after the stall point, has transferred) is
         * preserved either way. An ack for a tag that isn't currently stalled
         * is a no-op (nothing to resume). */
        {
            uint32_t t = value & 0x1Fu;
            uint32_t bit = 1u << t;
            if (mfc->stall_mask & bit) {
                mfc->stall_mask &= ~bit;
                int rc = mfc_do_list_transfer_from(mfc, spu, t,
                             mfc->stall_list_lsa[t], mfc->stall_ea_base[t],
                             mfc->stall_base_cmd[t], mfc->stall_next_elem[t],
                             mfc->stall_num_elements[t], mfc->stall_cur_lsa[t]);
                /* rc==1: the resumed remainder hit another stall-and-notify
                 * element; mfc_do_list_transfer_from already re-recorded
                 * mfc->stall_*[t] and re-set the stall bit + Sn event for
                 * this tag, so the tag correctly stays incomplete. Only a
                 * clean finish (rc==0) marks the tag's DMA as done, the
                 * same "list truly complete" gate mfc_submit applies to a
                 * fresh (non-resumed) list. */
                if (rc == 0) {
                    mfc_tag_finish(mfc, spu, t);
                    mfc_drain_deferred_tag(mfc, spu, t);
                } else if (rc < 0) {
                    mfc_tag_finish(mfc, spu, t);
                    mfc_drain_deferred_tag(mfc, spu, t);
                }
            }
        }
        break;
    default:
        break;
    }
}

static inline uint32_t mfc_channel_read(mfc_engine* mfc, spu_context* spu,
                                         uint32_t channel)
{
    switch (channel) {
    case MFC_RdTagStat:
        {
            uint32_t status = spu->mfc_tag_status;
            mfc->tag_status_ready = 0;
            spu_ch_wake(spu);
            return status;
        }
    case MFC_RdTagMask:
        return spu->mfc_tag_mask;
    case MFC_RdListStallStat:
        /* A per-tag bitmask of which tag groups' list transfers are currently
         * stalled, not an element index. Zero when nothing is stalled. */
        return mfc->stall_mask;
    case MFC_RdAtomicStat:
        {
            uint32_t status = mfc->atomic_stat;
            if (mfc->atomic_stat_ready)
                mfc->atomic_stat_ready = 0;
            return status;
        }
    default:
        return 0;
    }
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_DMA_H */
