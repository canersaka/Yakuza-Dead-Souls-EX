/*
 * ps3recomp - Native Render IR (NIR)
 *
 * A typed, ordered intermediate representation of RSX rendering work,
 * independent of the NV4097 packet wire format. Two producers can emit it:
 *
 *   1. The packet path: guest FIFO words -> rsx_dispatch (register-file
 *      decoder) -> rsx_nir_adapter -> NIR stream.
 *   2. A native interception path: typed calls placed at a recognized
 *      command-producing wrapper -> rsx_nir_emitter -> NIR stream, with no
 *      packet encode/decode round trip.
 *
 * Equivalence between the two producers is checked by folding each stream
 * into a sequence of ACTION records (clear/draw/transfer/present/semaphore/
 * report/reference/token/...), each carrying the complete normalized
 * pipeline state in effect at that action, and comparing the sequences
 * field by field. Emission order of state groups between two actions is
 * therefore not significant, but the order of actions (and all state they
 * observe) is strictly preserved.
 *
 * Storage comes in two modes:
 *   - growable (malloc) for offline capture-sized equivalence runs, and
 *   - FIXED-CAPACITY caller-provided storage for the live producer path:
 *     zero allocations after init; exhaustion sets a sticky .overflow that
 *     the interception layer must treat as "fall back to the FIFO path for
 *     this command family" -- commands are never silently dropped.
 *
 * The live submission transport is rsx_nr_ring.h (an ordered SPSC ring
 * carrying these same ops); this module holds only the representation,
 * normalization fold, and equivalence comparison. Ops deliberately carry
 * state DIFFS (one changed group per op), never complete pipeline
 * snapshots; the ~12 KB rsx_nir_pipeline exists only inside the offline
 * fold/compare machinery.
 *
 * This module touches no GPU API and no guest memory. Field layouts follow
 * the decoded (getter-level) state of rsx_dispatch.h, NOT the raw register
 * encodings, so a native backend can consume it without NV4097 knowledge.
 */

#ifndef PS3RECOMP_RSX_NIR_H
#define PS3RECOMP_RSX_NIR_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_NIR_NUM_VERTEX_ATTR     16
#define RSX_NIR_NUM_TEXTURES        16
#define RSX_NIR_NUM_VERTEX_TEXTURES 4
#define RSX_NIR_NUM_RENDER_TARGETS  4
#define RSX_NIR_NUM_CONSTANTS       512
#define RSX_NIR_VP_MAX_WORDS        (544 * 4)

/* Memory locations, matching rsx_dispatch.h RSX_LOCATION_* */
#define RSX_NIR_LOCATION_LOCAL  0
#define RSX_NIR_LOCATION_MAIN   1

/* ---------------------------------------------------------------------------
 * State groups. All fields are DECODED values (already unpacked from their
 * register encodings). A group is emitted as one op carrying the whole group.
 * -----------------------------------------------------------------------*/

/* Render-target + depth-target configuration */
typedef struct rsx_nir_surface {
    u32 color_format;                            /* gcm surface color fmt   */
    u32 depth_format;
    u32 raster_type;                             /* 1 pitch / 2 swizzle     */
    u32 clip_x, clip_y, clip_w, clip_h;
    u32 color_offset[RSX_NIR_NUM_RENDER_TARGETS];
    u32 color_pitch[RSX_NIR_NUM_RENDER_TARGETS];
    u32 color_location[RSX_NIR_NUM_RENDER_TARGETS];
    u32 color_target;                            /* RT_ENABLE selector      */
    u32 zeta_offset;
    u32 zeta_pitch;
    u32 zeta_location;
} rsx_nir_surface;

typedef struct rsx_nir_viewport {
    u32 x, y, w, h;
    float scale[4];
    float translate[4];
    float clip_min, clip_max;                    /* depth range             */
} rsx_nir_viewport;

typedef struct rsx_nir_scissor {
    u32 x, y, w, h;
} rsx_nir_scissor;

/* Rasterizer state + per-channel color mask */
typedef struct rsx_nir_raster {
    u32 cull_face_enable;
    u32 cull_face;                               /* GL enum (0x404/0x405)   */
    u32 front_face;                              /* GL enum (0x900/0x901)   */
    u32 color_mask;                              /* raw ARGB byte mask word */
    u32 mrt_color_mask;
} rsx_nir_raster;

typedef struct rsx_nir_depth_stencil {
    u32 depth_test_enable;
    u32 depth_func;
    u32 depth_write_enable;
    u32 stencil_test_enable;
    u32 stencil_func, stencil_ref, stencil_mask;
    u32 stencil_op_fail, stencil_op_zfail, stencil_op_zpass;
} rsx_nir_depth_stencil;

typedef struct rsx_nir_blend {
    u32 blend_enable;
    u32 sfactor;                                 /* packed rgb|alpha word   */
    u32 dfactor;
    u32 equation;
    u32 blend_color;
    u32 alpha_test_enable;
    u32 alpha_func;
    u32 alpha_ref;
} rsx_nir_blend;

/* Vertex (transform) program: identified by content hash + start slot.
 * The instruction words live in the stream's side buffer so a consumer can
 * recompile them; the hash makes comparison cheap and order-safe. */
typedef struct rsx_nir_vertex_program {
    u32 start_slot;                              /* VP_START_FROM_ID        */
    u32 word_count;                              /* uploaded instr words    */
    u32 words_ofs;                               /* side-buffer word offset */
    u64 hash;                                    /* FNV-1a over the words   */
    u32 attrib_input_mask;                       /* VP_ATTRIB_EN            */
    u32 attrib_output_mask;                      /* VP_RESULT_EN            */
} rsx_nir_vertex_program;

typedef struct rsx_nir_fragment_program {
    u32 offset;                                  /* byte offset             */
    u32 location;                                /* RSX_NIR_LOCATION_*      */
    u32 control;                                 /* SHADER_CONTROL word     */
} rsx_nir_fragment_program;

/* Vertex transform constants: one contiguous dirty range per op.
 * Values are vec4 f32 bit patterns in the side buffer (4 words per slot). */
typedef struct rsx_nir_constants {
    u32 first_slot;
    u32 slot_count;
    u32 words_ofs;                               /* side buffer, 4/slot     */
} rsx_nir_constants;

typedef struct rsx_nir_vertex_attr {
    u32 type;                                    /* RSX_VTX_TYPE_*; 0 = off */
    u32 size;
    u32 stride;
    u32 frequency;
    u32 offset;
    u32 location;
    float def[4];                                /* value when disabled     */
} rsx_nir_vertex_attr;

typedef struct rsx_nir_vertex_bindings {
    rsx_nir_vertex_attr attr[RSX_NIR_NUM_VERTEX_ATTR];
    u32 base_offset;                             /* VERTEX_DATA_BASE_OFFSET */
    u32 base_index;                              /* VB_ELEMENT_BASE         */
} rsx_nir_vertex_bindings;

typedef struct rsx_nir_index_binding {
    u32 offset;
    u32 location;
    u32 is_u32;
    u32 restart_enable;                          /* width-rule already applied */
    u32 restart_index;
} rsx_nir_index_binding;

/* One texture unit (fragment or vertex): decoded descriptor + sampler */
typedef struct rsx_nir_texture {
    u32 enabled;
    u32 offset;
    u32 location;
    u32 format;                                  /* gcm fmt byte + LN/UN    */
    u32 dimension;
    u32 cubemap;
    u32 mipmaps;
    u32 width, height;
    u32 pitch;
    u32 depth;
    u32 wrap;                                    /* raw sampler words       */
    u32 remap;
    u32 filter;
    u32 control0;
    u32 border_color;
} rsx_nir_texture;

/* ---------------------------------------------------------------------------
 * Ops
 * -----------------------------------------------------------------------*/

typedef enum rsx_nir_op_kind {
    RSX_NIR_OP_NONE = 0,

    /* state groups (payload = the group struct) */
    RSX_NIR_OP_SET_SURFACE,
    RSX_NIR_OP_SET_VIEWPORT,
    RSX_NIR_OP_SET_SCISSOR,
    RSX_NIR_OP_SET_RASTER,
    RSX_NIR_OP_SET_DEPTH_STENCIL,
    RSX_NIR_OP_SET_BLEND,
    RSX_NIR_OP_SET_VERTEX_PROGRAM,
    RSX_NIR_OP_SET_FRAGMENT_PROGRAM,
    RSX_NIR_OP_SET_CONSTANTS,       /* cumulative: successive ops layer     */
    RSX_NIR_OP_SET_VERTEX_BINDINGS,
    RSX_NIR_OP_SET_INDEX_BINDING,
    RSX_NIR_OP_SET_TEXTURE,         /* .unit selects the unit               */
    RSX_NIR_OP_SET_VERTEX_TEXTURE,  /* .unit selects the unit               */

    /* actions (ordered, observable) */
    RSX_NIR_OP_CLEAR,
    RSX_NIR_OP_DRAW,
    RSX_NIR_OP_TRANSFER,            /* NV0039 / NV3089 / NV308A data moves  */
    RSX_NIR_OP_BARRIER,
    RSX_NIR_OP_SEMAPHORE_RELEASE,
    RSX_NIR_OP_SEMAPHORE_ACQUIRE,
    RSX_NIR_OP_REPORT,
    RSX_NIR_OP_SET_REFERENCE,       /* NV406E SET_REFERENCE: flush + REF    */
    RSX_NIR_OP_USER_COMMAND,        /* 0xEB00 user-interrupt cause          */
    RSX_NIR_OP_TOKEN_SIGNAL,        /* native readiness token (producer)    */
    RSX_NIR_OP_TOKEN_WAIT,          /* native readiness token (dependency)  */
    RSX_NIR_OP_FALLBACK,            /* native<->FIFO ownership transition   */
    RSX_NIR_OP_PRESENT,

    RSX_NIR_OP_KIND_COUNT
} rsx_nir_op_kind;

/* CLEAR mask bits mirror rsx_dispatch.h RSX_CLEAR_* (Z/S/RGBA) */
typedef struct rsx_nir_clear {
    u32 mask;
    u32 color_value;                             /* A8R8G8B8                */
    u32 depth_value;                             /* 24-bit integer depth    */
    u32 stencil_value;                           /* 8-bit                   */
} rsx_nir_clear;

/* One draw = one BEGIN..END pair. Batches (the NV4097 multi-batch form)
 * live in the side buffer as {first,count} word pairs. */
typedef struct rsx_nir_draw {
    u32 primitive;                               /* RSX_PRIMITIVE_*         */
    u32 indexed;                                 /* 0 arrays / 1 indexed    */
    u32 batch_count;
    u32 batches_ofs;                             /* side buffer: 2 words ea */
    u32 total_count;                             /* sum of batch counts     */
} rsx_nir_draw;

/* Data-move classes. Field meanings by kind:
 *   BUFFER (NV0039): src/dst {location,offset,pitch}, line_length bytes x
 *     line_count lines, src_format/dst_format = per-line strides' element
 *     sizes on the wire (1 = tightly packed bytes).
 *   SCALED (NV3089 -> NV3062/swizzle dst): scaled/format-converting blit;
 *     in_* raw 16.16 source rect origin plus integer size, out_* integer
 *     dest rect, clip rect, ds_dx/dt_dy raw 16.16 steps, origin/interp the
 *     NV3089 sampling controls, src_format/dst_format the 2D color codes.
 *   INLINE (NV308A): word payload from the FIFO into a surface point;
 *     payload words in the side buffer.
 * Offsets are byte offsets into the location's space, matching dispatch. */
#define RSX_NIR_XFER_BUFFER 0
#define RSX_NIR_XFER_SCALED 1
#define RSX_NIR_XFER_INLINE 2

typedef struct rsx_nir_transfer {
    u32 kind;                                    /* RSX_NIR_XFER_*          */
    u32 src_location, src_offset, src_pitch;
    u32 dst_location, dst_offset, dst_pitch;
    u32 src_format, dst_format;
    u32 line_length, line_count;                 /* BUFFER                  */
    u32 in_x, in_y, in_w, in_h;                  /* SCALED (x/y raw 16.16)  */
    u32 out_x, out_y, out_w, out_h;
    u32 clip_x, clip_y, clip_w, clip_h;
    u32 ds_dx, dt_dy;                            /* raw 16.16 steps         */
    u32 origin, interpolator;
    u32 swizzle_log2_w, swizzle_log2_h;          /* swizzled dst shape      */
    u32 point_x, point_y, size_w, size_h;        /* INLINE dst rect         */
    u32 words_ofs, word_count;                   /* INLINE payload          */
} rsx_nir_transfer;

typedef struct rsx_nir_semaphore {
    u32 dma_context;                             /* semaphore DMA selector
                                                    (0x66616661 = labels)   */
    u32 offset;                                  /* semaphore ctx offset    */
    u32 value;
    u32 texture_read;                            /* release: 1 = 0x1D74 path
                                                    (word-swizzled write)   */
} rsx_nir_semaphore;

typedef struct rsx_nir_report {
    u32 kind;                                    /* 0 = GET, 1 = CLEAR      */
    u32 arg;                                     /* raw type|offset word    */
    u32 dma_report;                              /* CONTEXT_DMA_REPORT sel  */
} rsx_nir_report;

typedef struct rsx_nir_barrier {
    u32 kind;                                    /* 0 = WAIT_FOR_IDLE       */
} rsx_nir_barrier;

typedef struct rsx_nir_reference {
    u32 value;                                   /* REF register value      */
} rsx_nir_reference;

typedef struct rsx_nir_user_command {
    u32 cause;                                   /* handler cause argument  */
} rsx_nir_user_command;

/* Native readiness token: the producer-level replacement for a guest
 * publish/wait pair whose semantic dependency is understood (e.g. the
 * 0xFE0 "SPU data ready" label). SIGNAL publishes (token, value); WAIT
 * requires value >= .value for the same token before later ops execute.
 * Token ids are allocated by the interception layer (rsx_nr_intercept). */
typedef struct rsx_nir_token {
    u32 token;
    u32 value;
} rsx_nir_token;

/* Ownership transition between the native producer path and the FIFO
 * fallback path. Emitted by the interception layer so mixed segments stay
 * explicitly ordered and countable; never emitted by pure-capture replay. */
#define RSX_NIR_FALLBACK_ENTER 0                 /* native -> FIFO          */
#define RSX_NIR_FALLBACK_EXIT  1                 /* FIFO -> native          */

typedef struct rsx_nir_fallback {
    u32 dir;                                     /* RSX_NIR_FALLBACK_*      */
    u32 family;                                  /* rsx_nr command family   */
    u32 reason;                                  /* rsx_nr fallback reason  */
} rsx_nir_fallback;

typedef struct rsx_nir_present {
    u32 buffer;                                  /* flip queue argument     */
} rsx_nir_present;

typedef struct rsx_nir_op {
    u32 kind;                                    /* rsx_nir_op_kind         */
    u32 unit;                                    /* texture ops only        */
    union {
        rsx_nir_surface          surface;
        rsx_nir_viewport         viewport;
        rsx_nir_scissor          scissor;
        rsx_nir_raster           raster;
        rsx_nir_depth_stencil    depth_stencil;
        rsx_nir_blend            blend;
        rsx_nir_vertex_program   vertex_program;
        rsx_nir_fragment_program fragment_program;
        rsx_nir_constants        constants;
        rsx_nir_vertex_bindings  vertex_bindings;
        rsx_nir_index_binding    index_binding;
        rsx_nir_texture          texture;
        rsx_nir_clear            clear;
        rsx_nir_draw             draw;
        rsx_nir_transfer         transfer;
        rsx_nir_semaphore        semaphore;
        rsx_nir_report           report;
        rsx_nir_barrier          barrier;
        rsx_nir_reference        reference;
        rsx_nir_user_command     user_command;
        rsx_nir_token            token;
        rsx_nir_fallback         fallback;
        rsx_nir_present          present;
    } u;
} rsx_nir_op;

/* 1 for the ordered/observable op kinds, 0 for state groups. */
int rsx_nir_op_is_action(u32 kind);

/* ---------------------------------------------------------------------------
 * Stream: op array + u32 side buffer for bulk payloads.
 *
 * Growable mode (init): storage is malloc'd and doubles on demand; an
 * allocation failure sets .oom (sticky).
 * Fixed mode (init_fixed): caller-provided storage, ZERO allocations;
 * capacity exhaustion sets .overflow (sticky) and the push is refused.
 * Both flags make every later push fail, so a producer checks once per
 * command boundary and falls back; partial commands cannot linger.
 * -----------------------------------------------------------------------*/

typedef struct rsx_nir_stream {
    rsx_nir_op* ops;
    u32 op_count, op_cap;
    u32* side;                                   /* bulk payload words      */
    u32 side_count, side_cap;
    u32 oom;                                     /* sticky allocation fail  */
    u32 fixed;                                   /* 1 = caller storage      */
    u32 overflow;                                /* sticky capacity fail    */
} rsx_nir_stream;

void rsx_nir_stream_init(rsx_nir_stream* s);
void rsx_nir_stream_init_fixed(rsx_nir_stream* s,
                               rsx_nir_op* ops, u32 op_cap,
                               u32* side, u32 side_cap);
void rsx_nir_stream_free(rsx_nir_stream* s);
void rsx_nir_stream_reset(rsx_nir_stream* s);

/* Append one op. Bulk payloads must already be in the side buffer
 * (rsx_nir_side_push). Returns 0, or -1 on allocation/capacity failure
 * (sticky). */
int rsx_nir_push(rsx_nir_stream* s, const rsx_nir_op* op);

/* Copy words into the side buffer; returns their word offset (or ~0u). */
u32 rsx_nir_side_push(rsx_nir_stream* s, const u32* words, u32 count);

const u32* rsx_nir_side(const rsx_nir_stream* s, u32 ofs, u32 count);

/* ---------------------------------------------------------------------------
 * Sink: the emitter-facing output interface. rsx_nir_stream implements it;
 * rsx_nr_ring provides its own so the live producer path emits straight
 * into the submission ring with no intermediate copy.
 * -----------------------------------------------------------------------*/

typedef struct rsx_nir_sink {
    void* user;
    /* Both return 0 on success, -1 on refusal (sticky per the backing
     * store's contract). side_push returns the word offset or ~0u. */
    int (*push)(void* user, const rsx_nir_op* op);
    u32 (*side_push)(void* user, const u32* words, u32 count);
} rsx_nir_sink;

/* A sink writing into `s` (valid while `s` lives). */
rsx_nir_sink rsx_nir_stream_sink(rsx_nir_stream* s);

/* ---------------------------------------------------------------------------
 * Normalization + equivalence
 *
 * Folding replays a stream's state groups into a running rsx_nir_pipeline
 * and yields one rsx_nir_action per action op, carrying the complete state
 * in effect at that point. Two streams are equivalent iff their folded
 * action sequences are identical.
 * -----------------------------------------------------------------------*/

typedef struct rsx_nir_pipeline {
    rsx_nir_surface          surface;
    rsx_nir_viewport         viewport;
    rsx_nir_scissor          scissor;
    rsx_nir_raster           raster;
    rsx_nir_depth_stencil    depth_stencil;
    rsx_nir_blend            blend;
    rsx_nir_vertex_program   vertex_program;
    rsx_nir_fragment_program fragment_program;
    rsx_nir_vertex_bindings  vertex_bindings;
    rsx_nir_index_binding    index_binding;
    rsx_nir_texture          textures[RSX_NIR_NUM_TEXTURES];
    rsx_nir_texture          vertex_textures[RSX_NIR_NUM_VERTEX_TEXTURES];
    /* constants folded to their final values (4 words per slot) */
    u32 constants[RSX_NIR_NUM_CONSTANTS][4];
    u8  constants_written[RSX_NIR_NUM_CONSTANTS];
} rsx_nir_pipeline;

typedef struct rsx_nir_action {
    u32 kind;                                    /* action op kinds only    */
    u32 op_index;                                /* op position in stream   */
    union {
        rsx_nir_clear        clear;
        rsx_nir_draw         draw;
        rsx_nir_transfer     transfer;
        rsx_nir_semaphore    semaphore;
        rsx_nir_report       report;
        rsx_nir_barrier      barrier;
        rsx_nir_reference    reference;
        rsx_nir_user_command user_command;
        rsx_nir_token        token;
        rsx_nir_fallback     fallback;
        rsx_nir_present      present;
    } u;
    rsx_nir_pipeline state;
} rsx_nir_action;

void rsx_nir_pipeline_init(rsx_nir_pipeline* p);

/* Streaming fold: walk a stream action by action without materializing
 * every snapshot (a snapshot is ~12 KB; captures can hold thousands of
 * actions). The cursor's .state is valid after each successful next(). */
typedef struct rsx_nir_cursor {
    const rsx_nir_stream* s;
    u32 pos;
    rsx_nir_pipeline state;
} rsx_nir_cursor;

void rsx_nir_cursor_init(rsx_nir_cursor* c, const rsx_nir_stream* s);
/* Advance to the next action op. Returns 1 and fills *act (whose .state
 * is a copy of the folded pipeline) — or 0 at end of stream. */
int rsx_nir_cursor_next(rsx_nir_cursor* c, rsx_nir_action* act);

/* Fold a stream into malloc'd actions (caller frees *out). Returns the
 * action count, or -1 on allocation failure. Convenience wrapper over the
 * cursor for small streams; prefer the cursor for capture-sized input. */
int rsx_nir_fold(const rsx_nir_stream* s, rsx_nir_action** out);

/* Compare two streams for normalized equivalence. Returns 0 if equivalent;
 * otherwise -1 with a human-readable first divergence in errbuf. Draw
 * batch lists and vertex-program words are compared through the streams'
 * side buffers, so both streams must outlive the call. */
int rsx_nir_compare(const rsx_nir_stream* a, const rsx_nir_stream* b,
                    char* errbuf, unsigned errbuf_size);

/* FNV-1a 64 over words — the vertex-program identity hash. */
u64 rsx_nir_hash_words(const u32* words, u32 count);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_NIR_H */
