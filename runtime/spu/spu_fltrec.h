/*
 * ps3recomp - SPU FLIGHT RECORDER (phase 1 + s42 v2 multi-context)
 *
 * Low-overhead binary ring-buffer recorder that captures every LS store,
 * cross-function branch, channel op and DMA transfer in strict program
 * order, plus host-side "foreign write" events from the ctx save/restore
 * machinery that mutate a context's LS from outside its own instruction
 * stream. Built to replace sampled fprintf probes with a complete ordered
 * history dumped on the wedge signature (yz_fltrec_dump, below). Env-gated,
 * default OFF (YZ_FLTREC=1). See docs/FLAGS.md and
 * scratch/s41_fltrec_report.md for the phase-1 design rationale and measured
 * overhead, and scratch/s42_order_reconstruction.md for the sufficiency
 * verdict that drove the v2 changes below.
 *
 * v2 (s42, 2026-07-16) closes three gaps that verdict named:
 *  1. MULTI-CONTEXT: YZ_FLTREC_ALLCTX=1 records every registered lifted-SPU
 *     context, not just g_yz_consumer_ctx (yz_fltrec_hot below). The MAIN
 *     ring becomes a genuine multi-writer structure (atomic cursor, exactly
 *     like the FOREIGN ring always was) so this is safe; the cross-writer
 *     ORDER is reconstructible because every record's ring slot is assigned
 *     by one shared atomic fetch-add (`seq`, new field) -- physical ring
 *     position IS global arrival order, the same guarantee the FOREIGN ring
 *     already had for its multiple ctx save/restore writer threads.
 *  2. SOURCE IDENTITY: every record kind now carries `spu_id` (ctx->spu_id).
 *     For FOREIGN_WRITE specifically this names the ctx being WRITTEN INTO,
 *     which in every current call site (spu_channels.c) is also the ctx
 *     whose OWN host thread is performing the restore during its own
 *     dispatch -- a faithful writer identity for this codebase's actual
 *     call pattern, though NOT a generalized "which of N host threads did
 *     this" if a future call site ever writes into a ctx from a thread that
 *     isn't driving that ctx (none do today).
 *  3. DIRECT-CALL coverage: order_reconstruction measured the tag-dispatcher
 *     (spu_func_000070E8) alive-but-invisible because ordinary direct calls
 *     (tools/spu_lifter.py's brsl/bisl codegen, ~line 726-831) compile to a
 *     bare nested C call with NO runtime hook -- genuinely invisible without
 *     touching the lifter or generated code (out of scope here). The closest
 *     runtime-visible approximation: every such call site's RETURN bracket
 *     unconditionally calls spu_img_restore() (a runtime function, not
 *     generated code), so a new YZ_FR_CALL_RET record there gives the
 *     call/return BOUNDARY (not the callee's identity) for every direct AND
 *     indirect lifted call. YZ_FR_XIMG additionally records the existing
 *     cross-image adoption resolution (spu_channels.c's "[spu-ximg]"
 *     site) -- a workload-switch event the old BRANCH-only channel missed.
 *
 * Design notes (mostly unchanged from phase 1):
 *  - TWO rings. MAIN ring now ALWAYS uses an atomic cursor (previously
 *    single-writer-only, since only g_yz_consumer_ctx's pinned thread ever
 *    wrote it); the extra interlocked increment is negligible next to this
 *    feature's existing overhead and removing the special case avoids two
 *    divergent append paths. FOREIGN ring: unchanged, atomic cursor (the ctx
 *    save/restore machinery runs from whichever host thread is
 *    adopting/launching at that moment).
 *  - Every record is a fixed 28 bytes (yz_fltrec_rec, v2) so each ring is
 *    still a flat array: the dump is a raw sequential write, no per-record
 *    framing, no allocation in the hot path. v1 dumps on disk (16-byte
 *    records) stay readable -- see tools/fltrec_dump.py's format_version
 *    dispatch; the runtime itself only ever WRITES the current format.
 *  - STORE128 is logged as FOUR consecutive records (one per lane; aux =
 *    lane index 0..3, addr = the quadword's LS address, same pc on all
 *    four) rather than a variable-length record + side payload, so every
 *    record stays fixed-size and the ring stays a plain array. This is the
 *    dominant event kind (SPU stores are quadword-only -- there is no
 *    scalar LS store in the ISA).
 *  - DMA ops log TWO records per spec: kind DMA_GET/DMA_PUT (addr=LSA,
 *    value=EA low 32 bits) immediately followed by DMA_META (addr=EA high
 *    32 bits, value=raw cmd byte, aux=tag, len_or_ch=size).
 *  - A SYNC record (kind 0, addr=QPC low32, value=QPC high32) is emitted
 *    close to every YZ_FR_SYNC_INTERVAL-sized span of the MAIN ring's global
 *    sequence (checked via the low bits of the atomic `seq` a record was
 *    just assigned, so no extra shared counter is needed), so wall-clock
 *    time can be reconstructed from the ring alone without a per-record
 *    timestamp. In multi-writer (ALLCTX) mode the SYNC record can land one
 *    slot after the record whose boundary triggered it (a second writer may
 *    claim the intervening slot first) -- a documented, harmless
 *    approximation; the decoder already treats SYNC-to-time mapping as
 *    carry-forward, not exact.
 */
#ifndef SPU_FLTREC_H
#define SPU_FLTREC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spu_context;   /* full type in spu_context.h; only pointers needed here */

/* The recorded SPU: published by gs_task.c's poll-DMA probe (spu_channels.c
 * s40b). Declared once here so every hook translation unit sees the same
 * extern (previously each call site forward-declared it ad hoc). */
extern volatile void* g_yz_consumer_ctx;

enum {
    YZ_FR_SYNC          = 0,
    YZ_FR_STORE32       = 1,
    YZ_FR_STORE128      = 2,
    YZ_FR_BRANCH        = 3,
    YZ_FR_RDCH          = 4,
    YZ_FR_WRCH          = 5,
    YZ_FR_DMA_GET       = 6,
    YZ_FR_DMA_PUT       = 7,
    YZ_FR_DMA_META      = 8,
    YZ_FR_FOREIGN_WRITE = 9,
    /* v2 (s42) -- close the order-reconstruction sufficiency gaps, see the
     * header comment above and scratch/s42_order_reconstruction.md. */
    YZ_FR_XIMG          = 10,  /* cross-image adoption at a computed branch
                                  (spu_channels.c's "[spu-ximg] cross-image
                                  call" site): aux=from image_id,
                                  len_or_ch=to image_id, addr=LS target */
    YZ_FR_CALL_RET      = 11   /* call/return boundary via spu_img_restore --
                                  the closest runtime-visible approximation
                                  for direct-call coverage (records the
                                  RETURN edge, not the call's target/callee;
                                  see the note at spu_img_restore in
                                  spu_channels.c): aux=image_id before the
                                  call, len_or_ch=image_id after, addr=ctx->pc
                                  (the resumed address) */
};

/* Records land close to every YZ_FR_SYNC_INTERVAL entries on the main ring
 * (checked via the low bits of the atomic `seq` a record was just assigned --
 * must stay a power of two); a lower value gives finer wall-time resolution
 * at the cost of ring volume. */
#define YZ_FR_SYNC_INTERVAL 4096u

/* v2 (s42): 28-byte record -- adds `seq` (this ring's own atomic
 * fetch-add-assigned global sequence number, shared across every writer
 * context) and `spu_id` (ctx->spu_id, the source/subject identity) to the
 * phase-1 16-byte layout. v1 dumps already on disk keep their old 16-byte
 * records unread by the runtime (tools/fltrec_dump.py decodes both -- see
 * its format_version dispatch); the runtime only ever WRITES this v2 shape
 * now, so there is exactly one C struct, not two. */
#pragma pack(push, 1)
typedef struct yz_fltrec_rec {
    uint64_t seq;          /* v2: global monotonic sequence for THIS ring (main or foreign),
                              atomic fetch-add across every writer context */
    uint32_t pc;           /* ctx->pc at record time (0 for host-side FOREIGN_WRITE) */
    uint32_t spu_id;       /* v2: ctx->spu_id -- the writer/subject SPU's identity */
    uint8_t  kind;         /* YZ_FR_* */
    uint8_t  aux;          /* kind-specific: STORE128 lane 0..3 / DMA tag / foreign-write site / image ids */
    uint16_t len_or_ch;    /* kind-specific: channel id / DMA size / foreign-write length / image ids */
    uint32_t addr;         /* LS address / branch target / EA-high / QPC-low */
    uint32_t value;        /* stored/read/written value / EA-low / cmd / QPC-high / checksum */
} yz_fltrec_rec;
#pragma pack(pop)

/* g_yz_fltrec_on: -1 unarmed, 0 off, 1 on. Hot sites test this via
 * yz_fltrec_hot() below; only the very first call across the whole process
 * pays the getenv()+alloc cost (guarded by an atomic_flag in spu_fltrec.c so
 * a startup race can't double-allocate the rings). */
extern volatile int g_yz_fltrec_on;

/* v2 (s42) YZ_FLTREC_ALLCTX: 0 = phase-1 behavior (record ONLY
 * g_yz_consumer_ctx), 1 = record every registered lifted-SPU context. Read
 * once at arm time alongside g_yz_fltrec_on; -1/unarmed is never observed
 * from outside spu_fltrec.c (arming always resolves it to 0 or 1 first). */
extern volatile int g_yz_fltrec_allctx;

/* Arms the recorder from env (idempotent, thread-safe). Normally called
 * lazily by yz_fltrec_hot() the first time it sees g_yz_fltrec_on < 0;
 * exposed in case a future call site wants to arm eagerly. */
int yz_fltrec_enabled(void);

/* Cheap hot-path gate: the g_yz_fltrec_on check plus (only if armed) either
 * "any ctx" (ALLCTX) or the ctx==consumer pointer compare phase-1 used.
 * Static inline so the default (disabled) case is one predicted-not-taken
 * branch at every call site (spu_ls_write128/32, spu_wrch/rdch, mfc_submit,
 * SPU_DRAIN, spu_img_restore, the cross-image adopt site). */
static inline int yz_fltrec_hot(const void* ctx)
{
    int on = g_yz_fltrec_on;
    if (on == 0) return 0;
    if (on < 0) { if (!yz_fltrec_enabled()) return 0; }
    if (ctx == 0) return 0;
    if (g_yz_fltrec_allctx) return 1;
    return ctx == (const void*)g_yz_consumer_ctx;
}

void yz_fltrec_store32(struct spu_context* ctx, uint32_t lsa, uint32_t val);
void yz_fltrec_store128(struct spu_context* ctx, uint32_t lsa,
                         uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3);
void yz_fltrec_branch(struct spu_context* ctx, uint32_t target_pc);
void yz_fltrec_wrch(struct spu_context* ctx, uint32_t channel, uint32_t val);
void yz_fltrec_rdch(struct spu_context* ctx, uint32_t channel, uint32_t val);
void yz_fltrec_dma(struct spu_context* ctx, int is_put, uint32_t lsa,
                    uint32_t ea_lo, uint32_t ea_hi, uint32_t size,
                    uint32_t tag, uint32_t cmd);
/* site: 0=ctxsave hdr restore, 1=ctxsave region restore,
 *       2=shadow hdr restore, 3=shadow region restore,
 *       4=RO-guard heal memcpy (yz_task_segment_guard, spu_channels.c:368),
 *       5=BSS zero on fresh task entry (spu_image_zero_bss call site,
 *         spu_channels.c ~2816 -- one call per bss span),
 *       6=module staleness-guard reload memcpy (spu_channels.c ~2747)
 * (all in spu_channels.c). Sites 4-6 added 2026-07-16 (scratch/
 * s41_upstream_audit.md Probe 1) to close the flight recorder's blind spot:
 * phase-1 covered only the ctx save/restore memcpys (0-3) and missed these
 * three other host-side foreign LS writers. */
void yz_fltrec_foreign_write(struct spu_context* ctx, uint32_t lsa,
                              uint32_t len, int site);

/* v2 (s42) gap-3 hooks -- see the header comment above. */
void yz_fltrec_xadopt(struct spu_context* ctx, uint32_t addr, int from_img, int to_img);
void yz_fltrec_call_ret(struct spu_context* ctx, int32_t from_img, int32_t to_img);

/* Dump trigger: writes scratch/fltrec/<timestamp>_d<N>/{ring.bin,foreign.bin,
 * ls.bin,ctx.txt,arena.bin,meta.json}. No-op if never armed (g_yz_fltrec_on
 * != 1) or after 2 dumps in one process (guard). foreign.bin is an addition
 * beyond the originally specified file list, needed to preserve the
 * FOREIGN_WRITE history (see the header comment above and the report). */
void yz_fltrec_dump(const char* reason);

#ifdef __cplusplus
}
#endif
#endif /* SPU_FLTREC_H */
