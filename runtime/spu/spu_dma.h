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

/* a010-only producer/consumer root-cause gate, owned by import_overrides.cpp. */
extern volatile long g_yz_a010_root_active;
extern volatile long g_yz_a010_release_scene_active;
extern volatile long g_yz_a010_spu_puts;
extern volatile long g_yz_a010_spu_put_bytes;
extern volatile long g_yz_a010_spu_groups;
extern volatile long g_yz_a010_spu_group_bytes;
extern volatile long g_yz_a010_spu_headers;
extern volatile long g_yz_a010_spu_args;
extern volatile long g_yz_a010_spu_begin;
extern volatile long g_yz_a010_spu_end;
extern volatile long g_yz_a010_spu_array;
extern volatile long g_yz_a010_spu_index;
extern volatile long g_yz_a010_spu_vp;
extern volatile long g_yz_a010_spu_const;
extern volatile long g_yz_a010_spu_unparsed;
extern volatile long g_yz_a010_spucmd_headers;
extern volatile long g_yz_ucmd_handler_arg;
extern volatile long g_yz_ucmd_handler_completed;
extern int g_yz_a010_ppucmd;
void yz_ucmd_wait_for_handler_completion(void);
void yz_a010_record_output_put(spu_context* ctx, uint32_t producer_pc,
                               uint32_t ea, const uint8_t* payload,
                               uint32_t size);
void yz_a010_reltrace_spu_commit(
    uint32_t spu_id, uint32_t image_id, uint32_t pc,
    uint32_t ea, const uint8_t* payload, uint32_t size);
void yz_a010_reltrace_spu_atomic(
    uint32_t spu_id, uint32_t image_id, uint32_t pc,
    uint32_t line_ea, const uint8_t* new_line, uint32_t cmd);

/* ---------------------------------------------------------------------------
 * MFC command entry
 * -----------------------------------------------------------------------*/
typedef struct mfc_cmd {
    uint32_t lsa;       /* Local store address */
    uint64_t ea;        /* EA for ordinary DMA; EAH plus list-LS pointer for list DMA */
    uint32_t size;      /* Transfer size in bytes */
    uint32_t tag;       /* Tag group ID (0-31) */
    uint32_t cmd;       /* Command opcode (MFC_PUT_CMD, MFC_GET_CMD, etc.) */

    /* Status */
    int      active;    /* 1 = pending, 0 = completed/free */
} mfc_cmd;

/* DMA list element (used by PUTL/GETL) */
typedef struct mfc_list_element {
    /* In PS3 memory these are big-endian:
     *   bit       0 : stall-and-notify (MSB of the first word)
     *   bits   1-16 : reserved
     *   bits  17-31 : transfer size
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
     * if main memory still matches it (compare-and-swap semantics, the
     * same scheme RPCS3 uses). */
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

    /* ---- Appended 2026-07-03: DMA-list stall-and-notify (F11/P6). ----
     * CBEA v1.02 lists a stall-and-notify list element (bit 15 of the
     * element's size-and-flags halfword, "b" in RPCS3's naming) as a request
     * to SUSPEND list processing after that element completes, raise the Sn
     * event (event bit 0x2), and wait for a matching MFC_WrListStallAck
     * before continuing (Section 7.6.3 list-command description +
     * Section 9.12: Sn = MFC list command stall-and-notify tag event, page-
     * cited alongside F10 in specaudit_spu.md). RPCS3 SPUThread.cpp:3300-3320
     * confirms the exact resume contract we mirror: on hitting a stalled
     * element it records {tag (OR 0x80 = stalled), the EA of the NEXT
     * element, the LS cursor, and the remaining list byte count} and returns
     * without processing the rest; MFC_WrListStallAck (SPUThread.cpp:6280-
     * 6298) takes a TAG NUMBER (not an element index), clears that tag's
     * stall bit, and re-submits the remainder from the saved cursor.
     * ch_stall_stat (RdListStallStat, SPUThread.cpp:5298/3313) is a per-TAG
     * BITMASK (`rotl(1, tag)`), not an element index -- CBEA leaves the
     * stalled-tag reporting format to note "which tag group('s) commands
     * stalled"; we follow RPCS3's concrete, spec-consistent bitmask
     * encoding rather than inventing our own.
     *
     * One in-flight stall per tag (32 tags) is enough to be faithful: real
     * hardware processes the MFC queue in order per tag group, so a second
     * list on the SAME tag cannot be issued by well-formed guest code before
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
    if (g_yz_runtime_config.no_spubackoff || mfc->resv_cas_idle_n <= 16u)
        return -1;
    return mfc->resv_cas_idle_n > 256u ? 3 : 0;
}

/* Single process-wide lock serializing lock-line transactions across all
 * SPU host threads (defined in spu_channels.c). */
void spu_lockline_lock(void);
void spu_lockline_unlock(void);
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
    q->active = 1;
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

static inline int mfc_is_barrier(uint32_t cmd)
{
    return (cmd & 0x01) != 0; /* barrier variants have bit 0 set */
}

static inline int mfc_is_fence(uint32_t cmd)
{
    return (cmd & 0x02) != 0; /* fence variants have bit 1 set */
}

static inline int mfc_is_atomic(uint32_t cmd)
{
    return cmd == MFC_GETLLAR_CMD || cmd == MFC_PUTLLC_CMD ||
           cmd == MFC_PUTLLUC_CMD || cmd == MFC_PUTQLLUC_CMD;
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
 * The first static-stage worker writes its processed mesh table back to one
 * of nine stable orphanage allocations.  Its launch descriptor does not keep
 * the originating FE70A0 record EA, so recover the object at the first PUT
 * instead of relying on stale per-run descriptor addresses.
 */
static inline uint32_t yz_a010_stage_object_from_mesh_ea(uint32_t ea)
{
    if (ea >= 0x61574600u && ea < 0x61574620u) return 1u;
    if (ea >= 0x615C5420u && ea < 0x615C6BC0u) return 2u; /* main house */
    if (ea >= 0x615960C0u && ea < 0x61596720u) return 3u;
    if (ea >= 0x61588CA0u && ea < 0x61588DC0u) return 4u;
    if (ea >= 0x6157E190u && ea < 0x6157E230u) return 5u;
    if (ea >= 0x6157CDA0u && ea < 0x6157CDC0u) return 6u;
    if (ea >= 0x61577F50u && ea < 0x61577FB0u) return 7u;
    if (ea >= 0x61576570u && ea < 0x61576590u) return 8u;
    if (ea >= 0x6153E850u && ea < 0x6153E890u) return 9u;
    return 0u;
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

#if !defined(YZ_PERF_CLEAN)
    /*
     * SPU half of YZ_STAGE_TRANSFORM_TRACE.  This exact range check complements
     * the PPU vm_write observer, so a worker DMA can be identified without a
     * page guard or a second diagnostic boot.
     */
    {
        static int stage_transform_trace = -1;
        static volatile long stage_transform_dma_hits = 0;
        if (stage_transform_trace < 0)
            stage_transform_trace =
                getenv("YZ_STAGE_TRANSFORM_TRACE") ? 1 : 0;
        if (stage_transform_trace && g_yz_a010_root_active != 0 &&
            mfc_is_put(cmd)) {
            const uint32_t clean_ea =
                (uint32_t)(ea & ~0x80000000ull);
            const uint32_t transfer_end = clean_ea + size;
            const char* region = NULL;
            if (clean_ea < 0x015BD080u && transfer_end > 0x015BCF40u)
                region = "primary";
            else if (clean_ea < 0x015BD1C0u &&
                     transfer_end > 0x015BD080u)
                region = "secondary";
            else if (clean_ea < 0x01622690u &&
                     transfer_end > 0x01622590u)
                region = "shared";
            if (region) {
                const long n = _InterlockedIncrement(
                    &stage_transform_dma_hits);
                if (n <= 256l) {
                    fprintf(stderr,
                            "[stage-transform-dma] n=%ld region=%s spu=%X "
                            "img=%d pc=%05X lsa=%05X ea=%08X size=%X cmd=%02X\n",
                            n, region, spu->spu_id, spu->image_id,
                            spu->pc & SPU_LS_MASK, lsa, clean_ea, size, cmd);
                    fflush(stderr);
                }
            }
        }
    }

    /*
     * DMA leg of the focused a010 0xBA68 witness.  Scan PUT payloads on
     * four-byte boundaries because the queue entry can begin at any record
     * offset inside a larger transfer.  The helper filters to the nine exact
     * FE70A0 payload EAs and logs only the first PUT for each object.
     */
    {
        static int a010_ba68_put = -1;
        if (a010_ba68_put < 0)
            a010_ba68_put = getenv("YZ_A010_STAGE_BA68") ? 1 : 0;
        if (a010_ba68_put && spu->image_id == 0 &&
            g_yz_a010_root_active != 0 && mfc_is_put(cmd) &&
            size >= 16u) {
            for (uint32_t off = 0; off + 16u <= size; off += 4u) {
                uint32_t words[4];
                const uint32_t qlsa = (lsa + off) & SPU_LS_MASK;
                for (unsigned wi = 0; wi < 4u; ++wi) {
                    const uint32_t p =
                        (qlsa + wi * 4u) & SPU_LS_MASK;
                    words[wi] =
                        ((uint32_t)spu->ls[p] << 24) |
                        ((uint32_t)spu->ls[(p + 1u) & SPU_LS_MASK] << 16) |
                        ((uint32_t)spu->ls[(p + 2u) & SPU_LS_MASK] << 8) |
                        (uint32_t)spu->ls[(p + 3u) & SPU_LS_MASK];
                }
                if ((words[0] & 0x7FFFFFFFu) == 0x0000BA68u) {
                    extern void yz_a010_stage_ba68(
                        spu_context*, const char*, uint32_t,
                        const uint32_t*, uint32_t, uint32_t);
                    yz_a010_stage_ba68(
                        spu, "PUT", qlsa, words,
                        (uint32_t)((ea & ~0x80000000ull) + off), size);
                }
            }
        }
    }

    /*
     * Follow each compact static-stage Job A result across SPU images.  The
     * producer records one exact 0x80-byte arena address per object below;
     * the first later GET/atomic read overlapping it names the downstream
     * consumer.  Nine producer lines plus nine consumer lines is the entire
     * diagnostic volume.
     */
    {
        static int a010_result_handoff_on = -1;
        extern volatile uint32_t g_yz_a010_stage_result_ea[9];
        extern volatile long g_yz_a010_stage_result_get_mask;
        if (a010_result_handoff_on < 0)
            a010_result_handoff_on =
                getenv("YZ_A010_RESULT_HANDOFF") ? 1 : 0;
        if (a010_result_handoff_on &&
            g_yz_a010_root_active != 0 &&
            (mfc_is_get(cmd) || cmd == MFC_GETLLAR_CMD)) {
            const uint32_t clean_ea =
                (uint32_t)(ea & ~0x80000000ull);
            const uint32_t transfer_end = clean_ea + size;
            for (unsigned object_i = 0; object_i < 9u; object_i++) {
                const uint32_t result_ea =
                    g_yz_a010_stage_result_ea[object_i];
                const long object_bit = 1l << object_i;
                if (result_ea != 0u &&
                    clean_ea < result_ea + 0x80u &&
                    transfer_end > result_ea &&
                    (_InterlockedOr(
                         &g_yz_a010_stage_result_get_mask,
                         object_bit) & object_bit) == 0) {
                    fprintf(stderr,
                            "[a010-result-get] object=%u spu=%X img=%d "
                            "pc=%05X ea=%08X size=%X cmd=%02X "
                            "result=%08X lsa=%05X\n",
                            object_i + 1u, spu->spu_id, spu->image_id,
                            spu->pc & SPU_LS_MASK, clean_ea, size, cmd,
                            result_ea, lsa);
                    fflush(stderr);
                }
            }
        }
    }

    /*
     * Attribute the static-stage worker before its compact completion record
     * is published at pc 0xCF70.  Earlier probes tried to infer this from the
     * launch descriptor, but those addresses are allocator-dependent and
     * every job was therefore labelled object 0.  The D118 destination is
     * authoritative and stable; object 2 is the 189-mesh orphanage building.
     */
    if (g_yz_a010_root_active != 0 &&
        spu->image_id == 14 && mfc_is_put(cmd) &&
        (spu->pc & SPU_LS_MASK) == 0x0D118u) {
        const uint32_t clean_ea =
            (uint32_t)(ea & ~0x80000000ull);
        const uint32_t stage_object =
            yz_a010_stage_object_from_mesh_ea(clean_ea);
        if (stage_object != 0u) {
            static volatile long attributed_mask = 0;
            const long object_bit = 1l << (stage_object - 1u);
            spu->a010_job_active = 1u;
            spu->a010_job_stage_object = stage_object;
            spu->a010_job_stage_input = clean_ea;
            spu->a010_job_desc_kind = 1u;
            spu->a010_job_get_count = 0u;
            spu->a010_job_get_bytes = 0u;
            spu->a010_job_put_count = 0u;
            spu->a010_job_put_bytes = 0u;
            spu->a010_job_first_put_ea = 0u;
            spu->a010_job_last_put_ea = 0u;
            if ((_InterlockedOr(&attributed_mask, object_bit) &
                 object_bit) == 0) {
                fprintf(stderr,
                        "[a010-stage-attributed] object=%u%s spu=%X "
                        "mesh=%08X size=%X\n",
                        stage_object,
                        stage_object == 2u ? " HOUSE" : "",
                        spu->spu_id, clean_ea, size);
                fflush(stderr);
            }
        }
    }

    /*
     * Per-launch accounting for the static-stage Job A probes.  This is
     * intentionally independent of the broad YZ_A010_JOBTRACE stream: it
     * answers whether each known orphanage object merely launches or also
     * publishes main-memory output before returning to the job module.
     */
    if (spu->a010_job_active &&
        (spu->image_id == 14 || spu->image_id == 19)) {
        const uint32_t clean_ea =
            (uint32_t)(ea & ~0x80000000ull);
        if (mfc_is_get(cmd)) {
            spu->a010_job_get_count++;
            spu->a010_job_get_bytes += size;
        } else if (mfc_is_put(cmd)) {
            /*
             * A010 static-stage result witness.  Each known stage worker PUTs
             * its processed mesh span back to the input EA, then publishes a
             * compact 0x80-byte result into the jobchain arena.  Capture one
             * of each per object so we can tell whether the missing building
             * is already absent/empty at the worker boundary or is lost by a
             * later consumer.  Bounded to 18 short records per process.
             */
            static int a010_stage_result_on = -1;
            static volatile long a010_stage_input_dumped = 0;
            static volatile long a010_stage_tail_dumped = 0;
            if (a010_stage_result_on < 0)
                a010_stage_result_on =
                    getenv("YZ_A010_STAGE_RESULT") ? 1 : 0;
            if (a010_stage_result_on &&
                spu->a010_job_stage_object >= 1u &&
                spu->a010_job_stage_object <= 9u) {
                const long object_bit =
                    1l << (spu->a010_job_stage_object - 1u);
                volatile long* dumped =
                    clean_ea == spu->a010_job_stage_input
                        ? &a010_stage_input_dumped
                        : ((clean_ea >= 0x401AC000u &&
                            clean_ea < 0x401AE000u)
                               ? &a010_stage_tail_dumped : NULL);
                if (dumped &&
                    (_InterlockedOr(dumped, object_bit) & object_bit) == 0) {
                    const uint32_t bytes = size < 0x80u ? size : 0x80u;
                    fprintf(stderr,
                            "[a010-stage-result] object=%u kind=%s "
                            "pc=%05X ea=%08X lsa=%05X size=%X bytes=",
                            spu->a010_job_stage_object,
                            dumped == &a010_stage_input_dumped
                                ? "mesh" : "tail",
                            spu->pc & SPU_LS_MASK, clean_ea,
                            lsa & SPU_LS_MASK, size);
                    for (uint32_t bi = 0; bi < bytes; ++bi)
                        fprintf(stderr, "%s%02X",
                                (bi & 3u) ? "" : " ",
                                spu->ls[(lsa + bi) & SPU_LS_MASK]);
                    fprintf(stderr, "\n");
                    fflush(stderr);
                }
            }
            {
                static int a010_result_handoff_on = -1;
                extern volatile uint32_t
                    g_yz_a010_stage_result_ea[9];
                if (a010_result_handoff_on < 0)
                    a010_result_handoff_on =
                        getenv("YZ_A010_RESULT_HANDOFF") ? 1 : 0;
                if (a010_result_handoff_on &&
                    spu->a010_job_stage_object >= 1u &&
                    spu->a010_job_stage_object <= 9u &&
                    (spu->pc & SPU_LS_MASK) == 0x0CF70u &&
                    clean_ea >= 0x401AC000u &&
                    clean_ea < 0x401AE000u) {
                    const unsigned object_i =
                        spu->a010_job_stage_object - 1u;
                    if (_InterlockedCompareExchange(
                            (volatile long*)
                                &g_yz_a010_stage_result_ea[object_i],
                            (long)clean_ea, 0l) == 0l) {
                        fprintf(stderr,
                                "[a010-result-put] object=%u spu=%X "
                                "ea=%08X size=%X lsa=%05X\n",
                                object_i + 1u, spu->spu_id, clean_ea,
                                size, lsa);
                        fflush(stderr);
                    }
                }
            }
            if (spu->a010_job_put_count == 0u) {
                spu->a010_job_first_put_ea = clean_ea;
                static volatile long first_put_n = 0;
                const unsigned long n = (unsigned long)
                    _InterlockedIncrement(&first_put_n);
                if (n <= 64u ||
                    (n & (n - 1u)) == 0u) {
                    fprintf(stderr,
                            "[a010-stage-job-first-put] n=%lu spu=%X "
                            "object=%u input=%08X pc=%05X "
                            "ea=%08X size=%X\n",
                            n, spu->spu_id,
                            spu->a010_job_stage_object,
                            spu->a010_job_stage_input,
                            spu->pc & SPU_LS_MASK,
                            clean_ea, size);
                    fflush(stderr);
                }
            }
            spu->a010_job_put_count++;
            spu->a010_job_put_bytes += size;
            spu->a010_job_last_put_ea = clean_ea;
        }
    }

    /*
     * Follow the nine top-level FE70A0 stage records into every SPU image.  Their
     * addresses are published by the PPU dispatcher at creation time, so this
     * catches the real consumer even when the command arena rotates every
     * frame.  This was originally restricted to image 0 (gs_task), but a zero
     * result there only proved that gs_task was not the direct reader; the
     * journal/jobchain workers can own the intermediate handoff.  A fetched
     * object bit with no corresponding output establishes a worker-side loss;
     * an absent bit across every image establishes a PPU-side publication loss.
     */
    {
        static int stage_dma_on = -1;
        extern volatile long g_yz_a010_stage_generation;
        extern volatile uint32_t g_yz_a010_stage_record_ea[9];
        extern volatile uint32_t g_yz_a010_stage_payload_ea[9];
        if (stage_dma_on < 0)
            stage_dma_on =
                getenv("YZ_A010_STAGE_DMA_MAP") ? 1 : 0;
        if (stage_dma_on && g_yz_a010_root_active != 0) {
            const uint32_t clean_ea =
                (uint32_t)(ea & ~0x80000000ull);
            const uint32_t transfer_end = clean_ea + size;
            const uint32_t generation =
                (uint32_t)g_yz_a010_stage_generation;

            if (spu->a010_stage_generation != generation) {
                if (spu->a010_stage_fetch_mask != 0u) {
                    fprintf(stderr,
                            "[a010-stage-dma-summary] spu=%X img=%d gen=%u "
                            "mask=%03X get=%u/%u put=%u/%u "
                            "putEA=%08X..%08X\n",
                            spu->spu_id, spu->image_id,
                            spu->a010_stage_generation,
                            spu->a010_stage_fetch_mask,
                            spu->a010_stage_get_count,
                            spu->a010_stage_get_bytes,
                            spu->a010_stage_put_count,
                            spu->a010_stage_put_bytes,
                            spu->a010_stage_first_put_ea,
                            spu->a010_stage_last_put_ea);
                    fflush(stderr);
                }
                spu->a010_stage_generation = generation;
                spu->a010_stage_fetch_mask = 0u;
                spu->a010_stage_get_count = 0u;
                spu->a010_stage_get_bytes = 0u;
                spu->a010_stage_put_count = 0u;
                spu->a010_stage_put_bytes = 0u;
                spu->a010_stage_first_put_ea = 0u;
                spu->a010_stage_last_put_ea = 0u;
            }

            if (mfc_is_get(cmd) && generation != 0u) {
                uint32_t hit_mask = 0u;
                for (unsigned si = 0; si < 9u; si++) {
                    const uint32_t record =
                        g_yz_a010_stage_record_ea[si];
                    const uint32_t payload =
                        g_yz_a010_stage_payload_ea[si];
                    if ((record &&
                         clean_ea < record + 0x20u &&
                         transfer_end > record) ||
                        (payload &&
                         clean_ea < payload + 0xD0u &&
                         transfer_end > payload))
                        hit_mask |= 1u << si;
                }
                if (hit_mask) {
                    const uint32_t new_bits =
                        hit_mask & ~spu->a010_stage_fetch_mask;
                    spu->a010_stage_fetch_mask |= hit_mask;
                    spu->a010_stage_get_count++;
                    spu->a010_stage_get_bytes += size;
                    if (new_bits) {
                        fprintf(stderr,
                                "[a010-stage-dma-get] spu=%X img=%d gen=%u "
                                "new=%03X mask=%03X pc=%05X "
                                "ea=%08X size=%X\n",
                                spu->spu_id, spu->image_id,
                                generation, new_bits,
                                spu->a010_stage_fetch_mask,
                                spu->pc & SPU_LS_MASK,
                                clean_ea, size);
                        fflush(stderr);
                    }
                }
            } else if (mfc_is_put(cmd) &&
                       spu->a010_stage_fetch_mask != 0u) {
                if (spu->a010_stage_put_count == 0u) {
                    spu->a010_stage_first_put_ea = clean_ea;
                    fprintf(stderr,
                            "[a010-stage-dma-first-put] spu=%X img=%d gen=%u "
                            "mask=%03X pc=%05X ea=%08X size=%X\n",
                            spu->spu_id, spu->image_id, generation,
                            spu->a010_stage_fetch_mask,
                            spu->pc & SPU_LS_MASK,
                            clean_ea, size);
                    fflush(stderr);
                }
                spu->a010_stage_put_count++;
                spu->a010_stage_put_bytes += size;
                spu->a010_stage_last_put_ea = clean_ea;
            }
        }
    }

    /* Focused first-corruption trace for a010's bulk geometry job.  Its normal
     * built-in descriptor always fetches 0x20 bytes from 0x0125D800 to 0xDF00.
     * Keep counting the cheap normal path, but dump the first anomalous D808
     * transfer even when it occurs thousands of invocations into the scene. */
    {
        static int a010_job14_dma = -1;
        static unsigned long a010_job14_dma_n = 0;
        static unsigned long a010_job14_dma_bad_n = 0;
        if (a010_job14_dma < 0)
            a010_job14_dma = getenv("YZ_A010_JOBTRACE") ? 1 : 0;
        if (a010_job14_dma && g_yz_a010_root_active != 0 &&
            spu->image_id == 14 &&
            (spu->pc & SPU_LS_MASK) == 0x0D808u) {
            const uint32_t table = spu->gpr[8]._u32[0] & SPU_LS_MASK;
            const unsigned long n = ++a010_job14_dma_n;
            const uint64_t clean_ea = ea & ~0x80000000ull;
            const int expected =
                table == 0x0E120u && lsa == 0x0DF00u &&
                clean_ea == 0x0125D800ull && size == 0x20u &&
                spu->gpr[5]._u32[0] == 0u &&
                spu->ls[0x0E120u] == 0x00u &&
                spu->ls[0x0E121u] == 0x00u &&
                spu->ls[0x0E122u] == 0x93u &&
                spu->ls[0x0E123u] == 0x00u &&
                spu->ls[0x0E124u] == 0x00u &&
                spu->ls[0x0E125u] == 0x00u &&
                spu->ls[0x0E126u] == 0x00u &&
                spu->ls[0x0E127u] == 0x20u;
            if (n <= 16u || (!expected && a010_job14_dma_bad_n < 32u)) {
                if (!expected)
                    a010_job14_dma_bad_n++;
                fprintf(stderr,
                        "[a010-job14-dma-%s] n=%lu bad=%lu spu=%X "
                        "lsa=0x%05X ea=0x%08llX size=0x%X r5=%08X "
                        "table=0x%05X entry="
                        "%02X%02X%02X%02X_%02X%02X%02X%02X "
                        "sentinel=%02X%02X%02X%02X_%02X%02X%02X%02X "
                        "r2=%08X r6=%08X r7=%08X r10=%08X "
                        "r14=%08X r16=%08X r17=%08X r29=%08X "
                        "r30=%08X r31=%08X r32=%08X r33=%08X "
                        "r34=%08X r35=%08X r38=%08X\n",
                        expected ? "ok" : "BAD", n,
                        a010_job14_dma_bad_n, spu->spu_id, lsa,
                        (unsigned long long)clean_ea, size,
                        spu->gpr[5]._u32[0], table,
                        spu->ls[(table + 0u) & SPU_LS_MASK],
                        spu->ls[(table + 1u) & SPU_LS_MASK],
                        spu->ls[(table + 2u) & SPU_LS_MASK],
                        spu->ls[(table + 3u) & SPU_LS_MASK],
                        spu->ls[(table + 4u) & SPU_LS_MASK],
                        spu->ls[(table + 5u) & SPU_LS_MASK],
                        spu->ls[(table + 6u) & SPU_LS_MASK],
                        spu->ls[(table + 7u) & SPU_LS_MASK],
                        spu->ls[0x0E128u], spu->ls[0x0E129u],
                        spu->ls[0x0E12Au], spu->ls[0x0E12Bu],
                        spu->ls[0x0E12Cu], spu->ls[0x0E12Du],
                        spu->ls[0x0E12Eu], spu->ls[0x0E12Fu],
                        spu->gpr[2]._u32[0], spu->gpr[6]._u32[0],
                        spu->gpr[7]._u32[0], spu->gpr[10]._u32[0],
                        spu->gpr[14]._u32[0], spu->gpr[16]._u32[0],
                        spu->gpr[17]._u32[0], spu->gpr[29]._u32[0],
                        spu->gpr[30]._u32[0], spu->gpr[31]._u32[0],
                        spu->gpr[32]._u32[0], spu->gpr[33]._u32[0],
                        spu->gpr[34]._u32[0], spu->gpr[35]._u32[0],
                        spu->gpr[38]._u32[0]);
                fflush(stderr);
            }
        }
    }

    /* The image-13 loader normally restores Job A at 0xCC00 and therefore
     * covers this tail on every launch.  Emit only its first few good loads,
     * but retain every other image/transfer identity that can overwrite the
     * descriptor during the short loader-to-entry handoff. */
    {
        static int a010_job14_tail_dma = -1;
        static unsigned long a010_job14_tail_dma_n = 0;
        if (a010_job14_tail_dma < 0)
            a010_job14_tail_dma = getenv("YZ_A010_JOBTRACE") ? 1 : 0;
        const uint64_t transfer_end = (uint64_t)lsa + (uint64_t)size;
        const uint64_t clean_ea = ea & ~0x80000000ull;
        const int normal_job_load =
            spu->image_id == 13 &&
            (spu->pc & SPU_LS_MASK) == 0x0451Cu &&
            mfc_is_get(cmd) && lsa == 0x0CC00u &&
            clean_ea == 0x0125C500ull && size == 0x1540u;
        if (a010_job14_tail_dma && g_yz_a010_root_active != 0 &&
            lsa < 0x0E140u && transfer_end > 0x0E100u &&
            a010_job14_tail_dma_n < 64u &&
            (!normal_job_load || a010_job14_tail_dma_n < 4u)) {
            a010_job14_tail_dma_n++;
            fprintf(stderr,
                    "[a010-job14-tail-dma%s] n=%lu spu=%X img=%d pc=0x%05X "
                    "cmd=0x%02X lsa=0x%05X ea=0x%08llX size=0x%X "
                    "table=%08X entry="
                    "%02X%02X%02X%02X_%02X%02X%02X%02X "
                    "sentinel=%02X%02X%02X%02X_%02X%02X%02X%02X\n",
                    normal_job_load ? "-loader" : "",
                    a010_job14_tail_dma_n, spu->spu_id, spu->image_id,
                    spu->pc & SPU_LS_MASK, cmd, lsa,
                    (unsigned long long)clean_ea, size,
                    spu->gpr[8]._u32[0],
                    spu->ls[0x0E120u], spu->ls[0x0E121u],
                    spu->ls[0x0E122u], spu->ls[0x0E123u],
                    spu->ls[0x0E124u], spu->ls[0x0E125u],
                    spu->ls[0x0E126u], spu->ls[0x0E127u],
                    spu->ls[0x0E128u], spu->ls[0x0E129u],
                    spu->ls[0x0E12Au], spu->ls[0x0E12Bu],
                    spu->ls[0x0E12Cu], spu->ls[0x0E12Du],
                    spu->ls[0x0E12Eu], spu->ls[0x0E12Fu]);
            fflush(stderr);
        }
    }

#endif
    /* An MFC transfer may not cross the 256 KiB local-store boundary.  Masking
     * the start address alone is insufficient: a large transfer near the end
     * of LS would otherwise overwrite the spu_context fields that immediately
     * follow ls[] (including the per-SPU wait condition variable). */
    if (size > SPU_LS_SIZE - lsa) {
        static unsigned long oob_n = 0;
        const unsigned long n = ++oob_n;
        if (n <= 32 || (n & 0xFFu) == 0) {
            const uint32_t desc = spu->gpr[8]._u32[0] & SPU_LS_MASK;
            fprintf(stderr,
                    "[spu-dma] skip LS overflow n=%lu spu=%X img=%d "
                    "pc=0x%05X cmd=0x%02X lsa=0x%05X size=0x%X "
                    "r2=%08X r5=%08X r6=%08X r7=%08X r8=%08X "
                    "r10=%08X r12=%08X r16=%08X r30=%08X r31=%08X "
                    "r32=%08X r33=%08X r34=%08X r35=%08X r38=%08X "
                    "desc=%02X%02X%02X%02X_%02X%02X%02X%02X\n",
                    n, spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK,
                    cmd, lsa, size,
                    spu->gpr[2]._u32[0], spu->gpr[5]._u32[0],
                    spu->gpr[6]._u32[0], spu->gpr[7]._u32[0],
                    spu->gpr[8]._u32[0], spu->gpr[10]._u32[0],
                    spu->gpr[12]._u32[0], spu->gpr[16]._u32[0],
                    spu->gpr[30]._u32[0], spu->gpr[31]._u32[0],
                    spu->gpr[32]._u32[0], spu->gpr[33]._u32[0],
                    spu->gpr[34]._u32[0], spu->gpr[35]._u32[0],
                    spu->gpr[38]._u32[0],
                    spu->ls[(desc + 0u) & SPU_LS_MASK],
                    spu->ls[(desc + 1u) & SPU_LS_MASK],
                    spu->ls[(desc + 2u) & SPU_LS_MASK],
                    spu->ls[(desc + 3u) & SPU_LS_MASK],
                    spu->ls[(desc + 4u) & SPU_LS_MASK],
                    spu->ls[(desc + 5u) & SPU_LS_MASK],
                    spu->ls[(desc + 6u) & SPU_LS_MASK],
                    spu->ls[(desc + 7u) & SPU_LS_MASK]);
            if (n <= 4 && spu->image_id == 14 &&
                (spu->pc & SPU_LS_MASK) == 0x0D808u) {
                const uint32_t probes[4] = {
                    spu->gpr[8]._u32[0] & SPU_LS_MASK,
                    spu->gpr[14]._u32[0] & SPU_LS_MASK,
                    spu->gpr[16]._u32[0] & SPU_LS_MASK,
                    spu->gpr[17]._u32[0] & SPU_LS_MASK
                };
                const char* names[4] = {"r8", "r14", "r16", "r17"};
                fprintf(stderr,
                        "[spu-dma] D808 operands r5=%08X r8=%08X "
                        "r14=%08X r16=%08X r17=%08X "
                        "r29=%08X r30=%08X r31=%08X r38=%08X\n",
                        spu->gpr[5]._u32[0], spu->gpr[8]._u32[0],
                        spu->gpr[14]._u32[0], spu->gpr[16]._u32[0],
                        spu->gpr[17]._u32[0], spu->gpr[29]._u32[0],
                        spu->gpr[30]._u32[0], spu->gpr[31]._u32[0],
                        spu->gpr[38]._u32[0]);
                for (unsigned pi = 0; pi < 4; pi++) {
                    const uint32_t base = probes[pi] & ~0xFu;
                    fprintf(stderr, "[spu-dma] D808 LS[%s=%05X]:",
                            names[pi], probes[pi]);
                    for (unsigned bi = 0; bi < 64u; bi++)
                        fprintf(stderr, "%s%02X",
                                (bi & 3u) ? "" : " ",
                                spu->ls[(base + bi) & SPU_LS_MASK]);
                    fprintf(stderr, "\n");
                }
            }
            fflush(stderr);
        }
        /*
         * The recompiled caller retries a negative channel result.  Returning
         * an error here therefore turns one malformed command into a permanent
         * SPU busy loop and prevents the a010 loader from ever requesting its
         * later camera members.  Retire the invalid transfer as a benign no-op:
         * this preserves the LS/context boundary and matches the existing
         * guarded-EA behavior below, where an inaccessible DMA is skipped
         * without wedging the guest.
         */
        return 0;
    }

    uint8_t* ls_ptr = &spu->ls[lsa];
    uint8_t* ea_ptr = vm_base + (uint32_t)ea; /* PS3 uses 32-bit effective addresses for SPU DMA */

#ifdef SPU_DMA_LOG
    { extern int g_spu_dma_log; if (g_spu_dma_log-- > 0)
        fprintf(stderr, "[spu-dma] %s lsa=0x%05X ea=0x%08X size=%u\n",
                mfc_is_get(cmd) ? "GET" : "PUT", lsa, (uint32_t)ea, size); }
#endif
    /* The 4 GB guest space is RESERVED but only specific regions are COMMITTED
     * (main RAM, stacks, on-demand allocations). A DMA with a bad EA (e.g. the
     * SPURS GUID-trace buffer when tracing is unconfigured -- RPCS3 no-ops
     * cellSpursModulePutTrace entirely) points into reserved-but-uncommitted
     * memory, so `vm_base + ea` is a wild pointer and the memcpy segfaults the
     * host. Guard the copy: on an access violation, log + skip the transfer (a
     * benign no-op, matching RPCS3's trace stub) instead of crashing. */
#ifdef _WIN32
    __try {
#endif
#if !defined(YZ_PERF_CLEAN)
        /*
         * The broken orphanage draw uploads a skin palette whose c113.w is
         * the engine's 10000-unit offscreen sentinel (0x461C3FFE/FFF).
         * Follow that exact word across SPU DMA boundaries.  A GET identifies
         * the input buffer/job that receives it; a PUT identifies the worker
         * and PC that publishes it toward the RSX command stream.
         */
        {
            static int a010_skin_dma = -1;
            static unsigned long a010_skin_dma_n = 0;
            if (a010_skin_dma < 0)
                a010_skin_dma = getenv("YZ_A010_SKIN_DMA") ? 1 : 0;
            if (a010_skin_dma && g_yz_a010_root_active != 0 &&
                size >= 4u && a010_skin_dma_n < 128u) {
                const uint8_t* source =
                    mfc_is_get(cmd) ? ea_ptr : ls_ptr;
                for (uint32_t off = 0; off + 4u <= size; off += 4u) {
                    if (source[off + 0u] != 0x46u ||
                        source[off + 1u] != 0x1Cu ||
                        source[off + 2u] != 0x3Fu ||
                        (source[off + 3u] != 0xFEu &&
                         source[off + 3u] != 0xFFu))
                        continue;

                    const unsigned long n = ++a010_skin_dma_n;
                    fprintf(stderr,
                            "[a010-skin-dma] n=%lu %s spu=%X img=%d "
                            "pc=0x%05X lsa=0x%05X ea=0x%08X "
                            "size=0x%X off=0x%X bytes=",
                            n, mfc_is_get(cmd) ? "GET" : "PUT",
                            spu->spu_id, spu->image_id,
                            spu->pc & SPU_LS_MASK, lsa,
                            (uint32_t)ea, size, off);
                    const uint32_t begin = off > 16u ? off - 16u : 0u;
                    const uint32_t end =
                        off + 20u < size ? off + 20u : size;
                    for (uint32_t i = begin; i < end; ++i)
                        fprintf(stderr, "%s%02X",
                                (i == off || ((i - begin) & 3u) == 0u)
                                    ? " " : "",
                                source[i]);
                    fputc('\n', stderr);
                    fflush(stderr);
                    if (a010_skin_dma_n >= 128u)
                        break;
                }
            }
        }

        /* a010 healthy-model checkpoint.  Draw 3 in the known-good
         * orphanage replay uses this distinctive 8-index strip prefix:
         *   0051 0052 0053 FFFF 0054 0055 0056 FFFF
         * and the first transformed vertex begins with the second signature
         * below.  Follow those bytes through every SPU DMA while a010 is
         * active.  A GET-only sighting means the job was fed; a later PUT
         * sighting proves the job produced/published that model payload.
         * This is deliberately opt-in because it linearly scans DMA payloads. */
        {
            static int a010_signature = -1;
            static unsigned long a010_signature_n = 0;
            if (a010_signature < 0)
                a010_signature = getenv("YZ_A010_SIGNATURE") ? 1 : 0;
            if (a010_signature && g_yz_a010_root_active != 0 && size >= 16u) {
                static const uint8_t sig_index[16] = {
                    0x00, 0x51, 0x00, 0x52, 0x00, 0x53, 0xFF, 0xFF,
                    0x00, 0x54, 0x00, 0x55, 0x00, 0x56, 0xFF, 0xFF
                };
                static const uint8_t sig_vertex[16] = {
                    0x00, 0x13, 0x01, 0x00, 0x47, 0xF0, 0x6E, 0x7F,
                    0x88, 0x96, 0xFC, 0xFE, 0x00, 0x00, 0x00, 0x00
                };
                const uint8_t* source =
                    mfc_is_get(cmd) ? ea_ptr : ls_ptr;
                for (uint32_t off = 0; off + 16u <= size; off++) {
                    const char* kind = NULL;
                    if (memcmp(source + off, sig_index, 16u) == 0)
                        kind = "index";
                    else if (memcmp(source + off, sig_vertex, 16u) == 0)
                        kind = "vertex";
                    if (kind) {
                        const unsigned long n = ++a010_signature_n;
                        if (n <= 128u || (n & 0xFFu) == 0u) {
                            fprintf(stderr,
                                    "[a010-model3-%s] n=%lu %s spu=%X img=%d "
                                    "pc=0x%05X lsa=0x%05X ea=0x%08X "
                                    "size=0x%X off=0x%X\n",
                                    kind, n,
                                    mfc_is_get(cmd) ? "GET" : "PUT",
                                    spu->spu_id, spu->image_id,
                                    spu->pc & SPU_LS_MASK, lsa,
                                    (uint32_t)ea, size, off);
                            fflush(stderr);
                        }
                    }
                }
            }
        }
#endif
        if (mfc_is_get(cmd)) {
#if !defined(YZ_PERF_CLEAN)
            /* a010 Job A source witness.  The resident image is loaded from
             * [0x0125C500,0x0125DA40); its descriptor and sentinel occupy the
             * last 0x20 bytes.  Check the main-memory bytes immediately before
             * every overlapping GET so a corrupt source is separated from a
             * bad LS copy. */
            {
                static int a010_job14_source_get = -1;
                static unsigned long source_get_n = 0;
                static unsigned long source_get_bad_n = 0;
                if (a010_job14_source_get < 0)
                    a010_job14_source_get =
                        getenv("YZ_A010_JOBTRACE") ? 1 : 0;
                const uint64_t get_begin = (uint32_t)ea;
                const uint64_t get_end = get_begin + (uint64_t)size;
                if (a010_job14_source_get &&
                    g_yz_a010_root_active != 0 &&
                    get_begin < 0x0125DA40ull &&
                    get_end > 0x0125DA20ull) {
                    const uint8_t* tail = vm_base + 0x0125DA20u;
                    const int expected =
                        tail[0] == 0x00u && tail[1] == 0x00u &&
                        tail[2] == 0x93u && tail[3] == 0x00u &&
                        tail[4] == 0x00u && tail[5] == 0x00u &&
                        tail[6] == 0x00u && tail[7] == 0x20u &&
                        tail[8] == 0xFFu && tail[9] == 0xFFu &&
                        tail[10] == 0xFFu && tail[11] == 0xFFu &&
                        tail[12] == 0xFFu && tail[13] == 0xFFu &&
                        tail[14] == 0xFFu && tail[15] == 0xFFu;
                    const unsigned long n = ++source_get_n;
                    if (!expected)
                        source_get_bad_n++;
                    if (n <= 8u ||
                        (!expected && source_get_bad_n <= 32u)) {
                        fprintf(stderr,
                                "[a010-job14-source-get-%s] n=%lu bad=%lu "
                                "spu=%X img=%d pc=0x%05X "
                                "lsa=0x%05X ea=0x%08X size=0x%X tail=",
                                expected ? "ok" : "BAD", n,
                                source_get_bad_n, spu->spu_id,
                                spu->image_id, spu->pc & SPU_LS_MASK,
                                lsa, (uint32_t)ea, size);
                        for (unsigned i = 0; i < 32u; i++)
                            fprintf(stderr, "%s%02X",
                                    (i & 3u) ? "" : " ", tail[i]);
                        fprintf(stderr, "\n");
                        fflush(stderr);
                    }
                }
            }
#if !defined(YZ_PERF_CLEAN)
            /* s48 LS-CODE-WATCH DMA leg (env YZ_LS_CODE_WATCH, spu_channels.c
             * yz_lscw_check): pre-copy so current LS bytes are still comparable
             * against the incoming ea bytes; inside the __try (ea may point at
             * uncommitted guest space -- an AV here skips the transfer exactly
             * like the memcpy below would have). Covers plain GETs AND every
             * DMA-list element (mfc_do_list_transfer_from funnels here per
             * element). GETLLAR lands via the two lock-line legs, checked
             * there. */
            { extern volatile int g_yz_lscw_on;
              extern int yz_lscw_arm(void);
              extern void yz_lscw_check(spu_context*, uint32_t, uint32_t,
                                        const uint8_t*, unsigned long long, const char*);
              int lcw = g_yz_lscw_on; if (lcw < 0) lcw = yz_lscw_arm();
              if (lcw) yz_lscw_check(spu, lsa, size, ea_ptr,
                                     (unsigned long long)ea, "dma-get"); }
#endif
#endif
            /* GET: main memory -> local store */
            memcpy(ls_ptr, ea_ptr, size);
            yz_tagread_repair_fetch(
                spu, lsa, (unsigned long long)ea, size);
#if !defined(YZ_PERF_CLEAN)
            /* s49 TAG-READ fetch map (env YZ_TAGREAD, spu_channels.c yz_tr_fetch):
             * record LS-line -> arena-EA-line so the tag-read leg can name the
             * journal line EA it is reading instead of inferring it. */
            { extern volatile int g_yz_tr_on;
              extern volatile long g_yz_a010_stage_generation;
              extern int yz_tagread_arm(void);
              extern void yz_tr_fetch(spu_context*, uint32_t, unsigned long long, uint32_t);
              static int stage_map = -1;
              int trf = g_yz_tr_on; if (trf < 0) trf = yz_tagread_arm();
              if (stage_map < 0)
                  stage_map = getenv("YZ_A010_STAGE_KIND1") ? 1 : 0;
              if (trf || (stage_map && g_yz_a010_stage_generation != 0))
                  yz_tr_fetch(spu, lsa, (unsigned long long)ea, size); }
#endif
#if !defined(YZ_PERF_CLEAN)
            /* s38 arm-gate witness DMA-watch (env YZ_ARMGATE): a GET landing on LS
             * 0xBD70 would be the (non-store) writer of the apply_entry arm-gate witness.
             * Complements the spu_ls_write128 store-watch -- if BOTH stay silent the
             * witness is never written by the code (stuck at init). */
            { static int agd = -1;
              if (agd < 0) agd = getenv("YZ_ARMGATE") ? 1 : 0;
              if (agd && spu->image_id == 0 && lsa <= 0xBD70u && 0xBD70u < lsa + size) {
                  static int agdn = 0;
                  if (agdn < 200) { agdn++;
                      const uint8_t* w = &spu->ls[0xBD70u];
                      uint32_t nv = ((uint32_t)w[0]<<24)|((uint32_t)w[1]<<16)|((uint32_t)w[2]<<8)|w[3];
                      fprintf(stderr, "[armgate-dma] GET lsa=0x%05X ea=0x%08X size=0x%X -> LS[0xBD70]=0x%08X\n",
                              lsa, (uint32_t)ea, size, nv); fflush(stderr); }
              } }
            /* s32 [ls-wipe] watch (census verdict, scratch/s32_flavor_census.md:
             * every death boot shows gs_task/policy spans ZERO-WIPED with ragged
             * offsets, each flavor = a different unresurrected span meeting its
             * first reader; the wiper was never directly observed). This is THE
             * choke point for guest-driven LS writes: flag any >=64-byte
             * all-zeros GET landing in the policy-data/task-header windows
             * ([0xA00,0x3000) or [0xB800,0xBE00)) with full writer identity.
             * Default-off now that the census is complete: the all-GET scan
             * and stderr traffic materially perturb the renderer. */
            { static int ls_wipe_diag = -1;
              uint32_t wl = lsa & SPU_LS_MASK;
              if (ls_wipe_diag < 0)
                  ls_wipe_diag = getenv("YZ_LS_WIPE") ? 1 : 0;
              /* s34: WIDENED to cover gs_task's whole text+data [0xA00,0xBE00)
               * -- the churn-analysis verdict says gs_task text [0x3000,0xB800)
               * is zeroed ~32k x (a DMA-zeroer would land here; the old window
               * had a blind spot exactly there).
               * s40: WIDENED AGAIN 0xBE00 -> 0xC400: the item-slot region
               * [0xBF00,0xC400) was the remaining blind spot, and both wedge
               * modes start with zeroed quads exactly there
               * (scratch/s40_evidence.md, verifyA/verifyB). */
              if (ls_wipe_diag && size >= 64u && wl < 0x40000u
                  && (wl < 0xC400u && wl + size > 0xA00u)) {
                  const uint8_t* zp = (const uint8_t*)ls_ptr;
                  uint32_t zi = 0;
                  while (zi < size && zp[zi] == 0) zi++;
                  if (zi >= 64u) {  /* leading zero run covers the window? */
                      static unsigned long wn = 0; wn++;
                      if (wn <= 24 || (wn & 255u) == 0) {
                          fprintf(stderr, "[ls-wipe] n=%lu spu=%X img=%d pc=0x%05X "
                                  "cmd=0x%02X lsa=0x%05X ea=0x%08llX size=0x%X "
                                  "zeros=0x%X%s\n",
                                  wn, spu->spu_id, spu->image_id,
                                  spu->pc & SPU_LS_MASK, cmd, wl,
                                  (unsigned long long)ea, size, zi,
                                  zi == size ? " (ALL-ZERO)" : "");
                          fflush(stderr);
                      }
                  }
              } }
            /* s40 item-slot DMA-landing watch (env YZ_ZSLOT, companion of the
             * spu_context.h zero-store watch): log EVERY GET-class landing that
             * overlaps the gs_task item-slot region LS [0xBF00,0xC400) in image 0,
             * ANY size (the [ls-wipe] watch above needs >=64B and a 64B zero run;
             * a 16-byte zero landing on one sub-object is invisible to it). Reports
             * whether the landed overlap bytes are zero. */
            { static int zsd = -1;
              if (zsd < 0) { zsd = getenv("YZ_ZSLOT") ? 1 : 0;
                  if (zsd) { fprintf(stderr, "[z-slot] ARMED-DMA (any GET overlapping LS [0xBF00,0xC400) img0)\n"); fflush(stderr); } }
              if (zsd && spu->image_id == 0) {
                  /* s40 late: bound extended to 0xB800 (static-vtable band, s40_husk_residual.md) */
                  uint32_t wl2 = lsa & SPU_LS_MASK;
                  if (wl2 < 0xC400u && wl2 + size > 0xB800u) {
                      uint32_t lo = wl2 > 0xB800u ? wl2 : 0xB800u;
                      uint32_t hi = (wl2 + size) < 0xC400u ? (wl2 + size) : 0xC400u;
                      const uint8_t* op = &spu->ls[lo];
                      uint32_t oz = 0, on = hi - lo;
                      while (oz < on && op[oz] == 0) oz++;
                      { static unsigned long zdn = 0; zdn++;
                        if (zdn <= 2000 || (zdn & 63) == 0) {
                            fprintf(stderr, "[z-slot] dma pc=0x%05X cmd=0x%02X lsa=0x%05X ea=0x%08llX size=0x%X spu=%X overlap=[0x%05X,0x%05X) zeros=0x%X%s n=%lu\n",
                                    spu->pc & SPU_LS_MASK, cmd, wl2, (unsigned long long)ea, size,
                                    spu->spu_id, lo, hi, oz, oz == on ? " (ALL-ZERO)" : "", zdn);
                            fflush(stderr);
                        } }
                  }
                  /* s40b [anch-dma]: any GET landing over the poll-EA home
                   * (published by the [anch] probe; 0 until discovered). */
                  { extern uint32_t g_yz_anch_home;
                    if (g_yz_anch_home && wl2 <= g_yz_anch_home &&
                        g_yz_anch_home + 4u <= wl2 + size) {
                        static unsigned long adn = 0; adn++;
                        if (adn <= 200 || (adn & 63) == 0) {
                            fprintf(stderr, "[anch-dma] pc=0x%05X cmd=0x%02X lsa=0x%05X ea=0x%08llX size=0x%X spu=%X n=%lu\n",
                                    spu->pc & SPU_LS_MASK, cmd, wl2, (unsigned long long)ea,
                                    size, spu->spu_id, adn);
                            fflush(stderr);
                        }
                    } }
              } }
            /* s26 ROOT FIX (the varying-round wall — STATUS ⚡ stager-hunt):
             * the wid4 pool task's 64-byte work-record fetch can race the
             * game's OWN staging (hardware-watch MEASURED: the pxd module on
             * the intr thread stores the round value AFTER its SendSignal —
             * benign on real HW where SPU wake latency exceeds the store gap;
             * our task wakes faster and fetches a HALF-STAGED record: guard
             * EA armed at +4, value word still 0 → publishes 0/nothing → the
             * exact-equality acquire wedges the boot at a varying round).
             * Absorb the race where real hardware does — in the transfer
             * latency: order every wid4 work-record fetch after the callback
             * finishes staging, then re-copy. This covers both zero and stale
             * nonzero payloads without a timing loop. Kill-switch
             * YZ_NO_RECWAIT. */
#endif
            if (spu->image_id == 4 && size == 0x40u && cmd != MFC_GETLLAR_CMD) {
                uint32_t rea = (uint32_t)ea;
                if (rea >= 0x42452880u && rea < 0x424529E0u) {
                    const int nrw = g_yz_runtime_config.no_recwait;
#if !defined(YZ_PERF_CLEAN)
                    { static int announced = 0;
                      if (!announced && !nrw) { announced = 1;
                          fprintf(stderr, "[recwait] ARMED: half-staged record absorber live\n");
                          fflush(stderr); } }
#endif
                    if (!nrw) {
                        /*
                         * The GCM callback raises the SPU signal before
                         * publishing this record.  Order every wid4 record
                         * fetch after callback completion, then re-copy.
                         * Looking only for value==0 missed the stale-nonzero
                         * form where round N-1 remains while N is staged.
                         */
#if !defined(YZ_PERF_CLEAN)
                        uint32_t before_value;
                        memcpy(&before_value, ls_ptr, sizeof(before_value));
#endif
#ifdef _WIN32
                        yz_ucmd_wait_for_handler_completion();
#endif
                        memcpy(ls_ptr, ea_ptr, size);
#if !defined(YZ_PERF_CLEAN)
                        uint32_t after_value;
                        memcpy(&after_value, ls_ptr, sizeof(after_value));
                        if (before_value != after_value) {
                            static unsigned long rwn = 0; rwn++;
                            if (rwn <= 20 || (rwn & 0xFFu) == 0) {
                                fprintf(stderr,
                                        "[recwait] n=%lu spu=%X ea=0x%08X "
                                        "stale=%08X settled=%08X\n",
                                        rwn, spu->spu_id, rea,
                                        before_value, after_value);
                                fflush(stderr);
                            }
                        }
#endif
                    }
                }
            }
#if !defined(YZ_PERF_CLEAN)
            /* DIAG (env YZ_JOBDESC, 2026-07-08, capped): payload dump of every
             * GET from the CRI jobchain object area (0x4019xxxx) — the jobchain
             * header/commands/JOB DESCRIPTORS the module stages before calling
             * a job. Round-1 jobB (dual-base fix) runs its REAL code but
             * early-exits without attempting the flag PUTLLC (fixboot3: jobtrace
             * armed, flagcas armed, 0 attempts) — so the deciding values are in
             * these staged bytes. Guest EAs match RPCS3; the rpcs3clone twin
             * probe dumps the same GETs for a byte diff. */
            { static int jd = -1;
              if (jd < 0) { jd = getenv("YZ_JOBDESC") ? 1 : 0;
                  if (jd) { fprintf(stderr, "[jobdesc] armed\n"); fflush(stderr); } }
              if (jd && size <= 0x400u
                     /* jobchain-family SPUs only, ANY source ea: the JOB
                      * DESCRIPTOR lives in game memory (its EA is the command
                      * value), not in the 0x4019xxxx object area. Skip the
                      * kernel workload-info lines (stale-image polls at
                      * 0x40198xxx) that would eat the cap. */
                     && spu->image_id >= 13 && spu->image_id <= 15
                     && !((uint32_t)ea >= 0x40198000u && (uint32_t)ea < 0x40199000u)) {
                  static int jdn = 0;
                  if (jdn < 200) { jdn++;
                      /* single buffered write: multi-fprintf dumps get torn by
                       * other threads' stderr lines mid-payload */
                      char jb[512]; int jp;
                      unsigned dl = size < 64u ? size : 64u;
                      jp = snprintf(jb, sizeof jb, "[jobdesc] spu=%X img=%d pc=0x%05X GET "
                              "ea=0x%08X lsa=0x%05X size=0x%X:", spu->spu_id, spu->image_id,
                              spu->pc & SPU_LS_MASK, (uint32_t)ea, lsa, size);
                      for (unsigned di = 0; di < dl && jp < (int)sizeof jb - 4; di++)
                          jp += snprintf(jb + jp, sizeof jb - jp, "%s%02X",
                                         (di & 3) ? "" : " ", ls_ptr[di]);
                      fprintf(stderr, "%s\n", jb); fflush(stderr);
                  }
              } }
#endif
            /* SPURS workload dispatch: the kernel DMAs the selected workload's code
             * to LS 0xA00, then branches there. The system service and the taskset
             * POLICY module both live at LS 0xA00, so pick which lifted image
             * spu_indirect_branch resolves by the DMA SOURCE EA. (image ids match
             * main.cpp: 1=service, 2=policy @0x02023680.) */
            if (lsa == 0xA00u && size >= 0x400u) {
                /* Workload-module image keying by DMA source EA. EXPLICIT map --
                 * the old `ea==policy ? 2 : 1` binary silently ran any OTHER
                 * workload module as SERVICE code (bit us 2026-07-03: the game's
                 * jobchain (wid 1) loads Sony's JOB policy module 0x0202A180 and
                 * executed service garbage -> wild EA-0 atomics). Unknown module
                 * EAs stay on the old fallback but WARN loudly: that is the
                 * "unlifted libsre blob" class (service/policy/job/ts_exit so
                 * far) and each one costs a session to find quietly. */
                int img;
                switch ((uint32_t)ea) {
                case 0x02021480u: img = 1;  break;   /* system service        */
                case 0x02023680u: img = 2;  break;   /* taskset policy module */
                case 0x0202A180u: img = 13; break;   /* JOB policy module     */
                default:
                    img = 1;
                    { static int uw = 0;
                      if (uw < 8) { uw++;
                          fprintf(stderr, "[SPU] WARN: UNKNOWN workload module ea=0x%08X size=0x%X "
                                  "loaded to LS 0xA00 -- running as SERVICE image; if this SPU "
                                  "misbehaves, lift this blob (relift the SPU module)\n",
                                  (uint32_t)ea, size);
                          fflush(stderr); } }
                    break;
                }
                /* Residency record (s24 image model): dispatch re-adopts this
                 * at every pc==0xA00 entry, surviving host-return bracket
                 * restores between the load and the dispatch branch. */
                spu->module_img_a00 = img;
                spu->module_src_ea = (uint32_t)ea;
                spu->module_src_size = (uint32_t)size;
                if (spu->image_id != img) {
                    extern int g_spu_prof_on;
                    /* THROWAWAY DIAG (env YZ_IMGLOG, non-prof): log every kernel->workload
                     * image switch so we can tell if the SELECT actually DISPATCHES the
                     * taskset policy (image 2) -- without YZ_SPU_PROF's gs_task band-aid. */
                    static int imglog = -1; if (imglog < 0) imglog = getenv("YZ_IMGLOG") ? 1 : 0;
                    if (YZ_SPU_PROF_ACTIVE() || imglog)
                        fprintf(stderr, "[spu-img] spu=%X LS0xA00 <- ea=0x%08X size=0x%X => image %d (%s)\n",
                                spu->spu_id, (uint32_t)ea, size, img, img==2?"POLICY":"SERVICE");
                    spu->image_id = img;
                }
            }
            /* SPURS jobchain JOB-BINARY loads. The job module DMAs each
             * descriptor's EBOOT-static eaBinary blob into free LS and
             * branches there. Record the current LS residency per context so
             * spu_indirect_branch can select the matching fixed relocation.
             * NOT image-gated: a mid-cycle kernel adoption can leave the
             * module's SPU on a stale image id, while eaBinary is an exact
             * identity key. */
            if ((lsa & SPU_LS_MASK) >= 0x4880u) {
                static const uint32_t job_ea[5] = {
                    0x01254500u, 0x01275A00u, 0x0125DA80u, 0x01265180u,
                    0x01252680u
                };
                static const uint32_t job_span[5] = {
                    0x9540u, 0x14C0u, 0x7640u, 0x10610u, 0x1E80u
                };
                for (int ji = 0; ji < 5; ji++) {
                    if ((uint32_t)ea != job_ea[ji])
                        continue;
                    uint32_t base = lsa & SPU_LS_MASK;
                    /* A newly loaded binary owns its entire LS range. Forget
                     * any older recorded occupant that it overlaps. */
                    for (int oi = 0; oi < 5; oi++) {
                        uint32_t other = spu->job_bin_base[oi];
                        if (oi != ji && other
                                && base < other + job_span[oi]
                                && other < base + job_span[ji])
                            spu->job_bin_base[oi] = 0;
                    }
                    spu->job_bin_base[ji] = base;
                    break;
                }
            }
        } else if (mfc_is_put(cmd)) {
            /* PUT: local store -> main memory */
#if !defined(YZ_PERF_CLEAN)
            yz_a010_record_output_put(spu, spu->pc & SPU_LS_MASK,
                                      (uint32_t)(ea & ~0x80000000ull),
                                      ls_ptr, size);
#endif
#if !defined(YZ_PERF_CLEAN)
            /* First-writer witness for Job A's main-memory descriptor tail.
             * This runs before the PUT while both the old destination bytes
             * and the incoming LS bytes are available. */
            {
                static int a010_job14_source_put = -1;
                static unsigned long source_put_n = 0;
                if (a010_job14_source_put < 0)
                    a010_job14_source_put =
                        getenv("YZ_A010_JOBTRACE") ? 1 : 0;
                const uint64_t put_begin = (uint32_t)ea;
                const uint64_t put_end = put_begin + (uint64_t)size;
                if (a010_job14_source_put &&
                    g_yz_a010_root_active != 0 &&
                    put_begin < 0x0125DA40ull &&
                    put_end > 0x0125DA20ull &&
                    source_put_n < 64u) {
                    const uint32_t overlap_begin =
                        put_begin < 0x0125DA20ull
                            ? 0x0125DA20u : (uint32_t)put_begin;
                    const uint32_t overlap_end =
                        put_end > 0x0125DA40ull
                            ? 0x0125DA40u : (uint32_t)put_end;
                    const uint32_t source_off =
                        overlap_begin - (uint32_t)put_begin;
                    const uint32_t overlap_size =
                        overlap_end - overlap_begin;
                    source_put_n++;
                    fprintf(stderr,
                            "[a010-job14-source-PUT] n=%lu spu=%X img=%d "
                            "pc=0x%05X cmd=0x%02X lsa=0x%05X "
                            "ea=0x%08X size=0x%X overlap=0x%08X+0x%X "
                            "old=",
                            source_put_n, spu->spu_id, spu->image_id,
                            spu->pc & SPU_LS_MASK, cmd, lsa,
                            (uint32_t)ea, size, overlap_begin,
                            overlap_size);
                    for (uint32_t i = 0; i < overlap_size; i++)
                        fprintf(stderr, "%s%02X", (i & 3u) ? "" : " ",
                                vm_base[overlap_begin + i]);
                    fprintf(stderr, " incoming=");
                    for (uint32_t i = 0; i < overlap_size; i++)
                        fprintf(stderr, "%s%02X", (i & 3u) ? "" : " ",
                                ls_ptr[source_off + i]);
                    fprintf(stderr, "\n");
                    fflush(stderr);
                }
            }
            /* s24 (YZ_JOBPUT site 2 of 3): the plain-put EXECUTOR — the first
             * probe site missed the flag CAS that provably fires 23x/boot
             * (instrument-coverage rule); cover every execution path. */
            { static int jp2 = -1; if (jp2 < 0) { jp2 = getenv("YZ_JOBPUT") ? 1 : 0;
                  if (jp2) { fprintf(stderr, "[job-put2] ARMED: plain-put executor site live\n"); fflush(stderr); } }
              if (jp2 && spu->image_id >= 13 && spu->image_id <= 15) {
                  static unsigned long j2n = 0; j2n++;
                  if (j2n <= 200 || (j2n & 0xFFu) == 0) {
                      fprintf(stderr, "[job-put2] n=%lu spu=%X img=%d pc=0x%05X cmd=0x%02X ea=0x%08llX size=0x%X\n",
                              j2n, spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK, cmd,
                              (unsigned long long)(ea & ~0x80000000ull), size);
                      fflush(stderr);
                  }
              } }
            /* pt35e (env YZ_PUT_WATCH): the policy SPU (image 2) wild-writing into the GAME
             * data region (below the SPURS structs @0x40000000) is the corruption that makes
             * the PPU mixer recurse. Log the SPU PC + EA + size to trace it to the bad op. */
            { static int pw = -1; if (pw < 0) pw = getenv("YZ_PUT_WATCH") ? 1 : 0;
              if (pw && spu->image_id == 2 && (uint32_t)ea < 0x40000000u && size != 0) {
                  static int n = 0; if (n < 60) { n++;
                    fprintf(stderr, "[put-wild] spu=%X pc=0x%05X PUT ea=0x%08X size=0x%X lsa=0x%05X (policy -> GAME region)\n",
                            spu->spu_id, spu->pc & SPU_LS_MASK, (uint32_t)ea, size, lsa); fflush(stderr); } } }
#endif
            /* s25 (atomics-conformance harness, CONFIRMED-BUG invariant 2,
             * scratch/s25_atomics_conformance.md): this payload copy ran
             * UNLOCKED while GETLLAR snapshots the same line under the
             * lockline lock — torn mixed-writer 128-byte snapshots measured
             * at ~0.4% under contention. Every other writer path (GETLLAR/
             * PUTLLC/PUTLLUC) copies under the lock; mirror that HERE when
             * the span overlaps any reserved line, and keep the lock-free
             * copy for unreserved spans (bulk asset PUTs are large and hot).
             * Residual: a line reserved AFTER the span check but during an
             * unlocked copy can still observe a tear — a far narrower window
             * than the whole-copy exposure this closes; revisit if the
             * harness ever catches it. */
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
                    /* copy + generation bump + peer-reservation kill as ONE
                     * critical section (spu_coh_notify_write expects the lock
                     * held — same contract as the PUTLLC commit path). */
                    spu_lockline_lock();
                    memcpy(ea_ptr, ls_ptr, size);
                    for (uint32_t a = a0; ; a += 128u) {
                        if (spu_coh_is_reserved(a)) spu_coh_notify_write(a);
                        if (a == a1) break;
                    }
                    spu_lockline_unlock();
                } else {
                    memcpy(ea_ptr, ls_ptr, size);
                    /* s21 rule unchanged for the unreserved case: any line
                     * that turns out reserved still gets its generation bump
                     * (the fast-path staleness guard). */
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
            /*
             * Observation-only completion witness for the focused a010
             * stopper-release trace.  The pre-copy recorder proves what the
             * SPU attempted; this proves what guest memory contains after the
             * real PUT has completed.  It is a no-op unless
             * YZ_A010_RELEASE_TRACE is enabled.
             */
#if !defined(YZ_PERF_CLEAN)
            yz_a010_reltrace_spu_commit(
                spu->spu_id, (uint32_t)spu->image_id,
                spu->pc & SPU_LS_MASK,
                (uint32_t)(ea & ~0x80000000ull), ls_ptr, size);
#endif
#if !defined(YZ_PERF_CLEAN)
            /* The a010-specific 0x01252680 job was previously executed as
             * Job A.  Count its actual publications independently of the
             * broad producer census, whose bounded log is normally exhausted
             * by earlier image-14 asset work before this job begins. */
            if (g_yz_a010_root_active != 0 &&
                spu->image_id == 19 &&
                (getenv("YZ_A010_JOBTRACE") ||
                 getenv("YZ_A010_STAGE_KIND1"))) {
                static volatile long job_o_put_n = 0;
                const unsigned long n = (unsigned long)
                    _InterlockedIncrement(&job_o_put_n);
                if (n <= 256u || (n & (n - 1u)) == 0u) {
                    uint32_t hash = 2166136261u;
                    uint32_t nonzero = 0u;
                    for (uint32_t bi = 0; bi < size; bi++) {
                        hash = (hash ^ ls_ptr[bi]) * 16777619u;
                        nonzero += ls_ptr[bi] != 0u;
                    }
                    fprintf(stderr,
                            "[a010-jobO-PUT] n=%lu spu=%X pc=0x%05X "
                            "lsa=0x%05X ea=0x%08X size=0x%X "
                            "nz=%u hash=%08X head=%02X%02X%02X%02X\n",
                            n, spu->spu_id, spu->pc & SPU_LS_MASK,
                            lsa, (uint32_t)(ea & ~0x80000000ull), size,
                            nonzero, hash,
                            ls_ptr[0], ls_ptr[size > 1u ? 1u : 0u],
                            ls_ptr[size > 2u ? 2u : 0u],
                            ls_ptr[size > 3u ? 3u : 0u]);
                    fflush(stderr);
                }
            }
            /* YZ_A010_ROOT_TRACE: bounded producer-side census for the orphanage
             * scene.  We already know the live RSX consumer receives only the
             * tiny UI skeleton; this records SPU PUT payloads into the
             * SPURS/EDGE command-work region while a010 is active.  A dense,
             * non-zero method-shaped payload here with no matching FIFO
             * CALL/JUMP proves a publication/consumer seam bug.  Empty or
             * absent payloads prove the workload was never produced. */
            {
                static int ar = -1;
                if (ar < 0) {
                    ar = getenv("YZ_A010_ROOT_TRACE") ? 1 : 0;
                    if (ar) {
                        fprintf(stderr,
                                "[a010-spu-put] ARMED: a010-only SPU producer census\n");
                        fflush(stderr);
                    }
                }
                const uint32_t pea = (uint32_t)(ea & ~0x80000000ull);
                if (ar && g_yz_a010_root_active != 0 &&
                    size && pea >= 0x40000000u && pea < 0x43000000u &&
                    (spu->image_id == 0 || spu->image_id == 4 ||
                     (spu->image_id >= 13 && spu->image_id <= 15) ||
                     spu->image_id == 19)) {
                    if (spu->image_id == 0) {
                        const uint32_t ppc = spu->pc & SPU_LS_MASK;
                        _InterlockedIncrement(&g_yz_a010_spu_puts);
                        _InterlockedExchangeAdd(
                            &g_yz_a010_spu_put_bytes, (long)size);
                        if (ppc == 0x05F70u) {
                            uint32_t wi = 0;
                            _InterlockedIncrement(&g_yz_a010_spu_groups);
                            _InterlockedExchangeAdd(
                                &g_yz_a010_spu_group_bytes, (long)size);
                            while (wi + 4u <= size) {
                                const uint32_t w =
                                    ((uint32_t)ls_ptr[wi + 0] << 24) |
                                    ((uint32_t)ls_ptr[wi + 1] << 16) |
                                    ((uint32_t)ls_ptr[wi + 2] << 8) |
                                    (uint32_t)ls_ptr[wi + 3];
                                if ((w & 0xE0000003u) == 0x20000000u ||
                                    (w & 3u) == 1u || (w & 3u) == 2u ||
                                    (w & 0xFFFF0003u) == 0x00020000u) {
                                    wi += 4u;
                                    continue;
                                }
                                if ((w & 0xA0030003u) == 0u) {
                                    const uint32_t narg =
                                        (w >> 18) & 0x7FFu;
                                    const uint32_t method =
                                        w & 0x3FFFCu;
                                    const uint32_t packet_bytes =
                                        4u + narg * 4u;
                                    if (narg &&
                                        wi + packet_bytes <= size) {
                                        const uint32_t noninc =
                                            w & 0x40000000u;
                                        _InterlockedIncrement(
                                            &g_yz_a010_spu_headers);
                                        _InterlockedExchangeAdd(
                                            &g_yz_a010_spu_args,
                                            (long)narg);
                                        for (uint32_t ai = 0;
                                             ai < narg; ai++) {
                                            const uint32_t eff =
                                                noninc ? method :
                                                method + ai * 4u;
                                            const uint32_t canonical =
                                                eff & 0x1FFCu;
                                            const uint32_t ao =
                                                wi + 4u + ai * 4u;
                                            const uint32_t av =
                                                ((uint32_t)ls_ptr[ao + 0] << 24) |
                                                ((uint32_t)ls_ptr[ao + 1] << 16) |
                                                ((uint32_t)ls_ptr[ao + 2] << 8) |
                                                (uint32_t)ls_ptr[ao + 3];
                                            if (canonical == 0x1808u) {
                                                _InterlockedIncrement(
                                                    av ?
                                                    &g_yz_a010_spu_begin :
                                                    &g_yz_a010_spu_end);
                                            } else if (
                                                canonical == 0x1814u) {
                                                _InterlockedIncrement(
                                                    &g_yz_a010_spu_array);
                                            } else if (
                                                canonical == 0x1820u) {
                                                _InterlockedIncrement(
                                                    &g_yz_a010_spu_index);
                                            }
                                            if (canonical >= 0x0B80u &&
                                                canonical < 0x0C00u)
                                                _InterlockedIncrement(
                                                    &g_yz_a010_spu_vp);
                                            if (canonical >= 0x1F00u &&
                                                canonical < 0x2000u)
                                                _InterlockedIncrement(
                                                    &g_yz_a010_spu_const);
                                        }
                                        wi += packet_bytes;
                                        continue;
                                    }
                                }
                                _InterlockedIncrement(
                                    &g_yz_a010_spu_unparsed);
                                wi += 4u;
                            }
                        }
                    }
                    static unsigned long an = 0;
                    an++;
                    if (an <= 768u || (an & 0x3FFu) == 0u) {
                        const uint8_t* pl = ls_ptr;
                        uint32_t hash = 2166136261u;
                        uint32_t nonzero = 0, methodish = 0, flowish = 0;
                        for (uint32_t bi = 0; bi < size; bi++) {
                            hash = (hash ^ pl[bi]) * 16777619u;
                            nonzero += pl[bi] != 0;
                        }
                        for (uint32_t wi = 0; wi + 4u <= size; wi += 4u) {
                            const uint32_t w =
                                ((uint32_t)pl[wi + 0] << 24) |
                                ((uint32_t)pl[wi + 1] << 16) |
                                ((uint32_t)pl[wi + 2] << 8) |
                                (uint32_t)pl[wi + 3];
                            methodish +=
                                ((w & 0xA0030003u) == 0u) &&
                                ((w & 0x3FFFCu) != 0u);
                            flowish +=
                                (((w & 0xE0000003u) == 0x20000000u) ||
                                 ((w & 3u) == 1u) || ((w & 3u) == 2u) ||
                                 ((w & 0xFFFF0003u) == 0x00020000u));
                        }
                        fprintf(stderr,
                                "[a010-spu-put] n=%lu spu=%X img=%d pc=0x%05X "
                                "ea=0x%08X size=0x%X nz=%u hash=%08X "
                                "methodish=%u flowish=%u head=%02X%02X%02X%02X\n",
                                an, spu->spu_id, spu->image_id,
                                spu->pc & SPU_LS_MASK, pea, size, nonzero, hash,
                                methodish, flowish,
                                pl[0], pl[size > 1 ? 1 : 0],
                                pl[size > 2 ? 2 : 0], pl[size > 3 ? 3 : 0]);
                        fflush(stderr);
                    }
                }
            }
            /* a010 draw-source probe: catch draw-bearing command blocks
             * published directly by any SPU image. The earlier producer
             * census intentionally covered only likely EDGE images; this
             * normalized header scan is cheap enough to cover every image
             * and names the actual producer PC. */
            {
                const uint32_t pea = (uint32_t)(ea & ~0x80000000ull);
                if (g_yz_a010_ppucmd && g_yz_a010_root_active != 0 &&
                    size && pea >= 0x40400000u && pea < 0x43000000u) {
                    static unsigned long total = 0;
                    for (uint32_t off = 0; off + 4u <= size; off += 4u) {
                        const uint32_t word =
                            ((uint32_t)ls_ptr[off + 0] << 24) |
                            ((uint32_t)ls_ptr[off + 1] << 16) |
                            ((uint32_t)ls_ptr[off + 2] << 8) |
                            (uint32_t)ls_ptr[off + 3];
                        if ((word & 0xA0030003u) != 0u)
                            continue;
                        const uint32_t first = word & 0x3FFFCu;
                        const uint32_t canonical_first = first & 0x1FFCu;
                        const uint32_t count = (word >> 18) & 0x7FFu;
                        const uint32_t canonical_last =
                            canonical_first +
                            (count ? (count - 1u) * 4u : 0u);
                        const int noninc = (word & 0x40000000u) != 0;
                        const int draw_header =
                            count && count <= 64u && ((noninc &&
                                      (canonical_first == 0x1808u ||
                                       canonical_first == 0x1814u ||
                                       canonical_first == 0x1820u)) ||
                                     (!noninc &&
                                      ((canonical_first <= 0x1808u &&
                                        canonical_last >= 0x1808u) ||
                                       (canonical_first <= 0x1814u &&
                                        canonical_last >= 0x1814u) ||
                                       (canonical_first <= 0x1820u &&
                                        canonical_last >= 0x1820u))));
                        if (draw_header) {
                            _InterlockedIncrement(&g_yz_a010_spucmd_headers);
                            total++;
                            if (total <= 256u || (total & 0xFFu) == 0u) {
                                fprintf(stderr,
                                        "[a010-spucmd] n=%lu spu=%X img=%d "
                                        "pc=0x%05X ea=0x%08X+0x%X "
                                        "size=0x%X header=%08X method=%04X "
                                        "count=%u noninc=%d\n",
                                        total, spu->spu_id, spu->image_id,
                                        spu->pc & SPU_LS_MASK, pea, off, size,
                                        word, canonical_first, count, noninc);
                                fflush(stderr);
                            }
                        }
                    }
                }
            }
#endif
            /* s26 ~04:30 (ride17/25 lost-publish hunt): VERIFY-AFTER-WRITE for
             * any PUT covering the decode label 0x10200FE0 — ride17 PROVED an
             * issued publish ([fe0] val=8) that never became visible (5.2M
             * acquire re-reads saw 7), and ride25's losing round shows the
             * task treating an unlanded publish as done. Read back the label
             * word after the copy; on mismatch, log LOUDLY and re-write (the
             * mismatch itself names a concurrent line-restorer). Cheap: one
             * compare on a rare EA. */
            {
                uint32_t vea = (uint32_t)ea;
                if (vea <= 0x10200FE0u && vea + size > 0x10200FE0u && size <= 0x80u) {
                    uint32_t off = 0x10200FE0u - vea;
                    uint32_t want_, got_;
                    memcpy(&want_, ls_ptr + off, 4);
                    memcpy(&got_,  vm_base + 0x10200FE0u, 4);
                    if (want_ != got_) {
                        fprintf(stderr, "[put-verify] LABEL PUT DID NOT LAND: wrote=%02X%02X%02X%02X "
                                "readback=%02X%02X%02X%02X spu=%X pc=0x%05X — REWRITING\n",
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
#ifdef _WIN32
    } __except (GetExceptionCode() == 0xC0000005u /* EXCEPTION_ACCESS_VIOLATION */
                ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        static int w = 0; if (w < 24) { w++;
            fprintf(stderr, "[spu-dma] SKIP faulting %s ea=0x%08X size=%u lsa=0x%05X "
                    "(reserved/uncommitted guest mem -- e.g. unconfigured SPURS trace buffer)\n",
                    mfc_is_get(cmd) ? "GET" : "PUT", (uint32_t)ea, size, lsa);
            fflush(stderr); }
        return 0;   /* benign skip */
    }
#endif

    return 0;
}

/* SPU_EVENT_SN (stall-and-notify tag event), CBEA v1.02 Section 9.12 event bit
 * table (page-cited alongside F10 in specaudit_spu.md: "Sn = 0x2"). Raised the
 * first time ANY tag transitions into the stalled set (RPCS3 SPUThread.cpp:
 * 3308-3311: `if (!ch_stall_stat.get_count()) set_events(SPU_EVENT_SN);` --
 * i.e. edge-triggered on 0->nonzero stall_mask, not re-raised per stalled tag). */
#define SPU_EVENT_SN 0x2u

/*
 * Execute a DMA list command (scatter/gather), starting at element `start_elem`
 * of a list of `num_elements` total (both 0 unless resuming a previously
 * stalled list -- see mfc_channel_write's MFC_WrListStallAck case below).
 * The list resides in the SPU's local store at `list_lsa`; `cur_lsa` is the
 * running LS landing cursor (the caller's mfc_lsa on a fresh start, or the
 * saved cursor on resume). `base_cmd` is the GET/PUT opcode with the list bit
 * already stripped.
 *
 * Returns 0 on normal completion, 1 if the list stalled on a stall-and-notify
 * element (F11/P6: CBEA list commands + Section 9.12 Sn event; RPCS3
 * SPUThread.cpp:3300-3320's resume contract, mirrored via mfc->stall_*[tag]
 * rather than a re-queued mfc_cmd since our DMA is synchronous), or a
 * negative mfc_do_transfer error.
 */
static inline int mfc_do_list_transfer_from(mfc_engine* mfc, spu_context* spu,
                                             uint32_t tag, uint32_t list_lsa,
                                             uint64_t ea_base, uint32_t base_cmd,
                                             uint32_t start_elem, uint32_t num_elements,
                                             uint32_t cur_lsa)
{
    for (uint32_t i = start_elem; i < num_elements; i++) {
        uint32_t elem_lsa = (list_lsa + i * 8) & SPU_LS_MASK;

        /* Read list element from local store (big-endian):
         *   byte0 bit7 = stall-and-notify (=bit 31 of the BE word), bits 0-15 = size. */
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
        if (rc != 0) {
            static unsigned list_error_count = 0;
            if (list_error_count++ < 64) {
                fprintf(stderr,
                        "[mfc-list-error] spu=%X img=%d pc=0x%05X tag=%u "
                        "cmd=0x%02X list=0x%05X elem=%u/%u elem-lsa=0x%05X "
                        "raw=0x%08X eal=0x%08X xfer=%u cur-lsa=0x%05X rc=%d\n",
                        spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK,
                        tag, base_cmd, list_lsa, i, num_elements, elem_lsa,
                        size_and_flags, eal, xfer_size, transfer_lsa & SPU_LS_MASK, rc);
                fflush(stderr);
            }
            return rc;
        }

        if (xfer_size != 0)
            cur_lsa = (transfer_lsa + xfer_size + 15u) & ~15u;

        /* A stall bit on the final element is unsupported and ignored. */
        if (stall_notify && i + 1u < num_elements) {
            /* Suspend the list AFTER this element (the transfer above already
             * ran -- CBEA: the stalling element's OWN transfer completes; only
             * the REST of the list is held). Record enough to resume from
             * element i+1 on MFC_WrListStallAck, and raise Sn (edge-triggered
             * on the first tag to stall, matching RPCS3). */
            uint32_t bit = 1u << (tag & 0x1Fu);
            if (!mfc->stall_mask) {
                /* FIX 2 (2026-07-17): event_status is now _Atomic uint32_t
                 * (runtime/spu/spu_context.h); atomic_fetch_or_explicit, not a
                 * plain `|=`, for consistency with the field's other writers
                 * (this one is same-thread -- the issuing SPU's own list
                 * transfer -- so relaxed is enough; no cross-thread ordering
                 * is being established here). */
                atomic_fetch_or_explicit(&spu->event_status, SPU_EVENT_SN, memory_order_relaxed);
                spu_ch_wake(spu);   /* s39: wake a blocked RdEventStat */
            }
            mfc->stall_mask |= bit;
            mfc->stall_list_lsa[tag]     = list_lsa;
            mfc->stall_ea_base[tag]      = ea_base;
            mfc->stall_next_elem[tag]    = i + 1u;
            mfc->stall_num_elements[tag] = num_elements;
            mfc->stall_cur_lsa[tag]      = cur_lsa;
            mfc->stall_base_cmd[tag]     = base_cmd;
            return 1;   /* stalled -- caller must NOT mark the tag completed */
        }
    }

    return 0;
}

/* Back-compat entry point: a fresh (non-resuming) list transfer. Kept so the
 * existing mfc_submit call site reads the same as before; routes through
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
    extern unsigned long g_spu_getllar_n;
    extern uint32_t g_spu_getllar_ea;
    uint32_t lsa  = spu->mfc_lsa;
    uint64_t ea   = ((uint64_t)spu->mfc_eah << 32) | spu->mfc_eal;
    uint32_t size = spu->mfc_size;
    uint32_t tag  = spu->mfc_tag & 0x1F;
    int tagged = mfc_uses_tag(cmd);
    int rc = 0;

#if defined(YZ_PERF_PROFILE)
    {
        const int image = spu->image_id;
        if ((unsigned)image < 32u) {
            if (cmd == MFC_GETLLAR_CMD || cmd == MFC_PUTLLC_CMD ||
                cmd == MFC_PUTLLUC_CMD || cmd == MFC_PUTQLLUC_CMD)
                spu->perf_dma_atomic[image]++;
            else if (mfc_is_get(cmd))
                spu->perf_dma_get[image]++;
            else if (mfc_is_put(cmd))
                spu->perf_dma_put[image]++;
        }
    }
#endif

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

#if !defined(YZ_PERF_CLEAN)
    /* s44: durable consumer-identity publish (replaces the gs_task.c 0x6380
     * hand-edit the s43 relift wiped -- generated-file edits die on relift).
     * The journal consumer = the image-0 (gs_task) context GETting from the
     * journal arena [0x41F00000,0x42200000) (the same constant range
     * spu_fltrec.c's arena dump uses). First such GET publishes; same
     * diagnostic-only, fail-open-on-0 semantics as the old publish site. */
    if (!g_yz_consumer_ctx && spu->image_id == 0 && mfc_is_get(cmd)
        && ea >= 0x41F00000ull && ea < 0x42200000ull) {
        g_yz_consumer_ctx = (volatile void*)spu;
        fprintf(stderr, "[fltrec] consumer ctx published: spu=%X (journal GET ea=0x%08X pc=0x%05X)\n",
                spu->spu_id, (uint32_t)ea, spu->pc & SPU_LS_MASK);
        fflush(stderr);
    }

    /* s41 FLIGHT RECORDER (env YZ_FLTREC, consumer ctx only; spu_fltrec.h).
     * Covers every real transfer including the lock-line atomics (GETLLAR/
     * PUTLLC/PUTLLUC/PUTQLLUC all satisfy mfc_is_get/mfc_is_put). Two fixed
     * records per op: DMA_GET/DMA_PUT (addr=LSA, value=EA-low) then
     * DMA_META (addr=EA-high, value=raw cmd, aux=tag, len_or_ch=size). */
    if (yz_fltrec_hot(spu))
        yz_fltrec_dma(spu, mfc_is_put(cmd), lsa, (uint32_t)ea,
                      (uint32_t)(ea >> 32), size, tag, cmd);


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
#endif

#if !defined(YZ_PERF_CLEAN)
    /* DIAG (YZ_SPU_PROF): gs_task LAUNCH DMAs. The taskset policy, once it
     * selects a task, DMAs the task_info (to LS 0x2780), then spursTasksetLoadElf
     * DMAs the task ELF header + segments (GET from the gs_task ELF EA ~0x0127xxxx
     * into LS 0x3000+). These happen LATE -- after the [spu-wl] cap above is
     * spent on early kernel/service loads -- so log them separately (higher cap)
     * to confirm whether the launch actually reaches LoadElf. */
    /* Capture the gs_task CellSpursTaskset EA from the task_info DMA (the policy
     * GETs taskset->task_info[taskId] (taskset+0x80) into LS 0x2780). taskset base
     * = ea - 0x80; its bitset line (running@0x00) is at that 128-aligned addr.
     * Used by the YZ_CLEARRUN bootstrap below. */
    if (cmd == MFC_GET_CMD && (lsa & SPU_LS_MASK) == 0x2780u
            && (uint32_t)ea >= 0x40000000u && size <= 0x40u) {
        extern uint32_t g_yz_taskset_ea;
        g_yz_taskset_ea = ((uint32_t)ea - 0x80u) & ~0x7Fu;
    }
    /* pt35 (env YZ_ELF_WATCH, NOT prof-gated): does the policy's spursTasksetLoadElf
     * actually DMA-read the CODEC ELF (0x012B4980..)? If we see GETs from there, the
     * dispatch reached LoadElf; how far they go shows where LoadElf halts. gs_task is
     * band-aided past the real dispatch, so LoadElf may be untested. */
    { static int ew = -1; if (ew < 0) ew = getenv("YZ_ELF_WATCH") ? 1 : 0;
      if (ew && cmd == MFC_GET_CMD && (uint32_t)ea >= 0x012B4980u && (uint32_t)ea < 0x012E0000u) {
          extern int g_yz_codec_dispatch_spu;
          g_yz_codec_dispatch_spu = (int)spu->spu_id;   /* arm the dispatch-tail trace */
          static int ewn = 0;
          if (ewn < 60) { ewn++;
              fprintf(stderr, "[elf-dma] spu=%X LoadElf GET ea=0x%08X lsa=0x%05X size=0x%X\n",
                      spu->spu_id, (uint32_t)ea, lsa & SPU_LS_MASK, size);
              fflush(stderr);
          }
      } }
    /* Codec/pool output watch (env YZ_CODEC_PUT, 2026-07-02, diag — REMOVE when
     * the voice-init frontier closes): log every PUT-class DMA issued while a
     * TASK image (3 = cri_audio, 4 = the wid-4 pool) is active — the tasks'
     * init runs are expected to write an init-complete status the PPU-side CRI
     * layer polls before it will issue decode work / task signals. Names the
     * status EAs (or proves the init never notifies). */
    { static int cp = -1; if (cp < 0) cp = getenv("YZ_CODEC_PUT") ? 1 : 0;
      if (cp && (spu->image_id == 3 || spu->image_id == 4)
             && (mfc_is_put(cmd) || cmd == MFC_GETLLAR_CMD)) {
          /* 2026-07-02b: atomics now INCLUDED (the queue-header PUTLLC was
           * invisible before) + the submitting pc — names the push function.
           * For line atomics on the response-queue line, dump the first 16
           * bytes of the LS source (the front/back words being committed). */
          static int cpn = 0;
          if (cpn < 120) { cpn++;
              fprintf(stderr, "[codec-put] spu=%X img=%d pc=0x%05X cmd=0x%02X lsa=0x%05X ea=0x%08llX size=0x%X",
                      spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK, cmd,
                      lsa & SPU_LS_MASK, (unsigned long long)ea, size);
              if ((cmd == MFC_PUTLLC_CMD || cmd == MFC_PUTLLUC_CMD || cmd == MFC_GETLLAR_CMD)
                  && (((uint32_t)ea & ~0x7Fu) == 0x63D61600u
                      || ((uint32_t)ea & ~0x7Fu) == 0x63D61400u)) {
                  const uint8_t* l = &spu->ls[lsa & SPU_LS_MASK];
                  fprintf(stderr, "  LS[0..15]:");
                  for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", l[i]);
                  if (cmd == MFC_GETLLAR_CMD
                      && ((uint32_t)ea & ~0x7Fu) == 0x63D61400u) {
                      /* release event-armed tracing at the REQUEST-queue pop
                       * (the resumed codec locks front then compute-loops) */
                      extern int g_spu_trace_evarm;
                      g_spu_trace_evarm = 1;
                  }
              }
              fprintf(stderr, "\n");
              fflush(stderr);
          }
      } }
    /* Journal consumption watch (env YZ_JRNL_WATCH, 2026-07-02, diag — the
     * LAYER-1 discriminator probe): does OUR gs_task ever touch the gcm
     * journal HEAD lines (buffer A = 0x41F00080, B = 0x42100080) after its
     * initial batch, and does ANY SPU ever PUT into the 2 MB journal arena
     * (= the tag-zero/retire signal the RPCS3 oracle shows)? never-polls =>
     * park/exit or per-append wakeup bug; polls-and-ignores => the emptiness
     * test miscomputes on the dumped line content. REMOVE when the journal
     * consumer frontier closes. */
    { static int jw = -1; if (jw < 0) jw = getenv("YZ_JRNL_WATCH") ? 1 : 0;
      if (jw) {
          uint32_t jea   = (uint32_t)(ea & ~0x80000000ull);   /* atomics may tag bit 31 */
          uint32_t jline = jea & ~127u;
          int atomic = (cmd == MFC_GETLLAR_CMD || cmd == MFC_PUTLLC_CMD ||
                        cmd == MFC_PUTLLUC_CMD || cmd == MFC_PUTQLLUC_CMD);
          uint32_t jsz = atomic ? 128u : size;
          int head = (jline == 0x41F00080u || jline == 0x42100080u);
          int put  = (mfc_is_put(cmd) || cmd == MFC_PUTLLC_CMD ||
                      cmd == MFC_PUTLLUC_CMD || cmd == MFC_PUTQLLUC_CMD)
                     && jea < 0x42110000u && jea + jsz > 0x41F00000u;
          /* the consumer's journal-cursor polls stage through LS 0x37780 —
           * catching them by LSA (any EA) shows the walking cursor AND
           * proves the consumer is still alive late in the run */
          int curs = (cmd == MFC_GETLLAR_CMD && (lsa & SPU_LS_MASK) == 0x37780u);
          if (curs && !head) {
              /* s34: publish the live cursor EA for the [stop-jrnl] hexdump
               * (import_overrides.cpp) -- maintained on EVERY poll, not just
               * the capped print, so the FIFO side always sees the current
               * value even after the [jrnl-cur] log stops printing. */
              extern volatile uint32_t g_yz_jrnl_cur_ea;
              g_yz_jrnl_cur_ea = jea;
              static unsigned long cn = 0; cn++;
              if (cn <= 40 || (cn & 0x3FFu) == 0) {
                  /* s38 desync check: dump the TWO cursor counters + base so the
                   * wedge state shows whether walkctr (cb0.w1 @0xBC90, wraps 32,
                   * drives the gate shadow addr r11) and counter2 (cb2.w1 @0xBCB0,
                   * wraps 128, drives curEA) have DESYNCED after a segment wrap.
                   * Expected steady relation: curEA(jea) == base(cb2.w0) + counter2*0x80. */
                  const uint8_t* c0 = &spu->ls[0xBC90u]; const uint8_t* c2 = &spu->ls[0xBCB0u];
                  uint32_t walkctr = ((uint32_t)c0[4]<<24)|((uint32_t)c0[5]<<16)|((uint32_t)c0[6]<<8)|c0[7];
                  uint32_t base    = ((uint32_t)c2[0]<<24)|((uint32_t)c2[1]<<16)|((uint32_t)c2[2]<<8)|c2[3];
                  uint32_t counter2= ((uint32_t)c2[4]<<24)|((uint32_t)c2[5]<<16)|((uint32_t)c2[6]<<8)|c2[7];
                  uint32_t predEA  = base + counter2*0x80u;
                  fprintf(stderr, "[jrnl-cur] n=%lu spu=%X pc=0x%05X ea=0x%08X | walkctr=%u counter2=%u base=0x%08X predEA=0x%08X %s\n",
                          cn, spu->spu_id, spu->pc & SPU_LS_MASK, jea,
                          walkctr, counter2, base, predEA, (predEA==jea)?"EAok":"EA-MISMATCH");
                  fflush(stderr);
              }
              /* s32 RO-BRACKET probe (always on, ~1 memcmp per 256 polls):
               * s32coop3 proved the one remaining death = gs_task's RO span
               * zeroed MID-EXECUTION between a byte-identical restore and the
               * fault (block-0 round-trip h constant; DMA synchronous; no
               * foreign SPU activity in the bracket) -- a same-thread guest
               * store to a wrong computed address. This rides the consumer's
               * own poll: verify the vtable-carrying tail of the RO span
               * [0xB800,0xBC00) against the ELF source; the FIRST mismatch
               * brackets the zeroing store to ~256 polls, with offset bytes
               * for the store-watch that names the instruction next boot. */
              { static int brk_hit = 0;
                if (!brk_hit && (cn & 0xFFu) == 0 && spu->image_id == 0) {
                    const uint8_t* src = vm_base + 0x0127A580u + 0x100u + 0x8800u;
                    const uint8_t* dst = spu->ls + 0xB800u;
                    if (memcmp(dst, src, 0x400u) != 0) {
                        uint32_t bo = 0;
                        while (bo < 0x400u && dst[bo] == src[bo]) bo++;
                        brk_hit = 1;
                        fprintf(stderr, "[ro-brk] FIRST RO-span mismatch at poll n=%lu "
                                "spu=%X off=+0x%X (LS 0x%05X) ls=%02X%02X%02X%02X "
                                "elf=%02X%02X%02X%02X (zeroing store within ~256 polls "
                                "before this)\n",
                                cn, spu->spu_id, bo, 0xB800u + bo,
                                dst[bo], dst[bo+1], dst[bo+2], dst[bo+3],
                                src[bo], src[bo+1], src[bo+2], src[bo+3]);
                        fflush(stderr);
                    }
                } }
              /* s26 (task #4): dump the FULL 128-byte line the consumer just
               * GETLLAR'd — the consume gate tests the +0x20 sub-entry vs the
               * 0x4000 sentinel (gs_task.c 0x63F4), which a 32-byte dump cut
               * off. (The earlier gpr3 struct dump was measured-useless: the
               * routine zeroes gpr3 at 0x6368, before the Cmd wrch we catch.)
               * ride11 already proved the head is NON-EMPTY at the stall. */
              if (cn <= 12 || (cn & 0xFFFu) == 0) {
                  /* s47: the dump must not deref a wounded EA -- push boot 1
                   * died HERE, not in the game: consumer state zeroed -> jea=0
                   * -> vm_base+0 = the uncommitted null page -> host AV
                   * (LESSONS #6b). Out-of-arena EA -> note-and-skip. */
                  if ((jea & ~127u) < 0x41F00000u || (jea & ~127u) >= 0x42200000u) {
                      static unsigned long oo = 0; oo++;
                      if (oo <= 8 || (oo & 0xFFFu) == 0) {
                          fprintf(stderr, "[jrnl-line] n=%lu ea=0x%08X OUT-OF-ARENA "
                                  "(wounded consumer state; dump skipped)\n",
                                  cn, jea & ~127u);
                          fflush(stderr);
                      }
                  } else {
                  const uint8_t* hl = vm_base + (jea & ~127u);
                  char cb[420]; int ci = 0;
                  ci += snprintf(cb + ci, sizeof(cb) - (size_t)ci,
                          "[jrnl-line] n=%lu ea=0x%08X:", cn, jea & ~127u);
                  for (int k = 0; k < 128 && ci < (int)sizeof(cb) - 4; k += 4)
                      ci += snprintf(cb + ci, sizeof(cb) - (size_t)ci, "%s%02X%02X%02X%02X",
                                     (k & 31) ? " " : " | ", hl[k], hl[k+1], hl[k+2], hl[k+3]);
                  fprintf(stderr, "%s\n", cb); fflush(stderr);
                  }
              }
          }
          if (head && cmd == MFC_GETLLAR_CMD && jline == 0x41F00080u) {
              /* release event-armed tracing (YZ_SPU_TRACE_EVARM) at the
               * consumer's journal-head poll — captures the consume/park
               * decision path that follows the read */
              extern int g_spu_trace_evarm;
              g_spu_trace_evarm = 1;
          }
          if (head || put) {
              static unsigned long jn = 0; jn++;
              if (jn <= 80 || (jn & 0xFFFu) == 0) {
                  fprintf(stderr, "[jrnl] n=%lu spu=%X img=%d pc=0x%05X cmd=0x%02X lsa=0x%05X ea=0x%08X size=0x%X",
                          jn, spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK, cmd,
                          lsa & SPU_LS_MASK, jea, size);
                  if (head) {
                      const uint8_t* m = vm_base + jline;
                      fprintf(stderr, "  line[0..31]:");
                      for (int i = 0; i < 32; i++) fprintf(stderr, " %02X", m[i]);
                  }
                  fprintf(stderr, "\n"); fflush(stderr);
              }
          }
      } }
    /* gs_task ring-apply watch (env YZ_GSPUT, 2026-07-02, diag — the matrix
     * discriminator for the journal back half): does gs_task ever PUT the
     * journal patches into the command ring / io memory (or its geometry
     * output)? Logs every put-class DMA issued under image 0 with pc+ea+size;
     * no EA filter — classify offline by pc (retire pcs 0x63EC/0x6424/0x645C;
     * ctx saves lsa 0x2C80-0x3000; kernel lock-line PUTLLCs by their mgmt
     * EAs). REMOVE with the journal-consumer frontier. */
    /* Focused a010 jobB census.  The ordinary YZ_JOBPUT counter is exhausted
     * during startup, before the orphanage loads.  Keep this separate and arm
     * it only while the a010 root gate is active so the trace answers one
     * question: after the relocated image-15 entry is adopted, does it issue
     * any DMA at all, and if so where? */
    { static int a010_job_dma = -1;
      static unsigned long a010_job_dma_n = 0;
      if (a010_job_dma < 0)
          a010_job_dma = getenv("YZ_A010_JOBTRACE") ? 1 : 0;
      if (a010_job_dma && g_yz_a010_root_active != 0 &&
          spu->image_id == 15 &&
          spu->job_bin_base[1] == 0x06C00u &&
          a010_job_dma_n < 512) {
          a010_job_dma_n++;
          fprintf(stderr,
                  "[a010-job-dma] n=%lu spu=%X pc=0x%05X cmd=0x%02X "
                  "lsa=0x%05X ea=0x%08llX size=0x%X tag=%u\n",
                  a010_job_dma_n, spu->spu_id,
                  spu->pc & SPU_LS_MASK, cmd, lsa & SPU_LS_MASK,
                  (unsigned long long)(ea & ~0x80000000ull), size, tag);
          fflush(stderr);
      }
    }
    /* s24 late (env YZ_JOBPUT): jobB's journal half. The RPCS3 oracle showed
     * the slot-0x4C00 job (OUR image 15) performing the gcm journal
     * application + stopper releases (LS pc 0x5EB8 = base+0x12B8) during the
     * SAME jobB-only round era our boots run — while our jobB only ever does
     * its audio half (flag+spup17). Log every put-class DMA from images
     * 13/14/15 with pc, so "our jobB never writes the ring/journal" becomes
     * MEASURED, not inferred (DONT_RECHASE #46). Volume-bounded like gs-put. */
    { static int jp = -1; if (jp < 0) { jp = getenv("YZ_JOBPUT") ? 1 : 0;
          if (jp) { fprintf(stderr, "[job-put] ARMED: image-13/14/15 put-class DMA watch live\n"); fflush(stderr); } }
      if (jp && (spu->image_id >= 13 && spu->image_id <= 15)
             && (mfc_is_put(cmd) || cmd == MFC_PUTLLC_CMD ||
                 cmd == MFC_PUTLLUC_CMD || cmd == MFC_PUTQLLUC_CMD)) {
          static unsigned long jpn = 0; jpn++;
          if (jpn <= 200 || (jpn & 0xFFu) == 0) {
              fprintf(stderr, "[job-put] n=%lu spu=%X img=%d pc=0x%05X cmd=0x%02X lsa=0x%05X ea=0x%08llX size=0x%X\n",
                      jpn, spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK, cmd,
                      lsa & SPU_LS_MASK, (unsigned long long)(ea & ~0x80000000ull), size);
              fflush(stderr);
          }
      } }
    { static int gp = -1; if (gp < 0) { gp = getenv("YZ_GSPUT") ? 1 : 0;
          if (gp) { fprintf(stderr, "[gs-put] ARMED: image-0 put-class DMA watch live\n"); fflush(stderr); } }
      if (gp && spu->image_id == 0
             && (mfc_is_put(cmd) || cmd == MFC_PUTLLC_CMD ||
                 cmd == MFC_PUTLLUC_CMD || cmd == MFC_PUTQLLUC_CMD)) {
          static unsigned long gpn = 0; gpn++;
          if (gpn <= 100 || (gpn & 0x3FFu) == 0) {
              fprintf(stderr, "[gs-put] n=%lu spu=%X pc=0x%05X cmd=0x%02X lsa=0x%05X ea=0x%08llX size=0x%X\n",
                      gpn, spu->spu_id, spu->pc & SPU_LS_MASK, cmd,
                      lsa & SPU_LS_MASK, (unsigned long long)(ea & ~0x80000000ull), size);
              fflush(stderr);
          }
      } }
    /* Decode-sync gate-label watch (env YZ_FE0_WATCH, 2026-07-08 s22 — ⚡ item ③,
     * DONT_RECHASE #29): the movie-phase FIFO parks on SEMAPHORE_ACQUIRE of label
     * 0x10200FE0==1, published in RPCS3 by an SPU workload at LS pc 0x0AB70 (wkl4,
     * image 4, the monotonic per-round counter). Our PPU-side watch measured ZERO
     * writes; this closes the OTHER path — any SPU DMA (regular PUT or atomic)
     * touching the label word names the writer (spu/img/pc) + the value. Uncapped
     * (the oracle cadence is ~30 Hz), armed banner, observation only. */
    /*
     * Focused a010 publication census.  The arena reader arms this only after
     * the exact 819-record static-stage batch is fetched.  Count every later
     * gs_task PUT by source PC and show its destination plus leading payload.
     */
    {
        extern volatile long g_yz_a010_target_fetch_seen;
        extern volatile long g_yz_a010_stage_generation;
        static int a010_publish_flow = -1;
        static uint32_t image_pc_keys[512];
        static unsigned long image_pc_counts[512];
        static unsigned image_pc_key_count;
        if (a010_publish_flow < 0) {
            a010_publish_flow =
                getenv("YZ_A010_PUBLISH_FLOW") ? 1 : 0;
            if (a010_publish_flow) {
                fprintf(stderr,
                        "[a010-publish-flow] ARMED: post-stage-batch "
                        "gs_task PUT census\n");
                fflush(stderr);
            }
        }
        if (a010_publish_flow &&
            (g_yz_a010_stage_generation != 0 ||
             g_yz_a010_target_fetch_seen != 0) &&
            g_yz_a010_root_active != 0 &&
            (mfc_is_put(cmd) || cmd == MFC_PUTLLC_CMD ||
             cmd == MFC_PUTLLUC_CMD || cmd == MFC_PUTQLLUC_CMD)) {
            const uint32_t pc = spu->pc & SPU_LS_MASK;
            const uint32_t key =
                ((uint32_t)(spu->image_id & 0xFFFF) << 16) |
                ((pc >> 2) & 0xFFFFu);
            unsigned key_slot;
            unsigned long n = 0;
            for (key_slot = 0; key_slot < image_pc_key_count; ++key_slot) {
                if (image_pc_keys[key_slot] == key)
                    break;
            }
            if (key_slot == image_pc_key_count &&
                image_pc_key_count < 512u) {
                image_pc_keys[key_slot] = key;
                image_pc_counts[key_slot] = 0;
                ++image_pc_key_count;
            }
            if (key_slot < image_pc_key_count)
                n = ++image_pc_counts[key_slot];
            if (n != 0u &&
                (n <= 8u || (n & (n - 1u)) == 0u)) {
                const uint32_t sl = lsa & SPU_LS_MASK;
                uint32_t words[4] = {0, 0, 0, 0};
                for (unsigned wi = 0; wi < 4u; wi++) {
                    const uint32_t p = (sl + wi * 4u) & SPU_LS_MASK;
                    words[wi] =
                        ((uint32_t)spu->ls[p] << 24) |
                        ((uint32_t)spu->ls[(p + 1u) & SPU_LS_MASK] << 16) |
                        ((uint32_t)spu->ls[(p + 2u) & SPU_LS_MASK] << 8) |
                        (uint32_t)spu->ls[(p + 3u) & SPU_LS_MASK];
                }
                fprintf(stderr,
                        "[a010-publish-flow] img=%d pc=0x%05X n=%lu spu=%X "
                        "cmd=0x%02X lsa=0x%05X ea=0x%08llX size=0x%X "
                        "words=%08X/%08X/%08X/%08X\n",
                        spu->image_id, pc, n, spu->spu_id, cmd, sl,
                        (unsigned long long)(ea & ~0x80000000ull), size,
                        words[0], words[1], words[2], words[3]);
                fflush(stderr);
            }
        }
    }
    { static int fe0 = -1;
      if (fe0 < 0) { fe0 = getenv("YZ_FE0_WATCH") ? 1 : 0;
          if (fe0) { fprintf(stderr, "[fe0] watch armed (0x10200FE0 SPU PUT path)\n"); fflush(stderr); } }
      if (fe0) {
          uint32_t fea = (uint32_t)(ea & ~0x80000000ull);
          int fatomic = (cmd == MFC_GETLLAR_CMD || cmd == MFC_PUTLLC_CMD ||
                         cmd == MFC_PUTLLUC_CMD || cmd == MFC_PUTQLLUC_CMD);
          uint32_t fsz = fatomic ? 128u : size;
          uint32_t fbase = fatomic ? (fea & ~127u) : fea;
          int fput = (mfc_is_put(cmd) || cmd == MFC_PUTLLC_CMD ||
                      cmd == MFC_PUTLLUC_CMD || cmd == MFC_PUTQLLUC_CMD)
                     && fbase < 0x10200FE4u && fbase + fsz > 0x10200FE0u;
          if (fput) {
              static unsigned long fn = 0; fn++;
              uint32_t off = (fbase <= 0x10200FE0u) ? 0x10200FE0u - fbase : 0u;
              const uint8_t* pl = spu->ls + ((lsa & SPU_LS_MASK) + (fatomic ? off & 127u : off));
              fprintf(stderr, "[fe0] n=%lu spu=%X img=%d pc=0x%05X cmd=0x%02X ea=0x%08X "
                      "size=0x%X val=%02X%02X%02X%02X\n",
                      fn, spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK, cmd,
                      fea, size, pl[0], pl[1], pl[2], pl[3]);
              fflush(stderr);
          }
      } }
    /* wid4 work-record fetch watch (env YZ_W4REC, 2026-07-10 s26 ⚡1, diag —
     * scratch/s26_wkl4_publish_re.md): each wid4 pool dispatch is a FRESH task
     * run that blocking-GETs its 64-byte work record (value + EA-guard +
     * flags) from EA = word[1] of taskInfo->args (SPU pc 0x319C-0x31DC). Log
     * every such GET with the EA and the 64 record bytes as fetched (read from
     * vm_base at issue — same bytes the transfer copies). One capture
     * discriminates: EA fixed vs per-round; failing round's record
     * stale-repeat vs zero vs garbage — then the EA feeds a PPU-side
     * YZ_WATCH_WR. Low volume (~5 tasks/round), uncapped, armed banner. */
    { static int w4r = -1; static int w4root = -1;
      if (w4r < 0) { w4r = getenv("YZ_W4REC") ? 1 : 0;
          if (w4r) { fprintf(stderr, "[w4rec] ARMED: image-4 64-byte work-record GET watch live\n"); fflush(stderr); } }
      if (w4root < 0) w4root = getenv("YZ_A010_ROOT") ? 1 : 0;
      const int w4active =
          w4r && (!w4root || g_yz_a010_root_active != 0);
      /* s26 ~03:25 (STATUS frontier): the record STAGER watch — who WRITES the
       * work-record slots (not a lifted PPU store per YZ_SLOTSTORE) — any-image
       * PUT-class DMA covering the slot range, payload+pc dumped. One boot
       * names the stager; the fix enforces its stage→signal ordering. */
      if (w4active && (mfc_is_put(cmd) || cmd == MFC_PUTLLC_CMD || cmd == MFC_PUTLLUC_CMD
                  || cmd == MFC_PUTQLLUC_CMD)) {
          uint32_t pea = (uint32_t)(ea & ~0x80000000ull);
          uint32_t psz = (cmd == MFC_PUTLLC_CMD || cmd == MFC_PUTLLUC_CMD
                          || cmd == MFC_PUTQLLUC_CMD) ? 128u : size;
          uint32_t pb  = (cmd == MFC_PUTLLC_CMD || cmd == MFC_PUTLLUC_CMD
                          || cmd == MFC_PUTQLLUC_CMD) ? (pea & ~127u) : pea;
          if (pb < 0x424529E0u && pb + psz > 0x42452880u) {
              static unsigned long sn = 0; sn++;
              const uint8_t* pl = spu->ls + (lsa & SPU_LS_MASK);
              char sb[200]; int si = 0;
              si += snprintf(sb + si, sizeof(sb) - (size_t)si,
                      "[w4stage] n=%lu spu=%X img=%d pc=0x%05X cmd=0x%02X ea=0x%08X size=0x%X head:",
                      sn, spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK, cmd, pea, size);
              for (int i = 0; i < 16 && si < (int)sizeof(sb) - 4; i++)
                  si += snprintf(sb + si, sizeof(sb) - (size_t)si, "%s%02X", (i & 3) ? "" : " ", pl[i]);
              fprintf(stderr, "%s\n", sb); fflush(stderr);
          }
      }
      if (w4active && spu->image_id == 4 && mfc_is_get(cmd) && cmd != MFC_GETLLAR_CMD
              && size == 0x40u) {
          static unsigned long wn = 0; wn++;
          uint32_t rea = (uint32_t)(ea & ~0x80000000ull);
          const uint8_t* m = vm_base + rea;
          /* single-write print: interleaved per-byte fprintf tore mid-line
           * under concurrent stderr writers (s26ride8 lost the publish-gate
           * fields of the decisive fetch) */
          char rb[320]; int ri = 0;
          ri += snprintf(rb + ri, sizeof(rb) - (size_t)ri,
                  "[w4rec] n=%lu spu=%X pc=0x%05X lsa=0x%05X ea=0x%08X rec:",
                  wn, spu->spu_id, spu->pc & SPU_LS_MASK, lsa & SPU_LS_MASK, rea);
          for (int i = 0; i < 64 && ri < (int)sizeof(rb) - 4; i++)
              ri += snprintf(rb + ri, sizeof(rb) - (size_t)ri,
                             "%s%02X", (i & 3) ? "" : " ", m[i]);
          fprintf(stderr, "%s\n", rb); fflush(stderr);
      } }
    /* Display-list SPU-writer watch (env YZ_DLIST_SPU, s28 ledger #63): the
     * early-boot stall parks GET on the A2000500 placeholder at io 0x1104D00
     * (EA 0x41504Dxx) and ZERO PPU stores ever hit it — the writer is
     * SPU-side (gs_task geometry output). Log any SPU PUT covering the list
     * head so a stalled boot names whether the write is attempted at all. */
    { static int dls = -1;
      if (dls < 0) { dls = getenv("YZ_DLIST_SPU") ? 1 : 0;
          if (dls) { fprintf(stderr, "[dlist-spu] ARMED: SPU PUT watch on EA 0x41504C00-4E00\n"); fflush(stderr); } }
      if (dls && (mfc_is_put(cmd) || cmd == MFC_PUTLLC_CMD || cmd == MFC_PUTLLUC_CMD)) {
          uint32_t dea = (uint32_t)(ea & ~0x80000000ull);
          uint32_t dsz = (cmd == MFC_PUTLLC_CMD || cmd == MFC_PUTLLUC_CMD) ? 128u : size;
          if (dea < 0x41504E00u && dea + dsz > 0x41504C00u) {
              static unsigned long dn = 0; dn++;
              fprintf(stderr, "[dlist-spu] n=%lu spu=%X img=%d pc=0x%05X cmd=0x%02X ea=0x%08X size=0x%X\n",
                      dn, spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK, cmd, dea, size);
              fflush(stderr);
          }
      } }
    /* wid4 mgmt-line raise watch (env YZ_W4MGMT, s27 — STATUS 🌄 grace-window
     * verdict): does the pool's inlined task-SendSignal (A2F0/A5E8) ever raise
     * the WORKLOAD's readyCount/signal on the SPURS mgmt line 0x40197C80 so
     * kernels re-select the taskset? Log every img=4 atomic touching that
     * line. Healthy-rounds-with-raises + wedge-without = a lost-raise value
     * hunt; none-ever = decode the real kernel wake path. */
    { static int wm = -1;
      if (wm < 0) { wm = getenv("YZ_W4MGMT") ? 1 : 0;
          if (wm) { fprintf(stderr, "[w4mgmt] ARMED: img4 mgmt-line atomic watch live\n"); fflush(stderr); } }
      if (wm && spu->image_id == 4
             && (cmd == MFC_GETLLAR_CMD || cmd == MFC_PUTLLC_CMD || cmd == MFC_PUTLLUC_CMD)
             && (((uint32_t)(ea & ~0x80000000ull)) & ~127u) == 0x40197C80u) {
          static unsigned long wmn = 0; wmn++;
          if (wmn <= 400 || (wmn & 0xFFu) == 0) {
              const uint8_t* pl = spu->ls + (lsa & SPU_LS_MASK);
              fprintf(stderr, "[w4mgmt] n=%lu spu=%X pc=0x%05X cmd=0x%02X ls16:"
                      " %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                      wmn, spu->spu_id, spu->pc & SPU_LS_MASK, cmd,
                      pl[0],pl[1],pl[2],pl[3], pl[4],pl[5],pl[6],pl[7],
                      pl[8],pl[9],pl[10],pl[11], pl[12],pl[13],pl[14],pl[15]);
              fflush(stderr);
          }
      } }
    /* Taskset ctx-save EA watch (env YZ_CTXWATCH, 2026-07-10 s26 ⚡1, diag —
     * DONT_RECHASE #53/#54): the wid4 pool republished a STALE decode counter
     * ([fe0] val=1 twice, s25ride12) — the task's state never advanced between
     * WAIT_SIGNAL cycles. The context round-trips main memory via the taskInfo
     * ctxsave EA (registered by spu_task_launch into g_yz_ctxw_*): the yield's
     * plain-PUT is the SAVE, GETs are Sony's own restore reads (our host-memcpy
     * restore is logged separately as [ctxw] RESUME in spu_channels.c). SAVE
     * hashes the LS payload (the post-image being written); LOAD hashes main
     * memory at the block base (what a restore reads) — RESUME hashes share the
     * LOAD domain. NB the ctx save/restore path is plain-PUT/plain-GET, OUTSIDE
     * the c47c1cc lockline tear fix's coverage. Volume-bounded. */
    { static int cw = -1;
      if (cw < 0) { cw = getenv("YZ_CTXWATCH") ? 1 : 0;
          if (cw) { fprintf(stderr, "[ctxw] dma save/load watch armed\n"); fflush(stderr); } }
      if (cw) {
          extern uint32_t g_yz_ctxw_ea[32], g_yz_ctxw_len[32];
          extern volatile int g_yz_ctxw_n;
          uint32_t cea = (uint32_t)(ea & ~0x80000000ull);
          int catomic = (cmd == MFC_GETLLAR_CMD || cmd == MFC_PUTLLC_CMD ||
                         cmd == MFC_PUTLLUC_CMD || cmd == MFC_PUTQLLUC_CMD);
          uint32_t csz = catomic ? 128u : size;
          uint32_t cbase = catomic ? (cea & ~127u) : cea;
          int cn2 = g_yz_ctxw_n;
          for (int i = 0; i < cn2 && i < 32; i++) {
              if (cbase >= g_yz_ctxw_ea[i] + g_yz_ctxw_len[i]
                  || cbase + csz <= g_yz_ctxw_ea[i]) continue;
              static unsigned long cwn = 0; cwn++;
              if (cwn <= 3000 || (cwn & 0xFFu) == 0) {
                  int cput = mfc_is_put(cmd) || cmd == MFC_PUTLLC_CMD ||
                             cmd == MFC_PUTLLUC_CMD || cmd == MFC_PUTQLLUC_CMD;
                  uint32_t hb = csz < 0x380u ? csz : 0x380u;
                  const uint8_t* hp = cput ? spu->ls + (lsa & SPU_LS_MASK)
                                           : vm_base + cbase;
                  uint32_t h = 2166136261u;
                  for (uint32_t k = 0; k < hb; k++) { h ^= hp[k]; h *= 16777619u; }
                  /* s32 flavor-A hunt: the fatal POLL reads the kernel-ctx
                   * vector via LS 0x2FB0 (= offset +0x330 in the 0x380
                   * regblock at 0x2C80). Ride the slot's word on every
                   * regblock-window SAVE/LOAD so the FIRST zeroed sighting
                   * names where the poison enters (save-side = clobbered
                   * while live; load-side-only = ctxsave written wrong). */
                  uint32_t v2fb0 = 0;
                  if (hb >= 0x334u)
                      v2fb0 = ((uint32_t)hp[0x330]<<24)|((uint32_t)hp[0x331]<<16)
                            |((uint32_t)hp[0x332]<<8)|hp[0x333];
                  fprintf(stderr, "[ctxw] %s n=%lu spu=%X img=%d pc=0x%05X cmd=0x%02X "
                          "lsa=0x%05X ea=0x%08X size=0x%X blk=0x%08X h=%08X "
                          "head=%02X%02X%02X%02X k2FB0=%08X\n",
                          cput ? "SAVE" : "LOAD", cwn, spu->spu_id, spu->image_id,
                          spu->pc & SPU_LS_MASK, cmd, lsa & SPU_LS_MASK, cea, size,
                          g_yz_ctxw_ea[i], h, hp[0], hp[1], hp[2], hp[3], v2fb0);
                  fflush(stderr);
              }
              break;
          }
      } }
    /* Overlay + job-consumer watch (env YZ_OVL, 2026-07-03, diag — the entry-7
     * gate): (A) log code-sized GETs into HIGH LS (>=0x10000) per image — the
     * image-5 runtime overlay load names its SOURCE EA + size (needed both to
     * dump/lift the blob and for the reverse image-switch registration); for
     * image 5 also dump the source bytes to scratch\ovl_<ea>.bin. (B) log any
     * GET-class/atomic read of the published shader-stream job block
     * [0x40197100,0x40197400) — whoever reads it is the consumer (leading
     * suspect: image 5, dead at the overlay wall). REMOVE when the entry-7
     * frontier closes. */
    { static int ov = -1; if (ov < 0) ov = getenv("YZ_OVL") ? 1 : 0;
      if (ov) {
          uint32_t oea = (uint32_t)(ea & ~0x80000000ull);
          /* (A) overlay-class loads: per-image print cap so early gs_task
           * data GETs can't exhaust the budget before image 5 dispatches */
          /* image 13 = the JOB module: log ALL its code-sized GETs anywhere
           * past its own end (0x4880) -- job BINARIES load per-descriptor at
           * runtime (the third runtime-loaded-SPU-code instance after the
           * exit blob + the job module itself); their source EA + LS base is
           * what a lift needs. */
          if (spu->image_id == 13 && mfc_is_get(cmd) && cmd != MFC_GETLLAR_CMD
                  && (lsa & SPU_LS_MASK) >= 0x4880u && size >= 0x100u) {
              static int jbn = 0;
              if (jbn < 60) { jbn++;
                  fprintf(stderr, "[job-bin] spu=%X pc=0x%05X cmd=0x%02X lsa=0x%05X ea=0x%08X size=0x%X%s\n",
                          spu->spu_id, spu->pc & SPU_LS_MASK, cmd,
                          lsa & SPU_LS_MASK, oea, size, mfc_is_list(cmd) ? " LIST" : "");
                  fflush(stderr);
              }
          }
          /* (D) job-world DMA I/O pattern (2026-07-03 s7): every DMA issued
           * while a jobchain image (13/14/15) is active -- shows whether the
           * job's INPUT ever loads and whether any OUTPUT PUT is issued at all
           * (measured: all job output buffers stay zero across every batch).
           * pc >= 0x4C00 = job code, pc < 0x4880 = module code (image_id can
           * be stale across C-returns; pc is the discriminator). REMOVE with
           * the frontier. */
          if (spu->image_id >= 13 && spu->image_id <= 15
                  && cmd != MFC_GETLLAR_CMD && cmd != MFC_PUTLLC_CMD && cmd != MFC_PUTLLUC_CMD) {
              static int jio = 0;
              if (jio < 240) { jio++;
                  fprintf(stderr, "[job-io] spu=%X img=%d pc=0x%05X %s%s ea=0x%08X lsa=0x%05X size=0x%X\n",
                          spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK,
                          mfc_is_get(cmd) ? "GET" : "PUT", mfc_is_list(cmd) ? "L" : "",
                          oea, lsa & SPU_LS_MASK, size);
                  fflush(stderr);
              }
          }
          /* (C) jobchain COMMAND/DESCRIPTOR fetches (2026-07-03 s7, the parked-
           * SYNC frontier): fetches (incl. GETLLAR -- the interpreter reads
           * the command line atomically) from the command stream
           * [0x4019CA80,0x4019CD00) and the descriptor arena
           * [0x401AC000,0x401AE000). CHANGE-TRIGGERED, not capped: the parked
           * module polls the same line forever -- print only when the fetched
           * value differs from the last print for that ea (8-slot cache), so
           * the log shows every DISTINCT command the module decodes across the
           * whole boot. REMOVE with the jobchain frontier. */
          if (mfc_is_get(cmd) && size <= 0x100u
                  && ((oea >= 0x4019CA80u && oea < 0x4019CD00u)
                      || (oea >= 0x401AC000u && oea < 0x401AE000u))) {
              static uint32_t jc_ea[8]; static uint64_t jc_val[8]; static int jc_n = 0;
              const uint8_t* s = vm_base + oea;
              uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | s[i];
              int slot = -1;
              for (int i = 0; i < 8; i++) if (jc_ea[i] == oea) { slot = i; break; }
              if (slot < 0) { slot = jc_n & 7; jc_n++; jc_ea[slot] = oea; jc_val[slot] = ~v; }
              if (jc_val[slot] != v) {
                  jc_val[slot] = v;
                  static int jcn = 0;
                  if (jcn < 400) { jcn++;
                      fprintf(stderr, "[job-cmd] spu=%X img=%d pc=0x%05X ea=0x%08X size=0x%X cmd=0x%02X val=%016llX\n",
                              spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK,
                              oea, size, cmd, (unsigned long long)v);
                      fflush(stderr);
                  }
              } else {
                  /* s21: SAME-VALUE refetches were invisible (change-triggered
                   * dedup) -- ambiguous between "module never dispatched" and
                   * "module polls an unchanged command word". Log every 64th
                   * same-value refetch per slot so a live poll shows up. */
                  static uint32_t jc_re[8];
                  if ((++jc_re[slot] & 63u) == 1u) {
                      fprintf(stderr, "[job-cmd-re] spu=%X img=%d pc=0x%05X ea=0x%08X refetch#%u val=%016llX (unchanged)\n",
                              spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK,
                              oea, jc_re[slot], (unsigned long long)v);
                      fflush(stderr);
                  }
              }
          }
          if (mfc_is_get(cmd) && cmd != MFC_GETLLAR_CMD
                  && (lsa & SPU_LS_MASK) >= 0x10000u && size >= 0x200u) {
              static int ovn[16] = {0};
              int ii = (spu->image_id >= 0 && spu->image_id < 16) ? spu->image_id : 15;
              /* the taskset exit-blob always lands at exactly LS 0x10000
               * (RPCS3 spursTasksetOnTaskExit :1673 memcpy dst) — log those
               * UNCAPPED and dump the source bytes; everything else per-image
               * capped context */
              int exitblob = ((lsa & SPU_LS_MASK) == 0x10000u);
              if (exitblob || ovn[ii] < 40) { if (!exitblob) ovn[ii]++;
                  fprintf(stderr, "[ovl] spu=%X img=%d pc=0x%05X cmd=0x%02X lsa=0x%05X ea=0x%08X size=0x%X%s%s\n",
                          spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK, cmd,
                          lsa & SPU_LS_MASK, oea, size,
                          mfc_is_list(cmd) ? " LIST" : "",
                          exitblob ? " EXITBLOB" : "");
                  fflush(stderr);
              }
              if (exitblob && !mfc_is_list(cmd)) {
                  static int dumps = 0;
                  if (dumps < 16) { dumps++;
                      char fn[128];
                      snprintf(fn, sizeof fn, "scratch\\ovl_%08X_%05X.bin", oea, lsa & SPU_LS_MASK);
                      FILE* f = fopen(fn, "wb");
                      if (f) { fwrite(vm_base + oea, 1, size, f); fclose(f); }
                  }
              }
          }
          /* (B) job-block reads (0x40197180 job published at freeze time) */
          uint32_t oline = oea & ~127u;
          uint32_t osz = (cmd == MFC_GETLLAR_CMD) ? 128u : size;
          if ((mfc_is_get(cmd) || cmd == MFC_GETLLAR_CMD)
                  && oea < 0x40197400u && oea + osz > 0x40197100u) {
              static unsigned long on = 0; on++;
              if (on <= 80 || (on & 0x3FFu) == 0) {
                  fprintf(stderr, "[job-rd] n=%lu spu=%X img=%d pc=0x%05X cmd=0x%02X lsa=0x%05X ea=0x%08X size=0x%X\n",
                          on, spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK, cmd,
                          lsa & SPU_LS_MASK, oea, size);
                  fflush(stderr);
              }
          }
          /* (B2) job-block WRITES (2026-07-03 s8, same YZ_OVL gate — REMOVE with the
           * pxd frontier): every SPU PUT/PUTLLC touching the job block names who
           * writes done/result/state. The failing cycle shows done=size appearing
           * with NO result record — if no [job-wr] lands at that instant, the
           * writer was the PPU, not the task. First 16 bytes of the payload
           * inline (the done word rides +0x04). */
          if ((mfc_is_put(cmd) || cmd == MFC_PUTLLC_CMD || cmd == MFC_PUTLLUC_CMD)
                  && oea < 0x40197400u && oea + osz > 0x40197100u) {
              static unsigned long wn = 0; wn++;
              if (wn <= 200 || (wn & 0x3FFu) == 0) {
                  const uint8_t* pl = spu->ls + (lsa & SPU_LS_MASK & ~15u);
                  fprintf(stderr, "[job-wr] n=%lu spu=%X img=%d pc=0x%05X cmd=0x%02X ea=0x%08X size=0x%X\n"
                          "        pay+00=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n"
                          "        pay+10=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n"
                          "        pay+20=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                          wn, spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK, cmd, oea, size,
                          pl[0],pl[1],pl[2],pl[3], pl[4],pl[5],pl[6],pl[7],
                          pl[8],pl[9],pl[10],pl[11], pl[12],pl[13],pl[14],pl[15],
                          pl[16],pl[17],pl[18],pl[19], pl[20],pl[21],pl[22],pl[23],
                          pl[24],pl[25],pl[26],pl[27], pl[28],pl[29],pl[30],pl[31],
                          pl[32],pl[33],pl[34],pl[35], pl[36],pl[37],pl[38],pl[39],
                          pl[40],pl[41],pl[42],pl[43], pl[44],pl[45],pl[46],pl[47]);
                  fflush(stderr);
              }
          }
          (void)oline;
      } }
    /* Task context-save provenance (env YZ_CTXSAVE_WATCH, 2026-07-02): the SPURS
     * taskset yield must PUT the register block LS [0x2C80,0x3000) to the
     * taskInfo context-save EA (RPCS3 spursTasketSaveTaskContext); our resumes
     * read ZEROS there. Log every OUT-OF-LS transfer from that block (who saves,
     * where to) + any transfer touching it. Diag-only; REMOVE when settled. */
    { static int cw = -1; if (cw < 0) cw = getenv("YZ_CTXSAVE_WATCH") ? 1 : 0;
      if (cw && cmd != MFC_GETLLAR_CMD && cmd != MFC_PUTLLC_CMD
             && cmd != MFC_PUTLLUC_CMD && cmd != MFC_PUTQLLUC_CMD) {
          /* atomics excluded: the kernel lock-line (lsa 0x2D80) spams the window */
          uint32_t l0 = lsa & SPU_LS_MASK, l1 = l0 + size;
          if (l0 < 0x3000u && l1 > 0x2C80u) {
              static int cwn = 0;
              if (cwn < 200) { cwn++;
                  fprintf(stderr, "[ctxsave] spu=%X cmd=0x%02X lsa=0x%05X ea=0x%08llX size=0x%X\n",
                          spu->spu_id, cmd, l0, (unsigned long long)ea, size);
                  fflush(stderr);
              }
          }
      } }
    if (YZ_SPU_PROF_ACTIVE() && cmd == MFC_GET_CMD          /* regular GET only (NOT GETLLAR 0xD0) */
            && ((((lsa & SPU_LS_MASK) >= 0x2780u) && ((lsa & SPU_LS_MASK) < 0xC000u))
                || ((uint32_t)ea >= 0x01200000u && (uint32_t)ea < 0x01300000u))) {
        /* Arm the dispatch-tail hop trace (spu_channels.c spu_prof_hop) on the
         * gs_task ELF read, so we can watch where the policy diverges from StartTask. */
        { extern int g_spu_dtrace_spu;
          if ((uint32_t)ea >= 0x01200000u && (uint32_t)ea < 0x01300000u)
              g_spu_dtrace_spu = (int)spu->spu_id; }
        static int gl = 0;
        if (gl < 200) { gl++;
            fprintf(stderr, "[gst-dma] spu=%X cmd=0x%02X lsa=0x%05X ea=0x%08X size=0x%X\n",
                    spu->spu_id, cmd, lsa & SPU_LS_MASK, (uint32_t)ea, size);
            fflush(stderr);
        }
    }
#endif

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
         * keep bit 31. Verified live: base reads 0x40197C80, masked EA hits the
         * real instance and the SPURS CAS handshake then advances. */
        ea &= ~0x80000000ull;
        /* DIAG (2026-07-03, the post-gate crash): a lock-line atomic against the
         * NULL page is a fatal host AV in the unguarded 128-byte copies below.
         * Print the issuer's identity BEFORE the fault so the crash names its
         * own root (who computed a zero EA), then proceed -- the fault itself
         * stays visible; don't mask the symptom. */
#if !defined(YZ_PERF_CLEAN)
        if ((uint32_t)(ea & ~127ull) < 0x10000u) {
            static int nn = 0;
            if (nn < 8) { nn++;
                fprintf(stderr, "[dma-null] ATOMIC cmd=0x%02X spu=%X img=%d pc=0x%05X lsa=0x%05X ea=0x%08llX gpr0=%08X gpr1=%08X jobbase=%05X/%05X\n",
                        cmd, spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK,
                        lsa & SPU_LS_MASK, (unsigned long long)ea,
                        spu->gpr[0]._u32[0], spu->gpr[1]._u32[0],
                        spu->job_bin_base[0], spu->job_bin_base[1]);
                /* The known instance (gs_task journal cursor, pc 0x638C): the
                 * cursor EA is loaded from the work object at *(r3+0x10) --
                 * dump the object and the taskInfo quads IN this print (the
                 * unguarded memcpy AVs right after; nothing later runs). */
                {
                    uint32_t r3 = spu->gpr[3]._u32[0] & SPU_LS_MASK;
                    fprintf(stderr, "[dma-null]   gpr2=%08X gpr3=%08X gpr4=%08X gpr5=%08X\n",
                            spu->gpr[2]._u32[0], spu->gpr[3]._u32[0],
                            spu->gpr[4]._u32[0], spu->gpr[5]._u32[0]);
                    /* r80/r81 = the SAVED object pointers through the gs_task
                     * flush chain (0x7B08 ori r80,r3); zero here with a live
                     * chain object in memory = the ctx-restore lost the high
                     * nonvolatile registers on a yield resume. */
                    fprintf(stderr, "[dma-null]   gpr80=%08X gpr81=%08X gpr82=%08X gpr126=%08X gpr127=%08X\n",
                            spu->gpr[80]._u32[0], spu->gpr[81]._u32[0],
                            spu->gpr[82]._u32[0], spu->gpr[126]._u32[0], spu->gpr[127]._u32[0]);
                    for (int q = 0; q < 4; q++) {
                        const uint8_t* p = &spu->ls[(r3 + q*16) & SPU_LS_MASK];
                        fprintf(stderr, "[dma-null]   obj+0x%02X:", q*16);
                        for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", p[i]);
                        fprintf(stderr, "\n");
                    }
                    const uint8_t* ti = &spu->ls[0x2780u];
                    fprintf(stderr, "[dma-null]   taskInfo 0x2780:");
                    for (int i = 0; i < 32; i++) fprintf(stderr, " %02X", ti[i]);
                    fprintf(stderr, "\n");
                }
                fflush(stderr);
            }
        }
#endif
        /* ---- s31 §13 GUARD (ledger #74; scratch/s31_t1_crash.md): COMPLETE the
         * null-page atomic instead of letting the unguarded 128-byte copies AV
         * the whole process. The 2026-07-03 diag above chose "print then
         * proceed -- don't mask the symptom"; that was right for the diagnosis
         * era, and it is now measured to be THE recurring fatal "t1 crash"
         * (s31roll3/cure3: 0xC0000005 reading guest 0x00000000 in
         * spu_channels.c-obj statics, misattributed to tid=1 by the TLS
         * fallback -- SPU threads never set s_cur_tid -- with tramp_idx=0 as
         * the documented tell, main.cpp:758; the [crash-t1] dump is the HEALTHY
         * main thread). On the console EA 0 is mapped kernel memory: an SPU
         * lock-line atomic there completes without halting the machine; the
         * fatal host AV is purely our uncommitted-page artifact. Model the
         * conservative completion: GETLLAR reads zeros (no reservation kept),
         * PUTLLC fails (no valid reservation -- the faithful result), PUTLLUC/
         * PUTQLLUC are dropped. The [dma-null] print above still fires first
         * (the attribution signal is preserved; the null-EA PRODUCER -- the
         * yield-resume register-loss class the July-03 note names -- remains a
         * tracked bug, ledger #74). Kill-switch: YZ_NO_DMAGUARD=1 restores the
         * fatal proceed. */
        if ((uint32_t)(ea & ~127ull) < 0x10000u) {
            const int ng = g_yz_runtime_config.no_dmaguard;
            if (!ng) {
                /* s46 REVISION (scratch/s46_1c0_writers.py + the s46 section of
                 * scratch/s44_selfdiff_verdict.md): the conservative
                 * PUTLLC=always-FAIL completion turned a guest CAS retry loop
                 * at EA 0 into a PERMANENT hang -- spray shrapnel (guest pcs
                 * 0x32B0/0x32BC) zeroed the kernel's select-EA staging quad at
                 * LS 0x1C0, the next select CASed EA 0, and the forced failure
                 * spun it for 1.94M iterations until boot end (park 0x8A24,
                 * consumer SPU lost). On the console EA 0 is ordinary mapped
                 * memory: the CAS completes and the loop terminates. Model
                 * that: lockline ops against guest [0,0x10000) complete
                 * against a zero-init host backing store. The backing is not
                 * arbitrated across SPU threads (nothing legitimate lives
                 * there; stray CAS traffic only needs TERMINATION, not
                 * cross-thread coherency). Kill-switch YZ_NULLPAGE_FAIL=1
                 * restores the old always-fail completion; YZ_NO_DMAGUARD=1
                 * still restores the fatal proceed. */
                static uint8_t s_lowmem[0x10000];
                const int npf = g_yz_runtime_config.nullpage_fail;
                { uint32_t loff = (uint32_t)ea & 0xFFFFu & ~127u;
                  uint8_t* lls = &spu->ls[lsa & SPU_LS_MASK & ~127u];
#if !defined(YZ_PERF_CLEAN)
                  static unsigned long gn = 0; gn++;
                  if (gn <= 16 || (gn & 63u) == 0) {
                      fprintf(stderr, "[dma-guard] n=%lu spu=%X img=%d pc=0x%05X cmd=0x%02X "
                              "ea=0x%08llX -> %s\n",
                              gn, spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK,
                              cmd, (unsigned long long)ea,
                              npf ? "completed benignly (GETLLAR=zeros, PUTLLC=fail)"
                                  : "completed vs low-mem backing (CAS terminates)");
                      fflush(stderr);
                  }
#endif
                  if (cmd == MFC_GETLLAR_CMD) {
                      if (npf) {
                          memset(lls, 0, 128);
                          mfc->resv_active = 0;
                      } else {
                          memcpy(lls, s_lowmem + loff, 128);
                          memcpy(mfc->resv_data, s_lowmem + loff, 128);
                          mfc->resv_active = 1;
                          mfc->resv_ea = (ea & ~127ull);
                      }
                      mfc_publish_atomic_status(mfc, MFC_GETLLAR_SUCCESS);
                  } else if (cmd == MFC_PUTLLC_CMD) {
                      if (!npf && mfc->resv_active && mfc->resv_ea == (ea & ~127ull)
                          && memcmp(s_lowmem + loff, mfc->resv_data, 128) == 0) {
                          memcpy(s_lowmem + loff, lls, 128);
                          mfc->resv_active = 0;
                          mfc_publish_atomic_status(mfc, MFC_PUTLLC_SUCCESS);
                      } else {
                          mfc_publish_atomic_status(mfc, MFC_PUTLLC_FAILURE);
                      }
                  } else {
                      if (!npf) memcpy(s_lowmem + loff, lls, 128);
                      mfc_publish_atomic_status(mfc, MFC_PUTLLUC_SUCCESS);
                  }
                }
                if (tagged) mfc_tag_finish(mfc, spu, tag);
                return 0;
            }
        }
#if !defined(YZ_PERF_CLEAN)
        /* jobchain header-CAS watch (2026-07-03 s7, parked-SYNC frontier;
         * YZ_OVL, REMOVE with the frontier): every PUTLLC commit on the
         * CellSpursJobChain line 0x4019C880 -- pc + the +0x20..0x2F bytes
         * (the submit/complete counters suspected 4-apart at the park). */
        { static int jh = -1; if (jh < 0) jh = getenv("YZ_OVL") ? 1 : 0;
          if (jh && cmd == MFC_PUTLLC_CMD && ((uint32_t)ea & ~127u) == 0x4019C880u) {
              /* CHANGE-TRIGGERED: the idle grab/release cycle toggles only the
               * latch byte +0x29 -- mask it out and print only when pc or the
               * counter bytes actually change, so the trace spans the boot. */
              const uint8_t* l = &spu->ls[lsa & SPU_LS_MASK & ~127u];
              uint64_t pcw = 0; for (int i = 0; i < 8; i++) pcw = (pcw << 8) | l[i];
              uint64_t cw = 0; for (int i = 0x20; i < 0x30; i++)
                  if (i != 0x29) cw = (cw << 8) ^ l[i];
              static uint64_t last_pc = ~0ull, last_cw = ~0ull;
              if (pcw != last_pc || cw != last_cw) {
                  last_pc = pcw; last_cw = cw;
                  static int jn = 0;
                  if (jn < 400) { jn++;
                      fprintf(stderr, "[job-cas] spu=%X img=%d pc=0x%05X pc0-7=%016llX +20=%02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X\n",
                              spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK,
                              (unsigned long long)pcw,
                              l[0x20],l[0x21],l[0x22],l[0x23],l[0x24],l[0x25],l[0x26],l[0x27],
                              l[0x28],l[0x29],l[0x2A],l[0x2B],l[0x2C],l[0x2D],l[0x2E],l[0x2F]);
                      fflush(stderr);
                  }
              }
          } }
#endif
        uint8_t* line = vm_base + ((uint32_t)ea & ~127u);
        uint8_t* ls_ptr = &spu->ls[lsa & SPU_LS_MASK & ~127u];
        /* s21 SPEED (workpackage item 3; profiled 2026-07-09: five SPURS kernels
         * x ~a full core each spinning THROUGH the process-wide lock-line lock =
         * the boot's dominant CPU cost, scratch/asset_window_profile.md).
         * LOCK-FREE GETLLAR IDLE POLL: when this context re-GETLLARs the SAME
         * line and the line's write GENERATION has not moved, the cached
         * reservation copy still equals memory -- every writer bumps the
         * generation (PPU VM_WRITE_COH/stwcx/stdcx always did; SPU PUTLLC
         * commits and plain PUTs into reserved lines bump as of this change) --
         * so serve the cached copy without the lock. Reaction latency to a real
         * write stays one generation-check deep; LRWAKE edge semantics are
         * identical (cached copy == memory while the generation holds). A peer's
         * notify bumps the generation BEFORE clearing resv_active, so a stale
         * resv_active read here still fails the generation check. Kill-switch
         * YZ_NO_LLFAST restores the locked path for A/B. */
        if (cmd == MFC_GETLLAR_CMD) {
            const int llfast = !g_yz_runtime_config.no_llfast;
            uint64_t le = ea & ~127ull;
            extern uint32_t spu_coh_gen(uint32_t);
            /* WHITELIST (s21 boot-14 regression): host-side bulk writers (HLE
             * _sys_memset/_sys_memcpy, file reads) write guest memory WITHOUT
             * bumping the generation, so a general cached-serve can go stale
             * (boot 14: zero audio rounds -- t1's flywheel start lost). Until
             * those paths sweep the coherence bitmap, serve cache-fast ONLY for
             * the SPURS management line 0x40197C80 -- the measured hot poll
             * (5 kernels spinning; scratch/asset_window_profile.md) whose
             * writers are all CAS/VM_WRITE_COH paths (the one _sys_memset of a
             * control line, guard 0x4019C700 at init, predates any kernel
             * reservation). */
            if (llfast && le == 0x40197C80ull && mfc->resv_active && mfc->resv_ea == le
                && spu_coh_gen((uint32_t)le) == mfc->resv_gen) {
#if !defined(YZ_PERF_CLEAN)
                extern unsigned long g_spu_getllar_n; extern uint32_t g_spu_getllar_ea;
                g_spu_getllar_n++; g_spu_getllar_ea = (uint32_t)le;
                /* s48 LS-CODE-WATCH (fast cached GETLLAR leg): a lock line
                 * staged inside lifted text would be self-mod-adjacent. */
                { extern volatile int g_yz_lscw_on;
                  extern int yz_lscw_arm(void);
                  extern void yz_lscw_check(spu_context*, uint32_t, uint32_t,
                                            const uint8_t*, unsigned long long, const char*);
                  int lcw = g_yz_lscw_on; if (lcw < 0) lcw = yz_lscw_arm();
                  if (lcw) yz_lscw_check(spu, (uint32_t)(ls_ptr - spu->ls), 128,
                                         mfc->resv_data, (unsigned long long)le, "getllar"); }
#endif
                memcpy(ls_ptr, mfc->resv_data, 128);
                mfc->resv_poll_n++;
                /* s42 YZ_SPU_LOCKSTEP (★REVIEW D/E): the fast cached GETLLAR leg is
                 * the hottest SPURS management poll (5 kernels spinning here per the
                 * s21 profile above) -- tick BEFORE the idle-yield backoff below, so a
                 * quantum-expiry token pass never races a Sleep that would hold the
                 * token through it. No-op when YZ_SPU_LOCKSTEP is unset. */
                if (g_yz_runtime_config.spu_lockstep)
                    yz_lockstep_tick(spu);
                {
                  const int nb = g_yz_runtime_config.no_spubackoff;
                  if (!nb && mfc->resv_poll_n > 16)
                      spu_idle_yield(mfc->resv_poll_n > (1u << 20) ? 2
                                   : mfc->resv_poll_n > 256       ? 1 : 0); }
                {
                  const int lw = !g_yz_runtime_config.no_lrwake;
                  if (lw && (uint32_t)le == 0x40197C80u
                      && (mfc->resv_data[0x70] | mfc->resv_data[0x71])) {
                      /* FIX 2 (2026-07-17): atomic_fetch_or_explicit, not `|=` -- see the SN-raise
                       * site above; same-thread self-raise, relaxed order. */
                      atomic_fetch_or_explicit(&spu->event_status, 0x400u, memory_order_relaxed); spu_ch_wake(spu); /* s39 */ } }
                mfc_publish_atomic_status(mfc, MFC_GETLLAR_SUCCESS);
                if (tagged) mfc_tag_finish(mfc, spu, tag);
                return 0;
            }
        }
        /* s42 YZ_SPU_LOCKSTEP (★REVIEW D, DEADLOCK FIX): tick the slow GETLLAR
         * leg BEFORE taking the process-wide lock-line spinlock. yz_lockstep_tick
         * can hand the run token to a peer SPU and BLOCK until it is regranted;
         * doing that while holding spu_lockline_lock() wedges the whole ring --
         * every peer that reaches spu_lockline_lock() then spins forever on the
         * held spinlock, so none can reach a yield site to hand the token back
         * (the measured s42ls* wedge: 5 SPUs up, then zero heartbeat / zero
         * watchdog / zero ch-block). The fast cached leg above already ticks
         * outside the lock. Guarded on GETLLAR so only that command ticks here,
         * preserving the prior in-switch semantics. No-op when unset. */
        if (cmd == MFC_GETLLAR_CMD &&
            g_yz_runtime_config.spu_lockstep)
            yz_lockstep_tick(spu);
        spu_lockline_lock();
        switch (cmd) {
        case MFC_GETLLAR_CMD: {
            extern void spu_coh_reserve(uint32_t);   /* mark this line for PPU-write coherence */
#if !defined(YZ_PERF_CLEAN)
            extern unsigned long g_spu_getllar_n; extern uint32_t g_spu_getllar_ea;
            g_spu_getllar_n++; g_spu_getllar_ea = (uint32_t)(ea & ~127ull);
            { extern void spu_llar_note(uint32_t); extern int g_spu_prof_on;
              if (YZ_SPU_PROF_ACTIVE()) spu_llar_note((uint32_t)(ea & ~127ull)); }
#endif
            spu_coh_reserve((uint32_t)(ea & ~127ull));
#if !defined(YZ_PERF_CLEAN)
            /* s48 LS-CODE-WATCH (locked GETLLAR leg): companion of the fast
             * cached leg's check above. */
            { extern volatile int g_yz_lscw_on;
              extern int yz_lscw_arm(void);
              extern void yz_lscw_check(spu_context*, uint32_t, uint32_t,
                                        const uint8_t*, unsigned long long, const char*);
              int lcw = g_yz_lscw_on; if (lcw < 0) lcw = yz_lscw_arm();
              if (lcw) yz_lscw_check(spu, (uint32_t)(ls_ptr - spu->ls), 128,
                                     (const uint8_t*)line, (unsigned long long)(ea & ~127ull),
                                     "getllar"); }
#endif
            memcpy(ls_ptr, line, 128);
            yz_tagread_repair_fetch(
                spu, (uint32_t)(ls_ptr - spu->ls),
                (unsigned long long)(ea & ~127ull), 128u);
#if !defined(YZ_PERF_CLEAN)
            /* s49 TAG-READ fetch map (GETLLAR leg -- the consumer's journal
             * window arrives here, pc 0x638C). Same helper as the plain-GET leg. */
            { extern volatile int g_yz_tr_on;
              extern volatile long g_yz_a010_stage_generation;
              extern int yz_tagread_arm(void);
              extern void yz_tr_fetch(spu_context*, uint32_t, unsigned long long, uint32_t);
              static int stage_map = -1;
              int trf = g_yz_tr_on; if (trf < 0) trf = yz_tagread_arm();
              if (stage_map < 0)
                  stage_map = getenv("YZ_A010_STAGE_KIND1") ? 1 : 0;
              if (trf || (stage_map && g_yz_a010_stage_generation != 0))
                  yz_tr_fetch(spu, (uint32_t)(ls_ptr - spu->ls),
                              (unsigned long long)(ea & ~127ull), 128u); }
#endif
            /* s45 RESUME-HOLD v3 consumption (DIAGNOSTIC LEVER, default OFF --
             * s46 verdict: the switch-replay theory refuted, terminal park
             * unchanged ON vs OFF; see yz_resume_hold_arm's comment in
             * spu_channels.c for the honest status). Armed in
             * yz_task_ctx_restore when a consumer resume finds walk-pending
             * construct items; scratch/s44_selfdiff_verdict.md. v2's blanket
             * empty-poll hold starved the pending walk's own dispatch trigger
             * (record processing) -- s45fix2: no spray, but the pending patch
             * never applied either (RSX parked on the unpatched hole). v3
             * SERIALIZES instead: while walk-pending construct items remain
             * (re-scanned live each poll) and the deadline holds, journal
             * windows CONTAINING new construct/arm records (line tag 0x05 or
             * 0x0D) read as EMPTY; all other records flow, so the engine keeps
             * dispatching and the pending walk completes on its own consistent
             * restored inputs, then the held records apply unchanged. The
             * reservation and true memory stay intact throughout. */
            { extern volatile long long g_yz_resume_hold;   /* ms deadline (v3) */
              extern uint64_t yz_host_clock_ms(void);
              if (g_yz_resume_hold != 0 && (volatile void*)spu == g_yz_consumer_ctx
                  && ea >= 0x41F00080ull && ea < 0x42100080ull) {
                  if ((long long)yz_host_clock_ms() >= g_yz_resume_hold) {
                      fprintf(stderr, "[resume-hold] DEADLINE release (pending may remain)\n");
                      fflush(stderr);
                      g_yz_resume_hold = 0;
                  } else {
                      int pend = 0;
                      for (uint32_t k = 0; k < 12; k++) {
                          uint32_t it = 0xBDD0u + 0x80u * k;
                          const uint8_t* p = &spu->ls[it];
                          uint32_t ptee = ((uint32_t)p[0xC]<<24)|((uint32_t)p[0xD]<<16)|((uint32_t)p[0xE]<<8)|p[0xF];
                          uint32_t vt   = ((uint32_t)p[0x10]<<24)|((uint32_t)p[0x11]<<16)|((uint32_t)p[0x12]<<8)|p[0x13];
                          uint32_t st   = ((uint32_t)p[0x14]<<24)|((uint32_t)p[0x15]<<16)|((uint32_t)p[0x16]<<8)|p[0x17];
                          if (ptee != 0 && vt == 0xB988u && (st == 4u || st == 5u)) pend++;
                      }
                      if (!pend) {
                          fprintf(stderr, "[resume-hold] DRAINED release (pending=0)\n");
                          fflush(stderr);
                          g_yz_resume_hold = 0;
                      } else {
                          int hazard = 0;
                          for (int li = 0; li < 4; li++) {
                              uint32_t tag2 = ((uint32_t)ls_ptr[li*32]<<24)|((uint32_t)ls_ptr[li*32+1]<<16)
                                            |((uint32_t)ls_ptr[li*32+2]<<8)|ls_ptr[li*32+3];
                              if (tag2 == 0x05u || tag2 == 0x0Du) hazard = 1;
                          }
                          if (hazard) {
                              memset(ls_ptr, 0, 128);
                              { static long hn = 0;
                                if (hn < 10) { hn++;
                                    fprintf(stderr, "[resume-hold] hazard window held ea=0x%08X (pend=%d)\n",
                                            (uint32_t)ea, pend);
                                    fflush(stderr); } }
                          }
                      }
                  }
              } }
#if !defined(YZ_PERF_CLEAN)
            /* THROWAWAY DIAG (env YZ_SIGCNT): NON-SAMPLED count of GETLLARs whose
             * snapshot of the SPURS mgmt line (0x40197C80) has ANY wklSignal1 bit set
             * (signal half @ +0x70..71). Decisive bisection: if this stays 0 the whole
             * run, the PPU's SendWorkloadSignal write is lost before any SPU GETLLAR sees
             * it (coherence bug). If >0, the SPU DOES see the signal -> the bug is the
             * downstream SELECT/dispatch, not coherence. */
            { static int sc = -1; if (sc < 0) sc = getenv("YZ_SIGCNT") ? 1 : 0;
              if (sc && (ea & ~127ull) == 0x40197C80ull) {
                  unsigned sig = ((unsigned)line[0x70] << 8) | line[0x71];
                  if (sig) { static unsigned long seen = 0; seen++;
                      static int n = 0; if (n < 80) { n++;
                          fprintf(stderr, "[sigseen] GETLLAR wklSignal1=0x%04X seen=%lu getllar_n=%lu\n",
                                  sig, seen, g_spu_getllar_n); fflush(stderr); } } } }
            /* DIAG (env YZ_TS_PEEK, 2026-07-03 the wid-0 policy fork — RETIRE with
             * the pxd-dispatch frontier): change-triggered snapshot of the pxd
             * taskset bitset line (0x40199D00) at every GETLLAR. Word0 of each
             * bitset (task 0 = bit 0x80000000): running@0x00 ready@0x10
             * pending_ready@0x20 enabled@0x30 signalled@0x40 waiting@0x50.
             * Late-window read: img 2 sees pend=0x80000000 yet never launches
             * => the policy's SELECT_TASK lift; bitsets stay all-zero while the
             * PPU create-CAS commits => lost write / reservation visibility. */
            { static int tp = -1; if (tp < 0) tp = getenv("YZ_TS_PEEK") ? 1 : 0;
              if (tp && (ea & ~127ull) == 0x40199D00ull) {
                  uint32_t w[6]; int k;
                  for (k = 0; k < 6; k++)
                      w[k] = ((uint32_t)line[k*0x10] << 24) | ((uint32_t)line[k*0x10+1] << 16)
                           | ((uint32_t)line[k*0x10+2] << 8) | line[k*0x10+3];
                  static uint32_t lw[6] = {0xEE,0xEE,0xEE,0xEE,0xEE,0xEE};
                  static int tn = 0;
                  if (memcmp(w, lw, sizeof w) != 0 && tn < 240) { tn++;
                      memcpy(lw, w, sizeof w);
                      fprintf(stderr, "[ts-peek] spu=%X img=%d pc=0x%05X run=%08X rdy=%08X "
                              "pend=%08X enb=%08X sig=%08X wait=%08X\n",
                              spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK,
                              w[0], w[1], w[2], w[3], w[4], w[5]);
                       fflush(stderr); } } }
#endif
            /* CONFIRMATION EXPERIMENT (env YZ_FRC): force wklReadyCount1[2]=1 (the
             * gs_task taskset, wid=2) in the SPURS mgmt line so the kernel select's
             * `readyCount > contention` gate passes. Proves whether readyCount is
             * the sole remaining block to dispatching wid=2 -> the taskset policy
             * module -> gs_task. Patches the source line too so it persists across
             * the kernel's PUTLLC. */
            { if (g_yz_runtime_config.frc &&
                  (ea & ~127ull) == 0x40197C80ull) {
                  line[0x02] = 1; ls_ptr[0x02] = 1;
              } }
            /* pt35 CONFIRMATION (env YZ_FRC3): bootstrap the cri_audio codec
             * workload (wid 3) by clearing its wklCurrentContention[3] (+0x23) and
             * pendingContention (+0x33) and asserting readyCount[3] (+0x03) DIRECTLY
             * in the kernel's GETLLAR'd line. wid3 is runnable + has priority but
             * cur=1==maxContention with no SPU holding wklLocContention[3], so it can
             * never get its FIRST selection. Injecting cur=0 here (reliably, unlike
             * the vblank force) should let the kernel select wid3 -> policy dispatch
             * -> StartTask(codec) -> spu_task_launch runs cri_audio. Proves the
             * contention bootstrap is the gate. */
            { if (g_yz_runtime_config.frc3 &&
                  (ea & ~127ull) == 0x40197C80ull) {
                  line[0x23] = 0; ls_ptr[0x23] = 0;   /* wklCurrentContention[3] = 0 */
                  line[0x33] = 0; ls_ptr[0x33] = 0;   /* wklPendingContention[3] = 0 */
                  line[0x03] = 1; ls_ptr[0x03] = 1;   /* wklReadyCount1[3]        = 1 */
              } }
            /* TASK 1 FORCING EXPERIMENT (env YZ_FORCE_WID0, 2026-07-04,
             * DIAGNOSTIC ONLY -- band-aid hygiene: env-gated, default-OFF,
             * kill-switched off by default, never the shipped fix). Mirrors
             * YZ_FRC's mechanism (direct mutation of the GETLLAR'd SPURS mgmt
             * line so the write survives the kernel's own PUTLLC/CAS) but
             * targets wid0 instead of wid2: force wklReadyCount1[0]
             * (line/ls +0x00) to 1 and set the wklSignal1 bit0 (line/ls +0x70
             * bit 0x8000) EVERY time the kernel GETLLARs the mgmt line, so
             * wid0's SELECT gate
             *   run && prio>0 && maxContention>realContention && (signal||readyCount>realContention)
             * (spu_dma.h sel0 above) is forced true continuously -- not a one-shot,
             * because the kernel's own PUTLLC can clear a consumed signal bit and
             * we need wid0 selectable across the whole boot to see whether its
             * workload ever runs and fires the port-17 heartbeat doorbell
             * (measured already set). Does NOT touch maxContention/priority/run
             * (those are measured already-set; only the missing OR-term is
             * supplied). Report MEASURED effect; if wid0 forcing doesn't move t1,
             * retest with YZ_FORCE_WID1 (below) instead -- whichever produces the
             * heartbeat is the confirmed producer. */
            { if (g_yz_runtime_config.force_wid0 &&
                  (ea & ~127ull) == 0x40197C80ull) {
                  line[0x00] = 1; ls_ptr[0x00] = 1;         /* wklReadyCount1[0] = 1 */
                  line[0x70] |= 0x80; ls_ptr[0x70] |= 0x80; /* wklSignal1 bit0 (0x8000) */
                  static int n = 0; if (n < 8) { n++;
                      fprintf(stderr, "[force-wid0] forced rc0=1 sig0 @mgmt 0x40197C80\n");
                      fflush(stderr); }
              } }
            /* Same mechanism, wid1 (the pxd jobchain taskset, main.cpp:86) --
             * fallback probe if YZ_FORCE_WID0 does not produce the port-17
             * heartbeat. wklReadyCount1[1]=line[0x01], wklSignal1 bit1=0x4000. */
            { if (g_yz_runtime_config.force_wid1 &&
                  (ea & ~127ull) == 0x40197C80ull) {
                  line[0x01] = 1; ls_ptr[0x01] = 1;         /* wklReadyCount1[1] = 1 */
                  line[0x70] |= 0x40; ls_ptr[0x70] |= 0x40; /* wklSignal1 bit1 (0x4000) */
                  static int n = 0; if (n < 8) { n++;
                      fprintf(stderr, "[force-wid1] forced rc1=1 sig1 @mgmt 0x40197C80\n");
                      fflush(stderr); }
              } }
            /* BOOTSTRAP (env YZ_CLEARRUN): gs_task (task 0) is stuck `running`
             * (taskset running@0x00 bit 0x80) but never launched, so SELECT_TASK
             * (readyButNotRunning = ready & ~running) can't re-select it and the
             * policy idle-polls forever. Clear its running bit on the taskset
             * bitset GETLLAR so SELECT_TASK re-selects it -> StartTask -> (the
             * kernel resume above completes the poll) -> bi savedContextLr=0x3050
             * launches gs_task. Decisive test of the task-level bootstrap. */
            { extern uint32_t g_yz_taskset_ea;
              if (g_yz_runtime_config.clearrun &&
                  g_yz_taskset_ea &&
                  (uint32_t)(ea & ~127ull) == g_yz_taskset_ea
                  && (line[0x00] & 0x80u)) {
                  line[0x00] &= ~0x80u; ls_ptr[0x00] &= ~0x80u;   /* clear running task 0 */
                  extern int g_spu_prof_on;
                  if (YZ_SPU_PROF_ACTIVE()) { static int n=0; if (n<8){n++;
                      fprintf(stderr, "[yz-clearrun] cleared gs_task running bit @taskset 0x%08X -> selectable\n", g_yz_taskset_ea);
                      fflush(stderr);} }
              } }
            /* pt35: SAME task-level bootstrap for the cri_audio codec (wid 3,
             * task 0). The codec task is enabled+ready (t0.elf=0x012B4980) but
             * stuck `running` (taskset running@0x00 bit 0x80000000), so SELECT_TASK
             * (ready & ~running) finds nothing -> the policy reaches StartTask with
             * elf=0 -> codec never dispatched. Clear its running bit so the policy
             * re-selects it -> StartTask(0x012B4980) -> spu_task_launch runs cri_audio.
             * Env YZ_CLEARRUN3 (separate so the codec can be tested alone). */
            { extern uint32_t g_yz_codec_taskset;
              if (g_yz_runtime_config.clearrun3 &&
                  g_yz_codec_taskset &&
                  (uint32_t)(ea & ~127ull) == g_yz_codec_taskset
                  && (line[0x00] & 0x80u)) {
                  line[0x00] &= ~0x80u; ls_ptr[0x00] &= ~0x80u;   /* clear running task 0 */
                  { static int n=0; if (n<8){n++;
                      fprintf(stderr, "[yz-clearrun3] cleared cri_audio running bit @taskset 0x%08X -> selectable\n", g_yz_codec_taskset);
                      fflush(stderr);} }
              } }
            memcpy(mfc->resv_data, line, 128);
            /* LR re-derivation (RPCS3 SPUThread.cpp): if the line's write-generation
             * advanced since THIS context last reserved it, a cross-processor write was
             * missed while we held no reservation -> raise SPU_EVENT_LR so the SPURS idle
             * kernel wakes. Closes the lost-wakeup that made codec dispatch a coin-flip. */
            { extern uint32_t spu_coh_gen(uint32_t);
              uint64_t le = ea & ~127ull; uint32_t g = spu_coh_gen((uint32_t)le);
              if (mfc->resv_ea == le && g != mfc->resv_gen) {
                  /* FIX 2 (2026-07-17): atomic_fetch_or_explicit, not `|=` -- see the SN-raise
                       * site above; same-thread self-raise, relaxed order. */
                      atomic_fetch_or_explicit(&spu->event_status, 0x400u, memory_order_relaxed); spu_ch_wake(spu); /* s39 */ }
              /* IDLE-POLL BACKOFF (2026-07-03; kill-switch YZ_NO_SPUBACKOFF).
               * The SPURS kernels poll their management lines with GETLLAR at
               * full host speed -- measured 5 SPU threads x ~97% of a core --
               * and EVERY poll takes the process-wide lock-line spinlock, so
               * the PPU threads' atomics contend against five spinning cores
               * and boot pacing collapses (the post-voice asset phase made
               * less progress in 900 s than a lucky run made in 300 s).
               * Faithful: polling continues; but when the SAME line is re-read
               * and its write-generation has not moved, arrange to yield the
               * host core with escalation AFTER releasing the process-wide
               * lock-line lock. Yielding here while still owning that lock
               * can starve every SPU/PPU reservation user behind a sleeping
               * poller. Any observed change (or a different line) resets the
               * ladder, so reaction latency to real writes stays one poll
               * deep. */
              if (mfc->resv_ea == le && g == mfc->resv_gen) {
                  mfc->resv_poll_n++;
                  const int nb = g_yz_runtime_config.no_spubackoff;
                  /* Rung 3 (real sleep) only after ~seconds of continuous
                   * idleness: at yield pacing 1M unchanged polls is a long
                   * quiet stretch. A 16k threshold measurably taxed boot
                   * pacing (each wake costs up to ~15 ms at default timer
                   * resolution, and the boot is a chain of idle-then-
                   * handshake moments). */
                  if (!nb && mfc->resv_poll_n > 16)
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
            /* LOST-WAKEUP FIX (2026-07-05; DEFAULT ON, kill-switch YZ_NO_LRWAKE). ROOT
             * (MEASURED, confirmed two independent ways): the SPURS idle
             * kernel backs off (host-yield) between GETLLAR polls of its management line;
             * the PPU-side wklSignal/readyCount SET that submits a workload does NOT raise
             * the backed-off SPU's SPU_EVENT_LR (that write bypasses the coherence
             * write-generation path), so the transient signal lands in a poll gap and the
             * kernel never re-enters selection -> the workload that throws the port-17
             * heartbeat (SPU LS 0x05D10, the wake t1's cellSpursEventFlagWait on
             * 0x4019C680 bit0 depends on) is never dispatched -> the boot livelocks. Both
             * disabling the backoff (YZ_NO_SPUBACKOFF, SPU polls hot and catches the
             * transient) and this edge-delivery make the boot progress into rendering
             * (~560 set_render_target, stable). This keeps the backoff's perf and delivers
             * the edge faithfully: on a GETLLAR of the mgmt line that still carries a
             * pending workload signal (wklSignal1 @ +0x70 != 0), raise LR so the kernel
             * re-enters selection + runs the system service (rebuilds wklRunnable1 ->
             * dispatches the workload). This is the HW edge our emulation dropped, not a
             * game-logic force; it self-limits (once dispatched the kernel is busy, not
             * idle-GETLLARing). */
            {
              const int lw = !g_yz_runtime_config.no_lrwake;
              if (lw && (uint32_t)(ea & ~127ull) == 0x40197C80u
                  && (line[0x70] | line[0x71])) {
                  /* FIX 2 (2026-07-17): atomic_fetch_or_explicit, not `|=` -- see the SN-raise
                       * site above; same-thread self-raise, relaxed order. */
                      atomic_fetch_or_explicit(&spu->event_status, 0x400u, memory_order_relaxed); spu_ch_wake(spu); /* s39 */ } }
#if !defined(YZ_PERF_CLEAN)
            /* THROWAWAY DIAG (env YZ_SIGW): does the SPU kernel's GETLLAR of the SPURS
             * mgmt line ever SEE a nonzero wklSignal1 (offset 0x70)? Unconditional (not
             * gated by the wid3-present sampler) so it can't miss the transient signal. */
            { static int sigw = -1; if (sigw < 0) sigw = getenv("YZ_SIGW") ? 1 : 0;
              if (sigw && (ea & ~127ull) == 0x40197C80ull) {
                  unsigned s1 = ((unsigned)line[0x70] << 8) | line[0x71];
                  unsigned s2 = ((unsigned)line[0x78] << 8) | line[0x79];
                  if (s1 || s2) { static int n = 0; if (n < 80) { n++;
                      fprintf(stderr, "[sig-seen] GETLLAR mgmt wklSignal1=0x%04X wklSignal2=0x%04X rc[0..3]=%02X%02X%02X%02X\n",
                              s1, s2, line[0], line[1], line[2], line[3]); fflush(stderr); } } } }
            /* DIAG (YZ_SPU_PROF): on a wklState1-line GETLLAR (mgmt+0x80), if this
             * SPU's own sysSrvMsgUpdateWorkload bit (line[0x3D]) is SET, the SPU is
             * about to process a workload-update kick -- log the state it sees. This
             * tells us whether the kicks reach the SPU with RUNNABLE state. */
            if (YZ_SPU_PROF_ACTIVE() && (ea & ~127ull) == 0x40197D00ull) {
                uint32_t spuNum = ((uint32_t)spu->ls[0x1C8]<<24)|((uint32_t)spu->ls[0x1C9]<<16)
                                | ((uint32_t)spu->ls[0x1CA]<<8)|spu->ls[0x1CB];
                if (line[0x3D] & (1u << (spuNum & 31))) {
                    extern unsigned long g_spu_kick_log;
                    if (g_spu_kick_log < 40) {
                        g_spu_kick_log++;
                        fprintf(stderr, "[spu-kick] spu=%X spuNum=%u sees msgUpd=0x%02X (bit set) "
                                "state[0..7]=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                                spu->spu_id, spuNum, line[0x3D],
                                line[0],line[1],line[2],line[3],line[4],line[5],line[6],line[7]);
                        fflush(stderr);
                    }
                }
            }
            /* DIAG (YZ_SPU_PROF): on a mgmt-line GETLLAR, dump the
             * SpursKernelContext (LS 0x100) selection gates. Tests whether
             * wklCurrentId==SYS_SERVICE(0x20) -- the clear-guard the polling
             * service needs (oracle cellSpursSpu.cpp:492). Fields per
             * cellSpurs.h SpursKernelContext: spuNum@0x1C8, wklCurrentId@0x1DC,
             * sysSrvInitialised@0x1EA, wklRunnable1@0x1EC. */
            /* pt35: gate the kernel-ctx dump on its OWN env (YZ_LS_DUMP), NOT
             * g_spu_prof_on -- the latter also enables the gs_task launch hop,
             * which diverges the boot away from the movie (where wid3 is created).
             * With YZ_LS_DUMP alone, the default boot reaches the movie + creates
             * the codec taskset, so we can read wid3's real kernel context. */
            static int ls_dump_on = -1;
            if (ls_dump_on < 0) ls_dump_on = (YZ_SPU_PROF_ACTIVE() || getenv("YZ_LS_DUMP")) ? 1 : 0;
            if (ls_dump_on && (ea & ~127ull) == 0x40197C80ull) {
                extern unsigned long g_spu_lsdump_n;
                const uint8_t* k0 = spu->ls;
                uint16_t wrun0 = (uint16_t)(((uint16_t)k0[0x1EC]<<8)|k0[0x1ED]);
                /* pt35: focus sampling on WID3 (the codec) once it appears in the
                 * GETLLAR'd mgmt line, so the cap isn't exhausted by early wid2
                 * traffic before the movie creates wid3. Fire when wid3 has any
                 * nonzero footprint (readyCount@0x03 / curCont@0x23 / pendCont@0x33
                 * / maxCont@0x53 in the line, or runnable in this SPU's ctxt), plus
                 * a slow baseline so we still see the pre-movie state. */
                int wid3_present = line[0x03] || line[0x23] || line[0x33] || line[0x53] || (wrun0 & 0x1000);
                /* THROWAWAY DIAG: also fire whenever ANY wklSignal1 bit is live in the
                 * snapshot, so we capture the full SELECT gate at the instant the PPU's
                 * signal is visible to the kernel (else the sparse sampler misses it). */
                int sig_set = (line[0x70] | line[0x71]) != 0;
                if ((wid3_present || sig_set || ((g_spu_getllar_n % 8000000UL) < 2)) && g_spu_lsdump_n < 800) {
                    g_spu_lsdump_n++;
                    const uint8_t* k = spu->ls;
                    uint32_t spuNum = ((uint32_t)k[0x1C8]<<24)|((uint32_t)k[0x1C9]<<16)|((uint32_t)k[0x1CA]<<8)|k[0x1CB];
                    uint32_t wclId  = ((uint32_t)k[0x1DC]<<24)|((uint32_t)k[0x1DD]<<16)|((uint32_t)k[0x1DE]<<8)|k[0x1DF];
                    uint16_t wrun1  = (uint16_t)(((uint16_t)k[0x1EC]<<8)|k[0x1ED]);
                    /* k[0x2D80..] = the local copy of the spurs wklState1 line
                     * (mgmt+0x80) from the LAST ProcessRequests GETLLAR. Dump the
                     * per-workload state bytes + the message byte the SPU sees, the
                     * spuNum bit it tests, and ctxt->priority[0..3] (LS 0x1A0) +
                     * spuIdling (0x1EB) so we can tell whether ActivateWorkload ran
                     * and why the wklRunnable rebuild yields 0. */
                    const uint8_t* w = k + 0x2D80;
                    /* Select gate for wid=2 (oracle cellSpursSpu.cpp:519-521), read
                     * straight from the GETLLAR'd mgmt line + kernel ctx:
                     *   runnable = wklRunnable1 & (0x8000>>2)=0x2000  (k@0x1EC)
                     *   priority = ctxt->priority[2] & 0x0F           (k@0x1A2)
                     *   maxCont  = wklMaxContention[2] & 0x0F         (line@0x52)
                     *   cont     = wklCurrentContention[2]            (line@0x22)
                     *   readyCnt = wklReadyCount1[2]                  (line@0x02) */
                    unsigned rc2 = line[0x02], curcont2 = line[0x22], maxc2 = line[0x52] & 0x0F;
                    /* REAL contention per oracle cellSpursSpu.cpp:467-479:
                     *   contention[i] = (wklCurrentContention[i] - wklLocContention[i]) & 0xF
                     *   for a POLL, if i != wklCurrentId, also add pendingContention[i].
                     * The old dump used RAW wklCurrentContention -> overstated the gate. */
                    unsigned loccont2 = k[0x182] & 0x0F;
                    unsigned realcont2 = (curcont2 - loccont2) & 0x0F;
                    unsigned pend2 = (unsigned)((line[0x32] - k[0x192]) & 0x0F);
                    if (wclId != 2) realcont2 = (realcont2 + pend2) & 0x0F;  /* poll: non-current adds pending */
                    unsigned sig2 = ((((unsigned)line[0x70] << 8) | line[0x71]) & (0x8000u >> 2)) ? 1 : 0;
                    unsigned prio2 = k[0x1A2] & 0x0F, run2 = (wrun1 & 0x2000) ? 1 : 0;
                    int sel = run2 && prio2 > 0 && maxc2 > realcont2 && (sig2 || rc2 > realcont2);
                    /* pt35: same select-gate for wid3 (the cri_audio codec taskset).
                     * offsets: rc@line[0x03] curCont@line[0x23] maxCont@line[0x53]
                     * sig@(line[0x70..71]&0x1000) | ctxt: run=wklRun1&0x1000
                     * prio=k[0x1A3] locCont=k[0x183]. */
                    unsigned rc3 = line[0x03], curcont3 = line[0x23], maxc3 = line[0x53] & 0x0F;
                    unsigned loccont3 = k[0x183] & 0x0F;
                    unsigned realcont3 = (curcont3 - loccont3) & 0x0F;
                    unsigned pend3 = (unsigned)((line[0x33] - k[0x193]) & 0x0F);
                    if (wclId != 3) realcont3 = (realcont3 + pend3) & 0x0F;
                    unsigned sig3 = ((((unsigned)line[0x70] << 8) | line[0x71]) & (0x8000u >> 3)) ? 1 : 0;
                    unsigned prio3 = k[0x1A3] & 0x0F, run3 = (wrun1 & 0x1000) ? 1 : 0;
                    int sel3 = run3 && prio3 > 0 && maxc3 > realcont3 && (sig3 || rc3 > realcont3);
                    /* TASK 1 (2026-07-04): SAME select-gate for wid0/wid1 -- the
                     * heartbeat-workload candidates. The existing probe above only
                     * ever computed wid2/wid3, so we were BLIND to why wid0/wid1
                     * (run-bits ARE set per wklRun1 0xC000->0xE000, measured) never
                     * get SELECTed. Offsets mirror wid2/wid3 exactly, shifted to
                     * bit/byte index 0 and 1:
                     *   wid0: rc@line[0x00] curCont@line[0x20] maxCont@line[0x50]
                     *         sig@(line[0x70..71]&0x8000) run@(wrun1&0x8000) prio@k[0x1A0] locCont@k[0x180]
                     *   wid1: rc@line[0x01] curCont@line[0x21] maxCont@line[0x51]
                     *         sig@(line[0x70..71]&0x4000) run@(wrun1&0x4000) prio@k[0x1A1] locCont@k[0x181]
                     * wid1 = the pxd jobchain taskset (main.cpp:86); wid0 = the
                     * remaining SPURS-managed workload cluster (candidate heartbeat
                     * owner per the 0x4019C680 = mgmt+0x4A00 finding). */
                    unsigned rc0 = line[0x00], curcont0 = line[0x20], maxc0 = line[0x50] & 0x0F;
                    unsigned loccont0 = k[0x180] & 0x0F;
                    unsigned realcont0 = (curcont0 - loccont0) & 0x0F;
                    unsigned pend0 = (unsigned)((line[0x30] - k[0x190]) & 0x0F);
                    if (wclId != 0) realcont0 = (realcont0 + pend0) & 0x0F;
                    unsigned sig0 = ((((unsigned)line[0x70] << 8) | line[0x71]) & (0x8000u >> 0)) ? 1 : 0;
                    unsigned prio0 = k[0x1A0] & 0x0F, run0 = (wrun1 & 0x8000) ? 1 : 0;
                    int sel0 = run0 && prio0 > 0 && maxc0 > realcont0 && (sig0 || rc0 > realcont0);
                    unsigned rc1 = line[0x01], curcont1 = line[0x21], maxc1 = line[0x51] & 0x0F;
                    unsigned loccont1 = k[0x181] & 0x0F;
                    unsigned realcont1 = (curcont1 - loccont1) & 0x0F;
                    unsigned pend1 = (unsigned)((line[0x31] - k[0x191]) & 0x0F);
                    if (wclId != 1) realcont1 = (realcont1 + pend1) & 0x0F;
                    unsigned sig1 = ((((unsigned)line[0x70] << 8) | line[0x71]) & (0x8000u >> 1)) ? 1 : 0;
                    unsigned prio1 = k[0x1A1] & 0x0F, run1 = (wrun1 & 0x4000) ? 1 : 0;
                    int sel1 = run1 && prio1 > 0 && maxc1 > realcont1 && (sig1 || rc1 > realcont1);
                    static int wid01_on = -1;
                    if (wid01_on < 0) wid01_on = (YZ_SPU_PROF_ACTIVE() || getenv("YZ_WID01")) ? 1 : 0;
                    fprintf(stderr, "[spu-ls] spu=%X n%u wklCur=0x%X wklRun1=0x%04X idle=%u "
                            "| state1[0..3]=%02X%02X%02X%02X msgUpd=0x%02X msg72=0x%02X "
                            "| wid2: run=%u prio=%u maxc=%u cont=%u(loc=%u real=%u) rc=%u sig=%u SELECT=%d "
                            "| wid3: run=%u prio=%u maxc=%u cont=%u(loc=%u real=%u) rc=%u sig=%u SELECT=%d\n",
                            spu->spu_id, spuNum, wclId, wrun1, k[0x1EB],
                            w[0],w[1],w[2],w[3], w[0x3D], line[0x72],
                            run2, prio2, maxc2, curcont2, loccont2, realcont2, rc2, sig2, sel,
                            run3, prio3, maxc3, curcont3, loccont3, realcont3, rc3, sig3, sel3);
                    if (wid01_on) {
                        fprintf(stderr, "[spu-ls01] spu=%X n%u wklCur=0x%X wklRun1=0x%04X "
                                "| wid0: run=%u prio=%u maxc=%u cont=%u(loc=%u real=%u) rc=%u sig=%u SELECT=%d "
                                "| wid1: run=%u prio=%u maxc=%u cont=%u(loc=%u real=%u) rc=%u sig=%u SELECT=%d\n",
                                spu->spu_id, spuNum, wclId, wrun1,
                                run0, prio0, maxc0, curcont0, loccont0, realcont0, rc0, sig0, sel0,
                                run1, prio1, maxc1, curcont1, loccont1, realcont1, rc1, sig1, sel1);
                    }
                    fflush(stderr);
                }
            }
            /* TASK 1 follow-up (2026-07-04): the [spu-ls01] print above shares
             * the wid2/wid3 probe's g_spu_lsdump_n<800 cap and its wid3-present/
             * sig-set trigger, both tuned for wid2/wid3 -- it starves out BEFORE
             * t1's actual event-flag wedge (measured: cap exhausted ~line 9354,
             * t1 parks at [evflag-wait] ~line 10933 of a 26k-line boot). Add an
             * INDEPENDENT, uncapped low-rate sampler so we see wid0/wid1's
             * SELECT gate in the STEADY STATE after t1 is already stuck --
             * that's the state that matters for the fix (does it ever change?). */
            if (ls_dump_on && (ea & ~127ull) == 0x40197C80ull) {
                static int wid01_on2 = -1;
                if (wid01_on2 < 0) wid01_on2 = (YZ_SPU_PROF_ACTIVE() || getenv("YZ_WID01")) ? 1 : 0;
                if (wid01_on2) {
                    extern unsigned long g_spu_getllar_n;
                    static unsigned long n01 = 0;
                    /* one sample per ~500k mgmt-line GETLLARs per SPU context (this
                     * line is polled at host speed by every idle SPURS kernel) --
                     * frequent enough to catch a late transition, rare enough not
                     * to flood a multi-minute boot. */
                    if ((g_spu_getllar_n % 500000UL) == 0) {
                        n01++;
                        const uint8_t* k = spu->ls;
                        uint32_t spuNum = ((uint32_t)k[0x1C8]<<24)|((uint32_t)k[0x1C9]<<16)|((uint32_t)k[0x1CA]<<8)|k[0x1CB];
                        uint16_t wrun1  = (uint16_t)(((uint16_t)k[0x1EC]<<8)|k[0x1ED]);
                        unsigned rc0 = line[0x00], curcont0 = line[0x20], maxc0 = line[0x50] & 0x0F;
                        unsigned loccont0 = k[0x180] & 0x0F;
                        unsigned realcont0 = (curcont0 - loccont0) & 0x0F;
                        unsigned sig0 = ((((unsigned)line[0x70] << 8) | line[0x71]) & 0x8000u) ? 1 : 0;
                        unsigned prio0 = k[0x1A0] & 0x0F, run0 = (wrun1 & 0x8000) ? 1 : 0;
                        int sel0 = run0 && prio0 > 0 && maxc0 > realcont0 && (sig0 || rc0 > realcont0);
                        unsigned rc1 = line[0x01], curcont1 = line[0x21], maxc1 = line[0x51] & 0x0F;
                        unsigned loccont1 = k[0x181] & 0x0F;
                        unsigned realcont1 = (curcont1 - loccont1) & 0x0F;
                        unsigned sig1 = ((((unsigned)line[0x70] << 8) | line[0x71]) & 0x4000u) ? 1 : 0;
                        unsigned prio1 = k[0x1A1] & 0x0F, run1 = (wrun1 & 0x4000) ? 1 : 0;
                        int sel1 = run1 && prio1 > 0 && maxc1 > realcont1 && (sig1 || rc1 > realcont1);
                        fprintf(stderr, "[spu-ls01-slow] #%lu spu=%X n%u wklRun1=0x%04X "
                                "| wid0: run=%u prio=%u maxc=%u real=%u rc=%u sig=%u SELECT=%d "
                                "| wid1: run=%u prio=%u maxc=%u real=%u rc=%u sig=%u SELECT=%d\n",
                                n01, spu->spu_id, spuNum, wrun1,
                                run0, prio0, maxc0, realcont0, rc0, sig0, sel0,
                                run1, prio1, maxc1, realcont1, rc1, sig1, sel1);
                        fflush(stderr);
                    }
                }
            }
            /* DIAG (env YZ_WID4, s22 2026-07-08): steady-state SELECT-gate sampler
             * for wid4 (the pxd GPU-frame pool, image 4 = the 0xFE0 decode-sync
             * publisher, DONT_RECHASE #29). INDEPENDENT of the ls_dump/prof gates
             * so it runs on a plain diagnostic boot. Offsets mirror
             * [spu-ls01-slow] shifted to index 4: rc@line[0x04] curCont@line[0x24]
             * maxCont@line[0x54] sig bit 0x0800 run bit 0x0800 prio@k[0x1A4]
             * locCont@k[0x184]. The sig field also answers whether the kernel
             * ever CONSUMES t1's wid4 raise (sig stuck 1 = never seen/acted;
             * sig 0 after a raise = consumed but not dispatched). Observation
             * only, no state mutation. */
            { static int w4 = -1;
              if (w4 < 0) { w4 = getenv("YZ_WID4") ? 1 : 0;
                  if (w4) { fprintf(stderr, "[spu-ls4] armed\n"); fflush(stderr); } }
              if (w4 && (ea & ~127ull) == 0x40197C80ull) {
                  extern unsigned long g_spu_getllar_n;
                  static unsigned long n4 = 0;
                  if ((g_spu_getllar_n % 500000UL) == 0) {
                      n4++;
                      const uint8_t* k = spu->ls;
                      uint32_t spuNum4 = ((uint32_t)k[0x1C8]<<24)|((uint32_t)k[0x1C9]<<16)|((uint32_t)k[0x1CA]<<8)|k[0x1CB];
                      uint16_t wrun1_4 = (uint16_t)(((uint16_t)k[0x1EC]<<8)|k[0x1ED]);
                      unsigned rc4 = line[0x04], curcont4 = line[0x24], maxc4 = line[0x54] & 0x0F;
                      unsigned loccont4 = k[0x184] & 0x0F;
                      unsigned realcont4 = (curcont4 - loccont4) & 0x0F;
                      unsigned sig4 = ((((unsigned)line[0x70] << 8) | line[0x71]) & 0x0800u) ? 1 : 0;
                      unsigned prio4 = k[0x1A4] & 0x0F, run4 = (wrun1_4 & 0x0800) ? 1 : 0;
                      int sel4 = run4 && prio4 > 0 && maxc4 > realcont4 && (sig4 || rc4 > realcont4);
                      fprintf(stderr, "[spu-ls4] #%lu spu=%X n%u wklRun1=0x%04X "
                              "| wid4: run=%u prio=%u maxc=%u cont=%u(loc=%u real=%u) rc=%u sig=%u SELECT=%d\n",
                              n4, spu->spu_id, spuNum4, wrun1_4,
                              run4, prio4, maxc4, curcont4, loccont4, realcont4, rc4, sig4, sel4);
                      fflush(stderr);
                  }
              } }
#endif
            break; }
        case MFC_PUTLLC_CMD:
#if !defined(YZ_PERF_CLEAN)
            /* s46 TRIPWIRE (resume-hold review change #4): the hold's safety
             * argument rests on the MEASURED invariant that nothing ever
             * PUTLLCs the journal-record arena (it is consumed read-only via
             * GETLLAR; s45cap1: 11,264 lockline commits, zero to 0x41F...).
             * If that invariant ever breaks, a hold-zeroed LS copy could
             * commit zeros over real records. Enforce it as a loud tripwire
             * so the failure is visible, not silent. */
            if (ea >= 0x41F00080ull && ea < 0x42100080ull) {
                static unsigned long jw = 0; jw++;
                if (jw <= 8) {
                    fprintf(stderr, "[resume-hold] TRIPWIRE: PUTLLC into journal arena "
                            "ea=0x%08llX spu=%X img=%d pc=0x%05X (read-only invariant broken "
                            "-- re-audit the hold's zeroed-window safety)\n",
                            (unsigned long long)ea, spu->spu_id, spu->image_id,
                            spu->pc & SPU_LS_MASK);
                    fflush(stderr);
                }
            }
            /* s24 (YZ_JOBPUT site 3 of 3): the atomic lockline executor — the
             * path the jobB flag CAS actually takes (site 1 saw zero of its
             * 23 measured commits). Same census, tagged. */
            { static int jp3 = -1; if (jp3 < 0) { jp3 = getenv("YZ_JOBPUT") ? 1 : 0;
                  if (jp3) { fprintf(stderr, "[job-put3] ARMED: PUTLLC lockline site live\n"); fflush(stderr); } }
              if (jp3 && spu->image_id >= 13 && spu->image_id <= 15) {
                  static unsigned long j3n = 0; j3n++;
                  if (j3n <= 200 || (j3n & 0xFFu) == 0) {
                      fprintf(stderr, "[job-put3] n=%lu spu=%X img=%d pc=0x%05X ea=0x%08llX\n",
                              j3n, spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK,
                              (unsigned long long)(ea & ~0x80000000ull));
                      fflush(stderr);
                  }
              } }
            if (YZ_SPU_PROF_ACTIVE() && (ea & ~127ull) == 0x40197C80ull) {
                extern unsigned long g_spu_mgmt_put_ok, g_spu_mgmt_put_fail;
                if (mfc->resv_active && mfc->resv_ea == (ea & ~127ull) &&
                    memcmp(line, mfc->resv_data, 128) == 0) g_spu_mgmt_put_ok++;
                else g_spu_mgmt_put_fail++;
            }
            /* DIAG (env YZ_FLAGCAS, 2026-07-08, uncapped): every SPU PUTLLC
             * ATTEMPT on the IWL event-flag line 0x4019C680 (the flag t1's
             * cellSpursEventFlagWait polls, bit 0x1). The jobchain's notify job
             * (jobB) is supposed to set the flag bit here each round; with the
             * dual-base dispatch fix jobB's real code runs but the flag never
             * sets — this distinguishes "jobB never attempts the write"
             * (early-exit on descriptor data -> PPU-side value hunt) from
             * "attempts it and the CAS fails/mis-writes" (our reservation or
             * lift layer). Logs attempts BEFORE the success test, with verdict. */
            { static int fc = -1;
              if (fc < 0) { fc = getenv("YZ_FLAGCAS") ? 1 : 0;
                  if (fc) { fprintf(stderr, "[flagcas] spu probe armed\n"); fflush(stderr); } }
              if (fc && (ea & ~127ull) == 0x4019C680ull) {
                  int ok = mfc->resv_active && mfc->resv_ea == (ea & ~127ull) &&
                           memcmp(line, mfc->resv_data, 128) == 0;
                  const uint8_t* b = mfc->resv_data; const uint8_t* a = ls_ptr;
                  fprintf(stderr, "[flagcas] spu=%X img=%d pc=0x%05X %s bits %02X%02X%02X%02X"
                          "%02X%02X%02X%02X->%02X%02X%02X%02X%02X%02X%02X%02X\n",
                          spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK,
                          ok ? "COMMIT" : "FAIL",
                          b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
                          a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]);
                  fflush(stderr);
              } }
            /* DIAG (env YZ_W4TS, SPU side, 2026-07-08 s22): every SPU PUTLLC on the
             * wid4 pool taskset bitset line 0x42450E00 — the pool tasks' own state
             * transitions (running/waiting/signalled, docs/SPURS_TASKSET.md) with
             * the first 0x60 bytes' diff summarized as changed offsets. Companion
             * to the PPU-side [w4ts-ppu] census in shims.cpp. Uncapped. */
            { static int w4s = -1;
              if (w4s < 0) { w4s = getenv("YZ_W4TS") ? 1 : 0;
                  if (w4s) { fprintf(stderr, "[w4ts] spu probe armed\n"); fflush(stderr); } }
              if (w4s && (ea & ~127ull) == 0x42450E00ull) {
                  int ok = mfc->resv_active && mfc->resv_ea == (ea & ~127ull) &&
                           memcmp(line, mfc->resv_data, 128) == 0;
                  const uint8_t* b = mfc->resv_data; const uint8_t* a = ls_ptr;
                  char diff[128]; int o = 0; diff[0] = 0;
                  for (int i = 0; i < 0x60 && o < 100; i += 4) {
                      if (memcmp(b + i, a + i, 4)) {
                          o += snprintf(diff + o, sizeof(diff) - o,
                                        " +%02X:%02X%02X%02X%02X->%02X%02X%02X%02X", i,
                                        b[i],b[i+1],b[i+2],b[i+3], a[i],a[i+1],a[i+2],a[i+3]);
                      }
                  }
                  fprintf(stderr, "[w4ts-spu] spu=%X img=%d pc=0x%05X %s%s\n",
                          spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK,
                          ok ? "COMMIT" : "FAIL", o ? diff : " (no bitset change)");
                  fflush(stderr);
              } }
#endif
            if (mfc->resv_active && mfc->resv_ea == (ea & ~127ull) &&
                memcmp(line, mfc->resv_data, 128) == 0) {
                const int made_progress =
                    memcmp(mfc->resv_data, ls_ptr, 128) != 0;
#if !defined(YZ_PERF_CLEAN)
                yz_a010_reltrace_spu_atomic(
                    spu->spu_id, (uint32_t)spu->image_id,
                    spu->pc & SPU_LS_MASK,
                    (uint32_t)(ea & ~127ull), ls_ptr, cmd);
#endif
#if !defined(YZ_PERF_CLEAN)
                /* DIAG (YZ_SPU_PROF): on a successful CAS of the SPURS mgmt
                 * line (0x40197C80), log mutations to the fields that gate
                 * workload selection -- sysSrvMessage@0x72 and wklReadyCount1
                 * [0..3]@0x00. Pins whether the SERVICE itself makes wid=2
                 * selectable, or whether that must come from the PPU side. */
                if (YZ_SPU_PROF_ACTIVE() && (ea & ~127ull) == 0x40197C80ull) {
                    extern unsigned long g_spu_putllc_log;
                    const uint8_t* b = mfc->resv_data; const uint8_t* a = ls_ptr;
                    int changed = (b[0x72] != a[0x72]) || (b[0] != a[0]) ||
                                  (b[1] != a[1]) || (b[2] != a[2]) || (b[3] != a[3]);
                    if (changed && g_spu_putllc_log < 80) {
                        g_spu_putllc_log++;
                        fprintf(stderr, "[spu-put] spu=%X mgmt CAS: sysSrvMsg %02X->%02X "
                                "readyCnt[0..3] %02X%02X%02X%02X->%02X%02X%02X%02X\n",
                                spu->spu_id, b[0x72], a[0x72],
                                b[0], b[1], b[2], b[3], a[0], a[1], a[2], a[3]);
                        fflush(stderr);
                    }
                }
                /* DIAG (YZ_SPU_PROF): the SPURS wklState1 line CAS (0x40197D00).
                 * Show how ProcessRequests/ActivateWorkload mutate the gating
                 * bytes: msgUpd(0x3D), wklState1[0..3]@0x00, wklStatus1[0..3]@0x10. */
                if (YZ_SPU_PROF_ACTIVE() && (ea & ~127ull) == 0x40197D00ull) {
                    extern unsigned long g_spu_putllc_log;
                    const uint8_t* b = mfc->resv_data; const uint8_t* a = ls_ptr;
                    int changed = memcmp(b, a, 128) != 0;
                    if (changed && g_spu_putllc_log < 120) {
                        g_spu_putllc_log++;
                        fprintf(stderr, "[spu-put80] spu=%X CAS: msgUpd(0x3D) %02X->%02X "
                                "state[0..3] %02X%02X%02X%02X->%02X%02X%02X%02X "
                                "status[0..3] %02X%02X%02X%02X->%02X%02X%02X%02X\n",
                                spu->spu_id, b[0x3D], a[0x3D],
                                b[0],b[1],b[2],b[3], a[0],a[1],a[2],a[3],
                                b[0x10],b[0x11],b[0x12],b[0x13], a[0x10],a[0x11],a[0x12],a[0x13]);
                        fflush(stderr);
                    }
                }
                /* DIAG (env YZ_JGUARD, 2026-07-08, uncapped): SPU-side commits to the
                 * CRI jobchain's CellSpursJobGuard line 0x4019C700 (ncount0 at +0,
                 * ncount1 at +4). cellSpursJobGuardNotify decrements ncount0 and the
                 * LAST notify re-runs the jobchain; measured only ONE notify per boot
                 * on the PPU side, so count SPU-side notifies here (call-path
                 * independent: this sees the CAS itself). */
                { static int jg = -1;
                  if (jg < 0) { jg = getenv("YZ_JGUARD") ? 1 : 0;
                      if (jg) { fprintf(stderr, "[jguard] spu probe armed\n"); fflush(stderr); } }
                  if (jg && (ea & ~127ull) == 0x4019C700ull) {
                      const uint8_t* b = mfc->resv_data; const uint8_t* a = ls_ptr;
                      fprintf(stderr, "[jguard-spu] spu=%X pc=0x%05X ncount0 %02X%02X%02X%02X->"
                              "%02X%02X%02X%02X ncount1=%02X%02X%02X%02X\n",
                              spu->spu_id, spu->pc,
                              b[0],b[1],b[2],b[3], a[0],a[1],a[2],a[3],
                              a[4],a[5],a[6],a[7]);
                      fflush(stderr);
                  } }
                /* THROWAWAY DIAG (env YZ_SIGW): on a committing PUTLLC of the mgmt line,
                 * did the SPU kernel CLEAR a wklSignal1 bit (consume it without dispatch)? */
                { static int sigw = -1; if (sigw < 0) sigw = getenv("YZ_SIGW") ? 1 : 0;
                  if (sigw && (ea & ~127ull) == 0x40197C80ull) {
                      unsigned sb = ((unsigned)mfc->resv_data[0x70] << 8) | mfc->resv_data[0x71];
                      unsigned sa = ((unsigned)ls_ptr[0x70] << 8) | ls_ptr[0x71];
                      if (sb != sa) { static int n = 0; if (n < 80) { n++;
                          fprintf(stderr, "[sig-chg] PUTLLC mgmt wklSignal1 0x%04X -> 0x%04X\n", sb, sa); fflush(stderr); } } } }
#endif
                memcpy(line, ls_ptr, 128);
                /* s21: a committing PUTLLC is a line WRITE -- bump the coherence
                 * generation and kill PEER reservations (CBEA reservation-lost;
                 * required for the lock-free GETLLAR fast path: a peer polling
                 * its cached copy must observe this commit). Clear OUR
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
                  * (RPCS3 do_putlluc) this invalidates EVERY reservation on the line
                  * (this SPU + all others) and raises SPU_EVENT_LR -- else another SPU's
                  * pending PUTLLC can spuriously succeed against a now-stale snapshot. */
#if !defined(YZ_PERF_CLEAN)
            yz_a010_reltrace_spu_atomic(
                spu->spu_id, (uint32_t)spu->image_id,
                spu->pc & SPU_LS_MASK,
                (uint32_t)(ea & ~127ull), ls_ptr, cmd);
#endif
            memcpy(line, ls_ptr, 128);
            mfc->resv_cas_idle_n = 0;
            { extern void spu_coh_notify_write(uint32_t);
              spu_coh_notify_write((uint32_t)(ea & ~127ull)); }  /* clears resv_active + raises LR for all matching ctxs */
            mfc_publish_atomic_status(mfc, MFC_PUTLLUC_SUCCESS);
            break;
        }
#if !defined(YZ_PERF_CLEAN)
        /* TS-WATCH (env YZ_TS_WATCH, 2026-06-20 pt27): log every SPU atomic touch of
         * the CRI taskset bitset line -> who clears the created task's enabled/ready
         * bits and via PUTLLC(CAS) vs PUTLLUC(unconditional). resv_data = the GETLLAR
         * snapshot (before); ls_ptr = the SPU's new line (after). */
        { static int tw = -1; if (tw < 0) tw = getenv("YZ_TS_WATCH") ? 1 : 0;
          extern uint32_t g_yz_spurs_taskset; extern uint32_t g_yz_codec_taskset;
          uint32_t eaL = (uint32_t)(ea & ~127ull);
          int isGs = (g_yz_spurs_taskset && eaL == (g_yz_spurs_taskset & ~127u));
          int isCodec = (g_yz_codec_taskset && eaL == (g_yz_codec_taskset & ~127u));
          if (tw && (isGs || isCodec)) {
              const uint8_t* b = mfc->resv_data; const uint8_t* a = ls_ptr;
              #define TW_W(p,o) (((uint32_t)(p)[(o)]<<24)|((uint32_t)(p)[(o)+1]<<16)|((uint32_t)(p)[(o)+2]<<8)|(p)[(o)+3])
              int commit = (mfc->atomic_stat == MFC_PUTLLC_SUCCESS ||
                            mfc->atomic_stat == MFC_PUTLLUC_SUCCESS);
              /* only log PUTs that actually CHANGE running/ready (cut GETLLAR noise) */
              int changed = (cmd != MFC_GETLLAR_CMD) &&
                            (TW_W(b,0x00)!=TW_W(a,0x00) || TW_W(b,0x10)!=TW_W(a,0x10));
              static int twn = 0;
              if ((changed || cmd!=MFC_GETLLAR_CMD) && twn < 120) { twn++;
                fprintf(stderr, "[ts-watch] %s spu=%X %s %s | run %08X->%08X rdy %08X->%08X "
                        "pReady %08X->%08X en %08X->%08X\n",
                        isCodec?"CODEC":"gstask", spu->spu_id,
                        cmd==MFC_GETLLAR_CMD?"GETLLAR":(cmd==MFC_PUTLLC_CMD?"PUTLLC":"PUTLLUC"),
                        cmd==MFC_GETLLAR_CMD?"read":(commit?"COMMIT":"fail"),
                        TW_W(b,0x00),TW_W(a,0x00), TW_W(b,0x10),TW_W(a,0x10),
                        TW_W(b,0x20),TW_W(a,0x20), TW_W(b,0x30),TW_W(a,0x30));
                fflush(stderr);
              }
              #undef TW_W
          } }
#endif
        spu_lockline_unlock();
        if (post_unlock_idle_level >= 0)
            spu_idle_yield(post_unlock_idle_level);
        if (tagged) mfc_tag_finish(mfc, spu, tag);
        return 0;
    }

#if !defined(YZ_PERF_CLEAN)
    /* DIAG (YZ_SPU_PROF): steady-state sampler for GETs that read the SPURS
     * mgmt struct BEYOND the poll line (offset >= 0x80: wklState1@0x80,
     * wklInfo1@0xB00). The service's UpdateWorkload refreshes wklInfo here and
     * rebuilds wklRunnable1. Logging the first 40 such GETs AFTER 20M GETLLARs
     * (well into the steady-state deadlock) tests whether UpdateWorkload still
     * runs post-init, or whether the service stopped processing the update. */
    if (YZ_SPU_PROF_ACTIVE() && mfc_is_get(cmd) && !mfc_is_list(cmd)) {
        uint32_t off = (uint32_t)ea - 0x40197C80u;
        if (off >= 0x80 && off < 0x2000) {
            extern unsigned long g_spu_getllar_n, g_spu_getn2;
            if (g_spu_getllar_n > 20000000UL && g_spu_getn2 < 40) {
                g_spu_getn2++;
                fprintf(stderr, "[spu-get] spu=%X mgmt+0x%X -> LS 0x%05X size=0x%X (steady)\n",
                        spu->spu_id, off, lsa & SPU_LS_MASK, size);
                fflush(stderr);
            }
        }
    }
#endif

    /* Execute the transfer */
    if (mfc_is_list(cmd)) {
        rc = mfc_do_list_transfer(mfc, spu, tag, lsa, ea, size, cmd);
    } else {
        rc = mfc_do_transfer(spu, lsa, ea, size, cmd);
    }

    /* Mark tag as completed -- EXCEPT when the list stalled (rc==1, F11/P6):
     * the transfer is genuinely incomplete until MFC_WrListStallAck resumes
     * and finishes it, so RdTagStat/WrTagUpdate must not report this tag done
     * yet (CBEA: a stalled list is not "completed", it is paused). */
    if (rc != 1) {
#if !defined(YZ_PERF_CLEAN)
        if (rc < 0) {
            static unsigned error_count = 0;
            if (error_count++ < 24) {
                fprintf(stderr, "[mfc-error] cmd=0x%02X tag=%u ea=0x%08X "
                                "lsa=0x%05X size=%u rc=%d\n",
                        cmd, tag, (uint32_t)ea, lsa, size, rc);
                fflush(stderr);
            }
        }
#endif
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
        /* F11/P6 (CBEA v1.02 list commands + RPCS3 SPUThread.cpp:6280-6298):
         * the written value's low 5 bits select a TAG (not an element index --
         * matches RdListStallStat's per-tag bitmask below), whose stalled list
         * transfer resumes from the next unprocessed element. Our DMA model is
         * synchronous, so "resume" means finish the rest of the list right
         * here rather than re-queuing an async command like RPCS3 does; the
         * guest-visible contract (RdTagStat only reports the tag completed
         * once the WHOLE list -- including the part after the stall point --
         * has transferred) is preserved either way. An ack for a tag that
         * isn't currently stalled is a no-op (nothing to resume), matching
         * RPCS3's `if (ch_stall_mask & tag_mask)` guard. */
        {
            uint32_t t = value & 0x1Fu;
            uint32_t bit = 1u << t;
            if (mfc->stall_mask & bit) {
                mfc->stall_mask &= ~bit;
                int rc = mfc_do_list_transfer_from(mfc, spu, t,
                             mfc->stall_list_lsa[t], mfc->stall_ea_base[t],
                             mfc->stall_base_cmd[t], mfc->stall_next_elem[t],
                             mfc->stall_num_elements[t], mfc->stall_cur_lsa[t]);
                /* rc==1: the resumed remainder hit ANOTHER stall-and-notify
                 * element -- mfc_do_list_transfer_from already re-recorded
                 * mfc->stall_*[t] and re-set the stall bit + Sn event for
                 * this tag, so the tag correctly stays incomplete. Only a
                 * clean finish (rc==0) marks the tag's DMA as done -- this is
                 * the same "list truly complete" gate mfc_submit applies to a
                 * fresh (non-resumed) list. */
                if (rc == 0) {
                    mfc_tag_finish(mfc, spu, t);
                    mfc_drain_deferred_tag(mfc, spu, t);
                } else if (rc < 0) {
                    static unsigned resume_error_count = 0;
                    if (resume_error_count++ < 24) {
                        fprintf(stderr, "[mfc-error] resumed-list tag=%u rc=%d\n", t, rc);
                        fflush(stderr);
                    }
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
        /* F11/P6 (CBEA v1.02 Section 7.5.3/7.6.3 list commands +
         * Section 9.12 event table; RPCS3 SPUThread.cpp:5298
         * `case MFC_RdListStallStat: return ch_stall_stat.get_count();`,
         * :3313 `ch_stall_stat.set_value(rotl(1,tag) | ...)`): a per-tag
         * bitmask of which tag groups' list transfers are currently
         * stalled, not an element index. Zero when nothing is stalled --
         * an un-stalled list therefore reads back exactly as before this
         * change (F11's "behaves exactly as today" requirement). */
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
