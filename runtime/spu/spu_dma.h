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
#endif

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
    uint32_t    resv_gen;         /* line write-generation at GETLLAR (LR re-derivation) */
    uint32_t    resv_poll_n;      /* consecutive same-line GETLLARs with an unchanged
                                   * write-generation = an idle reservation poll loop;
                                   * drives the host-yield backoff (see MFC_GETLLAR_CMD) */
    uint32_t    atomic_stat;      /* last MFC_RdAtomicStat value */

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

/* Single process-wide lock serializing lock-line transactions across all
 * SPU host threads (defined in spu_channels.c). */
void spu_lockline_lock(void);
void spu_lockline_unlock(void);
/* Host-core yield for SPU idle-poll loops (defined in spu_channels.c):
 * level 0 = cpu pause, level 1 = scheduler yield. */
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
        if (mfc_is_get(cmd)) {
            /* GET: main memory -> local store */
            memcpy(ls_ptr, ea_ptr, size);
            /* s26 ROOT FIX (the varying-round wall — STATUS ⚡ stager-hunt):
             * the wid4 pool task's 64-byte work-record fetch can race the
             * game's OWN staging (hardware-watch MEASURED: the pxd module on
             * the intr thread stores the round value AFTER its SendSignal —
             * benign on real HW where SPU wake latency exceeds the store gap;
             * our task wakes faster and fetches a HALF-STAGED record: guard
             * EA armed at +4, value word still 0 → publishes 0/nothing → the
             * exact-equality acquire wedges the boot at a varying round).
             * Absorb the race where real hardware does — in the transfer
             * latency: when a wid4 record fetch shows the half-staged
             * signature, re-copy briefly until the value lands (bounded; the
             * measured gap is <2 vblank ticks). Timeout ⇒ keep the fetched
             * bytes (current behavior). Kill-switch YZ_NO_RECWAIT. */
            if (spu->image_id == 4 && size == 0x40u && cmd != MFC_GETLLAR_CMD) {
                uint32_t rea = (uint32_t)ea;
                if (rea >= 0x42452880u && rea < 0x424529E0u) {
                    static int nrw = -1;
                    if (nrw < 0) { nrw = getenv("YZ_NO_RECWAIT") ? 1 : 0;
                        if (!nrw) { fprintf(stderr, "[recwait] ARMED: half-staged record absorber live\n"); fflush(stderr); } }
                    if (!nrw) {
                        uint32_t w0, w1;
                        memcpy(&w0, ls_ptr, 4); memcpy(&w1, ls_ptr + 4, 4);
                        if (w0 == 0 && w1 != 0) {          /* guard armed, value empty */
                            volatile const uint32_t* vw = (volatile const uint32_t*)ea_ptr;
                            for (long spin = 0; spin < 4000000 && *vw == 0; spin++)
                                ;                            /* bounded ~ms-scale spin */
                            memcpy(ls_ptr, ea_ptr, size);    /* re-copy the settled record */
                            static unsigned long rwn = 0; rwn++;
                            if (rwn <= 20 || (rwn & 0xFFu) == 0) {
                                fprintf(stderr, "[recwait] n=%lu spu=%X ea=0x%08X settled val(be)=%02X%02X%02X%02X\n",
                                        rwn, spu->spu_id, rea,
                                        ls_ptr[0], ls_ptr[1], ls_ptr[2], ls_ptr[3]);
                                fflush(stderr);
                            }
                        }
                    }
                }
            }
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
                    if (g_spu_prof_on || imglog)
                        fprintf(stderr, "[spu-img] spu=%X LS0xA00 <- ea=0x%08X size=0x%X => image %d (%s)\n",
                                spu->spu_id, (uint32_t)ea, size, img, img==2?"POLICY":"SERVICE");
                    spu->image_id = img;
                }
            }
            /* SPURS jobchain JOB-BINARY loads (2026-07-03). The job module
             * (image 13) DMAs each descriptor's eaBinary blob into free LS past
             * its own end and branches there. Both binaries the game's pxd
             * jobchain uses are EBOOT-static (measured live: the 14-way bulk
             * worker + the event-flag notify job); they are lifted as images 14/15.
             * Record the LS base per context so spu_indirect_branch can switch
             * to the right lifted image when the module branches into the blob.
             * Chunked GETs (max 0x4000/transfer) only match on the FIRST
             * chunk's ea == eaBinary; continuation chunks don't re-record.
             * NOT image-gated: a mid-cycle kernel adoption can leave the
             * module's SPU on image 0 (measured, first jobval boot) and the
             * descriptor eaBinary values are unique keys on their own. */
            if ((lsa & SPU_LS_MASK) >= 0x4880u) {
                switch ((uint32_t)ea) {
                case 0x01254500u: spu->job_bin_base[0] = lsa & SPU_LS_MASK; break; /* image 14 */
                case 0x01275A00u: spu->job_bin_base[1] = lsa & SPU_LS_MASK; break; /* image 15 */
                default: break;
                }
            }
        } else if (mfc_is_put(cmd)) {
            /* PUT: local store -> main memory */
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

        int rc = mfc_do_transfer(spu, cur_lsa, ea, xfer_size, base_cmd);
        if (rc != 0) return rc;

        /* MFC 16-byte-aligns each list element's LS landing. */
        cur_lsa += (xfer_size + 15u) & ~15u;

        if (stall_notify) {
            /* Suspend the list AFTER this element (the transfer above already
             * ran -- CBEA: the stalling element's OWN transfer completes; only
             * the REST of the list is held). Record enough to resume from
             * element i+1 on MFC_WrListStallAck, and raise Sn (edge-triggered
             * on the first tag to stall, matching RPCS3). */
            uint32_t bit = 1u << (tag & 0x1Fu);
            if (!mfc->stall_mask)
                spu->event_status |= SPU_EVENT_SN;
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
                                        uint32_t tag, uint32_t list_lsa,
                                        uint64_t ea_base, uint32_t list_size,
                                        uint32_t cmd)
{
    /* list_size is in bytes; each element is 8 bytes */
    uint32_t num_elements = list_size / 8;
    uint32_t base_cmd = cmd & ~0x04u; /* strip the 'list' bit to get base GET/PUT */
    /* The LS landing accumulates across elements but the staging MFC_LSA register
     * must NOT be permanently mutated -- use a local cursor (RPCS3 SPUThread.cpp). */
    uint32_t cur_lsa = spu->mfc_lsa;

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
              static unsigned long cn = 0; cn++;
              if (cn <= 40 || (cn & 0x3FFu) == 0) {
                  fprintf(stderr, "[jrnl-cur] n=%lu spu=%X pc=0x%05X ea=0x%08X\n",
                          cn, spu->spu_id, spu->pc & SPU_LS_MASK, jea);
                  fflush(stderr);
              }
              /* s26 (task #4): dump the FULL 128-byte line the consumer just
               * GETLLAR'd — the consume gate tests the +0x20 sub-entry vs the
               * 0x4000 sentinel (gs_task.c 0x63F4), which a 32-byte dump cut
               * off. (The earlier gpr3 struct dump was measured-useless: the
               * routine zeroes gpr3 at 0x6368, before the Cmd wrch we catch.)
               * ride11 already proved the head is NON-EMPTY at the stall. */
              if (cn <= 12 || (cn & 0xFFFu) == 0) {
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
    { static int w4r = -1;
      if (w4r < 0) { w4r = getenv("YZ_W4REC") ? 1 : 0;
          if (w4r) { fprintf(stderr, "[w4rec] ARMED: image-4 64-byte work-record GET watch live\n"); fflush(stderr); } }
      /* s26 ~03:25 (STATUS frontier): the record STAGER watch — who WRITES the
       * work-record slots (not a lifted PPU store per YZ_SLOTSTORE) — any-image
       * PUT-class DMA covering the slot range, payload+pc dumped. One boot
       * names the stager; the fix enforces its stage→signal ordering. */
      if (w4r && (mfc_is_put(cmd) || cmd == MFC_PUTLLC_CMD || cmd == MFC_PUTLLUC_CMD
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
      if (w4r && spu->image_id == 4 && mfc_is_get(cmd) && cmd != MFC_GETLLAR_CMD
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
    if (g_spu_prof_on && cmd == MFC_GET_CMD          /* regular GET only (NOT GETLLAR 0xD0) */
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
        /* DIAG (2026-07-03, the post-gate crash): a lock-line atomic against the
         * NULL page is a fatal host AV in the unguarded 128-byte copies below.
         * Print the issuer's identity BEFORE the fault so the crash names its
         * own root (who computed a zero EA), then proceed -- the fault itself
         * stays visible; don't mask the symptom. */
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
            static int ng = -1;
            if (ng < 0) ng = getenv("YZ_NO_DMAGUARD") ? 1 : 0;
            if (!ng) {
                static unsigned long gn = 0; gn++;
                if (gn <= 16 || (gn & 63u) == 0) {
                    fprintf(stderr, "[dma-guard] n=%lu spu=%X img=%d pc=0x%05X cmd=0x%02X "
                            "ea=0x%08llX -> completed benignly (GETLLAR=zeros, PUTLLC=fail)\n",
                            gn, spu->spu_id, spu->image_id, spu->pc & SPU_LS_MASK,
                            cmd, (unsigned long long)ea);
                    fflush(stderr);
                }
                if (cmd == MFC_GETLLAR_CMD) {
                    memset(&spu->ls[lsa & SPU_LS_MASK & ~127u], 0, 128);
                    mfc->resv_active = 0;
                    mfc->atomic_stat = MFC_GETLLAR_SUCCESS;
                } else if (cmd == MFC_PUTLLC_CMD) {
                    mfc->atomic_stat = MFC_PUTLLC_FAILURE;
                } else {
                    mfc->atomic_stat = MFC_PUTLLUC_SUCCESS;   /* dropped write */
                }
                mfc->tag_completed |= (1u << tag);
                return 0;
            }
        }
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
            static int llfast = -1;
            if (llfast < 0) llfast = getenv("YZ_NO_LLFAST") ? 0 : 1;
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
                extern unsigned long g_spu_getllar_n; extern uint32_t g_spu_getllar_ea;
                g_spu_getllar_n++; g_spu_getllar_ea = (uint32_t)le;
                memcpy(ls_ptr, mfc->resv_data, 128);
                mfc->resv_poll_n++;
                { static int nb = -1;
                  if (nb < 0) nb = getenv("YZ_NO_SPUBACKOFF") ? 1 : 0;
                  if (!nb && mfc->resv_poll_n > 16)
                      spu_idle_yield(mfc->resv_poll_n > (1u << 20) ? 2
                                   : mfc->resv_poll_n > 256       ? 1 : 0); }
                { static int lw = -1; if (lw < 0) lw = getenv("YZ_NO_LRWAKE") ? 0 : 1;
                  if (lw && (uint32_t)le == 0x40197C80u
                      && (mfc->resv_data[0x70] | mfc->resv_data[0x71]))
                      spu->event_status |= 0x400u; }
                mfc->atomic_stat = MFC_GETLLAR_SUCCESS;
                mfc->tag_completed |= (1u << tag);
                return 0;
            }
        }
        spu_lockline_lock();
        switch (cmd) {
        case MFC_GETLLAR_CMD: {
            extern unsigned long g_spu_getllar_n; extern uint32_t g_spu_getllar_ea;
            extern void spu_coh_reserve(uint32_t);   /* mark this line for PPU-write coherence */
            g_spu_getllar_n++; g_spu_getllar_ea = (uint32_t)(ea & ~127ull);
            { extern void spu_llar_note(uint32_t); extern int g_spu_prof_on;
              if (g_spu_prof_on) spu_llar_note((uint32_t)(ea & ~127ull)); }
            spu_coh_reserve((uint32_t)(ea & ~127ull));
            memcpy(ls_ptr, line, 128);
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
            /* CONFIRMATION EXPERIMENT (env YZ_FRC): force wklReadyCount1[2]=1 (the
             * gs_task taskset, wid=2) in the SPURS mgmt line so the kernel select's
             * `readyCount > contention` gate passes. Proves whether readyCount is
             * the sole remaining block to dispatching wid=2 -> the taskset policy
             * module -> gs_task. Patches the source line too so it persists across
             * the kernel's PUTLLC. */
            { static int frc = -1; if (frc < 0) frc = getenv("YZ_FRC") ? 1 : 0;
              if (frc && (ea & ~127ull) == 0x40197C80ull) { line[0x02] = 1; ls_ptr[0x02] = 1; } }
            /* pt35 CONFIRMATION (env YZ_FRC3): bootstrap the cri_audio codec
             * workload (wid 3) by clearing its wklCurrentContention[3] (+0x23) and
             * pendingContention (+0x33) and asserting readyCount[3] (+0x03) DIRECTLY
             * in the kernel's GETLLAR'd line. wid3 is runnable + has priority but
             * cur=1==maxContention with no SPU holding wklLocContention[3], so it can
             * never get its FIRST selection. Injecting cur=0 here (reliably, unlike
             * the vblank force) should let the kernel select wid3 -> policy dispatch
             * -> StartTask(codec) -> spu_task_launch runs cri_audio. Proves the
             * contention bootstrap is the gate. */
            { static int frc3 = -1; if (frc3 < 0) frc3 = getenv("YZ_FRC3") ? 1 : 0;
              if (frc3 && (ea & ~127ull) == 0x40197C80ull) {
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
            { static int fw0 = -1; if (fw0 < 0) fw0 = getenv("YZ_FORCE_WID0") ? 1 : 0;
              if (fw0 && (ea & ~127ull) == 0x40197C80ull) {
                  line[0x00] = 1; ls_ptr[0x00] = 1;         /* wklReadyCount1[0] = 1 */
                  line[0x70] |= 0x80; ls_ptr[0x70] |= 0x80; /* wklSignal1 bit0 (0x8000) */
                  extern int g_spu_prof_on;
                  if (g_spu_prof_on || getenv("YZ_WID01")) {
                      static int n = 0; if (n < 8) { n++;
                          fprintf(stderr, "[force-wid0] forced rc0=1 sig0 @mgmt 0x40197C80\n");
                          fflush(stderr); } }
              } }
            /* Same mechanism, wid1 (the pxd jobchain taskset, main.cpp:86) --
             * fallback probe if YZ_FORCE_WID0 does not produce the port-17
             * heartbeat. wklReadyCount1[1]=line[0x01], wklSignal1 bit1=0x4000. */
            { static int fw1 = -1; if (fw1 < 0) fw1 = getenv("YZ_FORCE_WID1") ? 1 : 0;
              if (fw1 && (ea & ~127ull) == 0x40197C80ull) {
                  line[0x01] = 1; ls_ptr[0x01] = 1;         /* wklReadyCount1[1] = 1 */
                  line[0x70] |= 0x40; ls_ptr[0x70] |= 0x40; /* wklSignal1 bit1 (0x4000) */
                  extern int g_spu_prof_on;
                  if (g_spu_prof_on || getenv("YZ_WID01")) {
                      static int n = 0; if (n < 8) { n++;
                          fprintf(stderr, "[force-wid1] forced rc1=1 sig1 @mgmt 0x40197C80\n");
                          fflush(stderr); } }
              } }
            /* BOOTSTRAP (env YZ_CLEARRUN): gs_task (task 0) is stuck `running`
             * (taskset running@0x00 bit 0x80) but never launched, so SELECT_TASK
             * (readyButNotRunning = ready & ~running) can't re-select it and the
             * policy idle-polls forever. Clear its running bit on the taskset
             * bitset GETLLAR so SELECT_TASK re-selects it -> StartTask -> (the
             * kernel resume above completes the poll) -> bi savedContextLr=0x3050
             * launches gs_task. Decisive test of the task-level bootstrap. */
            { extern uint32_t g_yz_taskset_ea;
              static int cr = -1; if (cr < 0) cr = getenv("YZ_CLEARRUN") ? 1 : 0;
              if (cr && g_yz_taskset_ea && (uint32_t)(ea & ~127ull) == g_yz_taskset_ea
                  && (line[0x00] & 0x80u)) {
                  line[0x00] &= ~0x80u; ls_ptr[0x00] &= ~0x80u;   /* clear running task 0 */
                  extern int g_spu_prof_on;
                  if (g_spu_prof_on) { static int n=0; if (n<8){n++;
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
              static int cr3 = -1; if (cr3 < 0) cr3 = getenv("YZ_CLEARRUN3") ? 1 : 0;
              if (cr3 && g_yz_codec_taskset && (uint32_t)(ea & ~127ull) == g_yz_codec_taskset
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
              if (mfc->resv_ea == le && g != mfc->resv_gen) spu->event_status |= 0x400u;
              /* IDLE-POLL BACKOFF (2026-07-03; kill-switch YZ_NO_SPUBACKOFF).
               * The SPURS kernels poll their management lines with GETLLAR at
               * full host speed -- measured 5 SPU threads x ~97% of a core --
               * and EVERY poll takes the process-wide lock-line spinlock, so
               * the PPU threads' atomics contend against five spinning cores
               * and boot pacing collapses (the post-voice asset phase made
               * less progress in 900 s than a lucky run made in 300 s).
               * Faithful: polling continues; but when the SAME line is re-read
               * and its write-generation has not moved, yield the host core
               * with escalation. Any observed change (or a different line)
               * resets the ladder, so reaction latency to real writes stays
               * one poll deep. */
              if (mfc->resv_ea == le && g == mfc->resv_gen) {
                  mfc->resv_poll_n++;
                  static int nb = -1;
                  if (nb < 0) nb = getenv("YZ_NO_SPUBACKOFF") ? 1 : 0;
                  /* Rung 3 (real sleep) only after ~seconds of continuous
                   * idleness: at yield pacing 1M unchanged polls is a long
                   * quiet stretch. A 16k threshold measurably taxed boot
                   * pacing (each wake costs up to ~15 ms at default timer
                   * resolution, and the boot is a chain of idle-then-
                   * handshake moments). */
                  if (!nb && mfc->resv_poll_n > 16)
                      spu_idle_yield(mfc->resv_poll_n > (1u << 20) ? 2
                                   : mfc->resv_poll_n > 256       ? 1 : 0);
              } else {
                  mfc->resv_poll_n = 0;
              }
              mfc->resv_ea  = le;
              mfc->resv_gen = g; }
            mfc->resv_active = 1;
            mfc->atomic_stat = MFC_GETLLAR_SUCCESS;
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
            { static int lw = -1; if (lw < 0) lw = getenv("YZ_NO_LRWAKE") ? 0 : 1;
              if (lw && (uint32_t)(ea & ~127ull) == 0x40197C80u
                  && (line[0x70] | line[0x71])) spu->event_status |= 0x400u; }
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
            if (g_spu_prof_on && (ea & ~127ull) == 0x40197D00ull) {
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
            if (ls_dump_on < 0) ls_dump_on = (g_spu_prof_on || getenv("YZ_LS_DUMP")) ? 1 : 0;
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
                    if (wid01_on < 0) wid01_on = (g_spu_prof_on || getenv("YZ_WID01")) ? 1 : 0;
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
                if (wid01_on2 < 0) wid01_on2 = (g_spu_prof_on || getenv("YZ_WID01")) ? 1 : 0;
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
            break; }
        case MFC_PUTLLC_CMD:
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
            if (g_spu_prof_on && (ea & ~127ull) == 0x40197C80ull) {
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
            if (mfc->resv_active && mfc->resv_ea == (ea & ~127ull) &&
                memcmp(line, mfc->resv_data, 128) == 0) {
                /* DIAG (YZ_SPU_PROF): on a successful CAS of the SPURS mgmt
                 * line (0x40197C80), log mutations to the fields that gate
                 * workload selection -- sysSrvMessage@0x72 and wklReadyCount1
                 * [0..3]@0x00. Pins whether the SERVICE itself makes wid=2
                 * selectable, or whether that must come from the PPU side. */
                if (g_spu_prof_on && (ea & ~127ull) == 0x40197C80ull) {
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
                if (g_spu_prof_on && (ea & ~127ull) == 0x40197D00ull) {
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
                mfc->atomic_stat = MFC_PUTLLC_SUCCESS;
            } else {
                mfc->atomic_stat = MFC_PUTLLC_FAILURE;
            }
            mfc->resv_active = 0;
            break;
        default: /* PUTLLUC / PUTQLLUC: unconditional 128-byte line store. Per HW
                  * (RPCS3 do_putlluc) this invalidates EVERY reservation on the line
                  * (this SPU + all others) and raises SPU_EVENT_LR -- else another SPU's
                  * pending PUTLLC can spuriously succeed against a now-stale snapshot. */
            memcpy(line, ls_ptr, 128);
            { extern void spu_coh_notify_write(uint32_t);
              spu_coh_notify_write((uint32_t)(ea & ~127ull)); }  /* clears resv_active + raises LR for all matching ctxs */
            mfc->atomic_stat = MFC_PUTLLUC_SUCCESS;
            break;
        }
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
        spu_lockline_unlock();
        mfc->tag_completed |= (1u << tag);
        return 0;
    }

    /* DIAG (YZ_SPU_PROF): steady-state sampler for GETs that read the SPURS
     * mgmt struct BEYOND the poll line (offset >= 0x80: wklState1@0x80,
     * wklInfo1@0xB00). The service's UpdateWorkload refreshes wklInfo here and
     * rebuilds wklRunnable1. Logging the first 40 such GETs AFTER 20M GETLLARs
     * (well into the steady-state deadlock) tests whether UpdateWorkload still
     * runs post-init, or whether the service stopped processing the update. */
    if (g_spu_prof_on && mfc_is_get(cmd) && !mfc_is_list(cmd)) {
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
    if (rc != 1)
        mfc->tag_completed |= (1u << tag);

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
                if (rc == 0)
                    mfc->tag_completed |= bit;
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
        return spu->mfc_tag_status;
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
        return mfc->atomic_stat;
    default:
        return 0;
    }
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_DMA_H */
