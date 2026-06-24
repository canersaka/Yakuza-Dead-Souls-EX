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
            /* SPURS workload dispatch: the kernel DMAs the selected workload's code
             * to LS 0xA00, then branches there. The system service and the taskset
             * POLICY module both live at LS 0xA00, so pick which lifted image
             * spu_indirect_branch resolves by the DMA SOURCE EA. (image ids match
             * main.cpp: 1=service, 2=policy @0x02023680.) */
            if (lsa == 0xA00u && size >= 0x400u) {
                int img = ((uint32_t)ea == 0x02023680u) ? 2 : 1;
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
        } else if (mfc_is_put(cmd)) {
            /* PUT: local store -> main memory */
            /* pt35e (env YZ_PUT_WATCH): the policy SPU (image 2) wild-writing into the GAME
             * data region (below the SPURS structs @0x40000000) is the corruption that makes
             * the PPU mixer recurse. Log the SPU PC + EA + size to trace it to the bad op. */
            { static int pw = -1; if (pw < 0) pw = getenv("YZ_PUT_WATCH") ? 1 : 0;
              if (pw && spu->image_id == 2 && (uint32_t)ea < 0x40000000u && size != 0) {
                  static int n = 0; if (n < 60) { n++;
                    fprintf(stderr, "[put-wild] spu=%X pc=0x%05X PUT ea=0x%08X size=0x%X lsa=0x%05X (policy -> GAME region)\n",
                            spu->spu_id, spu->pc & SPU_LS_MASK, (uint32_t)ea, size, lsa); fflush(stderr); } } }
            memcpy(ea_ptr, ls_ptr, size);
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
    /* The LS landing accumulates across elements but the staging MFC_LSA register
     * must NOT be permanently mutated -- use a local cursor (RPCS3 SPUThread.cpp). */
    uint32_t cur_lsa = spu->mfc_lsa;

    for (uint32_t i = 0; i < num_elements; i++) {
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
        uint8_t* line = vm_base + ((uint32_t)ea & ~127u);
        uint8_t* ls_ptr = &spu->ls[lsa & SPU_LS_MASK & ~127u];
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
              mfc->resv_ea  = le;
              mfc->resv_gen = g; }
            mfc->resv_active = 1;
            mfc->atomic_stat = MFC_GETLLAR_SUCCESS;
            /* EXPERIMENT (env YZ_LRWAKE): the SPURS idle kernel waits on SPU_EVENT_LR
             * for the workload-ready wakeup, but the PPU-side wklSignal/readyCount SET
             * that should raise it bypasses the coherence path (PROVEN: lr_raised stalls
             * at 42; the signal IS visible in this snapshot but no LR edge arrives).
             * Deliver the missed edge: raise LR on a GETLLAR of the SPURS mgmt line that
             * carries a pending workload signal (wklSignal1 @ +0x70 != 0), so the kernel
             * re-enters selection + runs the system service (rebuilds wklRunnable1 ->
             * dispatches the SOFDEC workload). Self-limits: once dispatched the kernel is
             * busy, not idle-GETLLARing. */
            { static int lw = -1; if (lw < 0) lw = getenv("YZ_LRWAKE") ? 1 : 0;
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
                    fprintf(stderr, "[spu-ls] spu=%X n%u wklCur=0x%X wklRun1=0x%04X idle=%u "
                            "| state1[0..3]=%02X%02X%02X%02X msgUpd=0x%02X msg72=0x%02X "
                            "| wid2: run=%u prio=%u maxc=%u cont=%u(loc=%u real=%u) rc=%u sig=%u SELECT=%d "
                            "| wid3: run=%u prio=%u maxc=%u cont=%u(loc=%u real=%u) rc=%u sig=%u SELECT=%d\n",
                            spu->spu_id, spuNum, wclId, wrun1, k[0x1EB],
                            w[0],w[1],w[2],w[3], w[0x3D], line[0x72],
                            run2, prio2, maxc2, curcont2, loccont2, realcont2, rc2, sig2, sel,
                            run3, prio3, maxc3, curcont3, loccont3, realcont3, rc3, sig3, sel3);
                    fflush(stderr);
                }
            }
            break; }
        case MFC_PUTLLC_CMD:
            if (g_spu_prof_on && (ea & ~127ull) == 0x40197C80ull) {
                extern unsigned long g_spu_mgmt_put_ok, g_spu_mgmt_put_fail;
                if (mfc->resv_active && mfc->resv_ea == (ea & ~127ull) &&
                    memcmp(line, mfc->resv_data, 128) == 0) g_spu_mgmt_put_ok++;
                else g_spu_mgmt_put_fail++;
            }
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
                /* THROWAWAY DIAG (env YZ_SIGW): on a committing PUTLLC of the mgmt line,
                 * did the SPU kernel CLEAR a wklSignal1 bit (consume it without dispatch)? */
                { static int sigw = -1; if (sigw < 0) sigw = getenv("YZ_SIGW") ? 1 : 0;
                  if (sigw && (ea & ~127ull) == 0x40197C80ull) {
                      unsigned sb = ((unsigned)mfc->resv_data[0x70] << 8) | mfc->resv_data[0x71];
                      unsigned sa = ((unsigned)ls_ptr[0x70] << 8) | ls_ptr[0x71];
                      if (sb != sa) { static int n = 0; if (n < 80) { n++;
                          fprintf(stderr, "[sig-chg] PUTLLC mgmt wklSignal1 0x%04X -> 0x%04X\n", sb, sa); fflush(stderr); } } } }
                memcpy(line, ls_ptr, 128);
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
