/* Strict-native producer-island / pass compiler. See rsx_nr_island.h and
 * docs/HANA_ISLAND_COMPILER.md.
 *
 * The scanner is a pure first pass: it parses packet headers, proves every
 * method supported, classifies each argument word structural or dynamic,
 * fingerprints the structural words, and finds the island boundary (one
 * terminal action, mirroring rsx_nr_graph's policy). Nothing is mutated
 * until the whole island has a verdict, so refusal is atomic. Ownership
 * then resyncs register truth (no emitter, no actions), derives the touched
 * state groups from the register file, patches dynamic values, and executes
 * the ops through rsx_nr_backend_stream_step. The rsx_nir_adapter never
 * decodes a compiler-owned island; it remains the equivalence oracle and
 * the owner of every delegated stretch.
 */

#include "rsx_nr_island.h"

#include <string.h>

/* ---- method constants (NV4097 wire numbers; single source for the
 * classification and group-touch tables in this module) ------------------ */

#define IM_NOP4097              0x0100
#define IM_WAIT_FOR_IDLE        0x0110
#define IM_DMA_COLOR1           0x018C
#define IM_DMA_COLOR0           0x0194
#define IM_DMA_ZETA             0x0198
#define IM_CTX_DMA_SEMAPHORE_3D 0x01A4
#define IM_CONTEXT_DMA_REPORT   0x01A8
#define IM_DMA_COLOR2           0x01B4
#define IM_DMA_COLOR3           0x01B8
#define IM_RT_HORIZ             0x0200
#define IM_RT_VERT              0x0204
#define IM_RT_FORMAT            0x0208
#define IM_COLOR0_PITCH         0x020C
#define IM_COLOR0_OFFSET        0x0210
#define IM_ZETA_OFFSET          0x0214
#define IM_COLOR1_OFFSET        0x0218
#define IM_COLOR1_PITCH         0x021C
#define IM_RT_ENABLE            0x0220
#define IM_ZETA_PITCH           0x022C
#define IM_COLOR2_PITCH         0x0280
#define IM_COLOR3_PITCH         0x0284
#define IM_COLOR2_OFFSET        0x0288
#define IM_COLOR3_OFFSET        0x028C
#define IM_ALPHA_TEST_ENABLE    0x0304
#define IM_ALPHA_FUNC           0x0308
#define IM_ALPHA_REF            0x030C
#define IM_BLEND_ENABLE         0x0310
#define IM_BLEND_SFACTOR        0x0314
#define IM_BLEND_DFACTOR        0x0318
#define IM_BLEND_COLOR          0x031C
#define IM_BLEND_EQUATION       0x0320
#define IM_COLOR_MASK           0x0324
#define IM_STENCIL_TEST_ENABLE  0x0328
#define IM_STENCIL_WRITE_MASK   0x032C
#define IM_STENCIL_FUNC         0x0330
#define IM_STENCIL_FUNC_REF     0x0334
#define IM_STENCIL_FUNC_MASK    0x0338
#define IM_STENCIL_OP_FAIL      0x033C
#define IM_STENCIL_OP_ZFAIL     0x0340
#define IM_STENCIL_OP_ZPASS     0x0344
#define IM_TWO_SIDED_STENCIL    0x0348
#define IM_BACK_STENCIL_MASK    0x034C
#define IM_BACK_STENCIL_FIRST   0x034C
#define IM_BACK_STENCIL_LAST    0x0364
#define IM_MRT_COLOR_MASK       0x0370
#define IM_DEPTH_BOUNDS_ENABLE  0x0380
#define IM_DEPTH_BOUNDS_MIN     0x0384
#define IM_DEPTH_BOUNDS_MAX     0x0388
#define IM_CLIP_MIN             0x0394
#define IM_CLIP_MAX             0x0398
#define IM_FP_ACTIVE_PROGRAM    0x08E4
#define IM_SCISSOR_HORIZ        0x08C0
#define IM_SCISSOR_VERT         0x08C4
#define IM_VERTEX_TEXTURE       0x0900
#define IM_VIEWPORT_HORIZ       0x0A00
#define IM_VIEWPORT_VERT        0x0A04
#define IM_VIEWPORT_TRANSLATE   0x0A20
#define IM_VIEWPORT_SCALE       0x0A30
#define IM_POLY_OFFSET_POINT_EN 0x0A60
#define IM_POLY_OFFSET_LINE_EN  0x0A64
#define IM_POLY_OFFSET_FILL_EN  0x0A68
#define IM_DEPTH_FUNC           0x0A6C
#define IM_DEPTH_WRITE_ENABLE   0x0A70
#define IM_DEPTH_TEST_ENABLE    0x0A74
#define IM_POLY_OFFSET_SCALE    0x0A78
#define IM_POLY_OFFSET_BIAS     0x0A7C
#define IM_TEXCOORD_CONTROL     0x0B40
#define IM_VP_UPLOAD_INST       0x0B80
#define IM_VTXBUF_OFFSET        0x1680
#define IM_INVALIDATE_VERTEX    0x1710
#define IM_VTX_CACHE_INVAL      0x1714
#define IM_VERTEX_DATA_BASE     0x1738
#define IM_VB_ELEMENT_BASE      0x173C
#define IM_VTXFMT               0x1740
#define IM_GET_REPORT           0x1800
#define IM_VERTEX_BEGIN_END     0x1808
#define IM_VB_VERTEX_BATCH      0x1814
#define IM_IDXBUF_OFFSET        0x181C
#define IM_IDXBUF_FORMAT        0x1820
#define IM_VB_INDEX_BATCH       0x1824
#define IM_CULL_FACE            0x1830
#define IM_FRONT_FACE           0x1834
#define IM_CULL_FACE_ENABLE     0x183C
#define IM_TEX_SIZE1            0x1840
#define IM_CLEAR_REPORT_VALUE   0x17C8
#define IM_TEX_OFFSET           0x1A00
#define IM_VTX_ATTR_4F          0x1C00
#define IM_FP_CONTROL           0x1D60
#define IM_SEMAPHORE_OFFSET_3D  0x1D6C
#define IM_BACK_END_SEM_RELEASE 0x1D70
#define IM_TEX_READ_SEM_RELEASE 0x1D74
#define IM_SHADER_WINDOW        0x1D88
#define IM_CLEAR_DEPTH_VALUE    0x1D8C
#define IM_CLEAR_COLOR_VALUE    0x1D90
#define IM_CLEAR_BUFFERS        0x1D94
#define IM_RESTART_INDEX_ENABLE 0x1DAC
#define IM_RESTART_INDEX        0x1DB0
#define IM_RENDER_ENABLE        0x1E98
#define IM_VP_UPLOAD_FROM_ID    0x1E9C
#define IM_VP_START_FROM_ID     0x1EA0
#define IM_VP_UPLOAD_CONST_ID   0x1EFC
#define IM_VP_UPLOAD_CONST      0x1F00
#define IM_FREQUENCY_DIVIDER    0x1FC0
#define IM_VP_ATTRIB_EN         0x1FF0
#define IM_VP_RESULT_EN         0x1FF4
#define IM_TRANSFORM_BRANCH     0x1FF8
#define IM_USER_COMMAND_CAUSE   0xEB00
#define IM_USER_COMMAND_FIRE    0xEB04
#define IM_HEAD_PACKAGE         0xE924
#define IM_GCM_DRIVER_FLIP      0xE944

#define IM406E_SET_REFERENCE    0x0050
#define IM406E_SET_CTX_DMA_SEM  0x0060
#define IM406E_SEMAPHORE_OFFSET 0x0064
#define IM406E_SEMAPHORE_ACQ    0x0068
#define IM406E_SEMAPHORE_REL    0x006C

#define IM0039_OFFSET_IN        0x230C
#define IM0039_BUFFER_NOTIFY    0x2328
#define IM308A_POINT            0xA304
#define IM308A_SIZE_OUT         0xA308
#define IM308A_SIZE_IN          0xA30C
#define IM308A_COLOR_FIRST      0xA400
#define IM308A_COLOR_LAST       0xAAFC
#define IM3089_IN_POINT         0xC40C

#define ISLAND_RING_SIZE 0x800000u
#define ISLAND_RING_MASK (ISLAND_RING_SIZE - 1u)

/* group bit for the fixed-group touch mask */
#define GBIT(kind) (1u << ((kind) - RSX_NIR_OP_SET_SURFACE))

/* ---- classification ---------------------------------------------------- */

/* Argument dynamism: a dynamic argument is excluded from the fingerprint
 * and never recompiles a template. Its value reaches execution either
 * through the register-truth resync + group/constant derivation, or through
 * an explicit action patch slot. Anything unlisted is structural. */
int rsx_nr_island_method_arg_is_dynamic(u32 method)
{
    method &= 0xFFFFCu;
    if (method < 0x100u)
        return method == IM406E_SET_REFERENCE ||
               method == IM406E_SEMAPHORE_OFFSET ||
               method == IM406E_SEMAPHORE_ACQ ||
               method == IM406E_SEMAPHORE_REL;
    switch (method) {
    case IM_COLOR0_OFFSET: case IM_COLOR1_OFFSET:
    case IM_COLOR2_OFFSET: case IM_COLOR3_OFFSET:
    case IM_ZETA_OFFSET:
    case IM_ALPHA_REF: case IM_BLEND_COLOR:
    case IM_STENCIL_FUNC_REF: case 0x0354u /* back ref */:
    case IM_DEPTH_BOUNDS_MIN: case IM_DEPTH_BOUNDS_MAX:
    case IM_CLIP_MIN: case IM_CLIP_MAX:
    case IM_SCISSOR_HORIZ: case IM_SCISSOR_VERT:
    case IM_FP_ACTIVE_PROGRAM:
    case IM_VERTEX_DATA_BASE: case IM_VB_ELEMENT_BASE:
    case IM_IDXBUF_OFFSET:
    case IM_RESTART_INDEX:
    case IM_CLEAR_COLOR_VALUE: case IM_CLEAR_DEPTH_VALUE:
    case IM_SEMAPHORE_OFFSET_3D:
    case IM_BACK_END_SEM_RELEASE: case IM_TEX_READ_SEM_RELEASE:
    case IM_GET_REPORT: case IM_CLEAR_REPORT_VALUE:
    case IM_VB_VERTEX_BATCH: case IM_VB_INDEX_BATCH:
    case IM_USER_COMMAND_CAUSE: case IM_USER_COMMAND_FIRE:
    case IM_HEAD_PACKAGE: case IM_GCM_DRIVER_FLIP:
    case IM308A_POINT: case IM308A_SIZE_IN:
        return 1;
    default:
        break;
    }
    if (method >= IM_VIEWPORT_HORIZ && method <= IM_VIEWPORT_VERT)
        return 1;
    if (method >= IM_VIEWPORT_TRANSLATE &&
        method < IM_VIEWPORT_SCALE + 0x10u)
        return 1;
    if (method >= IM_VTXBUF_OFFSET && method < IM_VTXBUF_OFFSET + 16u * 4u)
        return 1;
    if (method >= IM_VTX_ATTR_4F && method < IM_VTX_ATTR_4F + 16u * 0x10u)
        return 1;
    if (method >= IM_VP_UPLOAD_CONST && method < IM_VP_UPLOAD_CONST + 32u * 4u)
        return 1;
    /* texture / vertex-texture guest offsets (first register of each unit) */
    if (method >= IM_TEX_OFFSET && method < IM_TEX_OFFSET + 16u * 0x20u &&
        ((method - IM_TEX_OFFSET) & 0x1Cu) == 0u)
        return 1;
    if (method >= IM_VERTEX_TEXTURE &&
        method < IM_VERTEX_TEXTURE + 4u * 0x20u &&
        ((method - IM_VERTEX_TEXTURE) & 0x1Cu) == 0u)
        return 1;
    /* border colors (word 7 of each unit block) */
    if (method >= IM_TEX_OFFSET && method < IM_TEX_OFFSET + 16u * 0x20u &&
        ((method - IM_TEX_OFFSET) & 0x1Fu) == 0x1Cu)
        return 1;
    /* transfer staging values (dma selectors stay structural via default) */
    if (method >= IM0039_OFFSET_IN && method < IM0039_BUFFER_NOTIFY)
        return 1;                              /* offsets/pitches/len/fmt   */
    if (method == IM0039_BUFFER_NOTIFY)
        return 1;                              /* notify argument is inert  */
    if (method >= 0x6300u && method <= 0x630Cu)
        return 1;                              /* s2d fmt/pitch/offsets     */
    if (method >= 0xC300u && method <= 0xC31Cu)
        return 1;                              /* sif rects/steps           */
    if (method >= 0xC400u && method <= 0xC40Cu)
        return 1;                              /* sif in size/fmt/ofs/point */
    if (method >= IM308A_COLOR_FIRST && method <= IM308A_COLOR_LAST)
        return 1;                              /* inline payload            */
    return 0;
}

/* Fixed-group touch mask for one method (zero when the method feeds no
 * derived group). Texture units are reported separately. */
static u32 island_method_group_mask(u32 method)
{
    if (method < 0x100u)
        return 0;
    switch (method) {
    case IM_DMA_COLOR0: case IM_DMA_COLOR1: case IM_DMA_COLOR2:
    case IM_DMA_COLOR3: case IM_DMA_ZETA:
    case IM_RT_HORIZ: case IM_RT_VERT: case IM_RT_FORMAT:
    case IM_COLOR0_PITCH: case IM_COLOR1_PITCH:
    case IM_COLOR2_PITCH: case IM_COLOR3_PITCH:
    case IM_COLOR0_OFFSET: case IM_COLOR1_OFFSET:
    case IM_COLOR2_OFFSET: case IM_COLOR3_OFFSET:
    case IM_ZETA_OFFSET: case IM_ZETA_PITCH: case IM_RT_ENABLE:
        return GBIT(RSX_NIR_OP_SET_SURFACE);
    case IM_VIEWPORT_HORIZ: case IM_VIEWPORT_VERT:
    case IM_CLIP_MIN: case IM_CLIP_MAX:
        return GBIT(RSX_NIR_OP_SET_VIEWPORT);
    case IM_SCISSOR_HORIZ: case IM_SCISSOR_VERT:
        return GBIT(RSX_NIR_OP_SET_SCISSOR);
    case IM_CULL_FACE: case IM_FRONT_FACE: case IM_CULL_FACE_ENABLE:
    case IM_POLY_OFFSET_POINT_EN: case IM_POLY_OFFSET_LINE_EN:
    case IM_POLY_OFFSET_FILL_EN:
    case IM_POLY_OFFSET_SCALE: case IM_POLY_OFFSET_BIAS:
    case IM_COLOR_MASK: case IM_MRT_COLOR_MASK:
        return GBIT(RSX_NIR_OP_SET_RASTER);
    case IM_DEPTH_TEST_ENABLE: case IM_DEPTH_FUNC:
    case IM_DEPTH_WRITE_ENABLE:
    case IM_DEPTH_BOUNDS_ENABLE: case IM_DEPTH_BOUNDS_MIN:
    case IM_DEPTH_BOUNDS_MAX:
    case IM_STENCIL_TEST_ENABLE: case IM_STENCIL_FUNC:
    case IM_STENCIL_FUNC_REF: case IM_STENCIL_FUNC_MASK:
    case IM_STENCIL_WRITE_MASK: case IM_STENCIL_OP_FAIL:
    case IM_STENCIL_OP_ZFAIL: case IM_STENCIL_OP_ZPASS:
    case IM_TWO_SIDED_STENCIL:
        return GBIT(RSX_NIR_OP_SET_DEPTH_STENCIL);
    case IM_ALPHA_TEST_ENABLE: case IM_ALPHA_FUNC: case IM_ALPHA_REF:
    case IM_BLEND_ENABLE: case IM_BLEND_SFACTOR: case IM_BLEND_DFACTOR:
    case IM_BLEND_COLOR: case IM_BLEND_EQUATION:
        return GBIT(RSX_NIR_OP_SET_BLEND);
    case IM_RENDER_ENABLE:
        return GBIT(RSX_NIR_OP_SET_RENDER_CONDITION);
    case IM_FP_ACTIVE_PROGRAM: case IM_FP_CONTROL: case IM_SHADER_WINDOW:
        return GBIT(RSX_NIR_OP_SET_FRAGMENT_PROGRAM);
    case IM_VP_UPLOAD_FROM_ID: case IM_VP_START_FROM_ID:
    case IM_VP_ATTRIB_EN: case IM_VP_RESULT_EN: case IM_TRANSFORM_BRANCH:
        return GBIT(RSX_NIR_OP_SET_VERTEX_PROGRAM);
    case IM_VERTEX_DATA_BASE: case IM_VB_ELEMENT_BASE:
    case IM_FREQUENCY_DIVIDER:
        return GBIT(RSX_NIR_OP_SET_VERTEX_BINDINGS);
    case IM_IDXBUF_OFFSET: case IM_IDXBUF_FORMAT:
    case IM_RESTART_INDEX_ENABLE: case IM_RESTART_INDEX:
        return GBIT(RSX_NIR_OP_SET_INDEX_BINDING);
    default:
        break;
    }
    if (method >= IM_BACK_STENCIL_FIRST && method <= IM_BACK_STENCIL_LAST)
        return GBIT(RSX_NIR_OP_SET_DEPTH_STENCIL);
    if (method >= IM_TEXCOORD_CONTROL &&
        method < IM_TEXCOORD_CONTROL + 10u * 4u)
        return GBIT(RSX_NIR_OP_SET_FRAGMENT_PROGRAM);
    if (method >= IM_VP_UPLOAD_INST && method < IM_VP_UPLOAD_INST + 32u * 4u)
        return GBIT(RSX_NIR_OP_SET_VERTEX_PROGRAM);
    if (method >= IM_VIEWPORT_TRANSLATE &&
        method < IM_VIEWPORT_SCALE + 0x10u)
        return GBIT(RSX_NIR_OP_SET_VIEWPORT);
    if (method >= IM_VTXBUF_OFFSET && method < IM_VTXBUF_OFFSET + 16u * 4u)
        return GBIT(RSX_NIR_OP_SET_VERTEX_BINDINGS);
    if (method >= IM_VTXFMT && method < IM_VTXFMT + 16u * 4u)
        return GBIT(RSX_NIR_OP_SET_VERTEX_BINDINGS);
    if (method >= IM_VTX_ATTR_4F && method < IM_VTX_ATTR_4F + 16u * 0x10u)
        return GBIT(RSX_NIR_OP_SET_VERTEX_BINDINGS);
    return 0;
}

/* Fragment/vertex texture unit touched by a method, or -1. */
static int island_method_texture_unit(u32 method, int* vertex_unit)
{
    *vertex_unit = 0;
    if (method >= IM_TEX_OFFSET && method < IM_TEX_OFFSET + 16u * 0x20u)
        return (int)((method - IM_TEX_OFFSET) >> 5);
    if (method >= IM_TEX_SIZE1 && method < IM_TEX_SIZE1 + 16u * 4u)
        return (int)((method - IM_TEX_SIZE1) >> 2);
    if (method >= IM_VERTEX_TEXTURE &&
        method < IM_VERTEX_TEXTURE + 4u * 0x20u) {
        *vertex_unit = 1;
        return (int)((method - IM_VERTEX_TEXTURE) >> 5);
    }
    return -1;
}

/* Methods needing per-word handling beyond the table (action recognition,
 * upload-window latching, inline payload runs). */
static int island_method_is_special(u32 method)
{
    switch (method) {
    case IM_VERTEX_BEGIN_END:
    case IM_VB_VERTEX_BATCH:
    case IM_VB_INDEX_BATCH:
    case IM_CLEAR_BUFFERS:
    case IM0039_BUFFER_NOTIFY:
    case IM3089_IN_POINT:
    case IM_WAIT_FOR_IDLE:
    case IM_BACK_END_SEM_RELEASE:
    case IM_TEX_READ_SEM_RELEASE:
    case IM406E_SEMAPHORE_ACQ:
    case IM406E_SEMAPHORE_REL:
    case IM406E_SET_REFERENCE:
    case IM_GET_REPORT:
    case IM_CLEAR_REPORT_VALUE:
    case IM_USER_COMMAND_CAUSE:
    case IM_USER_COMMAND_FIRE:
    case IM_GCM_DRIVER_FLIP:
    case IM_VP_UPLOAD_CONST_ID:
    case IM_VP_UPLOAD_FROM_ID:
        return 1;
    default:
        break;
    }
    if (method >= IM_VP_UPLOAD_CONST &&
        method < IM_VP_UPLOAD_CONST + 32u * 4u)
        return 1;
    if (method >= IM_VP_UPLOAD_INST &&
        method < IM_VP_UPLOAD_INST + 32u * 4u)
        return 1;
    if (method >= IM308A_COLOR_FIRST && method <= IM308A_COLOR_LAST)
        return 1;
    return 0;
}

/* method_props bit layout (built once at init; the classification
 * functions above stay the single authority). The fixed-group mask spans
 * SET_SURFACE..SET_INDEX_BINDING inclusive = 12 bits. */
#define IPROP_DYNAMIC        0x1u
#define IPROP_SUPPORT_SHIFT  1u          /* RSX_NIR_SUPPORT_* (2 bits)     */
#define IPROP_BOUNDARY_SHIFT 3u          /* rsx_nr_graph boundary (2 bits) */
#define IPROP_GROUP_SHIFT    5u          /* 12-bit fixed-group mask        */
#define IPROP_GROUP_BITS     0xFFFu
#define IPROP_SPECIAL        0x20000u
#define IPROP_TEX_VALID      0x40000u
#define IPROP_TEX_VERTEX     0x80000u
#define IPROP_TEX_UNIT_SHIFT 20u         /* 5 bits                         */

static void island_build_props(rsx_nr_island_compiler* ic)
{
    const rsx_nir_adapter* const ad = ic->owner->adapter;
    for (u32 idx = 0; idx < 0x4000u; ++idx) {
        const u32 method = idx << 2;
        u32 p = 0;
        if (rsx_nr_island_method_arg_is_dynamic(method))
            p |= IPROP_DYNAMIC;
        p |= (u32)rsx_nir_adapter_method_support_class(ad, method)
             << IPROP_SUPPORT_SHIFT;
        p |= (u32)rsx_nr_graph_classify_method(method)
             << IPROP_BOUNDARY_SHIFT;
        p |= island_method_group_mask(method) << IPROP_GROUP_SHIFT;
        if (island_method_is_special(method))
            p |= IPROP_SPECIAL;
        {
            int vunit = 0;
            const int unit = island_method_texture_unit(method, &vunit);
            if (unit >= 0) {
                p |= IPROP_TEX_VALID;
                if (vunit)
                    p |= IPROP_TEX_VERTEX;
                p |= (u32)unit << IPROP_TEX_UNIT_SHIFT;
            }
        }
        ic->method_props[idx] = p;
    }
}

/* ---- scanner ----------------------------------------------------------- */

typedef enum island_scan_verdict {
    ISLAND_SCAN_OWN = 0,        /* complete island; extent/action filled   */
    ISLAND_SCAN_DELEGATE,       /* forward to the wrapped owner            */
    ISLAND_SCAN_WAIT,           /* published words end mid-island          */
    ISLAND_SCAN_EMPTY,          /* nothing published at all                */
} island_scan_verdict;

typedef struct island_scan {
    island_scan_verdict verdict;
    u32 delegate_reason;
    u32 word_count;             /* island extent in words                  */
    u32 method_count;
    u32 group_mask;
    u32 tex_mask;
    u32 vtex_mask;
    u32 const_upload_words;     /* VP-constant window words in the island  */
    u32 batch_count;
    u32 action_kind;            /* 0 = state-only island                   */
    u32 action_a;               /* per-kind structural payload             */
    u32 value_word;             /* island word index of the value arg      */
    u32 run_first_index;        /* single inline run                       */
    u32 run_word;               /* island word index of first payload word */
    u32 run_count;              /* raw payload words (pre-clamp)           */
    u64 fingerprint;
    /* resumable loop context (published FIFO words in [get, put) are
     * immutable under the ring contract, so a WAIT verdict may keep its
     * progress and continue when more words publish) */
    u32 io;
    u32 begin_open;
    u32 saw_batches;
    u32 latched_const_id;
    u32 saw_const_id_set;
    u32 latched_vp_from_id;
    u32 saw_vp_from_id_set;
} island_scan;

static int island_read(rsx_nr_island_compiler* ic, u32 io, u32* value)
{
    rsx_nr_frame_owner* const o = ic->owner;
    return o->read32 && o->read32(o->read_user, io, value) == 0;
}

static u32 island_next_io(u32 io, u32 ret)
{
    return ret == ~0u ? ((io + 4u) & ISLAND_RING_MASK) : io + 4u;
}

static int island_io_is_reserved_tail(const rsx_nr_frame_owner* o, u32 io,
                                      u32 ret)
{
    if (ret != ~0u || io >= ISLAND_RING_SIZE)
        return 0;
    if (o->primary_segment_bytes &&
        (io + 4u) % o->primary_segment_bytes == 0u)
        return 1;
    if (o->generated_block_bytes &&
        (io + 4u) % o->generated_block_bytes == 0u)
        return 1;
    return 0;
}

static u64 island_fnv(u64 hash, u32 word)
{
    hash ^= word;
    hash *= 1099511628211ull;
    return hash;
}

/* One pure pass from `get`: parse, prove support, classify, fingerprint,
 * and find the island boundary. On OWN, ic->word_buf[0..word_count) holds
 * the island words, ic->dyn_mask flags dynamic words, and ic->resync_list
 * holds {method, word_idx} pairs in order. */
static void island_scan_run(rsx_nr_island_compiler* ic, u32 get, u32 put,
                            u32 ret, island_scan* s)
{
    rsx_nr_frame_owner* const o = ic->owner;
    rsx_nir_adapter* const ad = o->adapter;
    island_scan* const resume = (island_scan*)(void*)ic->scan_resume_raw;
    /* Resume a prior WAIT's progress when nothing was consumed since; the
     * words already scanned are published and therefore immutable. */
    if (ic->scan_resume_valid && ic->scan_resume_get == get &&
        resume->verdict == ISLAND_SCAN_WAIT) {
        *s = *resume;
        s->verdict = ISLAND_SCAN_DELEGATE;
        s->delegate_reason = RSX_NR_ISLAND_DELEGATE_GRAMMAR;
    } else {
        /* Constant/program upload slots resolve through the load-pointer
         * registers; when the island uploads WITHOUT first setting the
         * pointer, its identity depends on the entry pointer value. Latch
         * that value into the fingerprint exactly once so a different
         * entry pointer maps to a different template instead of a stale
         * slot list. */
        memset(s, 0, sizeof(*s));
        s->verdict = ISLAND_SCAN_DELEGATE;
        s->delegate_reason = RSX_NR_ISLAND_DELEGATE_GRAMMAR;
        s->value_word = ~0u;
        s->fingerprint = 1469598103934665603ull;
        s->io = get;
    }
    ic->scan_resume_valid = 0;
    u32 io = s->io;
    u32 begin_open = s->begin_open;
    u32 saw_batches = s->saw_batches;
    u32 latched_const_id = s->latched_const_id;
    u32 saw_const_id_set = s->saw_const_id_set;
    u32 latched_vp_from_id = s->latched_vp_from_id;
    u32 saw_vp_from_id_set = s->saw_vp_from_id_set;
#define ISLAND_SCAN_SAVE()                                      \
    do {                                                        \
        s->io = io;                                             \
        s->begin_open = begin_open;                             \
        s->saw_batches = saw_batches;                           \
        s->latched_const_id = latched_const_id;                 \
        s->saw_const_id_set = saw_const_id_set;                 \
        s->latched_vp_from_id = latched_vp_from_id;             \
        s->saw_vp_from_id_set = saw_vp_from_id_set;             \
    } while (0)

    if (get == put && !s->word_count) {
        s->verdict = ISLAND_SCAN_EMPTY;
        return;
    }
    /* Ownership requires idle decode state: a draw, inline run, or packet
     * left open by a delegated stretch keeps everything on the owner. */
    if (!s->word_count &&
        (ad->rsx.in_begin_end || ad->batch_count || ad->inline_count ||
         o->packet_active)) {
        s->verdict = ISLAND_SCAN_DELEGATE;
        s->delegate_reason = RSX_NR_ISLAND_DELEGATE_FLOW;
        return;
    }

    for (;;) {
        if (io == put) {
            if (s->word_count == 0u) {
                s->verdict = ISLAND_SCAN_EMPTY;
                return;
            }
            /* A complete state-only island only exists at a boundary the
             * producer has published past. At PUT we cannot distinguish a
             * finished state run from a half-published island; wait. */
            ISLAND_SCAN_SAVE();
            s->verdict = ISLAND_SCAN_WAIT;
            return;
        }
        if (s->word_count + 2048u > RSX_NR_ISLAND_MAX_WORDS) {
            s->delegate_reason = RSX_NR_ISLAND_DELEGATE_CAPACITY;
            return;
        }
        if (island_io_is_reserved_tail(o, io, ret)) {
            /* Reserved segment/block tail words are the producers' link
             * slots; the owner's publication rules must judge them. */
            s->delegate_reason = RSX_NR_ISLAND_DELEGATE_FLOW;
            return;
        }
        u32 header = 0;
        if (!island_read(ic, io, &header)) {
            s->delegate_reason = RSX_NR_ISLAND_DELEGATE_FLOW;
            return;
        }
        if (header == 0u ||
            (header & 0xE0000003u) == 0x20000000u ||
            (header & 3u) == 1u || (header & 3u) == 2u ||
            (header & 0xFFFF0003u) == 0x00020000u) {
            /* NOP or flow control: never inside a compiled island. If we
             * already hold a run of state packets, close them as a
             * state-only island first; the control word goes to the owner
             * on the next step. An open draw/inline run backtracks to full
             * delegation instead (the owner's adapter must accumulate it). */
            if (s->word_count && !begin_open && !s->run_count) {
                s->verdict = ISLAND_SCAN_OWN;
                return;
            }
            s->delegate_reason = RSX_NR_ISLAND_DELEGATE_FLOW;
            return;
        }
        if ((header & 0xA0030003u) != 0u || !((header >> 18) & 0x7FFu)) {
            /* Malformed candidate: generated-hole publication logic. */
            s->delegate_reason = RSX_NR_ISLAND_DELEGATE_FLOW;
            return;
        }
        const u32 count = (header >> 18) & 0x7FFu;
        const u32 non_inc = (header & 0x40000000u) != 0u;
        const u32 base_method = header & 0x3FFFCu;
        /* Whole packet must be published before any of it is owned. */
        if (rsx_nr_fifo_section_range_status(
                io, 4u + count * 4u, put, ret, ISLAND_RING_SIZE) !=
            RSX_NR_FIFO_RANGE_READY) {
            ISLAND_SCAN_SAVE();
            s->verdict = ISLAND_SCAN_WAIT;
            return;
        }
        if (s->word_count + 1u + count > RSX_NR_ISLAND_MAX_WORDS) {
            s->delegate_reason = RSX_NR_ISLAND_DELEGATE_CAPACITY;
            return;
        }
        /* Pre-decode boundary (mirrors the strict owner's island policy):
         * a dependency/new-pass method closes the preceding state run. */
        if (s->word_count &&
            ((ic->method_props[(base_method & 0xFFFCu) >> 2]
                  >> IPROP_BOUNDARY_SHIFT) & 3u) !=
                RSX_NR_GRAPH_METHOD_CONTINUE &&
            base_method < 0x10000u &&
            !begin_open && !s->run_count) {
            s->verdict = ISLAND_SCAN_OWN;
            return;
        }

        /* consume the header word */
        u32 w = s->word_count;
        ic->word_buf[w] = header;
        ic->dyn_mask[w >> 5] &= ~(1u << (w & 31u));
        s->fingerprint = island_fnv(s->fingerprint, header);
        s->word_count++;
        io = island_next_io(io, ret);

        u32 action_here = 0;
        for (u32 k = 0; k < count; ++k) {
            const u32 method = non_inc ? base_method : base_method + k * 4u;
            u32 arg = 0;
            if (!island_read(ic, io, &arg)) {
                s->delegate_reason = RSX_NR_ISLAND_DELEGATE_FLOW;
                return;
            }
            const u32 props = method < 0x10000u
                ? ic->method_props[method >> 2] : 0u;
            if (ad->context_image_open ||
                ((props >> IPROP_SUPPORT_SHIFT) & 3u) !=
                    RSX_NIR_SUPPORT_ALWAYS) {
                if (!rsx_nir_adapter_method_supported(ad, method, arg)) {
                    s->delegate_reason = RSX_NR_ISLAND_DELEGATE_UNSUPPORTED;
                    return;
                }
            }
            w = s->word_count;
            ic->word_buf[w] = arg;
            if (props & IPROP_DYNAMIC)
                ic->dyn_mask[w >> 5] |= 1u << (w & 31u);
            else {
                ic->dyn_mask[w >> 5] &= ~(1u << (w & 31u));
                s->fingerprint = island_fnv(s->fingerprint, arg);
            }
            ic->resync_list[s->method_count * 2u] = method;
            ic->resync_list[s->method_count * 2u + 1u] = w;
            s->method_count++;
            s->word_count++;
            io = island_next_io(io, ret);

            s->group_mask |= (props >> IPROP_GROUP_SHIFT) & IPROP_GROUP_BITS;
            if (props & IPROP_TEX_VALID) {
                const u32 unit = (props >> IPROP_TEX_UNIT_SHIFT) & 31u;
                if (props & IPROP_TEX_VERTEX)
                    s->vtex_mask |= 1u << unit;
                else
                    s->tex_mask |= 1u << unit;
            }
            if (!(props & IPROP_SPECIAL)) {
                if (s->run_count) {
                    /* a non-payload method inside an open inline run: the
                     * adapter's split/flush semantics own this shape */
                    s->delegate_reason = RSX_NR_ISLAND_DELEGATE_GRAMMAR;
                    return;
                }
                continue;
            }

            if (method == IM_VP_UPLOAD_CONST_ID) {
                saw_const_id_set = 1;
            } else if (method == IM_VP_UPLOAD_FROM_ID) {
                saw_vp_from_id_set = 1;
            } else if (method >= IM_VP_UPLOAD_CONST &&
                     method < IM_VP_UPLOAD_CONST + 32u * 4u) {
                s->const_upload_words++;
                if (!saw_const_id_set && !latched_const_id) {
                    latched_const_id = 1;
                    s->fingerprint = island_fnv(
                        s->fingerprint,
                        ad->rsx.regs[IM_VP_UPLOAD_CONST_ID >> 2]);
                }
                continue;
            } else if (method >= IM_VP_UPLOAD_INST &&
                       method < IM_VP_UPLOAD_INST + 32u * 4u) {
                if (!saw_vp_from_id_set && !latched_vp_from_id) {
                    latched_vp_from_id = 1;
                    s->fingerprint = island_fnv(
                        s->fingerprint,
                        ad->rsx.regs[IM_VP_UPLOAD_FROM_ID >> 2]);
                }
                continue;
            }

            /* inline NV308A payload run tracking */
            if (method >= IM308A_COLOR_FIRST && method <= IM308A_COLOR_LAST) {
                const u32 index = (method - IM308A_COLOR_FIRST) >> 2;
                if (!s->run_count) {
                    s->run_first_index = index;
                    s->run_word = w;
                    s->run_count = 1;
                } else if (index == s->run_first_index + s->run_count &&
                           w == s->run_word + (index - s->run_first_index)) {
                    s->run_count++;
                } else {
                    /* Non-contiguous run or interleaved non-payload words:
                     * the adapter's split/flush semantics own this shape. */
                    s->delegate_reason = RSX_NR_ISLAND_DELEGATE_GRAMMAR;
                    return;
                }
                continue;
            }
            if (s->run_count) {
                /* First non-COLOR method after payload: the run is
                 * complete and the island terminates with its transfer
                 * BEFORE this method. Mid-packet continuation is not
                 * owned; require the run to end exactly at a packet
                 * boundary (one-method-per-packet streams and the SDK
                 * inline shape both satisfy this). */
                s->delegate_reason = RSX_NR_ISLAND_DELEGATE_GRAMMAR;
                return;
            }

            /* action recognition (must mirror the adapter's emissions) */
            switch (method) {
            case IM_VERTEX_BEGIN_END:
                if (arg) {
                    if (begin_open || saw_batches) {
                        s->delegate_reason = RSX_NR_ISLAND_DELEGATE_GRAMMAR;
                        return;
                    }
                    begin_open = 1;
                    s->action_a = arg;      /* primitive                   */
                } else {
                    if (!begin_open) {
                        s->delegate_reason = RSX_NR_ISLAND_DELEGATE_GRAMMAR;
                        return;
                    }
                    begin_open = 0;
                    if (saw_batches) {
                        s->action_kind = RSX_NIR_OP_DRAW;
                        action_here = 1;
                    }
                    /* empty begin/end: no action, island continues */
                }
                break;
            case IM_VB_VERTEX_BATCH:
            case IM_VB_INDEX_BATCH: {
                const u32 indexed = method == IM_VB_INDEX_BATCH;
                if (!begin_open) {
                    s->delegate_reason = RSX_NR_ISLAND_DELEGATE_GRAMMAR;
                    return;
                }
                if (saw_batches && ic->batch_indexed != indexed) {
                    /* arrays + indexed inside one begin/end: adapter marks
                     * draw_mixed; keep that rarity on the owner. */
                    s->delegate_reason = RSX_NR_ISLAND_DELEGATE_GRAMMAR;
                    return;
                }
                if (s->batch_count >= RSX_NR_ISLAND_MAX_BATCHES) {
                    s->delegate_reason = RSX_NR_ISLAND_DELEGATE_CAPACITY;
                    return;
                }
                ic->batch_indexed = indexed;
                ic->batch_words[s->batch_count++] = w;
                saw_batches = 1;
                break;
            }
            case IM_CLEAR_BUFFERS:
                s->action_kind = RSX_NIR_OP_CLEAR;
                s->action_a = arg;          /* mask (structural)           */
                action_here = 1;
                break;
            case IM0039_BUFFER_NOTIFY:
                s->action_kind = RSX_NIR_OP_TRANSFER;
                s->action_a = RSX_NIR_XFER_BUFFER;
                action_here = 1;
                break;
            case IM3089_IN_POINT:
                s->action_kind = RSX_NIR_OP_TRANSFER;
                s->action_a = RSX_NIR_XFER_SCALED;
                s->value_word = w;          /* in_x/in_y                   */
                action_here = 1;
                break;
            case IM_WAIT_FOR_IDLE:
                s->action_kind = RSX_NIR_OP_BARRIER;
                action_here = 1;
                break;
            case IM_BACK_END_SEM_RELEASE:
                s->action_kind = RSX_NIR_OP_SEMAPHORE_RELEASE;
                s->action_a = 0;
                s->value_word = w;
                action_here = 1;
                break;
            case IM_TEX_READ_SEM_RELEASE:
                s->action_kind = RSX_NIR_OP_SEMAPHORE_RELEASE;
                s->action_a = 1;
                s->value_word = w;
                action_here = 1;
                break;
            case IM406E_SEMAPHORE_ACQ:
                s->action_kind = RSX_NIR_OP_SEMAPHORE_ACQUIRE;
                s->value_word = w;
                action_here = 1;
                break;
            case IM406E_SEMAPHORE_REL:
                s->action_kind = RSX_NIR_OP_SEMAPHORE_RELEASE;
                s->action_a = 2;
                s->value_word = w;
                action_here = 1;
                break;
            case IM406E_SET_REFERENCE:
                s->action_kind = RSX_NIR_OP_SET_REFERENCE;
                s->value_word = w;
                action_here = 1;
                break;
            case IM_GET_REPORT:
                s->action_kind = RSX_NIR_OP_REPORT;
                s->action_a = 0;
                s->value_word = w;
                action_here = 1;
                break;
            case IM_CLEAR_REPORT_VALUE:
                s->action_kind = RSX_NIR_OP_REPORT;
                s->action_a = 1;
                s->value_word = w;
                action_here = 1;
                break;
            case IM_USER_COMMAND_CAUSE:
            case IM_USER_COMMAND_FIRE:
                s->action_kind = RSX_NIR_OP_USER_COMMAND;
                s->value_word = w;
                action_here = 1;
                break;
            case IM_GCM_DRIVER_FLIP:
                s->action_kind = RSX_NIR_OP_PRESENT;
                s->value_word = w;
                action_here = 1;
                break;
            default:
                break;
            }
            if (action_here && k + 1u < count) {
                /* An action that is not the packet's last method would put
                 * trailing effects inside a closed island. Keep multi-
                 * action packets on the owner (mirrors delivery quirks like
                 * the EB00/EB04 pair being one packet). */
                s->delegate_reason = RSX_NR_ISLAND_DELEGATE_GRAMMAR;
                return;
            }
        }

        if (action_here) {
            s->verdict = ISLAND_SCAN_OWN;
            return;
        }
        if (s->run_count) {
            /* Payload packet complete; peek whether the run continues in
             * the next packet or the island can close with its transfer. */
            if (io == put) {
                ISLAND_SCAN_SAVE();
                s->verdict = ISLAND_SCAN_WAIT;
                return;
            }
            u32 next_header = 0;
            if (island_io_is_reserved_tail(o, io, ret) ||
                !island_read(ic, io, &next_header)) {
                s->delegate_reason = RSX_NR_ISLAND_DELEGATE_FLOW;
                return;
            }
            const int next_is_packet =
                next_header != 0u &&
                (next_header & 0xA0030003u) == 0u &&
                ((next_header >> 18) & 0x7FFu) != 0u;
            const u32 next_method = next_header & 0x3FFFCu;
            if (next_is_packet &&
                next_method >= IM308A_COLOR_FIRST &&
                next_method <= IM308A_COLOR_LAST) {
                /* A continued or restarted run in the next packet: payload
                 * would interleave header words, and split/flush semantics
                 * belong to the adapter. Keep the whole shape on the owner. */
                s->delegate_reason = RSX_NR_ISLAND_DELEGATE_GRAMMAR;
                return;
            }
            /* The run is complete: close the island with its transfer. */
            s->action_kind = RSX_NIR_OP_TRANSFER;
            s->action_a = RSX_NIR_XFER_INLINE;
            s->verdict = ISLAND_SCAN_OWN;
            return;
        }
    }
#undef ISLAND_SCAN_SAVE
}

/* ---- template store ---------------------------------------------------- */

typedef struct island_template {
    u64 fingerprint;
    u32 word_count;
    u32 method_count;
    u32 group_mask;
    u32 tex_mask;
    u32 vtex_mask;
    u32 const_count;            /* resolved constant slots                 */
    u32 batch_count;
    u32 action_kind;
    u32 action_a;
    u32 action_b;               /* DRAW: indexed flag                      */
    u32 value_word;
    u32 run_first_index;
    u32 run_word;
    u32 run_count;
    u32 validated_generation;
    u32 generation_valid;
    /* trailing data offsets (u32 units from record start) */
    u32 ofs_words;              /* word_count                              */
    u32 ofs_dyn;                /* (word_count + 31) / 32                  */
    u32 ofs_resync;             /* method_count * 2                        */
    u32 ofs_slots;              /* const_count                             */
    u32 ofs_batches;            /* batch_count                             */
    u32 total_u32;
} island_template;

static island_template* island_template_at(rsx_nr_island_compiler* ic,
                                           u32 ofs_plus_1)
{
    if (!ofs_plus_1 || ofs_plus_1 - 1u >= ic->arena_cap)
        return NULL;
    return (island_template*)(ic->arena + (ofs_plus_1 - 1u));
}

static u32* island_template_data(island_template* t)
{
    return (u32*)(t + 1);
}

static island_template* island_lookup(rsx_nr_island_compiler* ic,
                                      u64 fingerprint, u32* slot_out)
{
    const u32 mask = ic->index_cap - 1u;
    u32 slot = (u32)(fingerprint ^ (fingerprint >> 32)) & mask;
    for (u32 probe = 0; probe <= mask; ++probe) {
        const u32 ofs = ic->index_ofs[slot];
        if (!ofs) {
            *slot_out = slot;
            return NULL;
        }
        if (ic->index_fp_lo[slot] == (u32)fingerprint &&
            ic->index_fp_hi[slot] == (u32)(fingerprint >> 32)) {
            *slot_out = slot;
            return island_template_at(ic, ofs);
        }
        slot = (slot + 1u) & mask;
    }
    *slot_out = ~0u;
    return NULL;
}

/* Worst-case arena bytes for a template of this scan (constant slots are
 * bounded before they are resolved). Used to make the ownership decision
 * BEFORE any state is touched, so a full table/arena delegates atomically. */
static u32 island_compile_bound(const island_scan* s)
{
    const u32 dyn_u32 = (s->word_count + 31u) >> 5;
    u32 slots = s->const_upload_words / 4u + 1u;
    if (slots > RSX_NR_ISLAND_MAX_CONST_SLOTS)
        slots = RSX_NR_ISLAND_MAX_CONST_SLOTS;
    const u32 data_u32 = s->word_count + dyn_u32 + s->method_count * 2u +
                         slots + s->batch_count;
    return (((u32)sizeof(island_template) + data_u32 * 4u) + 7u) & ~7u;
}

/* Build a template from the current scan (ic->word_buf & friends). The
 * constant slots must already be resolved into ic->const_slots and the
 * capacity pre-checked with island_compile_bound. */
static island_template* island_compile(rsx_nr_island_compiler* ic,
                                       const island_scan* s, u32 index_slot,
                                       u32 const_count, u32 indexed)
{
    const u32 dyn_u32 = (s->word_count + 31u) >> 5;
    const u32 data_u32 = s->word_count + dyn_u32 + s->method_count * 2u +
                         const_count + s->batch_count;
    const u32 bytes = (u32)sizeof(island_template) + data_u32 * 4u;
    const u32 aligned = (bytes + 7u) & ~7u;
    if (index_slot == ~0u || aligned > ic->arena_cap - ic->arena_used)
        return NULL;
    island_template* const t =
        (island_template*)(ic->arena + ic->arena_used);
    const u32 record_ofs = ic->arena_used + 1u;
    ic->arena_used += aligned;

    memset(t, 0, sizeof(*t));
    t->fingerprint = s->fingerprint;
    t->word_count = s->word_count;
    t->method_count = s->method_count;
    t->group_mask = s->group_mask;
    t->tex_mask = s->tex_mask;
    t->vtex_mask = s->vtex_mask;
    t->const_count = const_count;
    t->batch_count = s->batch_count;
    t->action_kind = s->action_kind;
    t->action_a = s->action_a;
    t->action_b = indexed;
    t->value_word = s->value_word;
    t->run_first_index = s->run_first_index;
    t->run_word = s->run_word;
    t->run_count = s->run_count;
    t->ofs_words = 0;
    t->ofs_dyn = t->ofs_words + s->word_count;
    t->ofs_resync = t->ofs_dyn + dyn_u32;
    t->ofs_slots = t->ofs_resync + s->method_count * 2u;
    t->ofs_batches = t->ofs_slots + const_count;
    t->total_u32 = data_u32;

    u32* const data = island_template_data(t);
    memcpy(data + t->ofs_words, ic->word_buf, s->word_count * 4u);
    memcpy(data + t->ofs_dyn, ic->dyn_mask, dyn_u32 * 4u);
    memcpy(data + t->ofs_resync, ic->resync_list, s->method_count * 8u);
    if (const_count)
        memcpy(data + t->ofs_slots, ic->const_slots, const_count * 4u);
    if (s->batch_count)
        memcpy(data + t->ofs_batches, ic->batch_words, s->batch_count * 4u);

    ic->index_fp_lo[index_slot] = (u32)s->fingerprint;
    ic->index_fp_hi[index_slot] = (u32)(s->fingerprint >> 32);
    ic->index_ofs[index_slot] = record_ofs;
    ic->index_live++;
    ic->stats.templates_live = ic->index_live;
    ic->stats.template_arena_used = ic->arena_used;
    return t;
}

/* ---- ownership execution ----------------------------------------------- */

static unsigned long long island_now(rsx_nr_island_compiler* ic)
{
    return ic->now_ticks ? ic->now_ticks(ic->clock_user) : 0u;
}

static void island_tick(rsx_nr_island_compiler* ic,
                        unsigned long long* mark,
                        unsigned long long* bucket)
{
    if (!ic->now_ticks)
        return;
    const unsigned long long now = ic->now_ticks(ic->clock_user);
    if (now >= *mark)
        *bucket += now - *mark;
    *mark = now;
}

/* Resync register truth from the island words, resolving constant-upload
 * slots as the load pointer becomes current (slots are recorded once each,
 * in first-touch order; the count is bounded by the slot file). */
static void island_resync(rsx_nr_island_compiler* ic,
                          const u32* words, const u32* resync,
                          u32 method_count, u32 resolve_slots)
{
    rsx_nir_adapter* const ad = ic->owner->adapter;
    u32 slot_count = 0;
    rsx_nir_adapter_resync_begin(ad);
    for (u32 i = 0; i < method_count; ++i) {
        const u32 method = resync[i * 2u];
        const u32 arg = words[resync[i * 2u + 1u]];
        if (resolve_slots && method >= IM_VP_UPLOAD_CONST &&
            method < IM_VP_UPLOAD_CONST + 32u * 4u) {
            const u32 word = (method - IM_VP_UPLOAD_CONST) >> 2;
            const u32 slot =
                ad->rsx.regs[IM_VP_UPLOAD_CONST_ID >> 2] + (word >> 2);
            if (slot < RSX_NIR_NUM_CONSTANTS) {
                u32 seen = 0;
                for (u32 j = 0; j < slot_count; ++j)
                    if (ic->const_slots[j] == slot) {
                        seen = 1;
                        break;
                    }
                if (!seen && slot_count < RSX_NR_ISLAND_MAX_CONST_SLOTS)
                    ic->const_slots[slot_count++] = slot;
            }
        }
        rsx_nir_adapter_resync_method(ad, method, arg);
    }
    rsx_nir_adapter_resync_end(ad);
    if (resolve_slots)
        ic->resolved_const_count = slot_count;
}

/* Build the scratch typed stream for one island from its template plus the
 * current register file, then leave it ready for stream execution. */
static int island_build_stream(rsx_nr_island_compiler* ic,
                               island_template* t, const u32* words)
{
    rsx_nir_adapter* const ad = ic->owner->adapter;
    rsx_nir_stream* const stream = &ic->scratch;
    rsx_nir_stream_reset(stream);

    /* The very first action of an adapter's lifetime must observe complete
     * state (the emitter's prime contract); islands after that carry only
     * their own touched groups, untouched state persisting in the backend. */
    const int prime = t->action_kind != 0u &&
        (!ad->em.primed || ic->force_full_state);
    const u32 group_mask = prime
        ? ~0u & ~GBIT(RSX_NIR_OP_SET_CONSTANTS) : t->group_mask;
    const u32 tex_mask = prime ? 0xFFFFu : t->tex_mask;
    const u32 vtex_mask = prime ? 0xFu : t->vtex_mask;

    for (u32 kind = RSX_NIR_OP_SET_SURFACE;
         kind <= RSX_NIR_OP_SET_INDEX_BINDING; ++kind) {
        if (kind == RSX_NIR_OP_SET_CONSTANTS)
            continue;
        if (!(group_mask & GBIT(kind)))
            continue;
        if (rsx_nir_adapter_derive_group_op(ad, kind, 0, stream, 1) != 0)
            return -1;
        ic->stats.groups_derived++;
    }
    for (u32 u = 0; u < RSX_NIR_NUM_TEXTURES; ++u)
        if (tex_mask & (1u << u)) {
            if (rsx_nir_adapter_derive_group_op(
                    ad, RSX_NIR_OP_SET_TEXTURE, u, stream, 1) != 0)
                return -1;
            ic->stats.groups_derived++;
        }
    for (u32 u = 0; u < RSX_NIR_NUM_VERTEX_TEXTURES; ++u)
        if (vtex_mask & (1u << u)) {
            if (rsx_nir_adapter_derive_group_op(
                    ad, RSX_NIR_OP_SET_VERTEX_TEXTURE, u, stream, 1) != 0)
                return -1;
            ic->stats.groups_derived++;
        }
    if (prime) {
        for (u32 slot = 0; slot < RSX_NIR_NUM_CONSTANTS; ++slot) {
            if (!ad->em.pending.constants_written[slot])
                continue;
            if (rsx_nir_adapter_derive_constant_op(ad, slot, stream, 1) != 0)
                return -1;
        }
        ad->em.primed = 1;
    } else {
        const u32* const slots = island_template_data(t) + t->ofs_slots;
        for (u32 i = 0; i < t->const_count; ++i) {
            if (rsx_nir_adapter_derive_constant_op(
                    ad, slots[i], stream, 1) != 0)
                return -1;
            ic->stats.constants_slots_patched++;
        }
    }

    if (!t->action_kind)
        return 0;

    rsx_nir_op op;
    memset(&op, 0, sizeof(op));
    op.kind = t->action_kind;
    switch (t->action_kind) {
    case RSX_NIR_OP_DRAW: {
        const u32* const batch_words =
            island_template_data(t) + t->ofs_batches;
        u32* const pairs = ic->batch_pairs;
        u32 total = 0;
        for (u32 i = 0; i < t->batch_count; ++i) {
            const u32 arg = words[batch_words[i]];
            pairs[i * 2u] = arg & 0xFFFFFFu;
            pairs[i * 2u + 1u] = (arg >> 24) + 1u;
            total += pairs[i * 2u + 1u];
        }
        op.u.draw.primitive = t->action_a;
        op.u.draw.indexed = t->action_b;
        op.u.draw.batch_count = t->batch_count;
        op.u.draw.total_count = total;
        op.u.draw.batches_ofs =
            rsx_nir_side_push(stream, pairs, t->batch_count * 2u);
        if (t->batch_count && op.u.draw.batches_ofs == ~0u)
            return -1;
        break;
    }
    case RSX_NIR_OP_CLEAR:
        op.u.clear.mask = t->action_a;
        op.u.clear.color_value = rsx_dsp_clear_color(&ad->rsx);
        {
            const u32 zs = rsx_dsp_clear_zstencil(&ad->rsx);
            op.u.clear.depth_value = zs >> 8;
            op.u.clear.stencil_value = zs & 0xFFu;
        }
        break;
    case RSX_NIR_OP_TRANSFER:
        rsx_nir_adapter_derive_transfer(ad, t->action_a, &op.u.transfer);
        if (t->action_a == RSX_NIR_XFER_SCALED) {
            const u32 arg = words[t->value_word];
            op.u.transfer.in_x = arg & 0xFFFFu;
            op.u.transfer.in_y = arg >> 16;
        } else if (t->action_a == RSX_NIR_XFER_INLINE) {
            const u32 out_x = op.u.transfer.size_w;
            u32 valid = t->run_first_index < out_x
                ? out_x - t->run_first_index : 0u;
            u32 payload = t->run_count < valid ? t->run_count : valid;
            if (!payload)
                return 0;               /* padding-only run: no action op */
            op.u.transfer.point_x += t->run_first_index;
            op.u.transfer.word_count = payload;
            op.u.transfer.words_ofs = rsx_nir_side_push(
                stream, words + t->run_word, payload);
            if (op.u.transfer.words_ofs == ~0u)
                return -1;
        }
        break;
    case RSX_NIR_OP_BARRIER:
        op.u.barrier.kind = 0;
        break;
    case RSX_NIR_OP_SEMAPHORE_RELEASE:
        if (t->action_a == 2u) {
            op.u.semaphore.dma_context = ad->fifo_semaphore_dma;
            op.u.semaphore.offset = ad->fifo_semaphore_offset;
        } else {
            op.u.semaphore.dma_context =
                ad->rsx.regs[IM_CTX_DMA_SEMAPHORE_3D >> 2];
            op.u.semaphore.offset =
                ad->rsx.regs[IM_SEMAPHORE_OFFSET_3D >> 2];
        }
        op.u.semaphore.value = words[t->value_word];
        op.u.semaphore.texture_read = t->action_a;
        break;
    case RSX_NIR_OP_SEMAPHORE_ACQUIRE:
        op.u.semaphore.dma_context = ad->fifo_semaphore_dma;
        op.u.semaphore.offset = ad->fifo_semaphore_offset;
        op.u.semaphore.value = words[t->value_word];
        break;
    case RSX_NIR_OP_REPORT:
        op.u.report.kind = t->action_a;
        op.u.report.arg = words[t->value_word];
        op.u.report.dma_report = ad->rsx.regs[IM_CONTEXT_DMA_REPORT >> 2];
        break;
    case RSX_NIR_OP_SET_REFERENCE:
        op.u.reference.value = words[t->value_word];
        break;
    case RSX_NIR_OP_USER_COMMAND:
        op.u.user_command.cause = words[t->value_word];
        break;
    case RSX_NIR_OP_PRESENT:
        op.u.present.buffer = words[t->value_word];
        break;
    default:
        return -1;
    }
    const int pushed = rsx_nir_push(stream, &op);
    if (!pushed && ic->force_full_state)
        ic->force_full_state = 0;
    return pushed;
}

enum {
    ISLAND_ORACLE_ACTION_SHAPE = 1,
    ISLAND_ORACLE_PIPELINE = 2,
    ISLAND_ORACLE_ACTION_PAYLOAD = 3,
    ISLAND_ORACLE_SIDE_PAYLOAD = 4,
    ISLAND_ORACLE_STREAM_CAPACITY = 5,
};

/* Build the unchanged adapter's semantic answer in fixed scratch before
 * the compiler mutates the live adapter.  This is diagnostics-only and is
 * armed explicitly by the live embedder; no output or clocks occur here. */
static void island_oracle_prepare(rsx_nr_island_compiler* ic,
                                  const u32* words, const u32* resync,
                                  u32 method_count)
{
    if (!ic->oracle_adapter)
        return;
    rsx_nir_adapter* const oracle = ic->oracle_adapter;
    const rsx_nir_sink out = oracle->em.out;
    rsx_nir_stream_reset(&ic->oracle_stream);
    memcpy(oracle, ic->owner->adapter, sizeof(*oracle));
    oracle->em.out = out;
    oracle->resync_active = 0;
    rsx_nir_adapter_rebind(oracle);
    for (u32 i = 0; i < method_count; ++i) {
        const u32 method = resync[i * 2u];
        const u32 arg = words[resync[i * 2u + 1u]];
        rsx_nir_adapter_method(oracle, method, arg);
    }
    rsx_nir_adapter_finish(oracle);
}

static const rsx_nir_op* island_oracle_action(
    const rsx_nir_stream* stream, u32* count)
{
    const rsx_nir_op* action = NULL;
    *count = 0;
    for (u32 i = 0; i < stream->op_count; ++i)
        if (rsx_nir_op_is_action(stream->ops[i].kind)) {
            action = &stream->ops[i];
            (*count)++;
        }
    return action;
}

static void island_oracle_fold_state(rsx_nir_pipeline* pipeline,
                                     const rsx_nir_stream* stream)
{
    for (u32 i = 0; i < stream->op_count; ++i)
        rsx_nir_pipeline_apply_op(pipeline, stream, &stream->ops[i]);
    /* Side offsets are stream-local storage identities, not semantics. */
    pipeline->vertex_program.words_ofs = 0;
}

static int island_oracle_compare_side(const rsx_nir_stream* a,
                                      const rsx_nir_op* ao,
                                      const rsx_nir_stream* b,
                                      const rsx_nir_op* bo)
{
    if (!ao || !bo || ao->kind != bo->kind)
        return -1;
    if (ao->kind == RSX_NIR_OP_DRAW) {
        const u32 n = ao->u.draw.batch_count * 2u;
        if (n != bo->u.draw.batch_count * 2u)
            return -1;
        const u32* const av = rsx_nir_side(a, ao->u.draw.batches_ofs, n);
        const u32* const bv = rsx_nir_side(b, bo->u.draw.batches_ofs, n);
        return (!n || (av && bv && memcmp(av, bv, (size_t)n * 4u) == 0))
            ? 0 : -1;
    }
    if (ao->kind == RSX_NIR_OP_TRANSFER) {
        const u32 n = ao->u.transfer.word_count;
        if (n != bo->u.transfer.word_count)
            return -1;
        const u32* const av = rsx_nir_side(a, ao->u.transfer.words_ofs, n);
        const u32* const bv = rsx_nir_side(b, bo->u.transfer.words_ofs, n);
        return (!n || (av && bv && memcmp(av, bv, (size_t)n * 4u) == 0))
            ? 0 : -1;
    }
    return 0;
}

static void island_oracle_record_mismatch(rsx_nr_island_compiler* ic,
                                          u32 get, u32 action,
                                          u32 reason, u32 method,
                                          u32 word, u32 expected,
                                          u32 compiled)
{
    rsx_nr_island_oracle_stats* const s = &ic->oracle_stats;
    s->mismatches++;
    if (s->mismatches != 1u)
        return;
    s->first_get = get;
    s->first_action = action;
    s->first_reason = reason;
    s->first_method = method;
    s->first_word = word;
    s->first_expected = expected;
    s->first_compiled = compiled;
}

static u32 island_oracle_pipeline_mismatch(
    const rsx_nir_pipeline* expected, const rsx_nir_pipeline* compiled,
    u32* word, u32* expected_value, u32* compiled_value)
{
#define ISLAND_ORACLE_GROUP(ID, FIELD)                                      \
    do {                                                                     \
        if (memcmp(&expected->FIELD, &compiled->FIELD,                        \
                   sizeof(expected->FIELD)) != 0) {                           \
            const u32* const ew = (const u32*)(const void*)&expected->FIELD;  \
            const u32* const cw = (const u32*)(const void*)&compiled->FIELD;  \
            for (u32 wi = 0; wi < sizeof(expected->FIELD) / 4u; ++wi)         \
                if (ew[wi] != cw[wi]) {                                      \
                    *word = wi;                                               \
                    *expected_value = ew[wi];                                 \
                    *compiled_value = cw[wi];                                 \
                    break;                                                    \
                }                                                            \
            return 0x100u + (ID);                                             \
        }                                                                    \
    } while (0)
    ISLAND_ORACLE_GROUP(1u, surface);
    ISLAND_ORACLE_GROUP(2u, viewport);
    ISLAND_ORACLE_GROUP(3u, scissor);
    ISLAND_ORACLE_GROUP(4u, raster);
    ISLAND_ORACLE_GROUP(5u, depth_stencil);
    ISLAND_ORACLE_GROUP(6u, blend);
    ISLAND_ORACLE_GROUP(7u, render_condition);
    ISLAND_ORACLE_GROUP(8u, vertex_program);
    ISLAND_ORACLE_GROUP(9u, fragment_program);
    ISLAND_ORACLE_GROUP(10u, constants);
    ISLAND_ORACLE_GROUP(11u, constants_written);
    ISLAND_ORACLE_GROUP(12u, vertex_bindings);
    ISLAND_ORACLE_GROUP(13u, index_binding);
    ISLAND_ORACLE_GROUP(14u, textures);
    ISLAND_ORACLE_GROUP(15u, vertex_textures);
#undef ISLAND_ORACLE_GROUP
    return 0u;
}

/* Compare the compiler result against the unchanged adapter at the exact
 * action boundary, before either answer is executed.  Both streams are
 * folded over the current backend pipeline so compiler-elided unchanged
 * state is not mistaken for a semantic difference. */
static void island_oracle_verify(rsx_nr_island_compiler* ic, u32 get,
                                 const u32* resync, u32 method_count)
{
    if (!ic->oracle_adapter)
        return;
    rsx_nir_stream* const oracle = &ic->oracle_stream;
    rsx_nir_stream* const compiled = &ic->scratch;
    const u32 last_method = method_count ? resync[(method_count - 1u) * 2u]
                                         : 0u;
    u32 oracle_actions = 0, compiled_actions = 0;
    const rsx_nir_op* const oracle_action =
        island_oracle_action(oracle, &oracle_actions);
    const rsx_nir_op* const compiled_action =
        island_oracle_action(compiled, &compiled_actions);
    if (oracle->overflow || oracle->oom) {
        island_oracle_record_mismatch(
            ic, get, compiled_action ? compiled_action->kind : 0u,
            ISLAND_ORACLE_STREAM_CAPACITY, last_method, 0u, 0u, 0u);
        return;
    }
    if (!compiled_actions)
        return;
    ic->oracle_stats.action_islands_checked++;
    if (oracle_actions != 1u || compiled_actions != 1u ||
        !oracle_action || !compiled_action ||
        oracle_action->kind != compiled_action->kind) {
        island_oracle_record_mismatch(
            ic, get, compiled_action ? compiled_action->kind : 0u,
            ISLAND_ORACLE_ACTION_SHAPE, last_method, 0u,
            oracle_actions, compiled_actions);
        return;
    }

    rsx_nir_pipeline oracle_pipeline = ic->owner->backend->st;
    rsx_nir_pipeline compiled_pipeline = ic->owner->backend->st;
    island_oracle_fold_state(&oracle_pipeline, oracle);
    island_oracle_fold_state(&compiled_pipeline, compiled);
    u32 mismatch_word = 0, expected_value = 0, compiled_value = 0;
    const u32 pipeline_reason = island_oracle_pipeline_mismatch(
        &oracle_pipeline, &compiled_pipeline, &mismatch_word,
        &expected_value, &compiled_value);
    if (pipeline_reason) {
        island_oracle_record_mismatch(
            ic, get, compiled_action->kind, pipeline_reason,
            last_method, mismatch_word, expected_value, compiled_value);
        return;
    }

    rsx_nir_op oa = *oracle_action;
    rsx_nir_op ca = *compiled_action;
    if (oa.kind == RSX_NIR_OP_DRAW) {
        oa.u.draw.batches_ofs = 0;
        ca.u.draw.batches_ofs = 0;
    } else if (oa.kind == RSX_NIR_OP_TRANSFER) {
        oa.u.transfer.words_ofs = 0;
        ca.u.transfer.words_ofs = 0;
    }
    if (memcmp(&oa, &ca, sizeof(oa)) != 0) {
        island_oracle_record_mismatch(
            ic, get, compiled_action->kind, ISLAND_ORACLE_ACTION_PAYLOAD,
            last_method, 0u, 0u, 0u);
        return;
    }
    if (island_oracle_compare_side(
            oracle, oracle_action, compiled, compiled_action) != 0)
        island_oracle_record_mismatch(
            ic, get, compiled_action->kind, ISLAND_ORACLE_SIDE_PAYLOAD,
            last_method, 0u, 0u, 0u);
}

/* Execute the scratch stream from ic->exec_pos. Returns the step result;
 * WAIT_SEMAPHORE keeps exec state armed for a later resume. */
static rsx_nr_frame_step_result island_execute(rsx_nr_island_compiler* ic)
{
    rsx_nr_frame_owner* const o = ic->owner;
    while (ic->exec_pos < ic->scratch.op_count) {
        const unsigned long long errors_before =
            o->backend->stats.exec_errors;
        const rsx_nr_step_result r = rsx_nr_backend_stream_step(
            o->backend, &ic->scratch, ic->exec_pos);
        if (r == RSX_NR_STEP_BLOCKED_SEMAPHORE ||
            r == RSX_NR_STEP_BLOCKED_TOKEN) {
            o->stats.waits_semaphore++;
            return RSX_NR_FRAME_WAIT_SEMAPHORE;
        }
        if (r != RSX_NR_STEP_EXECUTED ||
            o->backend->stats.exec_errors != errors_before) {
            /* Execution failures remain the strict owner's exact fatal
             * contract; native work must never be silently retried. */
            o->fatal = 1;
            o->failure.kind = RSX_NR_FRAME_FAILURE_EXECUTION;
            o->failure.get = ic->exec_get;
            o->failure.command = ic->scratch.ops[ic->exec_pos].kind;
            return RSX_NR_FRAME_FATAL;
        }
        o->stats.backend_ops++;
        if (ic->scratch.ops[ic->exec_pos].kind == RSX_NIR_OP_PRESENT)
            o->stats.frames++;
        ic->exec_pos++;
    }
    return RSX_NR_FRAME_ADVANCED;
}

static void island_finish_action(rsx_nr_island_compiler* ic)
{
    rsx_nir_adapter* const ad = ic->owner->adapter;
    if (!ic->exec_action_kind)
        return;
    ic->stats.actions_executed++;
    switch (ic->exec_action_kind) {
    case RSX_NIR_OP_DRAW:
    case RSX_NIR_OP_PRESENT:
    case RSX_NIR_OP_TRANSFER:
        ad->context_image_open = 0;
        break;
    case RSX_NIR_OP_CLEAR:
        if (ic->exec_action_a)
            ad->context_image_open = 0;
        break;
    default:
        break;
    }
}

/* ---- public ------------------------------------------------------------ */

int rsx_nr_island_compiler_init(
    rsx_nr_island_compiler* ic, rsx_nr_frame_owner* owner,
    unsigned char* arena, u32 arena_bytes,
    u32* index_fp_lo, u32* index_fp_hi, u32* index_ofs, u32 index_cap,
    rsx_nir_op* scratch_ops, u32 scratch_op_cap,
    u32* scratch_side, u32 scratch_side_cap)
{
    if (!ic || !owner || !arena || !index_fp_lo || !index_fp_hi ||
        !index_ofs || !index_cap || (index_cap & (index_cap - 1u)) ||
        !scratch_ops || !scratch_side ||
        sizeof(island_scan) > sizeof(ic->scan_resume_raw) ||
        GBIT(RSX_NIR_OP_SET_INDEX_BINDING) > IPROP_GROUP_BITS)
        return -1;
    memset(ic, 0, sizeof(*ic));
    ic->owner = owner;
    ic->arena = arena;
    ic->arena_cap = arena_bytes & ~7u;
    ic->index_fp_lo = index_fp_lo;
    ic->index_fp_hi = index_fp_hi;
    ic->index_ofs = index_ofs;
    ic->index_cap = index_cap;
    ic->scratch_ops = scratch_ops;
    ic->scratch_op_cap = scratch_op_cap;
    ic->scratch_side = scratch_side;
    ic->scratch_side_cap = scratch_side_cap;
    memset(index_ofs, 0, (size_t)index_cap * 4u);
    rsx_nir_stream_init_fixed(&ic->scratch, scratch_ops, scratch_op_cap,
                              scratch_side, scratch_side_cap);
    island_build_props(ic);
    return 0;
}

void rsx_nr_island_compiler_set_clock(
    rsx_nr_island_compiler* ic, rsx_nr_frame_now_ticks_fn now_ticks,
    void* user)
{
    ic->now_ticks = now_ticks;
    ic->clock_user = user;
}

void rsx_nr_island_compiler_set_oracle(
    rsx_nr_island_compiler* ic, rsx_nir_adapter* oracle_adapter,
    rsx_nir_op* oracle_ops, u32 oracle_op_cap,
    u32* oracle_side, u32 oracle_side_cap)
{
    if (!ic || !oracle_adapter || !oracle_ops || !oracle_op_cap ||
        !oracle_side || !oracle_side_cap)
        return;
    ic->oracle_adapter = oracle_adapter;
    ic->oracle_ops = oracle_ops;
    ic->oracle_side = oracle_side;
    ic->oracle_op_cap = oracle_op_cap;
    ic->oracle_side_cap = oracle_side_cap;
    rsx_nir_stream_init_fixed(
        &ic->oracle_stream, oracle_ops, oracle_op_cap,
        oracle_side, oracle_side_cap);
    rsx_nir_adapter_init(oracle_adapter, &ic->oracle_stream);
}

void rsx_nr_island_compiler_invalidate_all(rsx_nr_island_compiler* ic)
{
    if (!ic)
        return;
    memset(ic->index_ofs, 0, (size_t)ic->index_cap * 4u);
    ic->index_live = 0;
    ic->arena_used = 0;
    /* A blocked in-flight island lives entirely in the fixed scratch
     * stream and exec fields; keep it so nothing executes twice or is
     * dropped when templates are invalidated underneath it. */
    ic->delegate_active = 0;
    ic->scan_resume_valid = 0;
    if (!ic->exec_active)
        rsx_nir_stream_reset(&ic->scratch);
    ic->stats.templates_live = 0;
    ic->stats.template_arena_used = 0;
    ic->stats.invalidations++;
}

static int island_owner_idle(const rsx_nr_island_compiler* ic)
{
    const rsx_nr_frame_owner* const o = ic->owner;
    const rsx_nir_adapter* const ad = o->adapter;
    return !o->packet_active && !o->method_inflight &&
           rsx_nr_ring_depth(o->ring) == 0u &&
           !ad->rsx.in_begin_end && !ad->batch_count && !ad->inline_count;
}

static void island_delegate_begin(rsx_nr_island_compiler* ic)
{
    ic->delegate_active = 1;
    ic->delegate_methods_start = ic->owner->adapter->methods_seen;
    ic->delegate_actions_start = ic->owner->adapter->actions_seen;
}

static void island_delegate_maybe_finish(
    rsx_nr_island_compiler* ic, rsx_nr_frame_step_result result)
{
    if (!island_owner_idle(ic) ||
        (result != RSX_NR_FRAME_ADVANCED &&
         result != RSX_NR_FRAME_WAIT_EMPTY))
        return;
    const rsx_nir_adapter* const ad = ic->owner->adapter;
    /* A delegated state-only stretch updates architectural registers but
     * cannot flush them to the backend.  The next compiler-owned action
     * must therefore carry one complete state catch-up.  Pure flow words
     * do not increment methods_seen, while a delegated action has already
     * performed the strict adapter's complete stage/flush itself. */
    if (ad->methods_seen != ic->delegate_methods_start &&
        ad->actions_seen == ic->delegate_actions_start)
        ic->force_full_state = 1;
    ic->delegate_active = 0;
}

rsx_nr_frame_step_result rsx_nr_island_compiler_step(
    rsx_nr_island_compiler* ic, u32 get, u32 put, u32 call_return,
    u32* next_get, u32* next_return)
{
    if (!ic || !ic->owner || !next_get || !next_return)
        return RSX_NR_FRAME_FATAL;
    rsx_nr_frame_owner* const o = ic->owner;
    *next_get = get;
    *next_return = call_return;
    ic->stats.steps++;
    if (o->fatal)
        return RSX_NR_FRAME_FATAL;

    /* resume a blocked island execution */
    if (ic->exec_active) {
        if (get != ic->exec_get || call_return != ic->exec_ret) {
            o->fatal = 1;
            o->failure.kind = RSX_NR_FRAME_FAILURE_CURSOR_CHANGED;
            o->failure.get = get;
            return RSX_NR_FRAME_FATAL;
        }
        unsigned long long mark = island_now(ic);
        const rsx_nr_frame_step_result r = island_execute(ic);
        island_tick(ic, &mark, &ic->stats.ticks_execute);
        if (r != RSX_NR_FRAME_ADVANCED)
            return r;
        island_finish_action(ic);
        ic->exec_active = 0;
        *next_get = ic->exec_next_get;
        return RSX_NR_FRAME_ADVANCED;
    }

    /* a delegated stretch stays with the owner until its decode is idle */
    if (ic->delegate_active) {
        const rsx_nr_frame_step_result r = rsx_nr_frame_owner_step(
            o, get, put, call_return, next_get, next_return);
        ic->stats.delegated_steps++;
        island_delegate_maybe_finish(ic, r);
        return r;
    }

    unsigned long long mark = island_now(ic);
    island_scan scan;
    island_scan_run(ic, get, put, call_return, &scan);
    island_tick(ic, &mark, &ic->stats.ticks_scan);

    if (scan.verdict == ISLAND_SCAN_EMPTY) {
        o->stats.waits_empty++;
        return RSX_NR_FRAME_WAIT_EMPTY;
    }
    if (scan.verdict == ISLAND_SCAN_WAIT) {
        /* retain the scan progress for the next attempt at this cursor */
        memcpy(ic->scan_resume_raw, &scan, sizeof(scan));
        ic->scan_resume_get = get;
        ic->scan_resume_valid = 1;
        o->stats.waits_partial++;
        return RSX_NR_FRAME_WAIT_PARTIAL;
    }
    if (scan.verdict == ISLAND_SCAN_DELEGATE) {
        ic->stats.islands_delegated[scan.delegate_reason]++;
        island_delegate_begin(ic);
        const rsx_nr_frame_step_result r = rsx_nr_frame_owner_step(
            o, get, put, call_return, next_get, next_return);
        ic->stats.delegated_steps++;
        island_delegate_maybe_finish(ic, r);
        return r;
    }

    /* own it: hit or compile */
    u32 slot = ~0u;
    island_template* t = island_lookup(ic, scan.fingerprint, &slot);
    int is_hit = 0;
    if (t) {
        /* structural identity: exact compare of invariant words (collision
         * guard), skippable only under an unchanged content generation */
        if (t->generation_valid &&
            t->validated_generation == ic->content_generation &&
            t->word_count == scan.word_count) {
            is_hit = 1;
            ic->stats.generation_fast_hits++;
        } else if (t->word_count == scan.word_count &&
                   t->method_count == scan.method_count) {
            const u32* const twords = island_template_data(t) + t->ofs_words;
            const u32* const tdyn = island_template_data(t) + t->ofs_dyn;
            is_hit = 1;
            for (u32 i = 0; i < scan.word_count; ++i) {
                if (!(tdyn[i >> 5] & (1u << (i & 31u))) &&
                    twords[i] != ic->word_buf[i]) {
                    is_hit = 0;
                    break;
                }
            }
            if (is_hit) {
                t->validated_generation = ic->content_generation;
                t->generation_valid = 1;
            }
        }
        if (!is_hit) {
            ic->stats.validation_mismatches++;
            /* A matching fingerprint whose invariant skeleton disagrees is
             * a failed ownership proof, not a new compile opportunity. Keep
             * the complete island on the established strict owner before
             * either adapter or backend state is touched. */
            ic->stats.islands_delegated[
                RSX_NR_ISLAND_DELEGATE_VALIDATION]++;
            island_delegate_begin(ic);
            const rsx_nr_frame_step_result r = rsx_nr_frame_owner_step(
                o, get, put, call_return, next_get, next_return);
            ic->stats.delegated_steps++;
            island_delegate_maybe_finish(ic, r);
            return r;
        }
    }
    island_tick(ic, &mark, &ic->stats.ticks_validate);

    /* A compile needs table, arena, and scratch-stream room; decide
     * BEFORE mutating any state so a full store delegates the island
     * atomically instead of half-owning it. (Post-resync delegation would
     * replay the constant and program upload auto-advance a second time.)
     * A later hit rebuild reuses the shape this bound admitted, plus the
     * one-time prime headroom. */
    const u32 scratch_ops_bound =
        (RSX_NIR_OP_SET_INDEX_BINDING - RSX_NIR_OP_SET_SURFACE + 1u) +
        RSX_NIR_NUM_TEXTURES + RSX_NIR_NUM_VERTEX_TEXTURES +
        RSX_NIR_NUM_CONSTANTS + scan.const_upload_words / 4u + 2u;
    const u32 scratch_side_bound =
        RSX_NIR_VP_MAX_WORDS + RSX_NIR_NUM_CONSTANTS * 4u +
        scan.const_upload_words + scan.batch_count * 2u +
        scan.run_count + 64u;
    if (!t && (slot == ~0u ||
               ic->index_live >= (ic->index_cap / 4u) * 3u ||
               island_compile_bound(&scan) >
                   ic->arena_cap - ic->arena_used ||
               scratch_ops_bound > ic->scratch_op_cap ||
               scratch_side_bound > ic->scratch_side_cap)) {
        ic->stats.islands_delegated[RSX_NR_ISLAND_DELEGATE_CAPACITY]++;
        island_delegate_begin(ic);
        const rsx_nr_frame_step_result r = rsx_nr_frame_owner_step(
            o, get, put, call_return, next_get, next_return);
        ic->stats.delegated_steps++;
        island_delegate_maybe_finish(ic, r);
        return r;
    }

    /* A completely preflighted non-flow island is equivalent to the
     * packet-start path in rsx_nr_frame_owner_step.  The strict owner
     * resets its bounded consecutive-flow guard as soon as it reaches an
     * ordinary packet; compiler-owned islands must do the same.  Without
     * this reset, delegated JUMP/CALL packets accumulate across otherwise
     * valid islands and eventually trip a false BAD_FLOW fatal. */
    o->control_streak = 0;

    /* Build the diagnostics-only strict answer before register resync
     * mutates the live adapter. */
    island_oracle_prepare(
        ic, ic->word_buf,
        t ? island_template_data(t) + t->ofs_resync : ic->resync_list,
        scan.method_count);

    /* register truth (both paths), resolving constant slots on compile */
    island_resync(ic, ic->word_buf,
                  t ? island_template_data(t) + t->ofs_resync
                    : ic->resync_list,
                  scan.method_count, t == NULL);
    island_tick(ic, &mark, &ic->stats.ticks_resync);

    if (!t) {
        t = island_compile(ic, &scan, slot, ic->resolved_const_count,
                           ic->batch_indexed);
        if (!t) {
            /* Unreachable by the pre-check above; refuse loudly rather
             * than guess (see the pre-check comment). */
            o->fatal = 1;
            o->failure.kind = RSX_NR_FRAME_FAILURE_RING_CAPACITY;
            o->failure.get = get;
            return RSX_NR_FRAME_FATAL;
        }
        t->validated_generation = ic->content_generation;
        t->generation_valid = 1;
        ic->stats.islands_compiled++;
        island_tick(ic, &mark, &ic->stats.ticks_compile);
    } else {
        is_hit = 1;
        ic->stats.islands_hit++;
        ic->stats.methods_hit += scan.method_count;
        ic->stats.adaptations_avoided += scan.method_count;
    }
    ic->stats.methods_owned += scan.method_count;

    /* build + execute */
    const int built = island_build_stream(ic, t, ic->word_buf);
    island_tick(ic, &mark, &ic->stats.ticks_derive_patch);
    if (built < 0) {
        o->fatal = 1;
        o->failure.kind = RSX_NR_FRAME_FAILURE_RING_CAPACITY;
        o->failure.get = get;
        return RSX_NR_FRAME_FATAL;
    }
    island_oracle_verify(
        ic, get, island_template_data(t) + t->ofs_resync,
        scan.method_count);

    ic->exec_get = get;
    ic->exec_ret = call_return;
    ic->exec_next_get = call_return == ~0u
        ? ((get + scan.word_count * 4u) & ISLAND_RING_MASK)
        : get + scan.word_count * 4u;
    ic->exec_action_kind = t->action_kind;
    ic->exec_action_a = t->action_a;
    ic->exec_pos = 0;
    ic->exec_active = 1;
    const rsx_nr_frame_step_result r = island_execute(ic);
    island_tick(ic, &mark, &ic->stats.ticks_execute);
    if (r != RSX_NR_FRAME_ADVANCED)
        return r;                      /* blocked acquire keeps exec state */
    island_finish_action(ic);
    ic->exec_active = 0;
    *next_get = ic->exec_next_get;
    return RSX_NR_FRAME_ADVANCED;
}
