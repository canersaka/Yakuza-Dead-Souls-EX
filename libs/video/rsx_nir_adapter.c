/*
 * ps3recomp - NIR adapter implementation. See rsx_nir_adapter.h.
 *
 * Method numbers referenced here beyond rsx_dispatch.c's set (envytools
 * rnndb NV30-40 + psdevwiki; RPCS3 consulted as a read-only semantics
 * oracle for the FIFO-engine split, NV308A raw word copies and the NV3089
 * IMAGE_IN trigger; the subchannel flattening and the NV3062/NV308A state
 * register meanings follow the live consumer's own map in
 * yakuza/import_overrides.cpp yz_rsx_method):
 *
 *   0x0110 WAIT_FOR_IDLE          0x17C8 CLEAR_REPORT_VALUE
 *   0x01A8 CONTEXT_DMA_REPORT     0x1800 GET_REPORT
 *   0x1D6C SET_SEMAPHORE_OFFSET   0x1D70 BACK_END_WRITE_SEMAPHORE_RELEASE
 *   0x1D74 TEXTURE_READ_SEMAPHORE_RELEASE
 *   0x0394 CLIP_MIN               0x0398 CLIP_MAX
 *   0x08C0 SCISSOR_HORIZ          0x08C4 SCISSOR_VERT
 *   0xEB00/0xEB04 GCM user command
 *   NV406E (FIFO engine, raw address < 0x80, any subchannel):
 *   0x0050 SET_REFERENCE   0x0060 SET_CONTEXT_DMA_SEMAPHORE
 *   0x0064 SEMAPHORE_OFFSET  0x0068 SEMAPHORE_ACQUIRE  0x006C SEMAPHORE_RELEASE
 *   NV0039 (sub 1, 0x2000+): 0x2184/0x2188 DMA in/out, 0x230C/0x2310
 *   OFFSET_IN/OUT, 0x2314/0x2318 PITCH_IN/OUT, 0x231C LINE_LENGTH_IN,
 *   0x2320 LINE_COUNT, 0x2324 FORMAT, 0x2328 BUFFER_NOTIFY (trigger)
 *   NV3062 (sub 3, 0x6000+): 0x6184/0x6188 DMA src/dst, 0x6300 COLOR_FORMAT,
 *   0x6304 PITCH (src<<16|dst), 0x6308/0x630C OFFSET_SOURCE/DESTIN
 *   NV308A (sub 5, 0xA000+): 0xA304 POINT (y<<16|x), 0xA308 SIZE_OUT,
 *   0xA30C SIZE_IN, 0xA400..0xAAFC COLOR data window
 *   NV3089 (sub 6, 0xC000+): 0xC184 DMA_IMAGE, 0xC300 COLOR_FORMAT,
 *   0xC304 OPERATION, 0xC308/0xC30C CLIP point/size, 0xC310/0xC314 OUT
 *   point/size, 0xC318/0xC31C DS_DX/DT_DY, 0xC400 IN_SIZE, 0xC404
 *   IN_FORMAT (pitch | origin | interpolator), 0xC408 IN_OFFSET, 0xC40C
 *   IN_POINT (trigger)
 */

#include "rsx_nir_adapter.h"

#include <string.h>

#define M_WAIT_FOR_IDLE         0x0110
#define M_CONTEXT_DMA_REPORT    0x01A8
#define M_ALPHA_TEST_ENABLE     0x0304
#define M_DITHER_ENABLE         0x0300
#define M_ALPHA_FUNC            0x0308
#define M_ALPHA_REF             0x030C
#define M_BLEND_ENABLE          0x0310
#define M_BLEND_SFACTOR         0x0314
#define M_BLEND_DFACTOR         0x0318
#define M_BLEND_COLOR           0x031C
#define M_BLEND_EQUATION        0x0320
#define M_COLOR_MASK            0x0324
#define M_STENCIL_TEST_ENABLE   0x0328
#define M_STENCIL_WRITE_MASK    0x032C
#define M_STENCIL_FUNC          0x0330
#define M_STENCIL_FUNC_REF      0x0334
#define M_STENCIL_FUNC_MASK     0x0338
#define M_STENCIL_OP_FAIL       0x033C
#define M_STENCIL_OP_ZFAIL      0x0340
#define M_STENCIL_OP_ZPASS      0x0344
#define M_MRT_COLOR_MASK        0x0370
#define M_CONTROL0              0x03B0
#define M_WINDOW_OFFSET         0x02B8
#define M_DEPTH_BOUNDS_ENABLE   0x0380
#define M_DEPTH_BOUNDS_MIN      0x0384
#define M_DEPTH_BOUNDS_MAX      0x0388
#define M_CLIP_MIN              0x0394
#define M_CLIP_MAX              0x0398
#define M_SCISSOR_HORIZ         0x08C0
#define M_SCISSOR_VERT          0x08C4
#define M_VERTEX_TEXTURE        0x0900
#define M_TEXCOORD_CONTROL      0x0B40
#define M_POLY_OFFSET_POINT_EN  0x0A60
#define M_POLY_OFFSET_LINE_EN   0x0A64
#define M_POLY_OFFSET_FILL_EN   0x0A68
#define M_DEPTH_FUNC            0x0A6C
#define M_DEPTH_WRITE_ENABLE    0x0A70
#define M_DEPTH_TEST_ENABLE     0x0A74
#define M_POLY_OFFSET_SCALE     0x0A78
#define M_POLY_OFFSET_BIAS      0x0A7C
#define M_CLEAR_REPORT_VALUE    0x17C8
#define M_ZPASS_COUNT_ENABLE    0x17CC
#define M_ZCULL_CONTROL0        0x1EA4
#define M_ZCULL_CONTROL1        0x1EA8
#define M_SCULL_CONTROL         0x1EAC
#define M_GET_REPORT            0x1800
#define M_RENDER_ENABLE         0x1E98
#define M_CULL_FACE             0x1830
#define M_FRONT_FACE            0x1834
#define M_CULL_FACE_ENABLE      0x183C
#define M_POLYGON_MODE_FRONT    0x1828
#define M_POLYGON_MODE_BACK     0x182C
#define M_SEMAPHORE_OFFSET_3D   0x1D6C
#define M_BACK_END_SEM_RELEASE  0x1D70
#define M_TEX_READ_SEM_RELEASE  0x1D74
#define M_ZMIN_MAX_CONTROL      0x1D78
#define M_ANTI_ALIAS_CONTROL    0x1D7C
#define M_CLEAR_COLOR_VALUE     0x1D90
#define M_CLEAR_DEPTH_VALUE     0x1D8C
#define M_SHADER_WINDOW         0x1D88
#define M_VP_UPLOAD_CONST_ID    0x1EFC
#define M_VP_UPLOAD_CONST       0x1F00
#define M_VP_ATTRIB_EN          0x1FF0
#define M_VP_RESULT_EN          0x1FF4
#define M_FREQUENCY_DIVIDER_OP  0x1FC0
#define M_INVALIDATE_L2         0x1FD8
#define M_INVALIDATE_VERTEX     0x1710
#define M_TRANSFORM_TIMEOUT     0x1EF8
#define M_TRANSFORM_BRANCH_BITS 0x1FF8
#define M_POINT_PARAMS_ENABLE   0x1EE4
#define M_POINT_SPRITE_CONTROL  0x1EE8
#define M_TWO_SIDED_STENCIL     0x0348
#define M_BACK_STENCIL_MASK     0x034C
#define M_BACK_STENCIL_FUNC     0x0350
#define M_BACK_STENCIL_FUNC_REF 0x0354
#define M_BACK_STENCIL_FUNC_MASK 0x0358
#define M_BACK_STENCIL_OP_FAIL  0x035C
#define M_BACK_STENCIL_OP_ZFAIL 0x0360
#define M_BACK_STENCIL_OP_ZPASS 0x0364
#define M_USER_COMMAND_CAUSE    0xEB00
#define M_USER_COMMAND_FIRE     0xEB04

#define M_CTX_DMA_SEMAPHORE_3D  0x01A4

#define M406E_SET_REFERENCE     0x0050
#define M406E_SET_CTX_DMA_SEM   0x0060
#define M406E_SEMAPHORE_OFFSET  0x0064
#define M406E_SEMAPHORE_ACQUIRE 0x0068
#define M406E_SEMAPHORE_RELEASE 0x006C

/* NV0039 (sub 1) */
#define M0039_DMA_BUFFER_IN     0x2184
#define M0039_DMA_BUFFER_OUT    0x2188
#define M0039_OFFSET_IN         0x230C
#define M0039_OFFSET_OUT        0x2310
#define M0039_PITCH_IN          0x2314
#define M0039_PITCH_OUT         0x2318
#define M0039_LINE_LENGTH_IN    0x231C
#define M0039_LINE_COUNT        0x2320
#define M0039_FORMAT            0x2324
#define M0039_BUFFER_NOTIFY     0x2328

/* NV3062 (sub 3) */
#define M3062_DMA_IMAGE_SOURCE  0x6184
#define M3062_DMA_IMAGE_DESTIN  0x6188
#define M3062_COLOR_FORMAT      0x6300
#define M3062_PITCH             0x6304
#define M3062_OFFSET_SOURCE     0x6308
#define M3062_OFFSET_DESTIN     0x630C

/* NV308A (sub 5) */
#define M308A_POINT             0xA304
#define M308A_SIZE_OUT          0xA308
#define M308A_SIZE_IN           0xA30C
#define M308A_COLOR_FIRST       0xA400
#define M308A_COLOR_LAST        0xAAFC

/* NV3089 (sub 6) */
#define M3089_DMA_IMAGE         0xC184
#define M3089_CONTEXT_SURFACE   0xC198
#define M3089_COLOR_CONVERSION  0xC2FC
#define M3089_COLOR_FORMAT      0xC300
#define M3089_OPERATION         0xC304
#define M3089_CLIP_POINT        0xC308
#define M3089_CLIP_SIZE         0xC30C
#define M3089_OUT_POINT         0xC310
#define M3089_OUT_SIZE          0xC314
#define M3089_DS_DX             0xC318
#define M3089_DT_DY             0xC31C
#define M3089_IN_SIZE           0xC400
#define M3089_IN_FORMAT         0xC404
#define M3089_IN_OFFSET         0xC408
#define M3089_IN_POINT          0xC40C

#define GCM_CONTEXT_SURFACE2D   0x313371C3u

/* DMA context handles: bit 0 of the gcm dma selector picks main memory
 * (matching rsx_dispatch.c's location decode for surfaces/textures). */
static u32 dma_location(u32 dma)
{
    return (dma & 1u) ? RSX_NIR_LOCATION_MAIN : RSX_NIR_LOCATION_LOCAL;
}

/* per-adapter tracking of which constant slots have defined values
 * (uploaded through the stream or seeded) — the dispatcher itself keeps
 * no dirty state. Storage lives in the adapter via a side table. */
static void note_constant_upload(rsx_nir_adapter* ad, u32 method)
{
    u32 word = (method - M_VP_UPLOAD_CONST) >> 2;
    u32 slot = rsx_dsp_reg(&ad->rsx, M_VP_UPLOAD_CONST_ID) + (word >> 2);
    if (slot < RSX_NIR_NUM_CONSTANTS)
        ad->em.pending.constants_written[slot] = 1; /* provisional; value
                                                       staged at flush     */
}

/* ---- state staging: getters -> emitter --------------------------------- */

/* Vertex-program extent: instructions from the execution start slot up to
 * and including the first one with the D3.end bit (rsx_vp_decompiler.c:62
 * uses the same rule). No END bit found = no valid program (count 0). */
static u32 vp_extent_words(const rsx_dispatch* rsx, u32 start_slot)
{
    for (u32 slot = start_slot; slot < RSX_DSP_VP_INSTR; slot++) {
        if (rsx->vp[slot * 4 + 3] & 1u)
            return (slot - start_slot + 1) * 4;
    }
    return 0;
}

static void stage_state(rsx_nir_adapter* ad)
{
    const rsx_dispatch* rsx = &ad->rsx;
    rsx_nir_emitter* em = &ad->em;

    /* surface */
    rsx_dsp_surface s;
    rsx_dsp_get_surface(rsx, &s);
    rsx_nir_surface ns;
    memset(&ns, 0, sizeof(ns));
    ns.color_format = s.color_format;
    ns.depth_format = s.depth_format;
    ns.raster_type  = s.raster_type;
    ns.clip_x = s.clip_x; ns.clip_y = s.clip_y;
    ns.clip_w = s.clip_w; ns.clip_h = s.clip_h;
    for (u32 i = 0; i < RSX_NIR_NUM_RENDER_TARGETS; i++) {
        ns.color_offset[i]   = s.color_offset[i];
        ns.color_pitch[i]    = s.color_pitch[i];
        ns.color_location[i] = s.color_location[i];
    }
    ns.color_target  = s.color_target;
    ns.zeta_offset   = s.zeta_offset;
    ns.zeta_pitch    = s.zeta_pitch;
    ns.zeta_location = s.zeta_location;
    rsx_nir_em_surface(em, &ns);

    /* viewport + depth range */
    rsx_dsp_viewport v;
    rsx_dsp_get_viewport(rsx, &v);
    rsx_nir_viewport nv;
    memset(&nv, 0, sizeof(nv));
    nv.x = v.x; nv.y = v.y; nv.w = v.w; nv.h = v.h;
    memcpy(nv.scale, v.scale, sizeof(nv.scale));
    memcpy(nv.translate, v.translate, sizeof(nv.translate));
    u32 cmin = rsx_dsp_reg(rsx, M_CLIP_MIN);
    u32 cmax = rsx_dsp_reg(rsx, M_CLIP_MAX);
    memcpy(&nv.clip_min, &cmin, 4);
    memcpy(&nv.clip_max, &cmax, 4);
    rsx_nir_em_viewport(em, &nv);

    /* scissor */
    rsx_nir_scissor sc;
    memset(&sc, 0, sizeof(sc));
    u32 sh = rsx_dsp_reg(rsx, M_SCISSOR_HORIZ);
    u32 sv = rsx_dsp_reg(rsx, M_SCISSOR_VERT);
    sc.x = sh & 0xFFFF; sc.w = sh >> 16;
    sc.y = sv & 0xFFFF; sc.h = sv >> 16;
    rsx_nir_em_scissor(em, &sc);

    /* raster */
    rsx_nir_raster ra;
    memset(&ra, 0, sizeof(ra));
    ra.cull_face_enable = rsx_dsp_reg(rsx, M_CULL_FACE_ENABLE);
    ra.cull_face        = rsx_dsp_reg(rsx, M_CULL_FACE);
    ra.front_face       = rsx_dsp_reg(rsx, M_FRONT_FACE);
    ra.polygon_offset_point_enable =
        rsx_dsp_reg(rsx, M_POLY_OFFSET_POINT_EN);
    ra.polygon_offset_line_enable =
        rsx_dsp_reg(rsx, M_POLY_OFFSET_LINE_EN);
    ra.polygon_offset_fill_enable =
        rsx_dsp_reg(rsx, M_POLY_OFFSET_FILL_EN);
    ra.polygon_offset_scale = rsx_dsp_reg(rsx, M_POLY_OFFSET_SCALE);
    ra.polygon_offset_bias  = rsx_dsp_reg(rsx, M_POLY_OFFSET_BIAS);
    ra.color_mask       = rsx_dsp_reg(rsx, M_COLOR_MASK);
    ra.mrt_color_mask   = rsx_dsp_reg(rsx, M_MRT_COLOR_MASK);
    rsx_nir_em_raster(em, &ra);

    /* Complete front/back stencil state. D3D12 shares read/write masks and
     * the dynamic reference between both faces; draw preflight therefore
     * keeps unequal front/back values on the whole-section legacy path. */
    rsx_nir_depth_stencil ds;
    memset(&ds, 0, sizeof(ds));
    ds.depth_test_enable   = rsx_dsp_reg(rsx, M_DEPTH_TEST_ENABLE);
    ds.depth_func          = rsx_dsp_reg(rsx, M_DEPTH_FUNC);
    ds.depth_write_enable  = rsx_dsp_reg(rsx, M_DEPTH_WRITE_ENABLE);
    ds.depth_bounds_test_enable =
        rsx_dsp_reg(rsx, M_DEPTH_BOUNDS_ENABLE);
    ds.depth_bounds_min = rsx_dsp_reg(rsx, M_DEPTH_BOUNDS_MIN);
    ds.depth_bounds_max = rsx_dsp_reg(rsx, M_DEPTH_BOUNDS_MAX);
    ds.stencil_test_enable = rsx_dsp_reg(rsx, M_STENCIL_TEST_ENABLE);
    ds.stencil_func        = rsx_dsp_reg(rsx, M_STENCIL_FUNC);
    ds.stencil_ref         = rsx_dsp_reg(rsx, M_STENCIL_FUNC_REF);
    ds.stencil_mask        = rsx_dsp_reg(rsx, M_STENCIL_FUNC_MASK);
    ds.stencil_write_mask  = rsx_dsp_reg(rsx, M_STENCIL_WRITE_MASK);
    ds.stencil_op_fail     = rsx_dsp_reg(rsx, M_STENCIL_OP_FAIL);
    ds.stencil_op_zfail    = rsx_dsp_reg(rsx, M_STENCIL_OP_ZFAIL);
    ds.stencil_op_zpass    = rsx_dsp_reg(rsx, M_STENCIL_OP_ZPASS);
    ds.two_sided_stencil_enable =
        rsx_dsp_reg(rsx, M_TWO_SIDED_STENCIL);
    ds.back_stencil_write_mask =
        rsx_dsp_reg(rsx, M_BACK_STENCIL_MASK);
    ds.back_stencil_func = rsx_dsp_reg(rsx, M_BACK_STENCIL_FUNC);
    ds.back_stencil_ref = rsx_dsp_reg(rsx, M_BACK_STENCIL_FUNC_REF);
    ds.back_stencil_mask = rsx_dsp_reg(rsx, M_BACK_STENCIL_FUNC_MASK);
    ds.back_stencil_op_fail = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_FAIL);
    ds.back_stencil_op_zfail = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZFAIL);
    ds.back_stencil_op_zpass = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZPASS);
    rsx_nir_em_depth_stencil(em, &ds);

    /* blend + alpha test */
    rsx_nir_blend bl;
    memset(&bl, 0, sizeof(bl));
    bl.blend_enable      = rsx_dsp_reg(rsx, M_BLEND_ENABLE);
    bl.sfactor           = rsx_dsp_reg(rsx, M_BLEND_SFACTOR);
    bl.dfactor           = rsx_dsp_reg(rsx, M_BLEND_DFACTOR);
    bl.equation          = rsx_dsp_reg(rsx, M_BLEND_EQUATION);
    bl.blend_color       = rsx_dsp_reg(rsx, M_BLEND_COLOR);
    bl.alpha_test_enable = rsx_dsp_reg(rsx, M_ALPHA_TEST_ENABLE);
    bl.alpha_func        = rsx_dsp_reg(rsx, M_ALPHA_FUNC);
    bl.alpha_ref         = rsx_dsp_reg(rsx, M_ALPHA_REF);
    rsx_nir_em_blend(em, &bl);
    rsx_nir_em_render_condition(em, &ad->render_condition);

    /* fragment program */
    rsx_nir_fragment_program fp;
    memset(&fp, 0, sizeof(fp));
    fp.offset  = rsx_dsp_fragment_program(rsx, &fp.location);
    fp.control = rsx_dsp_shader_control(rsx);
    fp.shader_window = rsx_dsp_reg(rsx, M_SHADER_WINDOW);
    for (u32 unit = 0; unit < 10u; ++unit)
        fp.texcoord_2d_mask |=
            (rsx_dsp_reg(rsx, M_TEXCOORD_CONTROL + unit * 4u) & 1u)
            << unit;
    rsx_nir_em_fragment_program(em, &fp);

    /* vertex program */
    u32 vp_start = rsx_dsp_vp_start(rsx);
    u32 vp_words = vp_extent_words(rsx, vp_start);
    rsx_nir_em_vertex_program(em, vp_start,
                              rsx->vp + vp_start * 4, vp_words,
                              rsx_dsp_reg(rsx, M_VP_ATTRIB_EN),
                              rsx_dsp_reg(rsx, M_VP_RESULT_EN),
                              rsx_dsp_reg(rsx, M_TRANSFORM_BRANCH_BITS));

    /* vertex bindings */
    rsx_nir_vertex_bindings vb;
    memset(&vb, 0, sizeof(vb));
    for (u32 i = 0; i < RSX_NIR_NUM_VERTEX_ATTR; i++) {
        rsx_dsp_vertex_attr a;
        rsx_dsp_get_vertex_attr(rsx, i, &a);
        vb.attr[i].type      = a.type;
        vb.attr[i].size      = a.size;
        vb.attr[i].stride    = a.stride;
        vb.attr[i].frequency = a.frequency;
        vb.attr[i].offset    = a.offset;
        vb.attr[i].location  = a.location;
        rsx_dsp_vertex_default(rsx, i, vb.attr[i].def);
    }
    vb.base_offset = rsx_dsp_vertex_data_base_offset(rsx);
    vb.base_index  = rsx_dsp_vertex_data_base_index(rsx);
    vb.freq_divider_op = rsx_dsp_reg(rsx, M_FREQUENCY_DIVIDER_OP);
    rsx_nir_em_vertex_bindings(em, &vb);

    /* index binding */
    rsx_dsp_index_array ia;
    rsx_dsp_get_index_array(rsx, &ia);
    rsx_nir_index_binding ib;
    memset(&ib, 0, sizeof(ib));
    ib.offset         = ia.offset;
    ib.location       = ia.location;
    ib.is_u32         = ia.is_u32;
    ib.restart_enable = (u32)rsx_dsp_restart_index_enabled(rsx, (int)ia.is_u32);
    ib.restart_index  = rsx_dsp_restart_index(rsx);
    rsx_nir_em_index_binding(em, &ib);

    /* textures */
    for (u32 t = 0; t < RSX_NIR_NUM_TEXTURES; t++) {
        rsx_dsp_texture tx;
        rsx_dsp_get_texture(rsx, t, &tx);
        rsx_nir_texture nt;
        memset(&nt, 0, sizeof(nt));
        nt.enabled = tx.enabled; nt.offset = tx.offset;
        nt.location = tx.location; nt.format = tx.format;
        nt.dimension = tx.dimension; nt.cubemap = tx.cubemap;
        nt.mipmaps = tx.mipmaps; nt.width = tx.width; nt.height = tx.height;
        nt.pitch = tx.pitch; nt.depth = tx.depth; nt.wrap = tx.wrap;
        nt.remap = tx.remap; nt.filter = tx.filter; nt.control0 = tx.control0;
        nt.border_color = tx.border_color;
        rsx_nir_em_texture(em, t, &nt);
    }
    for (u32 t = 0; t < RSX_NIR_NUM_VERTEX_TEXTURES; t++) {
        rsx_dsp_vertex_texture tx;
        rsx_dsp_get_vertex_texture(rsx, t, &tx);
        rsx_nir_texture nt;
        memset(&nt, 0, sizeof(nt));
        nt.enabled = tx.enabled; nt.offset = tx.offset;
        nt.location = tx.location; nt.format = tx.format;
        nt.dimension = tx.dimension; nt.cubemap = tx.cubemap;
        nt.mipmaps = tx.mipmaps; nt.width = tx.width; nt.height = tx.height;
        nt.pitch = tx.pitch; nt.depth = tx.depth; nt.wrap = tx.wrap;
        nt.filter = tx.filter; nt.control0 = tx.control0;
        nt.border_color = tx.border_color;
        rsx_nir_em_vertex_texture(em, t, &nt);
    }

    /* constants: stage every slot the stream (or seed) defined; the
     * emitter's shadow diff keeps re-staging cheap in the output. */
    for (u32 slot = 0; slot < RSX_NIR_NUM_CONSTANTS; slot++) {
        if (em->pending.constants_written[slot]) {
            rsx_nir_em_constants(em, slot, 1,
                                 &rsx->constants[slot][0]);
        }
    }
}

/* ---- NV308A inline-color accumulation ---------------------------------- */

void rsx_nir_adapter_stage_state(rsx_nir_adapter* ad)
{
    stage_state(ad);
}

static void flush_inline(rsx_nir_adapter* ad)
{
    if (!ad->inline_count)
        return;
    if (ad->shadow_mode) {
        ad->inline_count = 0;
        return;
    }
    /* Hardware skips COLOR operands whose window index is >= SIZE_OUT.x:
     * the SDK's SetInlineTransfer pads every run to an even word count
     * with a zero word (SetInlineTransfer
     * paddedSizeInWords), and the live consumer skips
     * index >= SIZE_OUT.x (import_overrides.cpp NV308A window). Clamp
     * the payload to the valid window so the fold equals a typed
     * producer's natural emission and no executor writes the padding. */
    {
        const u32 out_x = ad->inline_size_out & 0xFFFFu;
        const u32 valid = ad->inline_first_index < out_x
                              ? out_x - ad->inline_first_index
                              : 0;
        if (ad->inline_count > valid)
            ad->inline_count = valid;
        if (!ad->inline_count)
            return;                  /* run was padding/out-of-range only */
    }
    stage_state(ad);
    rsx_nir_transfer t;
    memset(&t, 0, sizeof(t));
    t.kind         = RSX_NIR_XFER_INLINE;
    t.dst_location = dma_location(ad->s2d_dma_dst);
    t.dst_offset   = ad->s2d_offset_dst;
    t.dst_pitch    = ad->s2d_pitch & 0xFFFFu;
    t.dst_format   = ad->s2d_color_format;
    t.point_x      = (ad->inline_point & 0xFFFFu) + ad->inline_first_index;
    t.point_y      = ad->inline_point >> 16;
    t.size_w       = ad->inline_size_out & 0xFFFFu;
    t.size_h       = ad->inline_size_out >> 16;
    t.word_count   = ad->inline_count;
    rsx_nir_em_transfer(&ad->em, &t, ad->inline_words);
    ad->actions_seen++;
    ad->inline_count = 0;
}

static void add_inline_word(rsx_nir_adapter* ad, u32 index, u32 arg)
{
    if (ad->shadow_mode)
        return;         /* payload delivery is the FIFO path's job */
    if (ad->inline_count &&
        index != ad->inline_first_index + ad->inline_count) {
        /* non-contiguous run: the previous transfer is complete */
        flush_inline(ad);
    }
    if (!ad->inline_count)
        ad->inline_first_index = index;
    if (ad->inline_count >= RSX_NIR_ADAPTER_MAX_INLINE) {
        ad->inline_overflow++;
        flush_inline(ad);
        ad->inline_first_index = index;
    }
    ad->inline_words[ad->inline_count++] = arg;
}

/* ---- dispatch sink callbacks ------------------------------------------- */

static void sink_clear(void* user, const rsx_dispatch* rsx, u32 mask)
{
    rsx_nir_adapter* ad = user;
    if (ad->shadow_mode)
        return;
    stage_state(ad);
    u32 zs = rsx_dsp_clear_zstencil(rsx);
    rsx_nir_em_clear(&ad->em, mask, rsx_dsp_clear_color(rsx),
                     zs >> 8, zs & 0xFF);
    ad->actions_seen++;
}

static void sink_begin(void* user, const rsx_dispatch* rsx, u32 primitive)
{
    rsx_nir_adapter* ad = user;
    (void)rsx; (void)primitive;
    ad->batch_count = 0;
    ad->draw_indexed = 0;
    ad->draw_mixed = 0;
}

static void add_batch(rsx_nir_adapter* ad, u32 first, u32 count, u32 indexed)
{
    if (ad->batch_count && ad->draw_indexed != indexed)
        ad->draw_mixed = 1;
    ad->draw_indexed = indexed;
    if (ad->batch_count >= RSX_NIR_ADAPTER_MAX_BATCHES) {
        ad->batch_overflow++;
        return;
    }
    ad->batches[ad->batch_count * 2]     = first;
    ad->batches[ad->batch_count * 2 + 1] = count;
    ad->batch_count++;
}

static void sink_draw_arrays(void* user, const rsx_dispatch* rsx,
                             u32 first, u32 count)
{
    (void)rsx;
    add_batch(user, first, count, 0);
}

static void sink_draw_index_array(void* user, const rsx_dispatch* rsx,
                                  u32 first, u32 count)
{
    (void)rsx;
    add_batch(user, first, count, 1);
}

static void sink_end(void* user, const rsx_dispatch* rsx)
{
    rsx_nir_adapter* ad = user;
    if (ad->shadow_mode) {
        ad->batch_count = 0;
        return;
    }
    if (!ad->batch_count)
        return;   /* state-only or immediate-mode begin/end: no batched draw */
    stage_state(ad);
    rsx_nir_em_draw(&ad->em, rsx->current_primitive, ad->draw_indexed,
                    ad->batches, ad->batch_count);
    ad->actions_seen++;
    ad->batch_count = 0;
}

static void sink_flip(void* user, const rsx_dispatch* rsx, u32 arg)
{
    rsx_nir_adapter* ad = user;
    (void)rsx;
    if (ad->shadow_mode)
        return;
    stage_state(ad);
    rsx_nir_em_present(&ad->em, arg);
    ad->actions_seen++;
}

/* ---- public ------------------------------------------------------------ */

void rsx_nir_adapter_rebind(rsx_nir_adapter* ad)
{
    if (ad)
        ad->rsx.sink.user = ad;
}

void rsx_nir_adapter_init_sink(rsx_nir_adapter* ad, const rsx_nir_sink* out)
{
    memset(ad, 0, sizeof(*ad));
    rsx_nir_emitter_init(&ad->em, out);

    rsx_dispatch_sink sink;
    memset(&sink, 0, sizeof(sink));
    sink.user = ad;
    sink.clear = sink_clear;
    sink.begin = sink_begin;
    sink.end = sink_end;
    sink.draw_arrays = sink_draw_arrays;
    sink.draw_index_array = sink_draw_index_array;
    sink.flip = sink_flip;
    rsx_dispatch_init(&ad->rsx, &sink);
    rsx_nir_adapter_rebind(ad);

    /* NV308A defaults matching the live consumer's initializers.
     * 0xB = CELL_GCM_TRANSFER_SURFACE_FORMAT_Y32, the SDK inline-
     * transfer default (the live consumer's "a8r8g8b8" comment for 0xB
     * is mislabeled; A8R8G8B8 is 0xA — both are 4-byte raw copies). */
    ad->s2d_pitch = 64;
    ad->s2d_color_format = 0xB;
    ad->inline_size_out = 0x00010001;
    ad->sif_context_surface = GCM_CONTEXT_SURFACE2D;
    ad->sif_color_conversion = 1u;
    /* NV406E's reset context matches the live FIFO engine.  Leaving this at
     * memset-zero made a section beginning with OFFSET/ACQUIRE unresolvable
     * until the title happened to issue SET_CONTEXT_DMA_SEMAPHORE. */
    ad->fifo_semaphore_dma = 0x66616661u;
}

void rsx_nir_adapter_init(rsx_nir_adapter* ad, rsx_nir_stream* out)
{
    rsx_nir_sink k = rsx_nir_stream_sink(out);
    rsx_nir_adapter_init_sink(ad, &k);
}

void rsx_nir_adapter_seed(rsx_nir_adapter* ad, const u32* regs, u32 reg_words,
                          const u32* vp, u32 vp_words,
                          const u32* constants, u32 constant_words)
{
    if (regs)
        rsx_dispatch_seed_registers(&ad->rsx, regs, reg_words);
    if (vp)
        rsx_dispatch_seed_transform_program(&ad->rsx, vp, vp_words);
    if (constants) {
        rsx_dispatch_seed_transform_constants(&ad->rsx, constants,
                                              constant_words);
        for (u32 slot = 0; slot < constant_words / 4 &&
                           slot < RSX_NIR_NUM_CONSTANTS; slot++)
            ad->em.pending.constants_written[slot] = 1;
    }
}

/* NV406E FIFO-engine methods (raw address < 0x80 on any subchannel).
 * Returns 1 when handled. */
static int fifo_engine_method(rsx_nir_adapter* ad, u32 m, u32 arg)
{
    switch (m) {
    case M406E_SET_REFERENCE:
        if (ad->shadow_mode)
            return 1;
        stage_state(ad);
        rsx_nir_em_set_reference(&ad->em, arg);
        ad->actions_seen++;
        return 1;
    case M406E_SET_CTX_DMA_SEM:
        ad->fifo_semaphore_dma = arg;
        return 1;
    case M406E_SEMAPHORE_OFFSET:
        ad->fifo_semaphore_offset = arg;
        return 1;
    case M406E_SEMAPHORE_ACQUIRE:
        if (ad->shadow_mode)
            return 1;
        stage_state(ad);
        rsx_nir_em_semaphore_acquire(&ad->em, ad->fifo_semaphore_dma,
                                     ad->fifo_semaphore_offset, arg);
        ad->actions_seen++;
        return 1;
    case M406E_SEMAPHORE_RELEASE:
        if (ad->shadow_mode)
            return 1;
        stage_state(ad);
        rsx_nir_em_semaphore_release(&ad->em, ad->fifo_semaphore_dma,
                                     ad->fifo_semaphore_offset, arg, 2);
        ad->actions_seen++;
        return 1;
    default:
        return m < 0x80;   /* other FIFO-engine controls: consumed, no op  */
    }
}

int rsx_nir_adapter_method_supported(
    const rsx_nir_adapter* ad, u32 method, u32 arg)
{
    if (!ad)
        return 0;
    method &= 0xFFFFCu;
    if (method < 0x100u) {
        switch (method) {
        case M406E_SET_REFERENCE:
        case M406E_SET_CTX_DMA_SEM:
        case M406E_SEMAPHORE_OFFSET:
        case M406E_SEMAPHORE_ACQUIRE:
        case M406E_SEMAPHORE_RELEASE:
            return 1;
        default:
            return 0;
        }
    }

    switch (method) {
    case M_WAIT_FOR_IDLE:
    case M_CONTEXT_DMA_REPORT:
    case M_CTX_DMA_SEMAPHORE_3D:
    case M_SEMAPHORE_OFFSET_3D:
    case M_BACK_END_SEM_RELEASE:
    case M_TEX_READ_SEM_RELEASE:
    case M_CLEAR_REPORT_VALUE:
    case M_GET_REPORT:
    case M_USER_COMMAND_CAUSE:
    case M_USER_COMMAND_FIRE:
    case 0xE924u: /* typed PRESENT publishes the companion head package */
    case M0039_DMA_BUFFER_IN:
    case M0039_DMA_BUFFER_OUT:
    case M0039_OFFSET_IN:
    case M0039_OFFSET_OUT:
    case M0039_PITCH_IN:
    case M0039_PITCH_OUT:
    case M0039_LINE_LENGTH_IN:
    case M0039_LINE_COUNT:
    case M0039_FORMAT:
    case M0039_BUFFER_NOTIFY:
    case M3062_DMA_IMAGE_SOURCE:
    case M3062_DMA_IMAGE_DESTIN:
    case M3062_COLOR_FORMAT:
    case M3062_PITCH:
    case M3062_OFFSET_SOURCE:
    case M3062_OFFSET_DESTIN:
    case M308A_POINT:
    case M308A_SIZE_OUT:
    case M308A_SIZE_IN:
    case M3089_DMA_IMAGE:
    case M3089_COLOR_FORMAT:
    case M3089_OPERATION:
    case M3089_CLIP_POINT:
    case M3089_CLIP_SIZE:
    case M3089_OUT_POINT:
    case M3089_OUT_SIZE:
    case M3089_DS_DX:
    case M3089_DT_DY:
    case M3089_IN_SIZE:
    case M3089_IN_FORMAT:
    case M3089_IN_OFFSET:
    case M3089_IN_POINT:
        return 1;
    default:
        break;
    }
    if (method >= M308A_COLOR_FIRST && method <= M308A_COLOR_LAST)
        return 1;
    /* Preserve the complete four-unit vertex-texture register file in typed
     * state.  Draw preflight currently rejects enabled vertex textures and
     * TXL microcode, while ordinary disabled register programming is inert. */
    if (method >= M_VERTEX_TEXTURE &&
        method < M_VERTEX_TEXTURE + RSX_NIR_NUM_VERTEX_TEXTURES * 0x20u)
        return 1;
    /* These stored-register methods are completely represented by the
     * derived typed state staged above, or are cache/scheduler hints whose
     * ordering is conservatively covered by the exact dirty-page mirror.
     * ZMIN_MAX is accepted only in the title's captured hardware-default
     * mode: normal depth clipping on, clamp/ignore-W off. */
    if (method == M_CLIP_MIN || method == M_CLIP_MAX ||
        method == M_SCISSOR_HORIZ || method == M_SCISSOR_VERT ||
        method == M_FREQUENCY_DIVIDER_OP ||
        (method >= M_STENCIL_TEST_ENABLE &&
         method <= M_STENCIL_OP_ZPASS) ||
        (method >= M_TEXCOORD_CONTROL &&
         method < M_TEXCOORD_CONTROL + 10u * 4u) ||
        method == 0x036Cu || /* MRT blend: preflight requires TARGET_0 */
        method == M_INVALIDATE_VERTEX || method == M_INVALIDATE_L2 ||
        method == M_TRANSFORM_TIMEOUT)
        return 1;
    if (method == M_SHADER_WINDOW) {
        const u32 origin = (arg >> 12) & 0xFu;
        const u32 center = (arg >> 16) & 0xFu;
        return origin <= 1u && center <= 1u && !(arg & 0xFFF00000u);
    }
    if (method == M_TWO_SIDED_STENCIL)
        return arg <= 1u;
    /* The branch register is retained in typed vertex-program state. Native
     * admission still validates the exact bound microcode before a complete
     * section can be owned. */
    if (method == M_TRANSFORM_BRANCH_BITS)
        return 1;
    /* Back-face stencil is fully retained in typed state. Whether D3D12 can
     * represent its independent masks/reference is decided against the
     * complete folded draw state during transactional preflight. */
    if (method >= M_BACK_STENCIL_MASK &&
        method <= M_BACK_STENCIL_OP_ZPASS)
        return 1;
    if (method == M_CONTROL0)
        return arg == 0x00100000u;
    /* D3D12 has no fixed-function color dithering equivalent.  The title's
     * live production stream explicitly disables it, which is bit-exactly
     * represented by doing nothing.  Keep enabled dithering on legacy. */
    if (method == M_DITHER_ENABLE)
        return arg == 0u;
    /* Point-distance attenuation is absent from the native pipeline.  The
     * captured title state explicitly disables it; admit only that exact
     * mode and retain legacy ownership for the enabled hardware feature. */
    if (method == M_POINT_PARAMS_ENABLE)
        return arg == 0u;
    /* Bit 0 is the actual point-sprite enable; bits 8..17 are texcoord
     * replacement masks and are inert while disabled.  Admit the hardware
     * reset and the title's exact disabled/TEX0-mask form only. */
    if (method == M_POINT_SPRITE_CONTROL)
        return arg == 0u || arg == 0x100u;
    if (method == M_ZPASS_COUNT_ENABLE)
        return arg <= 1u;
    if (method == M_RENDER_ENABLE) {
        const u32 mode = arg >> 24;
        return mode == 1u || mode == 2u;
    }
    /* ZCULL configuration changes the hardware early-Z accelerator, not
     * rasterization results.  Reports on this path already use the same
     * defined zero-count contract as legacy.  Fence to the captured mode. */
    if (method == M_ZCULL_CONTROL0)
        return arg == 0x10u;
    if (method == M_ZCULL_CONTROL1)
        return arg == 0x01000100u;
    if (method == M_SCULL_CONTROL)
        return arg == 0xFF000002u;
    if (method == M_ANTI_ALIAS_CONTROL)
        return arg == 0xFFFF0000u;
    /* The native backend currently implements the hardware-default variants
     * of these controls.  Admit only those exact values; other modes keep the
     * complete section on the legacy path. */
    if (method == M_WINDOW_OFFSET)
        return arg == 0u;
    if (method == M_DEPTH_BOUNDS_ENABLE)
        return arg <= 1u;
    if (method == M_DEPTH_BOUNDS_MIN || method == M_DEPTH_BOUNDS_MAX)
        return ((arg >> 23) & 0xFFu) != 0xFFu; /* finite IEEE-754 only */
    if (method == M_POLYGON_MODE_FRONT || method == M_POLYGON_MODE_BACK)
        return arg == 0x1B02u; /* CELL_GCM_POLYGON_MODE_FILL */
    if (method == M_POLY_OFFSET_POINT_EN ||
        method == M_POLY_OFFSET_LINE_EN ||
        method == M_POLY_OFFSET_FILL_EN)
        return arg <= 1u;
    if (method == M_POLY_OFFSET_SCALE || method == M_POLY_OFFSET_BIAS)
        return ((arg >> 23) & 0xFFu) != 0xFFu; /* finite IEEE-754 only */
    if (method == M3089_CONTEXT_SURFACE)
        return arg == GCM_CONTEXT_SURFACE2D;
    if (method == M3089_COLOR_CONVERSION)
        return arg == 1u;
    if (method == M3089_OPERATION)
        return arg == 3u; /* CELL_GCM_TRANSFER_OPERATION_SRCCOPY */
    if (method == M_ZMIN_MAX_CONTROL)
        return arg == 1u;
    if (method >= 0x10000u)
        return 0;
    return ad->rsx.klass[method >> 2] != RSX_DSP_CLASS_TODO;
}

void rsx_nir_adapter_method(rsx_nir_adapter* ad, u32 method, u32 arg)
{
    method &= 0xFFFFC;
    ad->methods_seen++;

    /* an NV308A inline run ends at the first non-COLOR method */
    if (ad->inline_count &&
        (method < M308A_COLOR_FIRST || method > M308A_COLOR_LAST))
        flush_inline(ad);

    if (method < 0x100) {
        if (fifo_engine_method(ad, method, arg))
            return;
    }

    if (method >= M_VP_UPLOAD_CONST && method < M_VP_UPLOAD_CONST + 32 * 4)
        note_constant_upload(ad, method);

    rsx_dispatch_method(&ad->rsx, method, arg);

    /* ordered synchronization + data moves the register-file model stores
     * silently */
    switch (method) {
    case M_RENDER_ENABLE: {
        const u32 mode = arg >> 24;
        if (mode == 1u) {
            memset(&ad->render_condition, 0,
                   sizeof(ad->render_condition));
        } else if (mode == 2u) {
            ad->render_condition.enabled = 1u;
            ad->render_condition.dma_report =
                rsx_dsp_reg(&ad->rsx, M_CONTEXT_DMA_REPORT);
            ad->render_condition.offset = arg & 0x00FFFFFFu;
        }
        break;
    }
    case M_WAIT_FOR_IDLE:
        if (ad->shadow_mode)
            break;
        stage_state(ad);
        rsx_nir_em_barrier(&ad->em, 0);
        ad->actions_seen++;
        break;
    case M_BACK_END_SEM_RELEASE:
        if (ad->shadow_mode)
            break;
        stage_state(ad);
        rsx_nir_em_semaphore_release(&ad->em,
                                     rsx_dsp_reg(&ad->rsx, M_CTX_DMA_SEMAPHORE_3D),
                                     rsx_dsp_reg(&ad->rsx, M_SEMAPHORE_OFFSET_3D),
                                     arg, 0);
        ad->actions_seen++;
        break;
    case M_TEX_READ_SEM_RELEASE:
        if (ad->shadow_mode)
            break;
        stage_state(ad);
        rsx_nir_em_semaphore_release(&ad->em,
                                     rsx_dsp_reg(&ad->rsx, M_CTX_DMA_SEMAPHORE_3D),
                                     rsx_dsp_reg(&ad->rsx, M_SEMAPHORE_OFFSET_3D),
                                     arg, 1);
        ad->actions_seen++;
        break;
    case M_GET_REPORT:
        if (ad->shadow_mode)
            break;
        stage_state(ad);
        rsx_nir_em_report(&ad->em, 0, arg,
                          rsx_dsp_reg(&ad->rsx, M_CONTEXT_DMA_REPORT));
        ad->actions_seen++;
        break;
    case M_CLEAR_REPORT_VALUE:
        if (ad->shadow_mode)
            break;
        stage_state(ad);
        rsx_nir_em_report(&ad->em, 1, arg,
                          rsx_dsp_reg(&ad->rsx, M_CONTEXT_DMA_REPORT));
        ad->actions_seen++;
        break;
    case M_USER_COMMAND_CAUSE:
    case M_USER_COMMAND_FIRE:
        if (ad->shadow_mode)
            break;
        /* the live consumer treats both words as one delivery carrying the
         * cause argument; model the pair as one ordered action per method
         * (coalescing is a delivery-side behavior, not a stream one) */
        stage_state(ad);
        rsx_nir_em_user_command(&ad->em, arg);
        ad->actions_seen++;
        break;

    /* NV0039 buffer copy */
    case M0039_DMA_BUFFER_IN:   ad->m2mf_dma_in = arg; break;
    case M0039_DMA_BUFFER_OUT:  ad->m2mf_dma_out = arg; break;
    case M0039_OFFSET_IN:       ad->m2mf_offset_in = arg; break;
    case M0039_OFFSET_OUT:      ad->m2mf_offset_out = arg; break;
    case M0039_PITCH_IN:        ad->m2mf_pitch_in = arg; break;
    case M0039_PITCH_OUT:       ad->m2mf_pitch_out = arg; break;
    case M0039_LINE_LENGTH_IN:  ad->m2mf_line_length = arg; break;
    case M0039_LINE_COUNT:      ad->m2mf_line_count = arg; break;
    case M0039_FORMAT:          ad->m2mf_format = arg; break;
    case M0039_BUFFER_NOTIFY: {
        if (ad->shadow_mode)
            break;
        stage_state(ad);
        rsx_nir_transfer t;
        memset(&t, 0, sizeof(t));
        t.kind         = RSX_NIR_XFER_BUFFER;
        t.src_location = dma_location(ad->m2mf_dma_in);
        t.src_offset   = ad->m2mf_offset_in;
        t.src_pitch    = ad->m2mf_pitch_in;
        t.dst_location = dma_location(ad->m2mf_dma_out);
        t.dst_offset   = ad->m2mf_offset_out;
        t.dst_pitch    = ad->m2mf_pitch_out;
        t.src_format   = ad->m2mf_format & 0xFFu;
        t.dst_format   = ad->m2mf_format >> 8;
        t.line_length  = ad->m2mf_line_length;
        t.line_count   = ad->m2mf_line_count;
        rsx_nir_em_transfer(&ad->em, &t, NULL);
        ad->actions_seen++;
        break;
    }

    /* NV3062 destination surface (consumed by NV308A/NV3089) */
    case M3062_DMA_IMAGE_SOURCE: ad->s2d_dma_src = arg; break;
    case M3062_DMA_IMAGE_DESTIN: ad->s2d_dma_dst = arg; break;
    case M3062_COLOR_FORMAT:     ad->s2d_color_format = arg & 0xFFFFu; break;
    case M3062_PITCH:            ad->s2d_pitch = arg; break;
    case M3062_OFFSET_SOURCE:    ad->s2d_offset_src = arg; break;
    case M3062_OFFSET_DESTIN:    ad->s2d_offset_dst = arg; break;

    /* NV308A inline image-from-cpu */
    case M308A_POINT:    ad->inline_point = arg; break;
    case M308A_SIZE_OUT: ad->inline_size_out = arg; break;
    case M308A_SIZE_IN:  ad->inline_size_in = arg; break;

    /* NV3089 scaled image */
    case M3089_DMA_IMAGE:    ad->sif_dma_src = arg; break;
    case M3089_CONTEXT_SURFACE:
        ad->sif_context_surface = arg;
        break;
    case M3089_COLOR_CONVERSION:
        ad->sif_color_conversion = arg;
        break;
    case M3089_COLOR_FORMAT: ad->sif_color_format = arg; break;
    case M3089_OPERATION:    ad->sif_operation = arg; break;
    case M3089_CLIP_POINT:   ad->sif_clip_point = arg; break;
    case M3089_CLIP_SIZE:    ad->sif_clip_size = arg; break;
    case M3089_OUT_POINT:    ad->sif_out_point = arg; break;
    case M3089_OUT_SIZE:     ad->sif_out_size = arg; break;
    case M3089_DS_DX:        ad->sif_ds_dx = arg; break;
    case M3089_DT_DY:        ad->sif_dt_dy = arg; break;
    case M3089_IN_SIZE:      ad->sif_in_size = arg; break;
    case M3089_IN_FORMAT:    ad->sif_in_format = arg; break;
    case M3089_IN_OFFSET:    ad->sif_in_offset = arg; break;
    case M3089_IN_POINT: {
        if (ad->shadow_mode)
            break;
        stage_state(ad);
        rsx_nir_transfer t;
        memset(&t, 0, sizeof(t));
        t.kind         = RSX_NIR_XFER_SCALED;
        t.src_location = dma_location(ad->sif_dma_src);
        t.src_offset   = ad->sif_in_offset;
        t.src_pitch    = ad->sif_in_format & 0xFFFFu;
        t.src_format   = ad->sif_color_format;
        t.dst_location = dma_location(ad->s2d_dma_dst);
        t.dst_offset   = ad->s2d_offset_dst;
        t.dst_pitch    = ad->s2d_pitch & 0xFFFFu;
        t.dst_format   = ad->s2d_color_format;
        t.in_x  = arg & 0xFFFFu;             /* raw 12.4 via IN_POINT   */
        t.in_y  = arg >> 16;
        t.in_w  = ad->sif_in_size & 0xFFFFu;
        t.in_h  = ad->sif_in_size >> 16;
        t.out_x = ad->sif_out_point & 0xFFFFu;
        t.out_y = ad->sif_out_point >> 16;
        t.out_w = ad->sif_out_size & 0xFFFFu;
        t.out_h = ad->sif_out_size >> 16;
        t.clip_x = ad->sif_clip_point & 0xFFFFu;
        t.clip_y = ad->sif_clip_point >> 16;
        t.clip_w = ad->sif_clip_size & 0xFFFFu;
        t.clip_h = ad->sif_clip_size >> 16;
        t.ds_dx = ad->sif_ds_dx;
        t.dt_dy = ad->sif_dt_dy;
        t.origin       = (ad->sif_in_format >> 16) & 0xFFu;
        t.interpolator = (ad->sif_in_format >> 24) & 0xFFu;
        rsx_nir_em_transfer(&ad->em, &t, NULL);
        ad->actions_seen++;
        break;
    }

    default:
        if (method >= M308A_COLOR_FIRST && method <= M308A_COLOR_LAST)
            add_inline_word(ad, (method - M308A_COLOR_FIRST) >> 2, arg);
        break;
    }
}

int rsx_nir_adapter_shadow_action(rsx_nir_adapter* ad, u32 method, u32 arg)
{
    if (!ad || !ad->shadow_mode)
        return 0;
    const u32 before = ad->actions_seen;
    ad->shadow_mode = 0;
    rsx_nir_adapter_method(ad, method, arg);
    ad->shadow_mode = 1;
    return ad->actions_seen != before;
}

void rsx_nir_adapter_finish(rsx_nir_adapter* ad)
{
    flush_inline(ad);
}

u32 rsx_nir_adapter_fifo(rsx_nir_adapter* ad, const u32* words, u32 count,
                         u32* stop_word)
{
    u32 i = 0;
    while (i < count) {
        u32 w = words[i];
        if (w == 0) {           /* FIFO NOP */
            i++;
            continue;
        }
        u32 incr;
        if ((w & 0xE0030003u) == 0)
            incr = 1;
        else if ((w & 0xE0030003u) == 0x40000000u)
            incr = 0;
        else {
            /* JUMP/CALL/RET or malformed: the linear parser stops here */
            if (stop_word)
                *stop_word = w;
            return i;
        }
        u32 method = w & 0x1FFCu;          /* engine-relative address      */
        u32 subch  = (w >> 13) & 7;
        u32 n      = (w >> 18) & 0x7FFu;
        if (i + 1 + n > count) {
            if (stop_word)
                *stop_word = w;
            return i;                       /* truncated packet             */
        }
        for (u32 k = 0; k < n; k++) {
            u32 arg = words[i + 1 + k];
            u32 m = method + (incr ? k * 4 : 0);
            if (m < 0x80) {
                /* NV406E FIFO-engine methods are valid on any subchannel */
                if (ad->inline_count)
                    flush_inline(ad);
                fifo_engine_method(ad, m, arg);
            } else {
                /* flatten subchannel into the register-file address space,
                 * matching how the .rxs exporter stores method addresses */
                rsx_nir_adapter_method(ad, (subch << 13) | m, arg);
            }
        }
        i += 1 + n;
    }
    if (stop_word)
        *stop_word = 0;
    return i;
}
