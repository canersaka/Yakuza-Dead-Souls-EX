/*
 * ps3recomp - NIR adapter: the packet-path producer.
 *
 * Drives an rsx_dispatch register-file decoder from an NV4097 method
 * stream (flat (method, arg) pairs — e.g. an .rxs capture record stream —
 * or raw FIFO words via the built-in parser) and emits the normalized
 * Native Render IR through the shared rsx_nir_emitter. This is the
 * "existing decoder -> IR" bridge: the live renderer is untouched; the
 * adapter is an additional consumer of the same method stream.
 *
 * Method addresses are the consumer's flattened form: (subchannel << 13) |
 * engine-relative address, matching yz_rsx_method and the .rxs exporter.
 * Beyond what rsx_dispatch models, the adapter recognizes the ordered
 * synchronization and data-move methods the dispatcher stores as plain
 * register writes:
 *
 *   0x0110 WAIT_FOR_IDLE                    -> BARRIER
 *   0x1D6C SET_SEMAPHORE_OFFSET             -> (state for the two below)
 *   0x1D70 BACK_END_WRITE_SEMAPHORE_RELEASE -> SEMAPHORE_RELEASE
 *   0x1D74 TEXTURE_READ_SEMAPHORE_RELEASE   -> SEMAPHORE_RELEASE (tex path)
 *   0x17C8 CLEAR_REPORT_VALUE               -> REPORT (kind CLEAR)
 *   0x1800 GET_REPORT                       -> REPORT (kind GET)
 *   0xEB00/0xEB04 GCM user command          -> USER_COMMAND
 *
 *   NV406E FIFO-engine methods (raw address < 0x80, any subchannel):
 *   0x0050 SET_REFERENCE                    -> SET_REFERENCE
 *   0x0060 SET_CONTEXT_DMA_SEMAPHORE        -> (state)
 *   0x0064 SEMAPHORE_OFFSET                 -> (state)
 *   0x0068 SEMAPHORE_ACQUIRE                -> SEMAPHORE_ACQUIRE
 *   0x006C SEMAPHORE_RELEASE                -> SEMAPHORE_RELEASE
 *
 *   Data moves (subchannel-flattened, per the live consumer's own map:
 *   sub1=NV0039 0x2000+, sub3=NV3062 0x6000+, sub5=NV308A 0xA000+,
 *   sub6=NV3089 0xC000+):
 *   NV0039 BUFFER_NOTIFY (0x2328)           -> TRANSFER (BUFFER)
 *   NV3089 IMAGE_IN (0xC40C)                -> TRANSFER (SCALED)
 *   NV308A COLOR window (0xA400..0xAAFC)    -> TRANSFER (INLINE), flushed
 *     when the color run ends (any non-COLOR method) or at parse end.
 *
 * The FIFO front end handles increment/non-increment method headers and
 * NOP words in a LINEAR buffer. JUMP/CALL/RET control words are the live
 * consumer's job (they need the guest address space); the parser stops and
 * reports them rather than guessing.
 */

#ifndef PS3RECOMP_RSX_NIR_ADAPTER_H
#define PS3RECOMP_RSX_NIR_ADAPTER_H

#include "rsx_dispatch.h"
#include "rsx_nir_emitter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_NIR_ADAPTER_MAX_BATCHES 4096
#define RSX_NIR_ADAPTER_MAX_INLINE  4096   /* accumulated NV308A words      */

typedef struct rsx_nir_adapter {
    rsx_dispatch rsx;
    rsx_nir_emitter em;

    /* draw accumulation between BEGIN and END */
    u32 batches[RSX_NIR_ADAPTER_MAX_BATCHES * 2];
    u32 batch_count;
    u32 batch_overflow;          /* batches dropped past the cap (loud)    */
    u32 draw_indexed;
    u32 draw_mixed;              /* arrays + indexed inside one begin/end  */

    /* raw-FIFO front-end semaphore context (NV406E) */
    u32 fifo_semaphore_dma;
    u32 fifo_semaphore_offset;

    /* Resolved when NV4097 SET_RENDER_ENABLE is processed. Keeping the DMA
     * selector here, rather than deriving it during a later draw, preserves
     * the method-time report binding across section boundaries. */
    rsx_nir_render_condition render_condition;

    /* NV0039 buffer-copy staging (trigger: BUFFER_NOTIFY) */
    u32 m2mf_dma_in, m2mf_dma_out;
    u32 m2mf_offset_in, m2mf_offset_out;
    u32 m2mf_pitch_in, m2mf_pitch_out;
    u32 m2mf_line_length, m2mf_line_count;
    u32 m2mf_format;

    /* NV3062 destination-surface staging (consumed by NV308A/NV3089) */
    u32 s2d_dma_src, s2d_dma_dst;
    u32 s2d_color_format;
    u32 s2d_pitch;               /* src<<16 | dst                          */
    u32 s2d_offset_src, s2d_offset_dst;

    /* NV3089 scaled-image staging (trigger: IMAGE_IN 0xC40C) */
    u32 sif_dma_src;
    u32 sif_context_surface;
    u32 sif_color_conversion;
    u32 sif_color_format;
    u32 sif_operation;
    u32 sif_clip_point, sif_clip_size;
    u32 sif_out_point, sif_out_size;
    u32 sif_ds_dx, sif_dt_dy;
    u32 sif_in_size, sif_in_format, sif_in_offset;

    /* NV308A inline-color accumulation (flushed on any non-COLOR method) */
    u32 inline_words[RSX_NIR_ADAPTER_MAX_INLINE];
    u32 inline_count;
    u32 inline_first_index;      /* (method - 0xA400) >> 2 of first word   */
    u32 inline_point, inline_size_out, inline_size_in;
    u32 inline_overflow;

    /* stats */
    u32 methods_seen;
    u32 actions_seen;

    /* Shadow mode (rsx_nr_intercept): mirror every method into the
     * register file and emitter staging WITHOUT emitting action ops —
     * the FIFO path owns execution of shadowed commands. State-group
     * knowledge stays fresh for the next native action. */
    int shadow_mode;
} rsx_nir_adapter;

void rsx_nir_adapter_init(rsx_nir_adapter* ad, rsx_nir_stream* out);
void rsx_nir_adapter_init_sink(rsx_nir_adapter* ad, const rsx_nir_sink* out);

/* rsx_dispatch embeds callbacks whose user pointer targets the containing
 * adapter. A bytewise adapter snapshot therefore must be rebound after every
 * copy before dispatch-based clear/draw/present methods are consumed. */
void rsx_nir_adapter_rebind(rsx_nir_adapter* ad);

/* Seed the underlying register file (captured initial state). */
void rsx_nir_adapter_seed(rsx_nir_adapter* ad, const u32* regs, u32 reg_words,
                          const u32* vp, u32 vp_words,
                          const u32* constants, u32 constant_words);

/* Feed one already-expanded method write (e.g. an .rxs record). */
void rsx_nir_adapter_method(rsx_nir_adapter* ad, u32 method, u32 arg);

/* Whether a flattened method is represented completely by this adapter.
 * The transactional live path uses this before owning a whole FIFO section:
 * a stored-only/TODO register makes the complete section fall back before
 * any native action begins.  This query has no side effects. */
int rsx_nir_adapter_method_supported(
    const rsx_nir_adapter* ad, u32 method, u32 arg);

/* Emit one terminal action from an otherwise shadow-only adapter. State and
 * draw batches must already have been mirrored through the method path. The
 * adapter returns to shadow mode before this call returns. This lets a live
 * FIFO consumer own a dynamic/SPU-authored terminal action without also
 * running the legacy action sink. Returns 1 only when an action was emitted. */
int rsx_nir_adapter_shadow_action(rsx_nir_adapter* ad, u32 method, u32 arg);

/* Flush any pending NV308A inline-color run (call at stream end; the
 * method path flushes automatically when a non-COLOR method arrives). */
void rsx_nir_adapter_finish(rsx_nir_adapter* ad);

/* Stage the register file's complete decoded state into the emitter's
 * pending set (no ops emitted until the next action flush). The native
 * interception layer calls this before emitting a typed action so the
 * action observes all state that arrived through the shadowed FIFO. */
void rsx_nir_adapter_stage_state(rsx_nir_adapter* ad);

/* Feed raw FIFO words from a linear buffer. Returns the number of words
 * consumed; a JUMP/CALL/RET control word stops the parse (the return value
 * then indexes that word, and *stop_word receives it if non-NULL). */
u32 rsx_nir_adapter_fifo(rsx_nir_adapter* ad, const u32* words, u32 count,
                         u32* stop_word);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_NIR_ADAPTER_H */
