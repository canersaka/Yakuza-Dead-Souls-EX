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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>     /* channel/event cross-thread atomics */

#if defined(_MSC_VER)
#include <tmmintrin.h>
#endif

#ifndef YZ_SPU_SIMD_LS128
#define YZ_SPU_SIMD_LS128 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Context of the SPU that consumes the command journal; recorded by
 * mfc_submit (spu_dma.h), defined in spu_channels.c. */
extern volatile void* g_yz_consumer_ctx;

/* Portable 16-byte alignment (MSVC __declspec vs GCC/clang __attribute__). */
#if defined(_MSC_VER)
#  define SPU_ALIGN16 __declspec(align(16))
#else
#  define SPU_ALIGN16 __attribute__((aligned(16)))
#endif

/* Local store size: 256 KB */
#define SPU_LS_SIZE         (256 * 1024)
#define SPU_LS_MASK         (SPU_LS_SIZE - 1)

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

/* Lock-line (128-byte cache line) atomics: the SPURS kernel scheduler
 * claims workloads with GETLLAR/PUTLLC loops. */
#define MFC_PUTLLC_CMD      0xB4
#define MFC_PUTLLUC_CMD     0xB0
#define MFC_PUTQLLUC_CMD    0xB8
#define MFC_GETLLAR_CMD     0xD0

/* MFC_RdAtomicStat result values */
#define MFC_PUTLLC_SUCCESS  0
#define MFC_PUTLLC_FAILURE  1
#define MFC_PUTLLUC_SUCCESS 2
#define MFC_GETLLAR_SUCCESS 4

/* ---------------------------------------------------------------------------
 * Channel state
 *
 * `count` is a C11 atomic. RdInMbox, RdSigNotify1/2 and RdEventStat's
 * underlying storage (this field, and event_status below) have a real or
 * intended cross-thread producer: the PPU (sys_spu_thread_write_snr) or
 * another SPU (the coherence-loss LR raise) writes it while the owning SPU
 * host thread polls/blocks on it in spu_ch_ready/spu_ch_wait
 * (spu_channels.c). x86 TSO alone would make a producer's plain store
 * globally visible (and the 10 ms spu_ch_wait re-poll is a missed-wake
 * safety net), but that covers cross-CPU visibility latency, not the
 * compiler's freedom to cache/reorder a plain load across loop iterations.
 * _Atomic plus an explicit memory_order closes that without weakening the
 * TSO/relaxed-visibility model (relaxed is still used wherever that argument
 * applies). `count` is used as a ready-flag guarding `value` (message-passing
 * idiom): the producer stores value then releases count; the consumer
 * acquire-loads count and, only once it observes nonzero, reads value, so
 * `value` itself does not need to be atomic. spu_channel is shared by all
 * four mailbox/signal instances (ch_out_mbox and ch_out_intr_mbox are
 * same-thread only per spu_wrch's own comments) with one struct definition,
 * so they get the same atomic field; that is harmless for the same-thread
 * instances (default sequentially-consistent ops on a single-thread-owned
 * atomic are just a locked no-op on x86, never a behavior change). */
typedef struct spu_channel {
    uint32_t          value;
    _Atomic uint32_t  count;   /* number of valid entries (0 or 1 for most channels) */
} spu_channel;
/* Struct-layout guard (spu_context's append-only rule, see below): atomic_uint
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

    /* 14-bit stop-and-signal code from the last stop/stopd (insn bits 18:31,
     * & 0x3FFF; stopd is fixed at 0x3FFF). Set by lifted stop/stopd codegen so
     * lv2 can dispatch the SYS_SPU_THREAD_STOP_* protocol (0x101 GROUP_EXIT /
     * 0x102 THREAD_EXIT pop the OutMbox for the guest-written exit status,
     * 0x110 RECEIVE_EVENT, ...). Mirrors SPU_Status.StopCode. */
    uint32_t stop_code;

    /* SPU thread identification */
    uint32_t spu_id;
    uint32_t spu_group_id;

    /* Active recompiled image for per-context indirect-branch dispatch. SPURS
     * loads kernel/policy/job into overlapping LS addresses at different times,
     * so spu_indirect_branch resolves pc within the image currently selected
     * here. 0 = match any image (back-compat for single-image contexts). */
    int image_id;

    /* SRR0 - Save/Restore Register (exception return address) */
    uint32_t srr0;

    /* Event status / mask. event_status is a C11 atomic: it has two writer
     * classes, the owning SPU thread itself (WrEventAck's clear, and the MFC
     * engine's own SN/LR self-raises in spu_dma.h) and a genuinely
     * cross-thread writer (spu_coh_notify_write's LR raise, called from the
     * PPU coherence-write path in another thread). Both `|=`/`&=` writer
     * sites go through atomic_fetch_or_explicit/atomic_fetch_and_explicit so
     * the read-modify-write itself is a single atomic op (closing a possible
     * lost-update between the two writer classes) in addition to the
     * spu_lockline_lock serialization (see SPU_WrEventAck in spu_channels.c);
     * belt-and-suspenders, not a replacement for the lock. event_mask has
     * exactly one writer, the owning SPU thread's own WrEventMask
     * (spu_channels.c), so it stays a plain uint32_t (only fields with a real
     * cross-thread writer are atomic). */
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

    /* ---- Fields below this line were appended (decrementer, DMA-list
     * stall-and-notify). Struct layout rule: append only; nothing above this
     * comment may move or change size. ---- */

    /* Decrementer. It is a free-running 32-bit down counter at the PPE
     * timebase rate (79.8 MHz on PS3) that starts counting at SPU thread
     * creation, not at the first `wrch SPU_WrDec`: the lifted images read
     * `rdch SPU_RdDec` at many sites with no matching `wrch SPU_WrDec`, so a
     * WrDec-triggered-only model would leave every read frozen forever.
     * dec_running is set at spu_context_init() and again on any
     * `wrch SPU_WrDec`. dec_start_tb is the guest timebase
     * (ppu_timebase_now(), the same 79.8 MHz clock as the PPU
     * decrementer/mftb) sampled at the moment dec_value was last set;
     * SpuRdDec computes `dec_value - (ppu_timebase_now() - dec_start_tb)`
     * (see spu_channels.c SPU_RdDec / SPU_WrDec). dec_running (0 = frozen)
     * models the ack-while-masked stop rule (WrEventAck of the Tm bit while
     * Tm is disabled in the event mask stops the count; re-armed by the next
     * WrDec). Decrementer event generation (Tm, status bit 0x20, MSb 0->1
     * edge) is intentionally not implemented; see the note at SPU_RdDec in
     * spu_channels.c. */
    uint64_t dec_start_tb;   /* guest timebase (ppu_timebase_now()) at the last WrDec/thread-start */
    uint32_t dec_value;      /* value written (or the POR initial value at thread start) */
    int      dec_running;    /* 0 = frozen (stopped by ack-while-masked) */

    /* Channel-stall contract: per-SPU wake mechanism for the blocking rdch
     * path (spu_ch_wait/spu_ch_wake, spu_channels.c). Storage for a Win32
     * CONDITION_VARIABLE + SRWLOCK; each is a single pointer-sized struct
     * whose all-zero state is the documented init value
     * (CONDITION_VARIABLE_INIT / SRWLOCK_INIT == {0}), so the spu_context_init /
     * lv2_register calloc+memset zero-init is a valid initialization and no
     * explicit Init call is needed. Declared as void* to keep <windows.h> out of
     * this shared header (the helpers in spu_channels.c cast &field). On non-
     * Windows these stay unused (the POSIX wait path polls). Appended per the
     * struct's append-only rule; nothing above moves. */
    void* ch_wait_cv;     /* CONDITION_VARIABLE */
    void* ch_wait_lock;   /* SRWLOCK */

    /* Floating-point status/control state. Arithmetic helpers currently use
     * fixed execution rules, but task save/restore code still relies on these
     * bits surviving fscrrd/fscrwr round trips. */
    SPU_ALIGN16 u128 fpscr;

    /* Firmware-free SPURS task-system-call bridge. Appended so generated
     * mirrors of the historical prefix keep their offsets. A native task
     * reaches the documented task syscall target (LS 0x0a70); the indirect
     * branch dispatcher hands the live architectural context to this callback.
     * The opaque pointer is host side-table state and is never stored in guest
     * memory. */
    int (*native_spurs_syscall)(struct spu_context*, void*);
    void* native_spurs_opaque;

    /* Set before the per-context MFC registry entry is retired.  Acquirers
     * re-check it while holding the registry lock so teardown cannot unlink a
     * slot and let a racing channel operation create a second one. */
    _Atomic uint32_t mfc_retiring;


} spu_context;

/* Guest timebase clock (runtime/syscalls/sys_timer.c), 79.8 MHz, the same
 * clock the PPU decrementer/mftb use; the SPU decrementer runs at this rate.
 * Declared here (not spu_dma.h) since spu_context_init needs it to start the
 * decrementer at thread creation. */
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
     * WrDec. Power-on initial value is 0 unless
     * SYS_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE was requested at thread
     * creation; that option is a per-thread create flag our
     * sys_spu_thread_create path does not currently plumb through, so the
     * default (value 0) is used, matching the common case. */
    ctx->dec_value     = 0;
    ctx->dec_start_tb  = ppu_timebase_now();
    ctx->dec_running   = 1;
}

/* ---------------------------------------------------------------------------
 * Local store access helpers
 * -----------------------------------------------------------------------*/
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
 * `_u32[i]` directly gives lane i's value and the preferred word is
 * `r._u32[0]`. The byte-swap below converts between the two on
 * every quadword load/store, mirroring what `spu_ls_read32` already
 * does explicitly. Doing the swap here (rather than in every channel
 * extractor) keeps the per-word `_u32[i]` semantics that the lifter
 * helpers in `spu_helpers.h` were written against. */
static inline u128 spu_ls_read128(const spu_context* ctx, uint32_t lsa)
{
    u128 v;
    lsa &= SPU_LS_MASK & ~0xFu;
    const uint8_t* p = &ctx->ls[lsa];
#if YZ_SPU_SIMD_LS128 && (defined(_MSC_VER) || defined(__SSSE3__))
    /* Reverse bytes independently inside each architectural 32-bit word.
     * This is exactly the scalar conversion below expressed as one PSHUFB. */
    const __m128i bytes = _mm_loadu_si128(
        (const __m128i*)(const void*)p);
    const __m128i word_reverse = _mm_setr_epi8(
        3, 2, 1, 0, 7, 6, 5, 4,
        11, 10, 9, 8, 15, 14, 13, 12);
    _mm_storeu_si128((__m128i*)(void*)&v,
                     _mm_shuffle_epi8(bytes, word_reverse));
#else
    for (int i = 0; i < 4; i++) {
        v._u32[i] = ((uint32_t)p[i*4]     << 24) |
                    ((uint32_t)p[i*4 + 1] << 16) |
                    ((uint32_t)p[i*4 + 2] <<  8) |
                    (uint32_t)p[i*4 + 3];
    }
#endif
    return v;
}

static inline void spu_ls_write128(spu_context* ctx, uint32_t lsa, u128 val)
{
    lsa &= SPU_LS_MASK & ~0xFu;
    uint8_t* p = &ctx->ls[lsa];
#if YZ_SPU_SIMD_LS128 && (defined(_MSC_VER) || defined(__SSSE3__))
    const __m128i words = _mm_loadu_si128(
        (const __m128i*)(const void*)&val);
    const __m128i word_reverse = _mm_setr_epi8(
        3, 2, 1, 0, 7, 6, 5, 4,
        11, 10, 9, 8, 15, 14, 13, 12);
    _mm_storeu_si128((__m128i*)(void*)p,
                     _mm_shuffle_epi8(words, word_reverse));
#else
    for (int i = 0; i < 4; i++) {
        uint32_t w = val._u32[i];
        p[i*4]     = (uint8_t)(w >> 24);
        p[i*4 + 1] = (uint8_t)(w >> 16);
        p[i*4 + 2] = (uint8_t)(w >>  8);
        p[i*4 + 3] = (uint8_t)w;
    }
#endif
}

/* Create a register with a value in the preferred word slot (element 0 of
 * the _u32 array) and zero in the other lanes. */
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
 * `count` is the ready-flag guarding `value` (see the spu_channel comment
 * above): release-store count after value is written, so a consumer's
 * acquire-load of count that observes the new count also observes the new
 * value (message-passing / "release sequence" idiom). `value` itself stays a
 * plain access; it never needs to be atomic on its own; only ordering
 * relative to `count` matters. Callers that are same-thread-only
 * (ch_out_mbox/ch_out_intr_mbox, per spu_wrch's own comments) get the same
 * ordering for free; release/acquire on a variable only that thread touches
 * is a same-cost no-op on x86.
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

/* The SPU context running on this host thread, kept for crash attribution. */
extern SPU_THREAD_LOCAL spu_context* g_spu_cur_ctx;

/* Stop the SPU (spu_channels.c): sets ctx->status and longjmps to the host
 * thread driver's setjmp target, fully unwinding the lifted call stack. Used by
 * the lifter for self-loop traps (`br .`/`brsl .`), which hang the SPU forever
 * on real hardware. */
void spu_halt(spu_context* ctx, int status);
/* Indirect-branch dispatcher (spu_channels.c): resolves ctx->pc to a lifted
 * function in the context's active image and runs it. Referenced by SPU_RET. */
void spu_indirect_branch(spu_context* ctx);

/* A lifted nested call still has a host frame that will publish its own
 * return continuation.  Only a top-level/restacked return needs an explicit
 * dispatcher hop. */
static inline int spu_return_needs_dispatch(const spu_context* ctx)
{
    return ctx->host_depth == 0;
}

/* Channel-stall wake (spu_channels.c): signal the per-SPU wait CV so a
 * host thread blocked in spu_rdch (RdInMbox / RdSigNotify1/2 / RdEventStat)
 * re-checks its predicate immediately. Every PPU/SPU-side producer must call
 * it after making such a predicate true (in-mbox/signal writes, event
 * status raises). The blocking waiter also re-polls every 10 ms, so a missed
 * wake only costs latency, never a permanent hang. No-op on non-Windows. */
void spu_ch_wake(spu_context* ctx);

/* Release the lazily-created per-context MFC engine before its spu_context is
 * destroyed. Native SPURS tasks/jobs use heap contexts, while LV2 owns
 * longer-lived thread contexts; both lifecycles must retire the registry
 * entry before freeing the context storage. */
void spu_mfc_unregister(spu_context* ctx);
void spu_mfc_context_switch(spu_context* ctx);

/* Issue one fully specified MFC command without five separate host calls for
 * its parameter-channel writes.  This is architecturally equivalent to
 * MFC_LSA/EAH/EAL/Size/TagID followed by MFC_Cmd.  `channel_pcs` is not used
 * and may be NULL. */
void spu_mfc_issue_compact(spu_context* ctx,
                           uint32_t lsa, uint32_t eah, uint32_t eal,
                           uint32_t size, uint32_t tag, uint32_t cmd,
                           const uint32_t channel_pcs[6]);

/* Combine the standard nonblocking atomic-status sequence
 * rchcnt(MFC_RdAtomicStat) + conditional rdch(MFC_RdAtomicStat) into one
 * MFC-engine lookup. Bit 32 is the channel-ready result; bits 31:0 are the
 * consumed status when ready. */
uint64_t spu_mfc_read_atomic_status_compact(spu_context* ctx);

/* SPURS task-system calls that promise DMA completion use this runtime
 * boundary instead of reaching into the per-context MFC registry.  It waits
 * for every tag group through the same channel path used by lifted SPU code. */
void spu_task_wait_all_dma(spu_context* ctx);

/* Restore image_id after a nested call. The lifted brsl/bisl call brackets
 * save image_id in a call-site local and hand it back here once the callee and
 * its drain complete, so an image adopted inside the callee cannot leak into
 * the caller's continuation. */
void spu_img_restore(spu_context* ctx, int32_t saved_img);

#define SPU_DRAIN(ctx) do {                                   \
        while (g_spu_trampoline_fn) {                          \
            void (*_tf)(spu_context*) = g_spu_trampoline_fn;   \
            g_spu_trampoline_fn = 0;                           \
            _tf(ctx);                                          \
        }                                                     \
    } while (0)

/* Depth-aware SPU link return (`bi $r0` / conditional `biz $r0` family).
 * $r0 is the ABI link register; the lifter models a matched brsl/bisl call as
 * a nested host C call, so the common return is a host `return` (cheap, keeps
 * loops iterative). That is only sound while the SPU-logical caller's host
 * frame is live. Two runtime mechanisms legally destroy those frames under a
 * still-running SPU: a cross-context task resume (yield -> context saved to
 * main memory -> re-dispatched later on a fresh top-level stack, possibly on
 * another SPU) and the host-call-depth restack unwind. After either, a bare
 * `return` surfaces at a dispatch drain that treats it as end-of-execution.
 * host_depth counts the live lifted call frames: 0 => no frame can satisfy
 * this return -> dispatch to the link register instead, faithful to the
 * architectural `bi` semantics (branch to word 0 of RA).
 * ctx->pc is set unconditionally so the continuation is always visible to
 * the dispatch machinery. */
#define SPU_RET(ctx) do {                                      \
        (ctx)->pc = (ctx)->gpr[0]._u32[0];                     \
        if (spu_return_needs_dispatch(ctx))                    \
            g_spu_trampoline_fn = spu_indirect_branch;         \
        return;                                                \
    } while (0)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_CONTEXT_H */
