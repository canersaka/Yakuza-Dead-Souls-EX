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
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MFC queue depth */
#define MFC_QUEUE_DEPTH     16

/* Maximum DMA transfer size per element */
#define MFC_MAX_DMA_SIZE    (16 * 1024)

/* ---------------------------------------------------------------------------
 * MFC command entry
 * -----------------------------------------------------------------------*/
typedef struct mfc_cmd {
    uint32_t lsa;       /* Local store address */
    uint64_t ea;        /* Effective (main memory) address: EAH << 32 | EAL */
    uint32_t size;      /* Transfer size in bytes */
    uint32_t tag;       /* Tag group ID (0-31) */
    uint32_t cmd;       /* Command opcode (MFC_PUT_CMD, MFC_GET_CMD, etc.) */

    /* Status */
    int      active;    /* 1 = pending, 0 = completed/free */
} mfc_cmd;

/* DMA list element (used by PUTL/GETL) */
typedef struct mfc_list_element {
    /* In PS3 memory these are big-endian:
     *   bits  0-14 : reserved (notify/stall flags in upper halfword)
     *   bit   15   : stall-and-notify
     *   bits 16-31 : transfer size (low 16 bits)
     *   bits 32-63 : effective address low 32 bits
     */
    uint32_t size_and_flags;
    uint32_t eal;
} mfc_list_element;

/* ---------------------------------------------------------------------------
 * MFC DMA engine state
 * -----------------------------------------------------------------------*/
typedef struct mfc_engine {
    mfc_cmd     queue[MFC_QUEUE_DEPTH];
    uint32_t    queue_count;

    /* Per-tag completion tracking (bitmask: bit N = tag N completed) */
    uint32_t    tag_completed;

    /* Lock-line reservation (GETLLAR/PUTLLC). A 128-byte snapshot of the
     * line is taken under the global lock-line lock; PUTLLC commits only
     * if main memory still matches it (compare-and-swap semantics, the
     * same scheme RPCS3 uses). */
    uint64_t    resv_ea;          /* line EA (128-aligned), 0 = none */
    uint8_t     resv_data[128];
    int         resv_active;
    uint32_t    atomic_stat;      /* last MFC_RdAtomicStat value */
} mfc_engine;

/* Single process-wide lock serializing lock-line transactions across all
 * SPU host threads (defined in spu_channels.c). */
void spu_lockline_lock(void);
void spu_lockline_unlock(void);

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

static inline int mfc_is_barrier(uint32_t cmd)
{
    return (cmd & 0x01) != 0; /* barrier variants have bit 0 set */
}

static inline int mfc_is_fence(uint32_t cmd)
{
    return (cmd & 0x02) != 0; /* fence variants have bit 1 set */
}

/*
 * Execute a single DMA transfer between local store and main memory.
 * Returns 0 on success, -1 on error.
 */
static inline int mfc_do_transfer(spu_context* spu, uint32_t lsa, uint64_t ea,
                                   uint32_t size, uint32_t cmd)
{
    /* Validate size */
    if (size == 0 || size > MFC_MAX_DMA_SIZE)
        return -1;

    /* Mask LSA to local store range */
    lsa &= SPU_LS_MASK;

    uint8_t* ls_ptr = &spu->ls[lsa];
    uint8_t* ea_ptr = vm_base + (uint32_t)ea; /* PS3 uses 32-bit effective addresses for SPU DMA */

#ifdef SPU_DMA_LOG
    { extern int g_spu_dma_log; if (g_spu_dma_log-- > 0)
        fprintf(stderr, "[spu-dma] %s lsa=0x%05X ea=0x%08X size=%u\n",
                mfc_is_get(cmd) ? "GET" : "PUT", lsa, (uint32_t)ea, size); }
#endif
    if (mfc_is_get(cmd)) {
        /* GET: main memory -> local store */
        memcpy(ls_ptr, ea_ptr, size);
    } else if (mfc_is_put(cmd)) {
        /* PUT: local store -> main memory */
        memcpy(ea_ptr, ls_ptr, size);
    }

    return 0;
}

/*
 * Execute a DMA list command (scatter/gather).
 * The list resides in the SPU's local store at `lsa`.
 * Each list element describes a (size, EA) pair for a sub-transfer.
 */
static inline int mfc_do_list_transfer(spu_context* spu, uint32_t list_lsa,
                                        uint64_t ea_base, uint32_t list_size,
                                        uint32_t cmd)
{
    /* list_size is in bytes; each element is 8 bytes */
    uint32_t num_elements = list_size / 8;
    uint32_t base_cmd = cmd & ~0x04u; /* strip the 'list' bit to get base GET/PUT */

    for (uint32_t i = 0; i < num_elements; i++) {
        uint32_t elem_lsa = (list_lsa + i * 8) & SPU_LS_MASK;

        /* Read list element from local store (big-endian) */
        uint32_t size_and_flags = spu_ls_read32(spu, elem_lsa);
        uint32_t eal = spu_ls_read32(spu, elem_lsa + 4);

        uint32_t xfer_size = size_and_flags & 0x7FFF; /* low 15 bits */
        int stall_notify = (size_and_flags >> 15) & 1;

        uint64_t ea = (ea_base & 0xFFFFFFFF00000000ull) | eal;

        /* Calculate target LSA: for list commands, the data starts at
         * the address given by the MFC_LSA channel and accumulates. */
        int rc = mfc_do_transfer(spu, spu->mfc_lsa, ea, xfer_size, base_cmd);
        if (rc != 0) return rc;

        spu->mfc_lsa += xfer_size;

        if (stall_notify) {
            /* In a full emulator we would raise a stall-and-notify event.
             * For recompiled code, we just continue. */
        }
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Enqueue an MFC command (called when SPU writes to MFC_Cmd channel)
 * -----------------------------------------------------------------------*/
static inline int mfc_enqueue(mfc_engine* mfc, spu_context* spu)
{
    if (mfc->queue_count >= MFC_QUEUE_DEPTH)
        return -1; /* queue full */

    uint32_t lsa  = spu->mfc_lsa;
    uint64_t ea   = ((uint64_t)spu->mfc_eah << 32) | spu->mfc_eal;
    uint32_t size = spu->mfc_size;
    uint32_t tag  = spu->mfc_tag;
    uint32_t cmd  = spu->ls[0]; /* overridden below */

    /* The cmd was just written to the MFC_Cmd channel -- we receive it
     * as a parameter to the channel write handler.  For this inline API,
     * the caller should pass it.  We use a wrapper below. */
    (void)cmd;
    return 0;
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
    int rc = 0;

    /* Mark tag as in-progress */
    mfc->tag_completed &= ~(1u << tag);

    /* Handle sync/barrier commands */
    if (cmd == MFC_BARRIER_CMD || cmd == MFC_EIEIO_CMD || cmd == MFC_SYNC_CMD) {
        /* For synchronous emulation these are no-ops; all prior DMAs
         * have already completed. */
        mfc->tag_completed |= (1u << tag);
        return 0;
    }

    /* TEMP DEBUG (7d bring-up): trace DMAs that load WORKLOAD CODE into LS
     * (lsa in the workload region >=0x900, code-sized). These reveal the
     * source EA + size of the SPURS system-service / policy blobs we must
     * lift. Strip once SPURS workloads run. */
    if (mfc_is_get(cmd) && (lsa & SPU_LS_MASK) >= 0x900 && size >= 0x80) {
        static int wl_log = 40;
        if (wl_log > 0) {
            wl_log--;
            fprintf(stderr, "[spu-wl] spu=%X cmd=0x%02X lsa=0x%05X ea=0x%08X size=0x%X\n",
                    spu->spu_id, cmd, lsa & SPU_LS_MASK, (uint32_t)ea, size);
            fflush(stderr);
        }
    }

    /* Lock-line atomics: a 128-byte line, EA implicitly 128-aligned, the
     * MFC_Size register is ignored. Status lands in MFC_RdAtomicStat. */
    if (cmd == MFC_GETLLAR_CMD || cmd == MFC_PUTLLC_CMD ||
        cmd == MFC_PUTLLUC_CMD || cmd == MFC_PUTQLLUC_CMD) {
        /* Sony's SPURS kernel/system-service tags lock-line atomic EAs with
         * bit 31 (a memory coherency/cache attribute the Cell MFC strips when
         * resolving the real address). The SPURS management area lives at the
         * instance base (e.g. 0x40197C80); the SPU does GETLLAR/PUTLLC against
         * base|0x80000000. We model one backing store for the line, so clear
         * the attribute bit to reach it. Scoped to lock-line only: regular DMA
         * already uses the clean base, and legit VRAM DMA (0xC0000000+) must
         * keep bit 31. Verified live: base reads 0x40197C80, masked EA hits the
         * real instance and the SPURS CAS handshake then advances. */
        ea &= ~0x80000000ull;
        uint8_t* line = vm_base + ((uint32_t)ea & ~127u);
        uint8_t* ls_ptr = &spu->ls[lsa & SPU_LS_MASK & ~127u];
        spu_lockline_lock();
        switch (cmd) {
        case MFC_GETLLAR_CMD:
            memcpy(ls_ptr, line, 128);
            memcpy(mfc->resv_data, line, 128);
            mfc->resv_ea     = ea & ~127ull;
            mfc->resv_active = 1;
            mfc->atomic_stat = MFC_GETLLAR_SUCCESS;
            break;
        case MFC_PUTLLC_CMD:
            if (mfc->resv_active && mfc->resv_ea == (ea & ~127ull) &&
                memcmp(line, mfc->resv_data, 128) == 0) {
                memcpy(line, ls_ptr, 128);
                mfc->atomic_stat = MFC_PUTLLC_SUCCESS;
            } else {
                mfc->atomic_stat = MFC_PUTLLC_FAILURE;
            }
            mfc->resv_active = 0;
            break;
        default: /* PUTLLUC / PUTQLLUC: unconditional line store */
            memcpy(line, ls_ptr, 128);
            mfc->atomic_stat = MFC_PUTLLUC_SUCCESS;
            break;
        }
        spu_lockline_unlock();
        mfc->tag_completed |= (1u << tag);
        return 0;
    }

    /* Execute the transfer */
    if (mfc_is_list(cmd)) {
        rc = mfc_do_list_transfer(spu, lsa, ea, size, cmd);
    } else {
        rc = mfc_do_transfer(spu, lsa, ea, size, cmd);
    }

    /* Mark tag as completed */
    mfc->tag_completed |= (1u << tag);

    return rc;
}

/* ---------------------------------------------------------------------------
 * Tag group synchronization
 * -----------------------------------------------------------------------*/

/* Tag update types (values written to MFC_WrTagUpdate) */
#define MFC_TAG_UPDATE_IMMEDIATE  0  /* return status immediately */
#define MFC_TAG_UPDATE_ANY        1  /* wait for any tag in mask */
#define MFC_TAG_UPDATE_ALL        2  /* wait for all tags in mask */

/*
 * Query tag status.  Returns the bitmask of completed tags that match
 * the tag mask.  In synchronous mode, all submitted DMAs are already
 * complete, so this just returns the intersection.
 */
static inline uint32_t mfc_read_tag_status(const mfc_engine* mfc, uint32_t tag_mask)
{
    return mfc->tag_completed & tag_mask;
}

/*
 * Poll for tag completion.
 * update_type: 0 = immediate, 1 = any, 2 = all.
 * Returns the completed tag mask.  In synchronous mode, always succeeds.
 */
static inline uint32_t mfc_tag_wait(const mfc_engine* mfc, uint32_t tag_mask,
                                     uint32_t update_type)
{
    uint32_t completed = mfc->tag_completed & tag_mask;

    switch (update_type) {
    case MFC_TAG_UPDATE_IMMEDIATE:
        return completed;
    case MFC_TAG_UPDATE_ANY:
        /* In async mode we would block until any bit is set.
         * Synchronous: always returns immediately. */
        return completed;
    case MFC_TAG_UPDATE_ALL:
        /* Block until all bits in mask are set. */
        return completed;
    default:
        return completed;
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
        spu->mfc_tag_status = mfc_tag_wait(mfc, spu->mfc_tag_mask, value);
        break;
    case MFC_WrListStallAck:
        /* Acknowledge stall -- no-op in synchronous mode */
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
        return spu->mfc_tag_status;
    case MFC_RdTagMask:
        return spu->mfc_tag_mask;
    case MFC_RdListStallStat:
        return 0; /* no stalls in synchronous mode */
    case MFC_RdAtomicStat:
        return mfc->atomic_stat;
    default:
        return 0;
    }
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_DMA_H */
