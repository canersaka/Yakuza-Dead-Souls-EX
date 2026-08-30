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
 *   0x1804 ZCULL_STATS_ENABLE
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
 *   0x6304 PITCH (pitch<<16|alignment), 0x6308/0x630C OFFSET_SOURCE/DESTIN
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
#define M_ZCULL_STATS_ENABLE    0x1804
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

/* Per-group decode from the register file. These are the single source of
 * the register->typed-group mapping: stage_state stages through them, and
 * the island compiler derives the exact same values for its templates. */
static void derive_surface(const rsx_nir_adapter* ad, rsx_nir_surface* ns)
{
    const rsx_dispatch* rsx = &ad->rsx;
    rsx_dsp_surface s;
    rsx_dsp_get_surface(rsx, &s);
    memset(ns, 0, sizeof(*ns));
    ns->color_format = s.color_format;
    ns->depth_format = s.depth_format;
    ns->raster_type  = s.raster_type;
    ns->clip_x = s.clip_x; ns->clip_y = s.clip_y;
    ns->clip_w = s.clip_w; ns->clip_h = s.clip_h;
    for (u32 i = 0; i < RSX_NIR_NUM_RENDER_TARGETS; i++) {
        ns->color_offset[i]   = s.color_offset[i];
        ns->color_pitch[i]    = s.color_pitch[i];
        ns->color_location[i] = s.color_location[i];
    }
    ns->color_target  = s.color_target;
    ns->zeta_offset   = s.zeta_offset;
    ns->zeta_pitch    = s.zeta_pitch;
    ns->zeta_location = s.zeta_location;
}

static void derive_viewport(const rsx_nir_adapter* ad, rsx_nir_viewport* nv)
{
    const rsx_dispatch* rsx = &ad->rsx;
    rsx_dsp_viewport v;
    rsx_dsp_get_viewport(rsx, &v);
    memset(nv, 0, sizeof(*nv));
    nv->x = v.x; nv->y = v.y; nv->w = v.w; nv->h = v.h;
    memcpy(nv->scale, v.scale, sizeof(nv->scale));
    memcpy(nv->translate, v.translate, sizeof(nv->translate));
    u32 cmin = rsx_dsp_reg(rsx, M_CLIP_MIN);
    u32 cmax = rsx_dsp_reg(rsx, M_CLIP_MAX);
    memcpy(&nv->clip_min, &cmin, 4);
    memcpy(&nv->clip_max, &cmax, 4);
}

static void derive_scissor(const rsx_nir_adapter* ad, rsx_nir_scissor* sc)
{
    const rsx_dispatch* rsx = &ad->rsx;
    memset(sc, 0, sizeof(*sc));
    u32 sh = rsx_dsp_reg(rsx, M_SCISSOR_HORIZ);
    u32 sv = rsx_dsp_reg(rsx, M_SCISSOR_VERT);
    sc->x = sh & 0xFFFF; sc->w = sh >> 16;
    sc->y = sv & 0xFFFF; sc->h = sv >> 16;
}

static void derive_raster(const rsx_nir_adapter* ad, rsx_nir_raster* ra)
{
    const rsx_dispatch* rsx = &ad->rsx;
    memset(ra, 0, sizeof(*ra));
    ra->cull_face_enable = rsx_dsp_reg(rsx, M_CULL_FACE_ENABLE);
    ra->cull_face        = rsx_dsp_reg(rsx, M_CULL_FACE);
    ra->front_face       = rsx_dsp_reg(rsx, M_FRONT_FACE);
    ra->polygon_offset_point_enable =
        rsx_dsp_reg(rsx, M_POLY_OFFSET_POINT_EN);
    ra->polygon_offset_line_enable =
        rsx_dsp_reg(rsx, M_POLY_OFFSET_LINE_EN);
    ra->polygon_offset_fill_enable =
        rsx_dsp_reg(rsx, M_POLY_OFFSET_FILL_EN);
    ra->polygon_offset_scale = rsx_dsp_reg(rsx, M_POLY_OFFSET_SCALE);
    ra->polygon_offset_bias  = rsx_dsp_reg(rsx, M_POLY_OFFSET_BIAS);
    ra->color_mask       = rsx_dsp_reg(rsx, M_COLOR_MASK);
    ra->mrt_color_mask   = rsx_dsp_reg(rsx, M_MRT_COLOR_MASK);
}

static void derive_depth_stencil(const rsx_nir_adapter* ad,
                                 rsx_nir_depth_stencil* ds)
{
    const rsx_dispatch* rsx = &ad->rsx;
    memset(ds, 0, sizeof(*ds));
    ds->depth_test_enable   = rsx_dsp_reg(rsx, M_DEPTH_TEST_ENABLE);
    ds->depth_func          = rsx_dsp_reg(rsx, M_DEPTH_FUNC);
    ds->depth_write_enable  = rsx_dsp_reg(rsx, M_DEPTH_WRITE_ENABLE);
    ds->depth_bounds_test_enable =
        rsx_dsp_reg(rsx, M_DEPTH_BOUNDS_ENABLE);
    ds->depth_bounds_min = rsx_dsp_reg(rsx, M_DEPTH_BOUNDS_MIN);
    ds->depth_bounds_max = rsx_dsp_reg(rsx, M_DEPTH_BOUNDS_MAX);
    ds->stencil_test_enable = rsx_dsp_reg(rsx, M_STENCIL_TEST_ENABLE);
    ds->stencil_func        = rsx_dsp_reg(rsx, M_STENCIL_FUNC);
    ds->stencil_ref         = rsx_dsp_reg(rsx, M_STENCIL_FUNC_REF);
    ds->stencil_mask        = rsx_dsp_reg(rsx, M_STENCIL_FUNC_MASK);
    ds->stencil_write_mask  = rsx_dsp_reg(rsx, M_STENCIL_WRITE_MASK);
    ds->stencil_op_fail     = rsx_dsp_reg(rsx, M_STENCIL_OP_FAIL);
    ds->stencil_op_zfail    = rsx_dsp_reg(rsx, M_STENCIL_OP_ZFAIL);
    ds->stencil_op_zpass    = rsx_dsp_reg(rsx, M_STENCIL_OP_ZPASS);
    ds->two_sided_stencil_enable =
        rsx_dsp_reg(rsx, M_TWO_SIDED_STENCIL);
    ds->back_stencil_write_mask =
        rsx_dsp_reg(rsx, M_BACK_STENCIL_MASK);
    ds->back_stencil_func = rsx_dsp_reg(rsx, M_BACK_STENCIL_FUNC);
    ds->back_stencil_ref = rsx_dsp_reg(rsx, M_BACK_STENCIL_FUNC_REF);
    ds->back_stencil_mask = rsx_dsp_reg(rsx, M_BACK_STENCIL_FUNC_MASK);
    ds->back_stencil_op_fail = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_FAIL);
    ds->back_stencil_op_zfail = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZFAIL);
    ds->back_stencil_op_zpass = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZPASS);
}

static void derive_blend(const rsx_nir_adapter* ad, rsx_nir_blend* bl)
{
    const rsx_dispatch* rsx = &ad->rsx;
    memset(bl, 0, sizeof(*bl));
    bl->blend_enable      = rsx_dsp_reg(rsx, M_BLEND_ENABLE);
    bl->sfactor           = rsx_dsp_reg(rsx, M_BLEND_SFACTOR);
    bl->dfactor           = rsx_dsp_reg(rsx, M_BLEND_DFACTOR);
    bl->equation          = rsx_dsp_reg(rsx, M_BLEND_EQUATION);
    bl->blend_color       = rsx_dsp_reg(rsx, M_BLEND_COLOR);
    bl->alpha_test_enable = rsx_dsp_reg(rsx, M_ALPHA_TEST_ENABLE);
    bl->alpha_func        = rsx_dsp_reg(rsx, M_ALPHA_FUNC);
    bl->alpha_ref         = rsx_dsp_reg(rsx, M_ALPHA_REF);
}

static void derive_fragment_program(const rsx_nir_adapter* ad,
                                    rsx_nir_fragment_program* fp)
{
    const rsx_dispatch* rsx = &ad->rsx;
    memset(fp, 0, sizeof(*fp));
    fp->offset  = rsx_dsp_fragment_program(rsx, &fp->location);
    fp->control = rsx_dsp_shader_control(rsx);
    fp->shader_window = rsx_dsp_reg(rsx, M_SHADER_WINDOW);
    for (u32 unit = 0; unit < 10u; ++unit)
        fp->texcoord_2d_mask |=
            (rsx_dsp_reg(rsx, M_TEXCOORD_CONTROL + unit * 4u) & 1u)
            << unit;
}

static void derive_vertex_bindings(const rsx_nir_adapter* ad,
                                   rsx_nir_vertex_bindings* vb)
{
    const rsx_dispatch* rsx = &ad->rsx;
    memset(vb, 0, sizeof(*vb));
    for (u32 i = 0; i < RSX_NIR_NUM_VERTEX_ATTR; i++) {
        rsx_dsp_vertex_attr a;
        rsx_dsp_get_vertex_attr(rsx, i, &a);
        vb->attr[i].type      = a.type;
        vb->attr[i].size      = a.size;
        vb->attr[i].stride    = a.stride;
        vb->attr[i].frequency = a.frequency;
        vb->attr[i].offset    = a.offset;
        vb->attr[i].location  = a.location;
        rsx_dsp_vertex_default(rsx, i, vb->attr[i].def);
    }
    vb->base_offset = rsx_dsp_vertex_data_base_offset(rsx);
    vb->base_index  = rsx_dsp_vertex_data_base_index(rsx);
    vb->freq_divider_op = rsx_dsp_reg(rsx, M_FREQUENCY_DIVIDER_OP);
}

static void derive_index_binding(const rsx_nir_adapter* ad,
                                 rsx_nir_index_binding* ib)
{
    const rsx_dispatch* rsx = &ad->rsx;
    rsx_dsp_index_array ia;
    rsx_dsp_get_index_array(rsx, &ia);
    memset(ib, 0, sizeof(*ib));
    ib->offset         = ia.offset;
    ib->location       = ia.location;
    ib->is_u32         = ia.is_u32;
    ib->restart_enable =
        (u32)rsx_dsp_restart_index_enabled(rsx, (int)ia.is_u32);
    ib->restart_index  = rsx_dsp_restart_index(rsx);
}

static void derive_texture(const rsx_nir_adapter* ad, u32 t,
                           rsx_nir_texture* nt)
{
    rsx_dsp_texture tx;
    rsx_dsp_get_texture(&ad->rsx, t, &tx);
    memset(nt, 0, sizeof(*nt));
    nt->enabled = tx.enabled; nt->offset = tx.offset;
    nt->location = tx.location; nt->format = tx.format;
    nt->dimension = tx.dimension; nt->cubemap = tx.cubemap;
    nt->mipmaps = tx.mipmaps; nt->width = tx.width; nt->height = tx.height;
    nt->pitch = tx.pitch; nt->depth = tx.depth; nt->wrap = tx.wrap;
    nt->remap = tx.remap; nt->filter = tx.filter; nt->control0 = tx.control0;
    nt->border_color = tx.border_color;
}

static void derive_vertex_texture(const rsx_nir_adapter* ad, u32 t,
                                  rsx_nir_texture* nt)
{
    rsx_dsp_vertex_texture tx;
    rsx_dsp_get_vertex_texture(&ad->rsx, t, &tx);
    memset(nt, 0, sizeof(*nt));
    nt->enabled = tx.enabled; nt->offset = tx.offset;
    nt->location = tx.location; nt->format = tx.format;
    nt->dimension = tx.dimension; nt->cubemap = tx.cubemap;
    nt->mipmaps = tx.mipmaps; nt->width = tx.width; nt->height = tx.height;
    nt->pitch = tx.pitch; nt->depth = tx.depth; nt->wrap = tx.wrap;
    nt->filter = tx.filter; nt->control0 = tx.control0;
    nt->border_color = tx.border_color;
}

static void stage_state(rsx_nir_adapter* ad)
{
    const rsx_dispatch* rsx = &ad->rsx;
    rsx_nir_emitter* em = &ad->em;

    /* surface */
    rsx_nir_surface ns;
    derive_surface(ad, &ns);
    rsx_nir_em_surface(em, &ns);

    /* viewport + depth range */
    rsx_nir_viewport nv;
    derive_viewport(ad, &nv);
    rsx_nir_em_viewport(em, &nv);

    /* scissor */
    rsx_nir_scissor sc;
    derive_scissor(ad, &sc);
    rsx_nir_em_scissor(em, &sc);

    /* raster */
    rsx_nir_raster ra;
    derive_raster(ad, &ra);
    rsx_nir_em_raster(em, &ra);

    /* Complete front/back stencil state. D3D12 shares read/write masks and
     * the dynamic reference between both faces; draw preflight therefore
     * keeps unequal front/back values on the whole-section legacy path. */
    rsx_nir_depth_stencil ds;
    derive_depth_stencil(ad, &ds);
    rsx_nir_em_depth_stencil(em, &ds);

    /* blend + alpha test */
    rsx_nir_blend bl;
    derive_blend(ad, &bl);
    rsx_nir_em_blend(em, &bl);
    rsx_nir_em_render_condition(em, &ad->render_condition);

    /* fragment program */
    rsx_nir_fragment_program fp;
    derive_fragment_program(ad, &fp);
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
    derive_vertex_bindings(ad, &vb);
    rsx_nir_em_vertex_bindings(em, &vb);

    /* index binding */
    rsx_nir_index_binding ib;
    derive_index_binding(ad, &ib);
    rsx_nir_em_index_binding(em, &ib);

    /* textures */
    for (u32 t = 0; t < RSX_NIR_NUM_TEXTURES; t++) {
        rsx_nir_texture nt;
        derive_texture(ad, t, &nt);
        rsx_nir_em_texture(em, t, &nt);
    }
    for (u32 t = 0; t < RSX_NIR_NUM_VERTEX_TEXTURES; t++) {
        rsx_nir_texture nt;
        derive_vertex_texture(ad, t, &nt);
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
    t.dst_pitch    = ad->s2d_pitch >> 16;
    t.dst_format   = ad->s2d_color_format;
    t.point_x      = (ad->inline_point & 0xFFFFu) + ad->inline_first_index;
    t.point_y      = ad->inline_point >> 16;
    t.size_w       = ad->inline_size_out & 0xFFFFu;
    t.size_h       = ad->inline_size_out >> 16;
    t.word_count   = ad->inline_count;
    rsx_nir_em_transfer(&ad->em, &t, ad->inline_words);
    ad->actions_seen++;
    ad->context_image_open = 0;
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
    if (mask)
        ad->context_image_open = 0;
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
    ad->context_image_open = 0;
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
    ad->context_image_open = 0;
}

/* ---- public ------------------------------------------------------------ */

void rsx_nir_adapter_rebind(rsx_nir_adapter* ad)
{
    if (ad)
        ad->rsx.sink.user = ad;
}

void rsx_nir_adapter_copy_render_state(rsx_nir_adapter* dst,
                                       const rsx_nir_adapter* src)
{
    if (!dst || !src || dst == src)
        return;

    rsx_dispatch_copy_architectural_state(&dst->rsx, &src->rsx);

    /* Keep dst->em.out: it belongs to the destination ring/stream. */
    dst->em.shadow = src->em.shadow;
    dst->em.pending = src->em.pending;
    memcpy(dst->em.const_dirty, src->em.const_dirty,
           sizeof(dst->em.const_dirty));
    memcpy(dst->em.vp_words, src->em.vp_words,
           sizeof(dst->em.vp_words));
    dst->em.primed = src->em.primed;

    memcpy(dst->batches, src->batches, sizeof(dst->batches));
    dst->batch_count = src->batch_count;
    dst->batch_overflow = src->batch_overflow;
    dst->draw_indexed = src->draw_indexed;
    dst->draw_mixed = src->draw_mixed;
    dst->render_condition = src->render_condition;
    dst->context_image_open = src->context_image_open;
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
    ad->s2d_pitch = (64u << 16) | 64u;
    ad->s2d_color_format = 0xB;
    ad->inline_size_out = 0x00010001;
    ad->sif_context_surface = GCM_CONTEXT_SURFACE2D;
    ad->sif_color_conversion = 1u;
    /* NV406E's reset context matches the live FIFO engine.  Leaving this at
     * memset-zero made a section beginning with OFFSET/ACQUIRE unresolvable
     * until the title happened to issue SET_CONTEXT_DMA_SEMAPHORE. */
    ad->fifo_semaphore_dma = 0x66616661u;
    ad->context_image_open = 1;
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

typedef struct rsx_nir_exact_method {
    u32 method;
    u32 argument;
} rsx_nir_exact_method;

/* The title installs one deterministic NV4097 context image before issuing
 * its first rendering action.  These writes are not independent render
 * operations: they populate hardware state which is either folded into a
 * later typed action or overwritten before first use.  Mixed section
 * admission used to reject the stored-only portion of this image even though
 * the adapter retained every word and action preflight validated the state
 * when it became observable.  Validate the encountered context image exactly
 * here.  An altered value remains unsupported and becomes the full-native
 * development failure instead of being silently ignored. */
static int title_context_image_method_supported(u32 method, u32 arg)
{
    static const rsx_nir_exact_method exact[] = {
        {0x00000u, 0x31337000u},
        {0x00180u, 0x66604200u}, {0x00184u, 0xFEED0000u},
        {0x00188u, 0xFEED0001u}, {0x00190u, 0x00000000u},
        {0x0019Cu, 0xFEED0000u}, {0x001A0u, 0xFEED0001u},
        {0x001ACu, 0x00000000u}, {0x001B0u, 0x00000000u},
        {0x00230u, 0x00000000u}, {0x00238u, 0x00000000u},
        {0x00240u, 0x0000FFFFu}, {0x00244u, 0x00000000u},
        {0x00248u, 0x00000000u}, {0x0024Cu, 0x00000000u},
        {0x002BCu, 0x00000000u}, {0x00300u, 0x00000001u},
        {0x00368u, 0x00001D01u}, {0x00374u, 0x00000000u},
        {0x00378u, 0x00001503u}, {0x0037Cu, 0x00000000u},
        {0x003B8u, 0x00000008u}, {0x003BCu, 0x00000000u},
        {0x008CCu, 0x00000800u}, {0x008D0u, 0x00000000u},
        {0x008D4u, 0x00000000u}, {0x008D8u, 0x00000000u},
        {0x00A0Cu, 0x00000000u},
        {0x01428u, 0x00000001u}, {0x0142Cu, 0x00000000u},
        {0x01450u, 0x00080003u}, {0x01454u, 0x00000000u},
        {0x0145Cu, 0x00000001u}, {0x01478u, 0x00000000u},
        {0x0147Cu, 0x00000000u}, {0x01838u, 0x00000000u},
        {0x01D64u, 0x02000000u}, {0x01D80u, 0x00000003u},
        {0x01D98u, 0x0FFF0000u}, {0x01D9Cu, 0x0FFF0000u},
        {0x01DA4u, 0x00000000u}, {0x01DB4u, 0x00000000u},
        {0x01E94u, 0x00000011u}, {0x01EE0u, 0x3F800000u},
        {0x01FC4u, 0x06144321u}, {0x01FC8u, 0xEDCBA987u},
        {0x01FCCu, 0x0000006Fu}, {0x01FD0u, 0x00171615u},
        {0x01FD4u, 0x001B1A19u}, {0x01FE0u, 0x00000001u},
        {0x01FE8u, 0x00000000u}, {0x01FECu, 0x00000000u},
        {0x02000u, 0x31337303u}, {0x02180u, 0x66604200u},
        {0x06000u, 0x313371C3u}, {0x06180u, 0x66604200u},
        {0x08000u, 0x31337A73u}, {0x08180u, 0x66604200u},
        {0x08184u, 0xFEED0000u}, {0x0A000u, 0x31337808u},
        {0x0A180u, 0x66604200u}, {0x0A184u, 0x00000000u},
        {0x0A188u, 0x00000000u}, {0x0A18Cu, 0x00000000u},
        {0x0A190u, 0x00000000u}, {0x0A194u, 0x00000000u},
        {0x0A198u, 0x00000000u}, {0x0A19Cu, 0x313371C3u},
        {0x0A2FCu, 0x00000003u}, {0x0A300u, 0x00000004u},
        {0x0C000u, 0x3137AF00u}, {0x0C180u, 0x66604200u},
        {0x0E000u, 0xCAFEBABEu},
    };
    static const u32 texture_control[16] = {
        0x9AABAA98u, 0x66666789u, 0x98766666u, 0x89AABAA9u,
        0x99999999u, 0x88888889u, 0x98888888u, 0x99999999u,
        0x56676654u, 0x33333345u, 0x54333333u, 0x45667665u,
        0xAABBBA99u, 0x66667899u, 0x99876666u, 0x99ABBBAAu,
    };

    if (method >= 0x002C0u && method <= 0x002FCu &&
        !(method & 3u) && arg == 0x0FFF0000u)
        return 1;
    if (method >= 0x003C0u && method <= 0x003FCu &&
        !(method & 3u) && arg == 0x00010101u)
        return 1;
    if (method >= 0x00400u && method <= 0x0043Cu &&
        !(method & 3u) && arg == 0x00007421u)
        return 1;
    if (method >= 0x00440u && method <= 0x0047Cu && !(method & 3u) &&
        arg == texture_control[(method - 0x00440u) >> 2])
        return 1;
    if (method >= 0x00B00u && method <= 0x00B3Cu &&
        !(method & 3u) && arg == 0x00002DC8u)
        return 1;
    for (u32 i = 0; i < sizeof(exact) / sizeof(exact[0]); ++i)
        if (method == exact[i].method && arg == exact[i].argument)
            return 1;
    return 0;
}

static int method_supported_base(
    const rsx_nir_adapter* ad, u32 method, u32 arg);

/* Support classification for table-driven scanners: whether a method's
 * admissibility depends on its argument. Must stay structurally in step
 * with rsx_nir_adapter_method_supported below (same file on purpose); the
 * island compiler's property table asserts agreement at init by probing. */
int rsx_nir_adapter_method_support_class(
    const rsx_nir_adapter* ad, u32 method)
{
    method &= 0xFFFFCu;
    if (method < 0x100u) {
        switch (method) {
        case M406E_SET_REFERENCE:
        case M406E_SET_CTX_DMA_SEM:
        case M406E_SEMAPHORE_OFFSET:
        case M406E_SEMAPHORE_ACQUIRE:
        case M406E_SEMAPHORE_RELEASE:
            return RSX_NIR_SUPPORT_ALWAYS;
        default:
            return RSX_NIR_SUPPORT_NEVER;
        }
    }
    switch (method) {
    case M_SHADER_WINDOW:
    case M_TWO_SIDED_STENCIL:
    case M_CONTROL0:
    case M_DITHER_ENABLE:
    case M_POINT_PARAMS_ENABLE:
    case M_ZCULL_STATS_ENABLE:
    case M_POINT_SPRITE_CONTROL:
    case M_ZPASS_COUNT_ENABLE:
    case M_RENDER_ENABLE:
    case M_ZCULL_CONTROL0:
    case M_ZCULL_CONTROL1:
    case M_SCULL_CONTROL:
    case M_ANTI_ALIAS_CONTROL:
    case M_WINDOW_OFFSET:
    case M_DEPTH_BOUNDS_ENABLE:
    case M_DEPTH_BOUNDS_MIN:
    case M_DEPTH_BOUNDS_MAX:
    case M_POLYGON_MODE_FRONT:
    case M_POLYGON_MODE_BACK:
    case M_POLY_OFFSET_POINT_EN:
    case M_POLY_OFFSET_LINE_EN:
    case M_POLY_OFFSET_FILL_EN:
    case M_POLY_OFFSET_SCALE:
    case M_POLY_OFFSET_BIAS:
    case M3089_CONTEXT_SURFACE:
    case M3089_COLOR_CONVERSION:
    case M3089_OPERATION:
    case M_ZMIN_MAX_CONTROL:
        return RSX_NIR_SUPPORT_ARG_DEPENDENT;
    default:
        break;
    }
    /* Probe the arg-independent remainder outside the context-image
     * window: boot-era exact values must not classify a method as always
     * admissible. */
    return method_supported_base(ad, method, 0u) ||
                   method_supported_base(ad, method, ~0u)
               ? RSX_NIR_SUPPORT_ALWAYS
               : RSX_NIR_SUPPORT_NEVER;
}

int rsx_nir_adapter_method_supported(
    const rsx_nir_adapter* ad, u32 method, u32 arg)
{
    if (!ad)
        return 0;
    method &= 0xFFFFCu;
    if (ad->context_image_open &&
        title_context_image_method_supported(method, arg))
        return 1;
    return method_supported_base(ad, method, arg);
}

static int method_supported_base(
    const rsx_nir_adapter* ad, u32 method, u32 arg)
{
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
    /* The production startup stream explicitly disables hardware ZCULL
     * statistic accumulation. Reports already retain their separate ordered
     * typed semantics; disabling the optional counters has no render or
     * publication side effect. Do not admit the enabled mode until its
     * accumulation/reset behavior is represented. */
    if (method == M_ZCULL_STATS_ENABLE)
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

    if (method < 0x80u) {
        /* NV406E methods still occupy the shared hardware register file.
         * Keep that architectural state identical to rsx_dispatch even when
         * the typed adapter handles their synchronization semantics itself. */
        rsx_dispatch_method(&ad->rsx, method, arg);
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
        ad->context_image_open = 0;
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
        t.dst_pitch    = ad->s2d_pitch >> 16;
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
        ad->context_image_open = 0;
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

/* ---- island-compiler support (docs/HANA_ISLAND_COMPILER.md) ------------ */

/* Derive one complete state-group op from the current register file and
 * append it to `out`. When update_shadow is nonzero the emitter's shadow
 * (what a folding consumer has seen) is updated to the derived value so a
 * later adapter-decoded action cannot wrongly diff-suppress against state
 * the caller already delivered to the backend by other means. Returns 0 on
 * success, -1 on stream refusal or an unknown kind. */
int rsx_nir_adapter_derive_group_op(rsx_nir_adapter* ad, u32 kind, u32 unit,
                                    rsx_nir_stream* out, int update_shadow)
{
    rsx_nir_op op;
    memset(&op, 0, sizeof(op));
    op.kind = kind;
    op.unit = unit;
    switch (kind) {
    case RSX_NIR_OP_SET_SURFACE:
        derive_surface(ad, &op.u.surface);
        if (update_shadow)
            ad->em.shadow.surface = op.u.surface;
        break;
    case RSX_NIR_OP_SET_VIEWPORT:
        derive_viewport(ad, &op.u.viewport);
        if (update_shadow)
            ad->em.shadow.viewport = op.u.viewport;
        break;
    case RSX_NIR_OP_SET_SCISSOR:
        derive_scissor(ad, &op.u.scissor);
        if (update_shadow)
            ad->em.shadow.scissor = op.u.scissor;
        break;
    case RSX_NIR_OP_SET_RASTER:
        derive_raster(ad, &op.u.raster);
        if (update_shadow)
            ad->em.shadow.raster = op.u.raster;
        break;
    case RSX_NIR_OP_SET_DEPTH_STENCIL:
        derive_depth_stencil(ad, &op.u.depth_stencil);
        if (update_shadow)
            ad->em.shadow.depth_stencil = op.u.depth_stencil;
        break;
    case RSX_NIR_OP_SET_BLEND:
        derive_blend(ad, &op.u.blend);
        if (update_shadow)
            ad->em.shadow.blend = op.u.blend;
        break;
    case RSX_NIR_OP_SET_RENDER_CONDITION:
        op.u.render_condition = ad->render_condition;
        if (update_shadow)
            ad->em.shadow.render_condition = op.u.render_condition;
        break;
    case RSX_NIR_OP_SET_FRAGMENT_PROGRAM:
        derive_fragment_program(ad, &op.u.fragment_program);
        if (update_shadow)
            ad->em.shadow.fragment_program = op.u.fragment_program;
        break;
    case RSX_NIR_OP_SET_VERTEX_PROGRAM: {
        const rsx_dispatch* rsx = &ad->rsx;
        const u32 vp_start = rsx_dsp_vp_start(rsx);
        u32 vp_words = vp_extent_words(rsx, vp_start);
        if (vp_words > RSX_NIR_VP_MAX_WORDS)
            vp_words = RSX_NIR_VP_MAX_WORDS;
        rsx_nir_vertex_program* vp = &op.u.vertex_program;
        vp->start_slot = vp_start;
        vp->word_count = vp_words;
        vp->hash = rsx_nir_hash_words(rsx->vp + vp_start * 4, vp_words);
        vp->attrib_input_mask = rsx_dsp_reg(rsx, M_VP_ATTRIB_EN);
        vp->attrib_output_mask = rsx_dsp_reg(rsx, M_VP_RESULT_EN);
        vp->branch_bits = rsx_dsp_reg(rsx, M_TRANSFORM_BRANCH_BITS);
        vp->words_ofs = rsx_nir_side_push(out, rsx->vp + vp_start * 4,
                                          vp_words);
        if (vp_words && vp->words_ofs == ~0u)
            return -1;
        if (update_shadow) {
            ad->em.shadow.vertex_program = *vp;
            /* Keep the staged program content coherent with the identity so
             * a later emitter flush cannot pair the new identity with stale
             * bytes. */
            ad->em.pending.vertex_program = *vp;
            if (vp_words)
                memcpy(ad->em.vp_words, rsx->vp + vp_start * 4,
                       (size_t)vp_words * 4u);
        }
        break;
    }
    case RSX_NIR_OP_SET_VERTEX_BINDINGS:
        derive_vertex_bindings(ad, &op.u.vertex_bindings);
        if (update_shadow)
            ad->em.shadow.vertex_bindings = op.u.vertex_bindings;
        break;
    case RSX_NIR_OP_SET_INDEX_BINDING:
        derive_index_binding(ad, &op.u.index_binding);
        if (update_shadow)
            ad->em.shadow.index_binding = op.u.index_binding;
        break;
    case RSX_NIR_OP_SET_TEXTURE:
        if (unit >= RSX_NIR_NUM_TEXTURES)
            return -1;
        derive_texture(ad, unit, &op.u.texture);
        if (update_shadow)
            ad->em.shadow.textures[unit] = op.u.texture;
        break;
    case RSX_NIR_OP_SET_VERTEX_TEXTURE:
        if (unit >= RSX_NIR_NUM_VERTEX_TEXTURES)
            return -1;
        derive_vertex_texture(ad, unit, &op.u.texture);
        if (update_shadow)
            ad->em.shadow.vertex_textures[unit] = op.u.texture;
        break;
    default:
        return -1;
    }
    return rsx_nir_push(out, &op);
}

/* Derive one SET_CONSTANTS op (one vec4 slot) from the dispatch constant
 * file. Shadow update mirrors what an emitter flush of this slot records. */
int rsx_nir_adapter_derive_constant_op(rsx_nir_adapter* ad, u32 slot,
                                       rsx_nir_stream* out, int update_shadow)
{
    if (slot >= RSX_NIR_NUM_CONSTANTS)
        return -1;
    rsx_nir_op op;
    memset(&op, 0, sizeof(op));
    op.kind = RSX_NIR_OP_SET_CONSTANTS;
    op.u.constants.first_slot = slot;
    op.u.constants.slot_count = 1;
    op.u.constants.words_ofs =
        rsx_nir_side_push(out, &ad->rsx.constants[slot][0], 4);
    if (op.u.constants.words_ofs == ~0u)
        return -1;
    if (update_shadow) {
        memcpy(ad->em.shadow.constants[slot], &ad->rsx.constants[slot][0],
               16);
        ad->em.shadow.constants_written[slot] = 1;
        memcpy(ad->em.pending.constants[slot], &ad->rsx.constants[slot][0],
               16);
        ad->em.pending.constants_written[slot] = 1;
    }
    return rsx_nir_push(out, &op);
}

/* Register-truth resync: apply one method's architectural effect (register
 * file, transform program/constant windows and their load cursors, DMA and
 * transfer staging, render-condition capture) with NO emitter staging, NO
 * diffing, and NO action emission. Between begin() and end() the dispatch
 * execution sink is disconnected so BEGIN/END/batch/flip/clear callbacks
 * cannot fire. This is what keeps a later adapter-decoded island's pulled
 * state truthful when the compiler executes islands without adaptation. */
void rsx_nir_adapter_resync_begin(rsx_nir_adapter* ad)
{
    if (ad->resync_active)
        return;
    ad->resync_saved_sink = ad->rsx.sink;
    memset(&ad->rsx.sink, 0, sizeof(ad->rsx.sink));
    ad->resync_active = 1;
}

void rsx_nir_adapter_resync_end(rsx_nir_adapter* ad)
{
    if (!ad->resync_active)
        return;
    ad->rsx.sink = ad->resync_saved_sink;
    ad->resync_active = 0;
}

void rsx_nir_adapter_resync_method(rsx_nir_adapter* ad, u32 method, u32 arg)
{
    method &= 0xFFFFC;
    if (method >= M_VP_UPLOAD_CONST && method < M_VP_UPLOAD_CONST + 32 * 4)
        note_constant_upload(ad, method);
    rsx_dispatch_method(&ad->rsx, method, arg);
    switch (method) {
    case M406E_SET_CTX_DMA_SEM:  ad->fifo_semaphore_dma = arg; break;
    case M406E_SEMAPHORE_OFFSET: ad->fifo_semaphore_offset = arg; break;
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
    case M0039_DMA_BUFFER_IN:   ad->m2mf_dma_in = arg; break;
    case M0039_DMA_BUFFER_OUT:  ad->m2mf_dma_out = arg; break;
    case M0039_OFFSET_IN:       ad->m2mf_offset_in = arg; break;
    case M0039_OFFSET_OUT:      ad->m2mf_offset_out = arg; break;
    case M0039_PITCH_IN:        ad->m2mf_pitch_in = arg; break;
    case M0039_PITCH_OUT:       ad->m2mf_pitch_out = arg; break;
    case M0039_LINE_LENGTH_IN:  ad->m2mf_line_length = arg; break;
    case M0039_LINE_COUNT:      ad->m2mf_line_count = arg; break;
    case M0039_FORMAT:          ad->m2mf_format = arg; break;
    case M3062_DMA_IMAGE_SOURCE: ad->s2d_dma_src = arg; break;
    case M3062_DMA_IMAGE_DESTIN: ad->s2d_dma_dst = arg; break;
    case M3062_COLOR_FORMAT:     ad->s2d_color_format = arg & 0xFFFFu; break;
    case M3062_PITCH:            ad->s2d_pitch = arg; break;
    case M3062_OFFSET_SOURCE:    ad->s2d_offset_src = arg; break;
    case M3062_OFFSET_DESTIN:    ad->s2d_offset_dst = arg; break;
    case M308A_POINT:    ad->inline_point = arg; break;
    case M308A_SIZE_OUT: ad->inline_size_out = arg; break;
    case M308A_SIZE_IN:  ad->inline_size_in = arg; break;
    case M3089_DMA_IMAGE:        ad->sif_dma_src = arg; break;
    case M3089_CONTEXT_SURFACE:  ad->sif_context_surface = arg; break;
    case M3089_COLOR_CONVERSION: ad->sif_color_conversion = arg; break;
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
    default:
        break;
    }
}

/* Fill a transfer action's staging-derived fields from the current adapter
 * state (the same fields the emission paths read). The caller supplies the
 * trigger-word values (SCALED in_x/in_y) and inline run shape/payload. */
void rsx_nir_adapter_derive_transfer(const rsx_nir_adapter* ad, u32 kind,
                                     rsx_nir_transfer* t)
{
    memset(t, 0, sizeof(*t));
    t->kind = kind;
    switch (kind) {
    case RSX_NIR_XFER_BUFFER:
        t->src_location = dma_location(ad->m2mf_dma_in);
        t->src_offset   = ad->m2mf_offset_in;
        t->src_pitch    = ad->m2mf_pitch_in;
        t->dst_location = dma_location(ad->m2mf_dma_out);
        t->dst_offset   = ad->m2mf_offset_out;
        t->dst_pitch    = ad->m2mf_pitch_out;
        t->src_format   = ad->m2mf_format & 0xFFu;
        t->dst_format   = ad->m2mf_format >> 8;
        t->line_length  = ad->m2mf_line_length;
        t->line_count   = ad->m2mf_line_count;
        break;
    case RSX_NIR_XFER_SCALED:
        t->src_location = dma_location(ad->sif_dma_src);
        t->src_offset   = ad->sif_in_offset;
        t->src_pitch    = ad->sif_in_format & 0xFFFFu;
        t->src_format   = ad->sif_color_format;
        t->dst_location = dma_location(ad->s2d_dma_dst);
        t->dst_offset   = ad->s2d_offset_dst;
        t->dst_pitch    = ad->s2d_pitch >> 16;
        t->dst_format   = ad->s2d_color_format;
        t->in_w  = ad->sif_in_size & 0xFFFFu;
        t->in_h  = ad->sif_in_size >> 16;
        t->out_x = ad->sif_out_point & 0xFFFFu;
        t->out_y = ad->sif_out_point >> 16;
        t->out_w = ad->sif_out_size & 0xFFFFu;
        t->out_h = ad->sif_out_size >> 16;
        t->clip_x = ad->sif_clip_point & 0xFFFFu;
        t->clip_y = ad->sif_clip_point >> 16;
        t->clip_w = ad->sif_clip_size & 0xFFFFu;
        t->clip_h = ad->sif_clip_size >> 16;
        t->ds_dx = ad->sif_ds_dx;
        t->dt_dy = ad->sif_dt_dy;
        t->origin       = (ad->sif_in_format >> 16) & 0xFFu;
        t->interpolator = (ad->sif_in_format >> 24) & 0xFFu;
        break;
    case RSX_NIR_XFER_INLINE:
        t->dst_location = dma_location(ad->s2d_dma_dst);
        t->dst_offset   = ad->s2d_offset_dst;
        t->dst_pitch    = ad->s2d_pitch >> 16;
        t->dst_format   = ad->s2d_color_format;
        t->point_y      = ad->inline_point >> 16;
        t->size_w       = ad->inline_size_out & 0xFFFFu;
        t->size_h       = ad->inline_size_out >> 16;
        /* point_x = (point & 0xFFFF) + first_index and word_count are run
         * shape, applied by the caller from its template. */
        t->point_x      = ad->inline_point & 0xFFFFu;
        break;
    default:
        break;
    }
}
