/*
 * ps3recomp - SPU (Synergistic Processing Unit) execution context
 *
 * Models the full architectural state of an SPU:
 *   - 128 x 128-bit general-purpose registers
 *   - 256 KB local store
 *   - Channel state (MFC command queue, mailboxes, signal notification)
 */

#ifndef SPU_CONTEXT_H
#define SPU_CONTEXT_H

#include "../../include/ps3emu/ps3types.h"
#include "spu_fltrec.h"    /* s41 flight recorder (env YZ_FLTREC, default off) */
#include "spu_lockstep.h"  /* s42 YZ_SPU_LOCKSTEP round-robin token (default off) */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>     /* FIX 2 (2026-07-17): channel/event cross-thread atomics */

#ifdef __cplusplus
extern "C" {
#endif

/* Portable 16-byte alignment (MSVC __declspec vs GCC/clang __attribute__). */
#if defined(_MSC_VER)
#  define SPU_ALIGN16 __declspec(align(16))
#else
#  define SPU_ALIGN16 __attribute__((aligned(16)))
#endif

/* Local store size: 256 KB */
#define SPU_LS_SIZE         (256 * 1024)
#define SPU_LS_MASK         (SPU_LS_SIZE - 1)

/* Maximum number of MFC tag groups */
#define SPU_MFC_MAX_TAGS    32

/* Mailbox / signal capacities */
#define SPU_MBOX_DEPTH      1    /* SPU write outbound mailbox depth */
#define SPU_INTR_MBOX_DEPTH 1    /* SPU write inbound interrupt mailbox depth */

/* ---------------------------------------------------------------------------
 * SPU channel IDs
 * -----------------------------------------------------------------------*/
#define SPU_RdEventStat     0
#define SPU_WrEventMask     1
#define SPU_WrEventAck      2
#define SPU_RdSigNotify1    3
#define SPU_RdSigNotify2    4
#define SPU_WrDec           7
#define SPU_RdDec           8
#define SPU_RdEventMask     11
#define SPU_RdMachStat      13
#define SPU_WrSRR0          14
#define SPU_RdSRR0          15
#define SPU_WrOutMbox       28
#define SPU_RdInMbox        29
#define SPU_WrOutIntrMbox   30

/* MFC channels */
#define MFC_WrMSSyncReq     9
#define MFC_RdTagMask       12
#define MFC_LSA             16
#define MFC_EAH             17
#define MFC_EAL             18
#define MFC_Size            19
#define MFC_TagID           20
#define MFC_Cmd             21
#define MFC_WrTagMask       22
#define MFC_WrTagUpdate     23
#define MFC_RdTagStat       24
#define MFC_RdListStallStat 25
#define MFC_WrListStallAck  26
#define MFC_RdAtomicStat    27

/* ---------------------------------------------------------------------------
 * MFC DMA command opcodes
 * -----------------------------------------------------------------------*/
#define MFC_PUT_CMD         0x20
#define MFC_PUTB_CMD        0x21
#define MFC_PUTF_CMD        0x22
#define MFC_GET_CMD         0x40
#define MFC_GETB_CMD        0x41
#define MFC_GETF_CMD        0x42
#define MFC_PUTL_CMD        0x24
#define MFC_PUTLB_CMD       0x25
#define MFC_PUTLF_CMD       0x26
#define MFC_GETL_CMD        0x44
#define MFC_GETLB_CMD       0x45
#define MFC_GETLF_CMD       0x46
#define MFC_SNDSIG_CMD      0xA0
#define MFC_BARRIER_CMD     0xC0
#define MFC_EIEIO_CMD       0xC8
#define MFC_SYNC_CMD        0xCC

/* Lock-line (128-byte cache line) atomics — the SPURS kernel scheduler
 * claims workloads with GETLLAR/PUTLLC loops. */
#define MFC_PUTLLC_CMD      0xB4
#define MFC_PUTLLUC_CMD     0xB0
#define MFC_PUTQLLUC_CMD    0xB8
#define MFC_GETLLAR_CMD     0xD0

/* MFC_RdAtomicStat result values (RPCS3 MFC.h, CBEA ch. 9) */
#define MFC_PUTLLC_SUCCESS  0
#define MFC_PUTLLC_FAILURE  1
#define MFC_PUTLLUC_SUCCESS 2
#define MFC_GETLLAR_SUCCESS 4

/* ---------------------------------------------------------------------------
 * Channel state
 *
 * `count` is a C11 atomic (2026-07-17, spu_channels.c FIX 2). RdInMbox,
 * RdSigNotify1/2 and RdEventStat's underlying storage (this field, and
 * event_status below) have a real or intended cross-thread producer -- the
 * PPU (sys_spu_thread_write_snr) or another SPU (the coherence-loss LR raise)
 * writes it while the owning SPU host thread polls/blocks on it in
 * spu_ch_ready/spu_ch_wait (spu_channels.c). Plain uint32_t reads/writes were
 * previously justified by x86 TSO (a producer's plain store is globally
 * visible, and the 10 ms spu_ch_wait re-poll is a missed-wake safety net) --
 * that argument covers cross-CPU VISIBILITY latency but not the compiler's
 * freedom to cache/reorder a plain load across loop iterations, which _Atomic
 * plus an explicit memory_order closes without weakening the existing
 * TSO/relaxed-visibility model (relaxed is still used wherever that argument
 * applies). `count` is used as a ready-flag guarding `value` (message-passing
 * idiom): the producer stores value then releases count; the consumer
 * acquire-loads count and, only once it observes nonzero, reads value -- so
 * `value` itself does not need to be atomic. spu_channel is shared by all
 * four mailbox/signal instances (ch_out_mbox and ch_out_intr_mbox are
 * same-thread only per spu_wrch's own comments) -- one struct definition, so
 * they get the same atomic field; harmless for the same-thread instances
 * (default sequentially-consistent ops on a single-thread-owned atomic are
 * just a locked no-op on x86, never a behavior change). */
typedef struct spu_channel {
    uint32_t          value;
    _Atomic uint32_t  count;   /* number of valid entries (0 or 1 for most channels) */
} spu_channel;
/* Struct-layout guard (spu_context's APPEND-ONLY rule, see below): atomic_uint
 * is guaranteed lock-free and same size/alignment as uint32_t on every target
 * this project builds for (x86-64 MSVC/clang/gcc); this assertion makes that
 * assumption load-bearing and checked, not just assumed. */
_Static_assert(sizeof(spu_channel) == 8, "spu_channel size must not change (APPEND-ONLY struct layout rule)");

/* ---------------------------------------------------------------------------
 * SPU execution context
 * -----------------------------------------------------------------------*/
typedef struct spu_context {
    /* 128 general-purpose 128-bit registers */
    SPU_ALIGN16 u128 gpr[128];

    /* 256 KB local store, 16-byte aligned */
    SPU_ALIGN16 uint8_t ls[SPU_LS_SIZE];

    /* Program counter (local store address, 0-0x3FFFF) */
    uint32_t pc;

    /* SPU status (running, stopped, etc.) */
    uint32_t status;
    #define SPU_STATUS_STOPPED      0x0
    #define SPU_STATUS_RUNNING      0x1
    #define SPU_STATUS_STOPPED_BY_STOP  0x2
    #define SPU_STATUS_STOPPED_BY_HALT  0x4
    #define SPU_STATUS_WAITING_CHANNEL  0x8
    #define SPU_STATUS_SINGLE_STEP  0x10

    /* 14-bit stop-and-signal code from the last stop/stopd (insn bits 18:31,
     * & 0x3FFF; stopd is fixed at 0x3FFF). Set by lifted stop/stopd codegen so
     * lv2 can dispatch the SYS_SPU_THREAD_STOP_* protocol (0x101 GROUP_EXIT /
     * 0x102 THREAD_EXIT pop the OutMbox for the guest-written exit status,
     * 0x110 RECEIVE_EVENT, ...). CBEA v1.02 p97 SPU_Status.StopCode. specaudit F18. */
    uint32_t stop_code;

    /* SPU thread identification */
    uint32_t spu_id;
    uint32_t spu_group_id;

    /* Active recompiled image for per-context indirect-branch dispatch. SPURS
     * loads kernel/policy/job into overlapping LS addresses at different times,
     * so spu_indirect_branch resolves pc within the image currently selected
     * here. 0 = match any image (back-compat for single-image contexts). */
    int image_id;

    /* SPURS jobchain: LS bases of the job BINARIES the job module (image 13)
     * has DMA'd in for the current dispatch, recorded by mfc_do_transfer from
     * the known descriptor eaBinary values (index = image_id - 14; 0 = not
     * loaded). spu_indirect_branch keys the module->job image switch on these
     * -- the DMA is the ground truth for what is resident where. */
    uint32_t job_bin_base[2];

    /* Decrementer (a free-running down counter) */
    uint32_t decrementer;

    /* SRR0 - Save/Restore Register (exception return address) */
    uint32_t srr0;

    /* Event status / mask. event_status is a C11 atomic (FIX 2, 2026-07-17):
     * it has TWO writer classes -- the owning SPU thread itself (WrEventAck's
     * clear, and the MFC engine's own SN/LR self-raises in spu_dma.h) and a
     * genuinely cross-thread writer (spu_coh_notify_write's LR raise, called
     * from the PPU coherence-write path in another thread). Both `|=`/`&=`
     * writer sites now go through atomic_fetch_or_explicit/
     * atomic_fetch_and_explicit so the read-modify-write itself is a single
     * atomic op (closing a possible lost-update between the two writer
     * classes) in addition to the pre-existing spu_lockline_lock serialization
     * (see SPU_WrEventAck in spu_channels.c) -- belt-and-suspenders, not a
     * replacement for the lock. event_mask has exactly one writer, the owning
     * SPU thread's own WrEventMask (spu_channels.c), so it stays plain
     * uint32_t per the conservative fix scope (only fields with a real
     * cross-thread writer get promoted). */
    _Atomic uint32_t event_status;
    uint32_t         event_mask;

    /* Channels */
    spu_channel ch_out_mbox;        /* SPU -> PPU outbound mailbox */
    spu_channel ch_in_mbox;         /* PPU -> SPU inbound mailbox */
    spu_channel ch_out_intr_mbox;   /* SPU -> PPU interrupt mailbox */
    spu_channel ch_sig_notify[2];   /* Signal notification 1 & 2 */

    /* MFC command staging registers (written via channels before issuing cmd) */
    uint32_t mfc_lsa;
    uint32_t mfc_eah;
    uint32_t mfc_eal;
    uint32_t mfc_size;
    uint32_t mfc_tag;

    /* MFC tag completion mask and status */
    uint32_t mfc_tag_mask;
    uint32_t mfc_tag_status;

    /* Live lifted host-call frames above this context's dispatch loop.
     * Incremented/decremented by the lifted brsl/bisl call emissions around
     * the nested host call; reset to 0 at the host-thread driver's setjmp
     * re-entry points (all longjmp unwinds land there: task launch/resume
     * context replacement, the host-call-depth restack guard, halt). Read by
     * SPU_RET (`bi $r0`): host_depth == 0 means the SPU-logical caller has no
     * live host frame, so a C return cannot reach it. */
    uint32_t host_depth;

    /* ---- Fields below this line were appended 2026-07-03 (SPU-runtime slice:
     * decrementer, DMA-list stall-and-notify). Struct layout rule: APPEND ONLY --
     * lifted code and any generated mirror depend on the offsets of the fields
     * above, so nothing above this comment may move or change size. ---- */

    /* Decrementer (CBEA v1.02 Section 9.7 p145-146; RPCS3 SPUThread.cpp:1313-1315).
     * The decrementer is a free-running 32-bit down counter at the PPE timebase
     * rate (79.8 MHz on PS3) that STARTS COUNTING AT SPU THREAD CREATION, not at
     * the first `wrch SPU_WrDec` -- confirmed live-exposure fact (specaudit_spu.md
     * F21): all 13 measured `rdch SPU_RdDec` sites in the lifted images have ZERO
     * matching `wrch SPU_WrDec` sites, so a WrDec-triggered-only model would leave
     * every read frozen forever. dec_running is set at spu_context_init() (mirrors
     * RPCS3's cpu_init/thread-constructor start) and again on any `wrch SPU_WrDec`
     * (CBEA p145: "the decrementer... starts... when a wrch instruction targets
     * SPU_WrDec"). dec_start_tb is the guest timebase (ppu_timebase_now(), the
     * same 79.8 MHz clock as the PPU decrementer/mftb -- CBEA Section 9.7 ties the
     * SPU decrementer to "the same rate as the PPE decrementer") sampled at the
     * moment dec_value was last set; SpuRdDec computes
     * `dec_value - (ppu_timebase_now() - dec_start_tb)` (see spu_channels.c
     * SPU_RdDec / SPU_WrDec). dec_running (0 = frozen) models CBEA Section
     * 9.11.2 p158's ack-while-masked stop rule (WrEventAck of the Tm bit while
     * Tm is disabled in the event mask stops the count; re-armed by the next
     * WrDec). Decrementer
     * EVENT generation (Tm, status bit 0x20, MSb 0->1 edge, CBEA Section 9.12.5
     * p166) is intentionally NOT implemented -- see the note at SPU_RdDec in
     * spu_channels.c; this is a documented remaining gap, not an oversight. */
    uint64_t dec_start_tb;   /* guest timebase (ppu_timebase_now()) at the last WrDec/thread-start */
    uint32_t dec_value;      /* value written (or the POR initial value at thread start) */
    int      dec_running;    /* 0 = frozen (stopped by ack-while-masked, CBEA p158 Section 9.11.2) */

    /* Which workload-module image is RESIDENT at LS 0xA00 in this context
     * (1=service, 2=taskset policy, 13=job module), recorded by the DMA layer
     * at the kernel's module load (spu_dma.h LS-0xA00 switch). Ground truth
     * for dispatch: the bracket restore (spu_img_restore) can undo a DMA-time
     * image_id switch when the kernel loads the module inside a subroutine
     * that returns before the dispatch branch, so spu_indirect_branch
     * re-adopts this recorded image at every pc==0xA00 entry. 0 = nothing
     * recorded yet (s24 adopt-on-serve model, ledger #51). */
    int32_t  module_img_a00;

    /* s32: the LS-0xA00 module's SOURCE (guest EA + byte size), recorded at
     * the same kernel module load as module_img_a00. Ground truth for the
     * module-image staleness guard at the 0xA00 entry (the s32coop1 death:
     * the policy's syscall jump table at LS 0x1CC8 read as zeros mid-boot --
     * the workload-level sibling of the #34 task-segment class; the
     * module-entry contract makes fresh redelivery always-safe: modules
     * rebuild their working state at entry, measured + RPCS3 dispatch). 0 = none. */
    uint32_t module_src_ea;
    uint32_t module_src_size;

    /* s39 (2026-07-15) faithful channel-stall contract: per-SPU wake mechanism
     * for the blocking rdch path (spu_ch_wait/spu_ch_wake, spu_channels.c).
     * Storage for a Win32 CONDITION_VARIABLE + SRWLOCK -- each is a single
     * pointer-sized struct whose all-zero state IS the documented INIT value
     * (CONDITION_VARIABLE_INIT / SRWLOCK_INIT == {0}), so the spu_context_init /
     * lv2_register calloc+memset zero-init is a valid initialization and no
     * explicit Init call is needed. Declared as void* to keep <windows.h> out of
     * this shared header (the helpers in spu_channels.c cast &field). On non-
     * Windows these stay unused (the POSIX wait path polls). Appended per the
     * struct's APPEND-ONLY rule -- nothing above moves. */
    void* ch_wait_cv;     /* CONDITION_VARIABLE */
    void* ch_wait_lock;   /* SRWLOCK */

    /* s42 YZ_SPU_LOCKSTEP (diagnostic, default OFF; runtime/spu/spu_lockstep.c):
     * per-context state for the round-robin SPU run token.
     * lockstep_quantum_ctr counts drain-ticks/getllars since this ctx's last
     * token-pass check. lockstep_release_tb is the guest timebase
     * (ppu_timebase_now()) at which this ctx most recently stopped holding
     * the token (or was registered, if it never held it yet); consumed at
     * the next acquire to advance dec_start_tb by exactly the paused
     * wall-clock span, so RdDec only drains while this SPU actually holds
     * the token (review change C, scratch/s42_lockstep_review.md Attack 4) --
     * the RdDec/WrDec formulas in spu_channels.c are UNCHANGED. Both fields
     * are inert (never written) when YZ_SPU_LOCKSTEP is unset. Appended per
     * the struct's APPEND-ONLY rule -- nothing above moves. */
    uint32_t lockstep_quantum_ctr;
    uint64_t lockstep_release_tb;

} spu_context;

/* Guest timebase clock (runtime/syscalls/sys_timer.c), 79.8 MHz, the same
 * clock the PPU decrementer/mftb use -- CBEA v1.02 Section 9.7 p145 ties the
 * SPU decrementer to this rate ("run at the same rate as the PPE
 * decrementer"). Declared here (not spu_dma.h) since spu_context_init needs
 * it to start the decrementer at thread creation (RPCS3 SPUThread.cpp:1313,
 * F21's fix). */
uint64_t ppu_timebase_now(void);

/* ---------------------------------------------------------------------------
 * Initialization
 * -----------------------------------------------------------------------*/
static inline void spu_context_init(spu_context* ctx, uint32_t spu_id)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->spu_id = spu_id;
    ctx->status = SPU_STATUS_STOPPED;

    /* Decrementer starts running at SPU thread creation, not at the first
     * WrDec (F21; RPCS3 SPUThread.cpp:1313-1315 cpu_init starts
     * ch_dec_start_timestamp/ch_dec_value here unconditionally). POR initial
     * value: RPCS3 sets ch_dec_value=0 unless SYS_SPU_THREAD_OPTION_DEC_SYNC_TB_
     * ENABLE was requested at thread creation (ch_dec_value = ~(u32)start_ts) --
     * that option is a per-thread create flag our sys_spu_thread_create path
     * does not currently plumb through, so we take the POR-default branch
     * (value 0) here, matching the common case. */
    ctx->dec_value     = 0;
    ctx->dec_start_tb  = ppu_timebase_now();
    ctx->dec_running   = 1;
}

/* ---------------------------------------------------------------------------
 * Local store access helpers
 * -----------------------------------------------------------------------*/
static inline uint8_t* spu_ls_ptr(spu_context* ctx, uint32_t lsa)
{
    return &ctx->ls[lsa & SPU_LS_MASK];
}

static inline uint32_t spu_ls_read32(const spu_context* ctx, uint32_t lsa)
{
    lsa &= SPU_LS_MASK;
    const uint8_t* p = &ctx->ls[lsa];
    /* SPU local store is big-endian */
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline void spu_ls_write32(spu_context* ctx, uint32_t lsa, uint32_t val)
{
    lsa &= SPU_LS_MASK;
    /* s41 FLIGHT RECORDER (env YZ_FLTREC, consumer ctx only; spu_fltrec.h).
     * No lifted SPU code currently calls this path (the ISA has no scalar
     * LS store -- see spu_fltrec.h), but it is hooked for completeness /
     * any future host-side scalar writer. */
    if (yz_fltrec_hot(ctx)) yz_fltrec_store32(ctx, lsa, val);
    uint8_t* p = &ctx->ls[lsa];
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);
    p[3] = (uint8_t)(val);
}

/* Local-store quadword access.
 *
 * LS bytes are big-endian (the SPU's native order); registers in our
 * model are stored in native (little-endian on x86) order so that
 * `_u32[i]` directly gives lane i's value and `spu_preferred_u32(r)`
 * == `r._u32[0]`. The byte-swap below converts between the two on
 * every quadword load/store, mirroring what `spu_ls_read32` already
 * does explicitly. Doing the swap here (rather than in every channel
 * extractor) keeps the per-word `_u32[i]` semantics that the lifter
 * helpers in `spu_helpers.h` were written against. */
static inline u128 spu_ls_read128(const spu_context* ctx, uint32_t lsa)
{
    u128 v;
    lsa &= SPU_LS_MASK & ~0xFu;
    const uint8_t* p = &ctx->ls[lsa];
    for (int i = 0; i < 4; i++) {
        v._u32[i] = ((uint32_t)p[i*4]     << 24) |
                    ((uint32_t)p[i*4 + 1] << 16) |
                    ((uint32_t)p[i*4 + 2] <<  8) |
                    (uint32_t)p[i*4 + 3];
    }
    /* s38 arm-gate witness read-watch (env YZ_ARMGATE). The gs_task release state
     * machine apply_entry (LS 0xB088) emits a stopper release (0xB170->0x5EB8->0x5F00)
     * for a CODE==0 descriptor ONLY if LS[0xBD70]==key(r11) at the gate 0xB0C4/0xB0CC
     * (recomp_prx/gs_task.c:41037-41062). LS[0xBD70] is READ here (0xB0C4) but has NO
     * literal-address writer in the whole image -> the arm-gate witness. Log every
     * gate read: CODE(r3), key(r11), the witness value, the descriptor(r80) + its
     * first quad, and whether they MATCH -- so one boot shows whether the witness ever
     * equals the stuck stopper's key (and thus whether the descriptor can ever arm). */
    { static int ag = -1;
      extern char* getenv(const char*);
      if (ag < 0) { ag = getenv("YZ_ARMGATE") ? 1 : 0;
          if (ag) { fprintf(stderr, "[armgate] ARMED (LS 0xBD70 gate read/write watch, img0)\n"); fflush(stderr); } }
      if (ag && ctx->image_id == 0 && lsa == 0xBD70u) {
          static unsigned long agn = 0;
          if (agn < 3000) { agn++;
              uint32_t desc = ctx->gpr[80]._u32[0] & (SPU_LS_MASK & ~0xFu);
              const uint8_t* d = &ctx->ls[desc];
              uint32_t d0=((uint32_t)d[0]<<24)|((uint32_t)d[1]<<16)|((uint32_t)d[2]<<8)|d[3];
              uint32_t d1=((uint32_t)d[4]<<24)|((uint32_t)d[5]<<16)|((uint32_t)d[6]<<8)|d[7];
              uint32_t d2=((uint32_t)d[8]<<24)|((uint32_t)d[9]<<16)|((uint32_t)d[10]<<8)|d[11];
              uint32_t d3=((uint32_t)d[12]<<24)|((uint32_t)d[13]<<16)|((uint32_t)d[14]<<8)|d[15];
              fprintf(stderr, "[armgate] pc=%05X code=%08X key=%08X witness=%08X desc=%05X dq=%08X_%08X_%08X_%08X %s\n",
                      ctx->pc, ctx->gpr[3]._u32[0], ctx->gpr[11]._u32[0], v._u32[0], desc, d0, d1, d2, d3,
                      (v._u32[0]==ctx->gpr[11]._u32[0]) ? "MATCH" : "nomatch");
              fflush(stderr);
          }
      } }
    return v;
}

static inline void spu_ls_write128(spu_context* ctx, uint32_t lsa, u128 val)
{
    lsa &= SPU_LS_MASK & ~0xFu;
    /* s41 FLIGHT RECORDER (env YZ_FLTREC, consumer ctx only; spu_fltrec.h).
     * SPU LS stores are quadword-only (no scalar LS store in the ISA), so
     * this is the dominant STORE event source -- logged as 4 fixed-size
     * lane records (see spu_fltrec.h's header comment). */
    if (yz_fltrec_hot(ctx))
        yz_fltrec_store128(ctx, lsa, val._u32[0], val._u32[1], val._u32[2], val._u32[3]);
    /* s41 FLIGHT RECORDER dump trigger B (env YZ_FLTREC_DUMP_ON_SWEEP): fire
     * yz_fltrec_dump once on the first write to the static-vtable band
     * [0xBA00,0xBC00) from guest pcs 0x3520-0x3560 (the reinit sweep),
     * consumer ctx only. Independent of YZ_FLTREC being armed for general
     * recording -- either trigger can fire the dump. */
    { static int swd = -1;
      extern char* getenv(const char*);
      if (swd < 0) swd = getenv("YZ_FLTREC_DUMP_ON_SWEEP") ? 1 : 0;
      if (swd == 1 && ctx == (spu_context*)g_yz_consumer_ctx
          && lsa >= 0xBA00u && lsa < 0xBC00u
          && ctx->pc >= 0x3520u && ctx->pc <= 0x3560u) {
          swd = 2;   /* fire once */
          yz_fltrec_dump("sweep-write");
      } }
    /* Queue-line-copy write watch (env YZ_QLINE, 2026-07-02, diag — REMOVE with
     * the voice frontier): the codec's SpursQueue push commits {front=1,...}
     * where front must stay 0; log every lifted store to the GETLLAR line copy
     * at LS 0x80 (image 3) with pc + value to name the word-0 corruptor. */
    { static int qw = -1;
      extern char* getenv(const char*);
      if (qw < 0) qw = getenv("YZ_QLINE") ? 1 : 0;
      if (qw && lsa == 0x80u && ctx->image_id == 3) {
          static int qn = 0;
          if (qn < 40) { qn++;
              fprintf(stderr, "[qline] pc=0x%05X LS80 <= %08X %08X %08X %08X\n",
                      ctx->pc, val._u32[0], val._u32[1], val._u32[2], val._u32[3]);
              fflush(stderr);
          }
      } }
    /* s38 arm-gate witness WRITE-watch (env YZ_ARMGATE): catch any lifted store to
     * LS 0xBD70 (the apply_entry arm-gate witness read at 0xB0C4). If ZERO hits across
     * a boot AND the DMA watch is also silent, the witness is never written by the code
     * (stuck at init -> only descriptors whose key==init ever arm). If hit, this names
     * the writer pc + old->new so we can see whether it ever advances to the stuck key. */
    { static int agw = -1;
      extern char* getenv(const char*);
      if (agw < 0) agw = getenv("YZ_ARMGATE") ? 1 : 0;
      if (agw && ctx->image_id == 0 && lsa == 0xBD70u) {
          static unsigned long agwn = 0;
          if (agwn < 400) { agwn++;
              const uint8_t* q = &ctx->ls[lsa];
              uint32_t ow0=((uint32_t)q[0]<<24)|((uint32_t)q[1]<<16)|((uint32_t)q[2]<<8)|q[3];
              fprintf(stderr, "[armgate-wr] pc=%05X old=%08X new=%08X\n", ctx->pc, ow0, val._u32[0]);
              fflush(stderr);
          }
      } }
    /* s40 item-slot zero-store watch (env YZ_ZSLOT): the gs_task item-slot region
     * LS [0xBF00,0xC400) (image 0) holds the journal work items + their sub-objects;
     * two verified failure modes both start with quads in this region reading zero
     * (husk-dispatch crash / silent release loss — scratch/s40_evidence.md). The old
     * [ls-wipe] watch is blind here (window ends 0xBE00, DMA-only, >=64B-only). Log
     * every LIFTED store of an ALL-ZERO quad into the region with writer pc + spu.
     * A zero from game code doing legal retirement names the pc just the same —
     * the pc discriminates wiper vs legit retire. */
    { static int zs = -1;
      extern char* getenv(const char*);
      if (zs < 0) { zs = getenv("YZ_ZSLOT") ? 1 : 0;
          if (zs) { fprintf(stderr, "[z-slot] ARMED (zero-quad stores into LS [0xBF00,0xC400) img0)\n"); fflush(stderr); } }
      /* s40 late: lower bound extended 0xBF00 -> 0xB800 (s40_husk_residual.md — the
       * static vtables at 0xBA88/0xBAA8 live in [0xB800,0xBC00) and one got zeroed). */
      if (zs && ctx->image_id == 0 && lsa >= 0xB800u && lsa < 0xC400u
          && val._u32[0] == 0 && val._u32[1] == 0 && val._u32[2] == 0 && val._u32[3] == 0) {
          static unsigned long zn = 0; zn++;
          if (zn <= 4000 || (zn & 63) == 0) {
              fprintf(stderr, "[z-slot] st pc=%05X lsa=0x%05X spu=%X n=%lu\n",
                      ctx->pc, lsa, ctx->spu_id, zn);
              fflush(stderr);
          }
      } }
    /* s40 singleton-slot rotation watch (env YZ_BD70W): LS 0xBD70 (image 0 = gs_task)
     * caches the ONE item-slot address whose stage-2 release path may fire (0xB088's
     * identity gate). The s39 stg2 probe measured items retrying as no-ops until the
     * cache rotates to them. This watch is UNCAPPED change-only (the old YZ_ARMGATE
     * watch above caps at 400 — useless at the minutes-in wedge, LESSONS #6d):
     * logs every old!=new write (decimated 1/16 after 20000) + a heartbeat with the
     * current value + total write count every 4096 writes, so a FROZEN rotation at
     * the wedge is measurable, not inferred from silence. */
    /* s40b [anch-wr]: lifted-store watch on the journal-consumer poll-EA home
     * (obj+0x1C, published at runtime by the [anch] probe at gs_task 0x6380 --
     * 0 until discovered). Logs pc + old/new word; ALWAYS when either is zero
     * (the wipe / the rebuild), sampled otherwise (the cursor advance rewrites
     * it once per consumed line). Rides YZ_BD70W. */
    { static int aw = -1;
      extern char* getenv(const char*);
      extern uint32_t g_yz_anch_home;
      if (aw < 0) aw = getenv("YZ_BD70W") ? 1 : 0;
      if (aw && ctx->image_id == 0 && g_yz_anch_home &&
          lsa <= g_yz_anch_home && g_yz_anch_home + 4u <= lsa + 16u) {
          const uint8_t* op = &ctx->ls[g_yz_anch_home];
          uint32_t oldw = ((uint32_t)op[0]<<24)|((uint32_t)op[1]<<16)|((uint32_t)op[2]<<8)|op[3];
          uint32_t bo = g_yz_anch_home - lsa;
          uint32_t neww = val._u32[bo >> 2];
          if (oldw != neww) {
              static unsigned long awn = 0; awn++;
              if (oldw == 0 || neww == 0 || awn <= 64 || (awn & 63u) == 0) {
                  fprintf(stderr, "[anch-wr] pc=%05X old=%08X new=%08X spu=%X n=%lu\n",
                          ctx->pc, oldw, neww, ctx->spu_id, awn);
                  fflush(stderr);
              }
          }
      } }
    /* s40b iter-6 [vtbl-wr]: ALL lifted stores into the static-vtable cells
     * [0xBA80,0xBAC0) with pc + first word — the sweep (zeros) and the rebuild
     * (nonzero) both visible, so the death's last-writer is attributable.
     * Rides YZ_BD70W. */
    { static int vw = -1;
      extern char* getenv(const char*);
      if (vw < 0) vw = getenv("YZ_BD70W") ? 1 : 0;
      if (vw && ctx->image_id == 0 && lsa >= 0xBA80u && lsa < 0xBAC0u) {
          static unsigned long vwn = 0; vwn++;
          if (vwn <= 400 || (vwn & 31) == 0) {
              fprintf(stderr, "[vtbl-wr] pc=%05X lsa=0x%05X w0=%08X%s spu=%X n=%lu\n",
                      ctx->pc, lsa, val._u32[0],
                      (val._u32[0]|val._u32[1]|val._u32[2]|val._u32[3]) == 0 ? " ZERO" : "",
                      ctx->spu_id, vwn);
              fflush(stderr);
          }
      } }
    /* s40b iter-5 [feed-wr]: the rotation's FEEDSTOCK list at mgr+0x110
     * (LS 0xBDA0..0xBDD0). c7AC4 POPS it (zeroing cells) and halts at n~5,500
     * every boot — who PUSHES into it, and why does that stop at the phase
     * transition? Log every write that lands a NONZERO word in the range
     * (pushes), change-only per cell; zero-writes (pops) counted only.
     * Rides YZ_BD70W. */
    { static int fw = -1;
      extern char* getenv(const char*);
      if (fw < 0) { fw = getenv("YZ_BD70W") ? 1 : 0;
          if (fw) { fprintf(stderr, "[feed-wr] ARMED (mgr+0x110 push witness)\n"); fflush(stderr); } }
      if (fw && ctx->image_id == 0 && lsa >= 0xBDA0u && lsa < 0xBDD0u) {
          static unsigned long fwn = 0, fzn = 0;
          int nz = (val._u32[0] | val._u32[1] | val._u32[2] | val._u32[3]) != 0;
          if (nz) { fwn++;
              if (fwn <= 200 || (fwn & 63) == 0) {
                  fprintf(stderr, "[feed-wr] pc=%05X lsa=0x%05X q=%08X %08X %08X %08X n=%lu z=%lu spu=%X\n",
                          ctx->pc, lsa, val._u32[0], val._u32[1], val._u32[2], val._u32[3],
                          fwn, fzn, ctx->spu_id);
                  fflush(stderr);
              }
          } else { fzn++;
              if ((fzn & 511) == 0) {
                  fprintf(stderr, "[feed-wr] zero-writes n=%lu (pushes=%lu) spu=%X\n", fzn, fwn, ctx->spu_id);
                  fflush(stderr);
              } } } }
    { static int bw = -1;
      extern char* getenv(const char*);
      if (bw < 0) { bw = getenv("YZ_BD70W") ? 1 : 0;
          if (bw) { fprintf(stderr, "[bd70-wr] ARMED (uncapped change-only + hb4096)\n"); fflush(stderr); } }
      if (bw && ctx->image_id == 0 && lsa == 0xBD70u) {
          static unsigned long bwn = 0, bwch = 0;
          const uint8_t* q = &ctx->ls[lsa];
          uint32_t ow0=((uint32_t)q[0]<<24)|((uint32_t)q[1]<<16)|((uint32_t)q[2]<<8)|q[3];
          bwn++;
          if (ow0 != val._u32[0]) {
              bwch++;
              if (bwch <= 20000 || (bwch & 15) == 0) {
                  fprintf(stderr, "[bd70-wr] pc=%05X old=%08X new=%08X n=%lu ch=%lu\n",
                          ctx->pc, ow0, val._u32[0], bwn, bwch);
                  fflush(stderr);
              }
              /* s40b review-1's missing measurement: the compaction's ELECTION
               * INPUT at front changes — all 12 item word0 markers + the mgr
               * counts (+0xB4 pool, +0xC0 work). Every 64th change (~40/boot);
               * diffing the last healthy election vs the freeze names whether
               * the input is legitimate-but-strandable or corrupt. */
              if ((bwch & 63) == 0 || bwch <= 8) {
                  char eb[220]; int ep = 0;
                  for (int ei = 0; ei < 12; ei++) {
                      uint32_t a2 = 0xBDD0u + (uint32_t)ei*0x80u;
                      uint32_t w2 = ((uint32_t)ctx->ls[a2]<<24)|((uint32_t)ctx->ls[a2+1]<<16)|((uint32_t)ctx->ls[a2+2]<<8)|ctx->ls[a2+3];
                      ep += snprintf(eb + ep, sizeof(eb) - ep, " %08X", w2);
                  }
                  fprintf(stderr, "[elect] ch=%lu new=%08X w0:%s poolcnt=%02X workcnt=%02X\n",
                          bwch, val._u32[0], eb, ctx->ls[0xBD47], ctx->ls[0xBD53]);
                  fflush(stderr);
              }
          }
          if ((bwn & 4095) == 0) {
              fprintf(stderr, "[bd70-hb] cur=%08X n=%lu ch=%lu\n", val._u32[0], bwn, bwch);
              fflush(stderr);
          }
      } }
    /* s38 window-LIMIT write-watch (env YZ_ARMGATE): the consumer's poll-loop
     * advance (gs_task.c ~0x64E0) preserves cb1.word0/1/2 and only rewrites
     * cb1.word3 (curEA), so the window LIMIT = cb1.word2 (@LS 0xBCA0+8) is set
     * ELSEWHERE and gates the whole ring-slide (measured stuck at 0x41F12880 while
     * the producer head is ~52 lines ahead). Log every CHANGE to cb1.word2 (its
     * writer pc + old->new) so the last one names the stale-limit setter = the fix
     * locus (is it a once-only init, or a head-read that returns stale?). */
    { static int lw = -1;
      extern char* getenv(const char*);
      if (lw < 0) lw = getenv("YZ_ARMGATE") ? 1 : 0;
      if (lw && ctx->image_id == 0 && lsa == 0xBCA0u) {
          const uint8_t* q = &ctx->ls[lsa];
          uint32_t ow2 = ((uint32_t)q[8]<<24)|((uint32_t)q[9]<<16)|((uint32_t)q[10]<<8)|q[11];
          if (ow2 != val._u32[2]) {
              static unsigned long lwn = 0;
              if (lwn < 400) { lwn++;
                  fprintf(stderr, "[limwatch] pc=%05X cb1.word2(limit) %08X -> %08X (w0=%08X w1=%08X w3=%08X)\n",
                          ctx->pc, ow2, val._u32[2], val._u32[0], val._u32[1], val._u32[3]);
                  fflush(stderr); }
          }
      } }
    /* s34 CB-write watchpoint (env YZ_CBWATCH): gs_task's journal-consumer
     * control block sits at LS 0xBC90 (cb0: w1=walkctr) and 0xBCB0 (cb2:
     * w0=segment base, w1=counter2 = the real cursor index). At the wedge the
     * base advanced 7 segments then pinned at 0x41F30080 with counter2 stuck
     * BELOW 128 while advance() runs 82k+ times; the CB is NEVER DMA-reloaded
     * (measured), so a single-thread SPU store must revert the counter. This
     * catches the WRITER pc with zero assumptions: log every store to the CB
     * quads in the FREEZE window (g_yz_cadv > 45000), old vs new, capped. A pc
     * NOT in {064EC,064F4,064F8,06500,06528} that writes the counter DOWN is
     * the root. Reads ctx->ls BEFORE the store below (old value). */
    { static int cbw = -1;
      extern char* getenv(const char*);
      extern volatile unsigned long g_yz_cadv;
      if (cbw < 0) cbw = getenv("YZ_CBWATCH") ? 1 : 0;
      if (cbw && ctx->image_id == 0 && ctx->pc == 0x32BCu && lsa == 0xBCB0u) {
          static unsigned long cbn = 0;
          if (cbn < 400) { cbn++;
              const uint8_t* q = &ctx->ls[lsa];
              uint32_t ow0 = ((uint32_t)q[0]<<24)|((uint32_t)q[1]<<16)|((uint32_t)q[2]<<8)|q[3];
              uint32_t ow1 = ((uint32_t)q[4]<<24)|((uint32_t)q[5]<<16)|((uint32_t)q[6]<<8)|q[7];
              fprintf(stderr, "[cbw] pc=%05X lsa=%05X old=%08X_%08X new=%08X_%08X r3=%08X r7=%08X r8=%08X r16=%08X cadv=%lu\n",
                      ctx->pc, lsa, ow0, ow1, val._u32[0], val._u32[1],
                      ctx->gpr[3]._u32[0], ctx->gpr[7]._u32[0], ctx->gpr[8]._u32[0], ctx->gpr[16]._u32[0], g_yz_cadv);
              fflush(stderr);
          }
      } }
    uint8_t* p = &ctx->ls[lsa];
    for (int i = 0; i < 4; i++) {
        uint32_t w = val._u32[i];
        p[i*4]     = (uint8_t)(w >> 24);
        p[i*4 + 1] = (uint8_t)(w >> 16);
        p[i*4 + 2] = (uint8_t)(w >>  8);
        p[i*4 + 3] = (uint8_t)w;
    }
    /* DIAG (YZ_SPU_PROF): watch ActivateWorkload's write to the kernel-context
     * wklRunnable1 field (LS 0x1EC, word2 of the 0x1E0 quadword). Logs when the
     * rebuilt runnable mask is nonzero -- proves whether the rebuild produced
     * the expected 0xE0000000 (wids 0,1,2). */
    extern int g_spu_prof_on;
    /* DIAG: ActivateWorkload's wklStatus1 writeback (LS 0x2D90) is rare and
     * unique to ActivateWorkload. Dump the wklState1 line (0x2D80) it sees and
     * the status it computes, to tell whether it saw RUNNABLE(2) and rebuilt. */
    if (g_spu_prof_on && lsa == 0x2D90u) {
        extern unsigned long g_spu_wrun_log;
        if (g_spu_wrun_log < 40) {
            g_spu_wrun_log++;
            const uint8_t* st = &ctx->ls[0x2D80];   /* wklState1[0..15] copy */
            const uint8_t* nv = (const uint8_t*)&val; /* new wklStatus1[0..15] */
            fprintf(stderr, "[spu-aw] spu=%X state[0..7]=%02X%02X%02X%02X%02X%02X%02X%02X "
                    "newStatus(host bytes)=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                    ctx->spu_id, st[0],st[1],st[2],st[3],st[4],st[5],st[6],st[7],
                    nv[0],nv[1],nv[2],nv[3],nv[4],nv[5],nv[6],nv[7]);
            fflush(stderr);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Preferred slot extraction
 *
 * SPU instructions operate on the "preferred slot" of a 128-bit register,
 * which is the leftmost (highest-address in big-endian) element.
 * For word operations, preferred slot = element 0 of the _u32 array
 * (since our u128 stores big-endian element order).
 * -----------------------------------------------------------------------*/
static inline uint32_t spu_preferred_u32(const u128* reg)
{
    return reg->_u32[0];
}

static inline int32_t spu_preferred_s32(const u128* reg)
{
    return reg->_s32[0];
}

static inline float spu_preferred_f32(const u128* reg)
{
    return reg->_f32[0];
}

static inline uint64_t spu_preferred_u64(const u128* reg)
{
    return reg->_u64[0];
}

/* Create a register with a value splatted to the preferred word slot */
static inline u128 spu_make_preferred_u32(uint32_t val)
{
    u128 r;
    memset(&r, 0, sizeof(r));
    r._u32[0] = val;
    return r;
}

/* ---------------------------------------------------------------------------
 * Channel read/write helpers
 *
 * FIX 2 (2026-07-17): `count` is the ready-flag guarding `value` (see the
 * spu_channel comment above) -- release-store count AFTER value is written,
 * so a consumer's acquire-load of count that observes the new count also
 * observes the new value (message-passing / "release sequence" idiom, C11
 * 5.1.2.4p16-17). `value` itself stays a plain access; it never needs to be
 * atomic on its own; only ordering relative to `count` matters. Callers that
 * are same-thread-only (ch_out_mbox/ch_out_intr_mbox, per spu_wrch's own
 * comments) get the same ordering for free -- release/acquire on a variable
 * only that thread touches is a same-cost no-op on x86.
 * -----------------------------------------------------------------------*/
static inline void spu_channel_write(spu_channel* ch, uint32_t val)
{
    ch->value = val;
    atomic_store_explicit(&ch->count, 1u, memory_order_release);
}

static inline uint32_t spu_channel_read(spu_channel* ch)
{
    uint32_t val = ch->value;
    atomic_store_explicit(&ch->count, 0u, memory_order_release);
    return val;
}

static inline int spu_channel_has_data(const spu_channel* ch)
{
    /* Cast away const to call atomic_load_explicit: reading is non-mutating,
     * but the C11 atomic_load API's `object` parameter is `volatile A*`
     * (never `const`-qualified), same as any other atomic accessor here. */
    return atomic_load_explicit((_Atomic uint32_t*)&ch->count, memory_order_acquire) > 0;
}

/* ---------------------------------------------------------------------------
 * Tail-call trampoline
 *
 * A cross-function SPU branch (br / bi to another function) is a tail call:
 * it transfers control without growing the SPU call depth. The lifter must
 * not emit it as a nested host C call, or an infinite tail-call loop (e.g.
 * the SPURS scheduler) would overflow the host stack. Instead the tail
 * transfer sets g_spu_trampoline_fn and returns; an enclosing SPU_DRAIN loop
 * (after every call site, and at the host-thread driver) re-enters
 * iteratively, so such loops run at constant stack depth. Mirrors the PPU
 * lifter's g_trampoline_fn / DRAIN_TRAMPOLINE.
 * -----------------------------------------------------------------------*/
#if defined(_MSC_VER)
#  define SPU_THREAD_LOCAL __declspec(thread)
#else
#  define SPU_THREAD_LOCAL _Thread_local
#endif

extern SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
/* Current ctx of the SPU thread inside SPU_DRAIN -- lets spu_prof_hop() read LS
 * for targeted gate diagnostics (e.g. dumping a polled flag). Diagnostic only. */
extern SPU_THREAD_LOCAL spu_context* g_spu_cur_ctx;

/* Function-level spin profiler (spu_channels.c, env YZ_SPU_PROF). When the
 * gate is set, each trampoline re-entry is histogrammed by target LS address
 * so we can see which lifted SPURS functions the SPU threads spin in. Off by
 * default -- one predicted-not-taken branch per hop. */
extern int g_spu_prof_on;
void spu_prof_hop(void* fn);
/* Default-boot SPU taskset task launcher (spu_channels.c). On the non-prof path
 * it intercepts the policy's StartTask hop so cri_audio/gs_task launch without
 * YZ_SPU_PROF. Cheap: early-out unless the SPU's active image is the policy (2). */
void spu_task_launch_check(spu_context* ctx, void* fn);
/* Stop the SPU (spu_channels.c): sets ctx->status and longjmps to the host
 * thread driver's setjmp target, fully unwinding the lifted call stack. Used by
 * the lifter for self-loop traps (`br .`/`brsl .`), which hang the SPU forever
 * on real hardware. Logs under env YZ_HALT_LOG. */
void spu_halt(spu_context* ctx, int status);
/* Indirect-branch dispatcher (spu_channels.c): resolves ctx->pc to a lifted
 * function in the context's active image and runs it. Referenced by SPU_RET. */
void spu_indirect_branch(spu_context* ctx);

/* s39 channel-stall wake (spu_channels.c): signal the per-SPU wait CV so a
 * host thread blocked in spu_rdch (RdInMbox / RdSigNotify1/2 / RdEventStat)
 * re-checks its predicate immediately. MUST be called by every PPU/SPU-side
 * producer AFTER it makes such a predicate true (in-mbox/signal writes, event
 * status raises). The blocking waiter also re-polls every 10 ms, so a missed
 * wake only costs latency, never a permanent hang. No-op on non-Windows. */
void spu_ch_wake(spu_context* ctx);

/* Restore-on-host-return (s24 adopt-on-serve image model, ledger #51): the
 * lifted brsl/bisl call brackets save image_id in a call-site local and hand
 * it back here after the nested call + drain complete, so an image adopted
 * inside the callee (dispatcher wildcard serve, foreign-resident adoption,
 * tail-chain hops) cannot leak into the caller's continuation. The host C
 * stack of these locals IS the shadow image stack. Persistent switches the
 * restore would undo (the LS-0xA00 workload-module DMA) are re-applied at
 * dispatch time from ctx->module_img_a00. Kill-switch: YZ_NO_IMGSTACK. */
void spu_img_restore(spu_context* ctx, int32_t saved_img);

#define SPU_DRAIN(ctx) do {                                   \
        while (g_spu_trampoline_fn) {                          \
            void (*_tf)(spu_context*) = g_spu_trampoline_fn;   \
            g_spu_trampoline_fn = 0;                           \
            /* s41 FLIGHT RECORDER: this re-entry is a cross-function tail   \
             * branch; (ctx)->pc already holds the target (every trampoline \
             * setter assigns ctx->pc before g_spu_trampoline_fn -- see the  \
             * cri_audio.c-style setters). This is the ONE central pc-level \
             * hook site (function/tail-call granularity, not per intra-     \
             * function branch instruction -- see spu_fltrec.h). */         \
            if (yz_fltrec_hot(ctx)) yz_fltrec_branch((ctx), (ctx)->pc);      \
            /* s42 YZ_SPU_LOCKSTEP gate site (★REVIEW B): SPU_DRAIN, not      \
             * spu_indirect_branch -- see spu_lockstep.h. No-op when unset. */ \
            yz_lockstep_tick(ctx);                                          \
            if (g_spu_prof_on) { g_spu_cur_ctx = (ctx); spu_prof_hop((void*)_tf); } \
            else spu_task_launch_check((ctx), (void*)_tf);     \
            _tf(ctx);                                          \
        }                                                     \
    } while (0)

/* Depth-aware SPU link return (`bi $r0` / conditional `biz $r0` family).
 * $r0 is the ABI link register; the lifter models a matched brsl/bisl call as
 * a nested host C call, so the common return is a host `return` (cheap, keeps
 * loops iterative). That is only sound while the SPU-logical caller's host
 * frame is live. Two runtime mechanisms legally destroy those frames under a
 * still-running SPU: a cross-context task RESUME (yield -> context saved to
 * main memory -> re-dispatched later on a fresh top-level stack, possibly on
 * another SPU) and the host-call-depth restack unwind. After either, a bare
 * `return` surfaces at a dispatch drain that treats it as end-of-execution
 * (measured 2026-07-03: the CRI codec's resumed queue-pop died at its final
 * `bi $r0`, LS 0x20A40 -> [SPU] tid=0x2002 stopped, r0=0xA388 discarded).
 * host_depth counts the live lifted call frames: 0 => no frame can satisfy
 * this return -> dispatch to the link register instead, faithful to CBEA
 * `bi` semantics (branch to word 0 of RA; RPCS3 SPUThread BI does the same).
 * ctx->pc is set unconditionally so the continuation is always visible to
 * the dispatch machinery. */
#define SPU_RET(ctx) do {                                      \
        (ctx)->pc = (ctx)->gpr[0]._u32[0];                     \
        if ((ctx)->host_depth == 0)                            \
            g_spu_trampoline_fn = spu_indirect_branch;         \
        return;                                                \
    } while (0)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_CONTEXT_H */
