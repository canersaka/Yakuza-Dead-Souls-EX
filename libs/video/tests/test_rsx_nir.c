/*
 * NIR equivalence tests: the packet path (FIFO words / method stream ->
 * rsx_dispatch -> rsx_nir_adapter) versus the typed native path
 * (rsx_nir_emitter calls) must fold to identical action sequences.
 *
 * Also covers: state persistence across draws, emission dedup, ordering
 * of synchronization actions, divergence detection (negative cases), the
 * FIFO-word front end, and an optional real-capture replay leg.
 *
 * The capture leg feeds an exported .rxs stream (tools/rrc_export.py
 * format, same loader layout as live_replay_main.c) through TWO adapter
 * instances and requires identical folded IR plus basic invariants. It
 * runs only when a capture is supplied (argv[1] or YZ_NIR_RXS) because
 * captures are large, local, untracked oracles — absent capture = SKIP,
 * not FAIL, so CTest stays hermetic.
 *
 * Build (see top-level CMakeLists.txt rsx_nir_tests): compiles
 * rsx_dispatch.c + rsx_nir*.c directly; no runtime lib, no GPU, no game.
 */

#include "../rsx_nir_adapter.h"
#include "../rsx_nr_intercept.h"
#include "../rsx_nr_ring.h"
#include "../rsx_nr_backend.h"
#include "../rsx_commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n");                                           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ---- FIFO word builder ------------------------------------------------- */

typedef struct fifo_buf {
    u32 words[4096];
    u32 n;
} fifo_buf;

static void fput(fifo_buf* f, u32 w)
{
    if (f->n < 4096)
        f->words[f->n++] = w;
}

/* increment-mode method header: count args at method, method+4, ... */
static void fmethod(fifo_buf* f, u32 method, u32 count)
{
    fput(f, (count << 18) | (method & 0x1FFC));
}

static void fm1(fifo_buf* f, u32 method, u32 arg)
{
    fmethod(f, method, 1);
    fput(f, arg);
}

/* non-increment header (all args to the same method) */
static void fmethod_ni(fifo_buf* f, u32 method, u32 count)
{
    fput(f, 0x40000000u | (count << 18) | (method & 0x1FFC));
}

static u32 f32bits(float v)
{
    u32 w;
    memcpy(&w, &v, 4);
    return w;
}

/* ---- shared synthetic scene -------------------------------------------- */
/* Method-level constants (same sources as rsx_dispatch.c). */
#define M_DMA_COLOR0     0x0194
#define M_DMA_ZETA       0x0198
#define M_RT_HORIZ       0x0200
#define M_RT_VERT        0x0204
#define M_RT_FORMAT      0x0208
#define M_COLOR0_PITCH   0x020C
#define M_COLOR0_OFFSET  0x0210
#define M_ZETA_OFFSET    0x0214
#define M_RT_ENABLE      0x0220
#define M_ZETA_PITCH     0x022C
#define M_BLEND_ENABLE   0x0310
#define M_BLEND_SFACTOR  0x0314
#define M_BLEND_DFACTOR  0x0318
#define M_DEPTH_FUNC     0x0A6C
#define M_DEPTH_WRITE    0x0A70
#define M_DEPTH_TEST     0x0A74
#define M_SCISSOR_HORIZ  0x08C0
#define M_SCISSOR_VERT   0x08C4
#define M_VIEWPORT_HORIZ 0x0A00
#define M_VIEWPORT_VERT  0x0A04
#define M_VIEWPORT_TRANSLATE 0x0A20
#define M_VIEWPORT_SCALE 0x0A30
#define M_VP_UPLOAD_INST 0x0B80
#define M_FP_PROGRAM     0x08E4
#define M_FP_CONTROL     0x1D60
#define M_VTXBUF_OFFSET  0x1680
#define M_VTXFMT         0x1740
#define M_BEGIN_END      0x1808
#define M_DRAW_ARRAYS    0x1814
#define M_IDXBUF_OFFSET  0x181C
#define M_IDXBUF_FORMAT  0x1820
#define M_DRAW_INDEX     0x1824
#define M_TEX_OFFSET_U2  (0x1A00 + 2 * 0x20)
#define M_TEX_SIZE1_U2   (0x1840 + 2 * 4)
#define M_CLEAR_DEPTH    0x1D8C
#define M_CLEAR_COLOR    0x1D90
#define M_CLEAR_BUFFERS  0x1D94
#define M_SEM_OFFSET_3D  0x1D6C
#define M_BACKEND_SEM    0x1D70
#define M_VP_UPLOAD_FROM 0x1E9C
#define M_VP_START       0x1EA0
#define M_VP_CONST_ID    0x1EFC
#define M_VP_CONST       0x1F00
#define M_CONTEXT_REPORT 0x01A8
#define M_RENDER_ENABLE  0x1E98
#define M_GCM_FLIP       0xE944

/* A tiny 2-instruction vertex program, END bit on the second instruction */
static const u32 VP_WORDS[8] = {
    0x00000000, 0x0040001D, 0x8106C083, 0x60403F80,
    0x00000000, 0x004C009D, 0x8106C083, 0x60409F81, /* bit0 set = END */
};

/* Typed-path defaults matching the rsx_dispatch reset register file:
 * everything zero except the seeded registers (COLOR_MASK, FRONT_FACE,
 * ZSTENCIL_CLEAR_VALUE) and decode artifacts (index fmt 0 -> u32). */
static void typed_stage_defaults(rsx_nir_emitter* em)
{
    rsx_nir_raster ra;
    memset(&ra, 0, sizeof(ra));
    ra.color_mask = 0x01010101u;
    ra.front_face = 0x0901u;
    rsx_nir_em_raster(em, &ra);

    rsx_nir_depth_stencil ds;
    memset(&ds, 0, sizeof(ds));
    ds.stencil_mask = 0xFFu;
    ds.stencil_write_mask = 0xFFu;
    ds.back_stencil_mask = 0xFFu;
    ds.back_stencil_write_mask = 0xFFu;
    ds.depth_bounds_max = 0x3F800000u;
    rsx_nir_em_depth_stencil(em, &ds);

    rsx_nir_index_binding ib;
    memset(&ib, 0, sizeof(ib));
    ib.is_u32 = 1;               /* IDXBUF_FORMAT 0 decodes as 32-bit */
    rsx_nir_em_index_binding(em, &ib);
}

/* Build the packet-path scene into a FIFO buffer. One frame:
 * surface+viewport+scissor setup, clear, VP+constants upload, texture,
 * vertex attr, depth+blend state, draw (2 batches), semaphore release,
 * state change, indexed draw, flip. */
static void build_scene_fifo(fifo_buf* f)
{
    /* surface */
    fm1(f, M_DMA_COLOR0, 0xFEED0000u);
    fm1(f, M_DMA_ZETA, 0xFEED0000u);
    fm1(f, M_RT_FORMAT, 0x145);                 /* A8R8G8B8, Z24S8, pitch */
    fm1(f, M_RT_HORIZ, 1280u << 16);
    fm1(f, M_RT_VERT, 720u << 16);
    fm1(f, M_COLOR0_OFFSET, 0x00000000u);
    fm1(f, M_COLOR0_PITCH, 5120);
    fm1(f, M_ZETA_OFFSET, 0x00E00000u);
    fm1(f, M_ZETA_PITCH, 5120);
    fm1(f, M_RT_ENABLE, 1);

    /* viewport + scissor */
    fm1(f, M_VIEWPORT_HORIZ, 1280u << 16);
    fm1(f, M_VIEWPORT_VERT, 720u << 16);
    fmethod(f, M_VIEWPORT_SCALE, 4);
    fput(f, f32bits(640.0f)); fput(f, f32bits(-360.0f));
    fput(f, f32bits(0.5f));   fput(f, f32bits(0.0f));
    fmethod(f, M_VIEWPORT_TRANSLATE, 4);
    fput(f, f32bits(640.0f)); fput(f, f32bits(360.0f));
    fput(f, f32bits(0.5f));   fput(f, f32bits(0.0f));
    fm1(f, M_SCISSOR_HORIZ, 1280u << 16);
    fm1(f, M_SCISSOR_VERT, 720u << 16);

    /* clear */
    fm1(f, M_CLEAR_COLOR, 0xFF204060u);
    fm1(f, M_CLEAR_BUFFERS, 0xF3);

    /* vertex program upload + start (increment burst: the 0xB80..0xBFC
     * window advances the load pointer per completed vec4) */
    fm1(f, M_VP_UPLOAD_FROM, 0);
    fmethod(f, M_VP_UPLOAD_INST, 8);
    for (int i = 0; i < 8; i++)
        fput(f, VP_WORDS[i]);
    fm1(f, M_VP_START, 0);

    /* constants: slots 5..6 */
    fm1(f, M_VP_CONST_ID, 5);
    fmethod(f, M_VP_CONST, 8);
    for (int i = 0; i < 8; i++)
        fput(f, f32bits(1.0f + (float)i));

    /* fragment program */
    fm1(f, M_FP_PROGRAM, 0x00124500u | 1);      /* offset | local */
    fm1(f, M_FP_CONTROL, 0x00000400u);

    /* texture unit 2 */
    fm1(f, M_TEX_OFFSET_U2, 0x00100000u);
    fm1(f, M_TEX_OFFSET_U2 + 4, 0x00018521u);   /* local, 2D, A8R8G8B8, 1 mip */
    fm1(f, M_TEX_OFFSET_U2 + 0x0C, 0x80000000u);/* enable */
    fm1(f, M_TEX_OFFSET_U2 + 0x18, (64u << 16) | 32u);
    fm1(f, M_TEX_SIZE1_U2, 256);

    /* vertex attr 0: float3, stride 12, offset 0x2000, local */
    fm1(f, M_VTXFMT, (12u << 8) | (3u << 4) | 2u);
    fm1(f, M_VTXBUF_OFFSET, 0x00002000u);

    /* depth + blend */
    fm1(f, M_DEPTH_TEST, 1);
    fm1(f, M_DEPTH_FUNC, 0x0203);
    fm1(f, M_DEPTH_WRITE, 1);
    fm1(f, M_BLEND_ENABLE, 0);

    /* draw 1: two array batches */
    fm1(f, M_BEGIN_END, 5);                     /* TRIANGLES */
    fmethod_ni(f, M_DRAW_ARRAYS, 2);
    fput(f, (0u) | ((64u - 1u) << 24));
    fput(f, (64u) | ((32u - 1u) << 24));
    fm1(f, M_BEGIN_END, 0);

    /* ordered semaphore release between draws */
    fm1(f, M_SEM_OFFSET_3D, 0x40);
    fm1(f, M_BACKEND_SEM, 0x11223344u);

    /* state change: enable blend */
    fm1(f, M_BLEND_ENABLE, 1);
    fm1(f, M_BLEND_SFACTOR, 0x03020302u);
    fm1(f, M_BLEND_DFACTOR, 0x03030303u);

    /* indexed draw: u16 indices at 0x4000 */
    fm1(f, M_IDXBUF_OFFSET, 0x00004000u);
    fm1(f, M_IDXBUF_FORMAT, 0x10);              /* type 1 = u16, location 0 */
    fm1(f, M_BEGIN_END, 6);                     /* TRIANGLE_STRIP */
    fm1(f, M_DRAW_INDEX, (0u) | ((96u - 1u) << 24));
    fm1(f, M_BEGIN_END, 0);

    /* flip: the driver-queue methods live at flat address 0xE944, i.e.
     * FIFO subchannel 7, engine-relative 0x944 */
    fput(f, (1u << 18) | (7u << 13) | (M_GCM_FLIP & 0x1FFCu));
    fput(f, 1);
}

/* Build the same scene through the typed native path: what an intercepted
 * producer would emit directly, with no packet encode/decode. */
static void build_scene_typed(rsx_nir_emitter* em)
{
    typed_stage_defaults(em);

    rsx_nir_surface s;
    memset(&s, 0, sizeof(s));
    s.color_format = 5;                          /* 0x145 decoded            */
    s.depth_format = 2;
    s.raster_type = 1;
    s.clip_w = 1280; s.clip_h = 720;
    s.color_offset[0] = 0;
    s.color_pitch[0] = 5120;
    s.color_location[0] = RSX_NIR_LOCATION_LOCAL;
    s.color_target = 1;
    s.zeta_offset = 0x00E00000u;
    s.zeta_pitch = 5120;
    s.zeta_location = RSX_NIR_LOCATION_LOCAL;
    rsx_nir_em_surface(em, &s);

    rsx_nir_viewport v;
    memset(&v, 0, sizeof(v));
    v.w = 1280; v.h = 720;
    v.scale[0] = 640.0f; v.scale[1] = -360.0f; v.scale[2] = 0.5f;
    v.translate[0] = 640.0f; v.translate[1] = 360.0f; v.translate[2] = 0.5f;
    rsx_nir_em_viewport(em, &v);

    rsx_nir_scissor sc;
    memset(&sc, 0, sizeof(sc));
    sc.w = 1280; sc.h = 720;
    rsx_nir_em_scissor(em, &sc);

    /* clear (depth value = seeded ZSTENCIL default 0xFFFFFF00 >> 8) */
    rsx_nir_em_clear(em, 0xF3, 0xFF204060u, 0xFFFFFF, 0x00);

    rsx_nir_em_vertex_program(em, 0, VP_WORDS, 8, 0, 0, 0);

    u32 cw[8];
    for (int i = 0; i < 8; i++)
        cw[i] = f32bits(1.0f + (float)i);
    rsx_nir_em_constants(em, 5, 2, cw);

    rsx_nir_fragment_program fp;
    memset(&fp, 0, sizeof(fp));
    fp.offset = 0x00124500u;                     /* location bits stripped   */
    fp.location = RSX_NIR_LOCATION_LOCAL;
    fp.control = 0x00000400u;
    fp.shader_window = 0x1000u;
    rsx_nir_em_fragment_program(em, &fp);

    rsx_nir_texture t;
    memset(&t, 0, sizeof(t));
    t.enabled = 1;
    t.offset = 0x00100000u;
    t.location = RSX_NIR_LOCATION_LOCAL;
    t.format = 0x85; t.dimension = 2; t.mipmaps = 1;
    t.width = 64; t.height = 32; t.pitch = 256;
    t.control0 = 0x80000000u;
    rsx_nir_em_texture(em, 2, &t);

    rsx_nir_vertex_bindings vb;
    memset(&vb, 0, sizeof(vb));
    for (u32 i = 0; i < RSX_NIR_NUM_VERTEX_ATTR; i++)
        vb.attr[i].def[3] = 1.0f;
    vb.attr[0].type = RSX_VTX_TYPE_FLOAT;
    vb.attr[0].size = 3;
    vb.attr[0].stride = 12;
    vb.attr[0].offset = 0x00002000u;
    vb.attr[0].location = RSX_NIR_LOCATION_LOCAL;
    rsx_nir_em_vertex_bindings(em, &vb);

    rsx_nir_depth_stencil ds;
    memset(&ds, 0, sizeof(ds));
    ds.depth_test_enable = 1;
    ds.depth_func = 0x0203;
    ds.depth_write_enable = 1;
    ds.stencil_mask = 0xFFu;
    ds.stencil_write_mask = 0xFFu;
    ds.back_stencil_mask = 0xFFu;
    ds.back_stencil_write_mask = 0xFFu;
    ds.depth_bounds_max = 0x3F800000u;
    rsx_nir_em_depth_stencil(em, &ds);

    /* draw 1 */
    u32 batches1[4] = { 0, 64, 64, 32 };
    rsx_nir_em_draw(em, 5, 0, batches1, 2);

    rsx_nir_em_semaphore_release(em, 0, 0x40, 0x11223344u, 0);

    rsx_nir_blend bl;
    memset(&bl, 0, sizeof(bl));
    bl.blend_enable = 1;
    bl.sfactor = 0x03020302u;
    bl.dfactor = 0x03030303u;
    rsx_nir_em_blend(em, &bl);

    rsx_nir_index_binding ib;
    memset(&ib, 0, sizeof(ib));
    ib.offset = 0x00004000u;
    ib.is_u32 = 0;                               /* type 1 = u16             */
    rsx_nir_em_index_binding(em, &ib);

    u32 batches2[2] = { 0, 96 };
    rsx_nir_em_draw(em, 6, 1, batches2, 1);

    rsx_nir_em_present(em, 1);
}

/* ---- tests ------------------------------------------------------------- */

static void test_fifo_vs_typed(void)
{
    fifo_buf f = {0};
    build_scene_fifo(&f);

    rsx_nir_stream sa, sb;
    rsx_nir_stream_init(&sa);
    rsx_nir_stream_init(&sb);

    rsx_nir_adapter ad;
    rsx_nir_adapter_init(&ad, &sa);
    u32 stop = 0;
    u32 used = rsx_nir_adapter_fifo(&ad, f.words, f.n, &stop);
    CHECK(used == f.n, "fifo parse consumed %u of %u (stop %08X)",
          used, f.n, stop);
    CHECK(ad.batch_overflow == 0, "batch overflow %u", ad.batch_overflow);

    rsx_nir_emitter em;
    rsx_nir_emitter_init_stream(&em, &sb);
    build_scene_typed(&em);

    char err[256];
    int rc = rsx_nir_compare(&sa, &sb, err, sizeof(err));
    CHECK(rc == 0, "packet vs typed: %s", rc ? err : "");

    /* the scene has 5 actions: clear, draw, sem-release, draw, present */
    rsx_nir_action* acts = NULL;
    int n = rsx_nir_fold(&sa, &acts);
    CHECK(n == 5, "action count %d", n);
    if (n == 5) {
        CHECK(acts[0].kind == RSX_NIR_OP_CLEAR, "a0 kind %u", acts[0].kind);
        CHECK(acts[1].kind == RSX_NIR_OP_DRAW, "a1 kind %u", acts[1].kind);
        CHECK(acts[2].kind == RSX_NIR_OP_SEMAPHORE_RELEASE, "a2 kind %u",
              acts[2].kind);
        CHECK(acts[3].kind == RSX_NIR_OP_DRAW, "a3 kind %u", acts[3].kind);
        CHECK(acts[4].kind == RSX_NIR_OP_PRESENT, "a4 kind %u", acts[4].kind);
    }
    free(acts);
    rsx_nir_stream_free(&sa);
    rsx_nir_stream_free(&sb);
}

static void test_state_persistence_and_dedup(void)
{
    fifo_buf f = {0};
    build_scene_fifo(&f);

    rsx_nir_stream s;
    rsx_nir_stream_init(&s);
    rsx_nir_adapter ad;
    rsx_nir_adapter_init(&ad, &s);
    rsx_nir_adapter_fifo(&ad, f.words, f.n, NULL);

    rsx_nir_action* acts = NULL;
    int n = rsx_nir_fold(&s, &acts);
    CHECK(n == 5, "action count %d", n);
    if (n == 5) {
        /* draw 1 has blend off, draw 2 has blend on; viewport persists */
        CHECK(acts[1].state.blend.blend_enable == 0, "draw1 blend on");
        CHECK(acts[3].state.blend.blend_enable == 1, "draw2 blend off");
        CHECK(acts[3].state.viewport.w == 1280 &&
              acts[3].state.viewport.h == 720, "draw2 viewport lost");
        CHECK(acts[3].state.depth_stencil.depth_func == 0x0203,
              "draw2 depth func lost");
        /* texture persisted across the state change */
        CHECK(acts[3].state.textures[2].enabled == 1 &&
              acts[3].state.textures[2].width == 64, "draw2 texture lost");
        /* constants folded */
        CHECK(acts[1].state.constants_written[5] == 1 &&
              acts[1].state.constants_written[6] == 1 &&
              acts[1].state.constants_written[7] == 0, "constants written set");
        float c50;
        memcpy(&c50, &acts[1].state.constants[5][0], 4);
        CHECK(c50 == 1.0f, "constant 5.x %f", c50);
        /* vertex program identity */
        CHECK(acts[1].state.vertex_program.word_count == 8, "vp words %u",
              acts[1].state.vertex_program.word_count);
        CHECK(acts[1].state.vertex_program.hash ==
              rsx_nir_hash_words(VP_WORDS, 8), "vp hash");
    }

    /* dedup: between the semaphore release (after draw 1) and draw 2 only
     * blend + index binding changed, so exactly those two SET ops appear */
    if (n == 5) {
        u32 between = acts[3].op_index - acts[2].op_index - 1;
        CHECK(between == 2, "expected 2 SET ops between sem and draw2, got %u",
              between);
    }
    free(acts);
    rsx_nir_stream_free(&s);
}

static void test_divergence_detected(void)
{
    fifo_buf f = {0};
    build_scene_fifo(&f);

    rsx_nir_stream sa, sb;
    rsx_nir_stream_init(&sa);
    rsx_nir_stream_init(&sb);
    rsx_nir_adapter ad;
    rsx_nir_adapter_init(&ad, &sa);
    rsx_nir_adapter_fifo(&ad, f.words, f.n, NULL);

    rsx_nir_emitter em;
    rsx_nir_emitter_init_stream(&em, &sb);
    build_scene_typed(&em);

    /* perturb: an extra draw-time depth-func change never in the packets */
    rsx_nir_depth_stencil ds;
    memset(&ds, 0, sizeof(ds));
    ds.depth_test_enable = 1;
    ds.depth_func = 0x0207;                      /* ALWAYS instead of LEQUAL */
    ds.depth_write_enable = 1;
    rsx_nir_em_depth_stencil(&em, &ds);
    u32 batch[2] = { 0, 3 };
    rsx_nir_em_draw(&em, 5, 0, batch, 1);

    char err[256] = {0};
    int rc = rsx_nir_compare(&sa, &sb, err, sizeof(err));
    CHECK(rc != 0, "divergent streams compared equal");
    CHECK(strstr(err, "action count") != NULL,
          "unexpected divergence report: %s", err);

    /* same action count, different state: two fresh minimal scenes whose
     * only difference is the depth func at the draw */
    rsx_nir_stream_free(&sb);
    rsx_nir_stream_init(&sb);
    rsx_nir_emitter_init_stream(&em, &sb);
    typed_stage_defaults(&em);
    rsx_nir_em_clear(&em, 0xF3, 0xFF204060u, 0xFFFFFF, 0x00);
    u32 b1[2] = { 0, 4 };
    rsx_nir_em_draw(&em, 5, 0, b1, 1);

    rsx_nir_stream sc;
    rsx_nir_stream_init(&sc);
    rsx_nir_emitter em2;
    rsx_nir_emitter_init_stream(&em2, &sc);
    typed_stage_defaults(&em2);
    rsx_nir_em_clear(&em2, 0xF3, 0xFF204060u, 0xFFFFFF, 0x00);
    rsx_nir_depth_stencil ds2;
    memset(&ds2, 0, sizeof(ds2));
    ds2.depth_func = 0x0207;
    rsx_nir_em_depth_stencil(&em2, &ds2);
    rsx_nir_em_draw(&em2, 5, 0, b1, 1);

    err[0] = 0;
    rc = rsx_nir_compare(&sb, &sc, err, sizeof(err));
    CHECK(rc != 0, "state-divergent streams compared equal");
    CHECK(strstr(err, "depth_stencil") != NULL,
          "divergence did not name depth_stencil: %s", err);

    rsx_nir_stream_free(&sa);
    rsx_nir_stream_free(&sb);
    rsx_nir_stream_free(&sc);
}

static void test_ordering(void)
{
    /* A: draw, semaphore, draw   B: draw, draw, semaphore  -> kind diff */
    rsx_nir_stream sa, sb;
    rsx_nir_stream_init(&sa);
    rsx_nir_stream_init(&sb);
    rsx_nir_emitter ea, eb;
    rsx_nir_emitter_init_stream(&ea, &sa);
    rsx_nir_emitter_init_stream(&eb, &sb);
    u32 b[2] = { 0, 3 };

    typed_stage_defaults(&ea);
    rsx_nir_em_draw(&ea, 5, 0, b, 1);
    rsx_nir_em_semaphore_release(&ea, 0, 0x40, 7, 0);
    rsx_nir_em_draw(&ea, 5, 0, b, 1);

    typed_stage_defaults(&eb);
    rsx_nir_em_draw(&eb, 5, 0, b, 1);
    rsx_nir_em_draw(&eb, 5, 0, b, 1);
    rsx_nir_em_semaphore_release(&eb, 0, 0x40, 7, 0);

    char err[256] = {0};
    int rc = rsx_nir_compare(&sa, &sb, err, sizeof(err));
    CHECK(rc != 0, "reordered sync compared equal");
    CHECK(strstr(err, "kind") != NULL, "report lacks kind diff: %s", err);

    rsx_nir_stream_free(&sa);
    rsx_nir_stream_free(&sb);
}

static void test_fifo_front_end(void)
{
    /* same methods, once through the FIFO parser, once via method calls */
    fifo_buf f = {0};
    fm1(&f, M_CLEAR_COLOR, 0xAABBCCDDu);
    fm1(&f, M_CLEAR_BUFFERS, 0xF3);
    fput(&f, 0);                                 /* FIFO NOP word            */
    /* NV406E semaphore on subchannel 3: engine methods work anywhere */
    fput(&f, (1u << 18) | (3u << 13) | 0x0064);
    fput(&f, 0x30);
    fput(&f, (1u << 18) | (3u << 13) | 0x006C);
    fput(&f, 99);
    fput(&f, (1u << 18) | (7u << 13) | (M_GCM_FLIP & 0x1FFCu));
    fput(&f, 0);

    rsx_nir_stream sa, sb;
    rsx_nir_stream_init(&sa);
    rsx_nir_stream_init(&sb);
    rsx_nir_adapter aa, ab;
    rsx_nir_adapter_init(&aa, &sa);
    rsx_nir_adapter_init(&ab, &sb);

    u32 used = rsx_nir_adapter_fifo(&aa, f.words, f.n, NULL);
    CHECK(used == f.n, "fifo consumed %u of %u", used, f.n);

    /* B: same methods via the flat entry; the NV406E release (a FIFO-word
     * construct with no flat-method form) maps to a typed release carrying
     * the FIFO-context offset. The direct emitter call rides on the state
     * the adapter already staged at the CLEAR action. */
    rsx_nir_adapter_method(&ab, M_CLEAR_COLOR, 0xAABBCCDDu);
    rsx_nir_adapter_method(&ab, M_CLEAR_BUFFERS, 0xF3);
    rsx_nir_em_semaphore_release(&ab.em, 0x66616661u, 0x30, 99, 2);
    rsx_nir_adapter_method(&ab, M_GCM_FLIP, 0);

    char err[256] = {0};
    int rc = rsx_nir_compare(&sa, &sb, err, sizeof(err));
    CHECK(rc == 0, "fifo vs method: %s", err);

    /* jump word stops the linear parser */
    u32 jump[2] = { 0x20000000u | 0x1000u, 0 };
    u32 stop = 0;
    used = rsx_nir_adapter_fifo(&aa, jump, 2, &stop);
    CHECK(used == 0 && stop == jump[0], "jump not reported (used %u)", used);

    rsx_nir_stream_free(&sa);
    rsx_nir_stream_free(&sb);
}

/* defined with the ring tests below */
static void copy_op_to_stream(const rsx_nr_ring* r, const rsx_nir_op* op,
                              rsx_nir_stream* out);
static void drain_ring_to_stream(rsx_nr_ring* r, rsx_nir_stream* out);

static void test_flip_intercept(void)
{
    /* family parsing */
    CHECK(rsx_nr_parse_families(NULL) == 0, "NULL spec not disabled");
    CHECK(rsx_nr_parse_families("") == 0, "empty spec not disabled");
    CHECK(rsx_nr_parse_families("0") == 0, "0 spec not disabled");
    CHECK(rsx_nr_parse_families("1") == (1u << RSX_NR_FAM_COUNT) - 1,
          "1 spec not all");
    CHECK(rsx_nr_parse_families("all") == (1u << RSX_NR_FAM_COUNT) - 1,
          "all spec not all");
    CHECK(rsx_nr_parse_families("flip,draw") ==
              ((1u << RSX_NR_FAM_FLIP) | (1u << RSX_NR_FAM_DRAW)),
          "flip,draw parse wrong");
    CHECK(rsx_nr_parse_families("bogus,clear") == (1u << RSX_NR_FAM_CLEAR),
          "unknown family name not ignored");

    /* packet path: the exact committed flip word sequence, decoded */
    u32 pkt[10];
    u32 n = rsx_nr_flip_packet_spec(pkt, 2, 1, 0x40, 0x1234);
    CHECK(n == 10, "flip packet word count %u", n);

    rsx_nir_stream sa, sb;
    rsx_nir_stream_init(&sa);
    rsx_nir_stream_init(&sb);

    rsx_nir_adapter ad;
    rsx_nir_adapter_init(&ad, &sa);
    u32 used = rsx_nir_adapter_fifo(&ad, pkt, n, NULL);
    CHECK(used == n, "flip packet consumed %u of %u", used, n);

    /* native path: intercept layer -> ring -> rebuilt stream */
    rsx_nr_ring ring;
    rsx_nr_tokens tokens;
    rsx_nr_tokens_init(&tokens);
    CHECK(rsx_nr_ring_init(&ring, 1024, 32768) == 0, "ring init");
    rsx_nr_intercept it;
    rsx_nr_intercept_init(&it, &ring, &tokens,
                          1u << RSX_NR_FAM_FLIP, 1);
    typed_stage_defaults(&it.shadow.em);
    int rc = rsx_nr_try_flip(&it, 2, 1, 0x40, 0x1234);
    CHECK(rc == 1, "try_flip refused valid args");
    drain_ring_to_stream(&ring, &sb);

    char err[256] = {0};
    rc = rsx_nir_compare(&sa, &sb, err, sizeof(err));
    CHECK(rc == 0, "flip packet vs native: %s", err);

    /* fold shape: acquire (dma/offset/value) then present(buffer) */
    rsx_nir_action* acts = NULL;
    int na = rsx_nir_fold(&sa, &acts);
    CHECK(na == 2, "flip action count %d", na);
    if (na == 2) {
        CHECK(acts[0].kind == RSX_NIR_OP_SEMAPHORE_ACQUIRE, "a0 kind %u",
              acts[0].kind);
        CHECK(acts[0].u.semaphore.dma_context == RSX_NR_DMA_SEMAPHORE_RW &&
              acts[0].u.semaphore.offset == 0x400 &&
              acts[0].u.semaphore.value == 0x1234,
              "acquire payload %08X/%08X/%08X",
              acts[0].u.semaphore.dma_context, acts[0].u.semaphore.offset,
              acts[0].u.semaphore.value);
        CHECK(acts[1].kind == RSX_NIR_OP_PRESENT &&
              acts[1].u.present.buffer == 2, "present payload");
    }
    free(acts);

    /* invalid buffer id refused: no flip ops, but the refusal correctly
     * opens a fallback episode (the caller will run the FIFO path) */
    rc = rsx_nr_try_flip(&it, 8, 0, 0, 0);
    CHECK(rc == 0, "buffer_id 8 accepted");
    CHECK(rsx_nr_ring_depth(&ring) == 1, "refusal should emit exactly the "
          "fallback ENTER marker (depth %u)", rsx_nr_ring_depth(&ring));
    const rsx_nr_slot* ms = rsx_nr_ring_peek(&ring);
    CHECK(ms && ms->op.kind == RSX_NIR_OP_FALLBACK &&
          ms->op.u.fallback.dir == RSX_NIR_FALLBACK_ENTER &&
          ms->op.u.fallback.family == RSX_NR_FAM_FLIP &&
          ms->op.u.fallback.reason == RSX_NR_FB_UNSUPPORTED,
          "marker is not the flip UNSUPPORTED ENTER");
    rsx_nr_ring_pop(&ring);

    /* disabled default: every try_* refuses with reason DISABLED and the
     * ring receives nothing (fully-off = no ordering markers either) */
    rsx_nr_intercept off;
    rsx_nr_intercept_init(&off, &ring, &tokens, 0, 0);
    CHECK(rsx_nr_try_flip(&off, 1, 0, 0, 0) == 0, "disabled flip accepted");
    CHECK(rsx_nr_try_user_command(&off, 3) == 0, "disabled user accepted");
    CHECK(rsx_nr_ring_depth(&ring) == 0, "disabled layer touched the ring");
    rsx_nr_stats st;
    rsx_nr_intercept_get_stats(&off, &st);
    CHECK(st.fallbacks[RSX_NR_FAM_FLIP][RSX_NR_FB_DISABLED] == 1 &&
          st.fallbacks[RSX_NR_FAM_USER][RSX_NR_FB_DISABLED] == 1,
          "disabled fallbacks not counted");
    CHECK(st.fallback_episodes == 0, "disabled layer opened episodes");

    rsx_nir_stream_free(&sa);
    rsx_nir_stream_free(&sb);
    rsx_nr_ring_destroy(&ring);
}

/* Mixed-mode: some families native, the rest through the FIFO with the
 * decode-side shadow keeping state coherent; ordering markers + the
 * FIFO-drain token gate the native consumer. */
static void test_intercept_mixed_mode(void)
{
    rsx_nr_ring ring;
    rsx_nr_tokens tokens;
    rsx_nr_tokens_init(&tokens);
    CHECK(rsx_nr_ring_init(&ring, 1024, 32768) == 0, "ring init");
    rsx_nr_intercept it;
    rsx_nr_intercept_init(&it, &ring, &tokens,
                          (1u << RSX_NR_FAM_CLEAR) |
                          (1u << RSX_NR_FAM_SEMAPHORE) |
                          (1u << RSX_NR_FAM_FLIP), 1);
    typed_stage_defaults(&it.shadow.em);

    /* state arrives through the shadowed FIFO stream */
    rsx_nr_intercept_shadow_method(&it, 0x0310, 1);        /* BLEND_ENABLE */
    rsx_nr_intercept_shadow_method(&it, 0x1D90, 0x11223344); /* CLEAR_COLOR */

    /* native clear observes the shadowed state */
    CHECK(rsx_nr_try_clear(&it, 0xF3, 0x11223344, 0xFFFFFF, 0) == 1,
          "try_clear refused");

    /* a draw falls back (family off): caller runs the FIFO path, which the
     * shadow mirrors state-only */
    u32 one_batch[2] = { 0, 12 };
    CHECK(rsx_nr_try_draw(&it, 5, 0, one_batch, 1) == 0,
          "draw intercepted though family off");
    rsx_nr_intercept_shadow_method(&it, 0x1808, 5);        /* BEGIN tris   */
    rsx_nr_intercept_shadow_method(&it, 0x1814, (11u << 24) | 0);
    rsx_nr_intercept_shadow_method(&it, 0x1808, 0);        /* END          */

    /* native again: semaphore release must be ordered behind the drain */
    CHECK(rsx_nr_try_semaphore_release(&it, 0x66616661, 0x40, 7, 0) == 1,
          "sem release refused");
    CHECK(rsx_nr_try_flip(&it, 1, 0, 0, 0) == 1, "flip refused");

    /* expected ring action sequence */
    rsx_nir_stream got;
    rsx_nir_stream_init(&got);
    /* consume with the ordering contract: stop at an unsatisfied wait */
    int waited_at_token = 0;
    const rsx_nr_slot* slot;
    while ((slot = rsx_nr_ring_peek(&ring)) != NULL) {
        if (slot->op.kind == RSX_NIR_OP_TOKEN_WAIT &&
            !rsx_nr_tokens_satisfied(&tokens, slot->op.u.token.token,
                                     slot->op.u.token.value)) {
            waited_at_token = 1;
            break;
        }
        copy_op_to_stream(&ring, &slot->op, &got);
        rsx_nr_ring_pop(&ring);
    }
    CHECK(waited_at_token, "consumer never blocked on the drain token");
    /* the FIFO consumer reports the episode drained; consumption resumes */
    rsx_nr_intercept_fifo_drained(&it, 1);
    while ((slot = rsx_nr_ring_peek(&ring)) != NULL) {
        if (slot->op.kind == RSX_NIR_OP_TOKEN_WAIT &&
            !rsx_nr_tokens_satisfied(&tokens, slot->op.u.token.token,
                                     slot->op.u.token.value)) {
            CHECK(0, "drain token still unsatisfied after signal");
            break;
        }
        copy_op_to_stream(&ring, &slot->op, &got);
        rsx_nr_ring_pop(&ring);
    }

    rsx_nir_action* acts = NULL;
    int na = rsx_nir_fold(&got, &acts);
    CHECK(na == 6, "mixed-mode action count %d", na);
    if (na == 6) {
        CHECK(acts[0].kind == RSX_NIR_OP_CLEAR, "a0 %u", acts[0].kind);
        CHECK(acts[0].state.blend.blend_enable == 1,
              "shadowed blend state missing at native clear");
        CHECK(acts[1].kind == RSX_NIR_OP_FALLBACK &&
              acts[1].u.fallback.dir == RSX_NIR_FALLBACK_ENTER &&
              acts[1].u.fallback.family == RSX_NR_FAM_DRAW &&
              acts[1].u.fallback.reason == RSX_NR_FB_DISABLED,
              "a1 not the draw fallback ENTER");
        CHECK(acts[2].kind == RSX_NIR_OP_FALLBACK &&
              acts[2].u.fallback.dir == RSX_NIR_FALLBACK_EXIT,
              "a2 not fallback EXIT");
        CHECK(acts[3].kind == RSX_NIR_OP_TOKEN_WAIT &&
              acts[3].u.token.token == RSX_NR_TOKEN_FIFO_DRAIN &&
              acts[3].u.token.value == 1, "a3 not the drain wait");
        CHECK(acts[4].kind == RSX_NIR_OP_SEMAPHORE_RELEASE, "a4 %u",
              acts[4].kind);
        CHECK(acts[5].kind == RSX_NIR_OP_PRESENT &&
              acts[5].u.present.buffer == 1, "a5 %u", acts[5].kind);
    }
    free(acts);

    /* the filtered compare drops the markers: the render-relevant
     * subsequence equals a pure typed emission of the same commands */
    rsx_nir_stream pure;
    rsx_nir_stream_init(&pure);
    rsx_nir_emitter em;
    rsx_nir_emitter_init_stream(&em, &pure);
    typed_stage_defaults(&em);
    rsx_nir_blend bl;
    memset(&bl, 0, sizeof(bl));
    bl.blend_enable = 1;
    rsx_nir_em_blend(&em, &bl);
    rsx_nir_em_clear(&em, 0xF3, 0x11223344, 0xFFFFFF, 0);
    rsx_nir_em_semaphore_release(&em, 0x66616661, 0x40, 7, 0);
    rsx_nir_em_present(&em, 1);
    char err[256] = {0};
    u32 skip = (1u << RSX_NIR_OP_FALLBACK) | (1u << RSX_NIR_OP_TOKEN_WAIT) |
               (1u << RSX_NIR_OP_TOKEN_SIGNAL);
    int rc = rsx_nir_compare_ex(&got, &pure, skip, err, sizeof(err));
    CHECK(rc == 0, "mixed vs pure (markers skipped): %s", err);

    /* stats + formatter sanity */
    rsx_nr_stats st;
    rsx_nr_intercept_get_stats(&it, &st);
    CHECK(st.native_ops[RSX_NR_FAM_CLEAR] == 1 &&
          st.native_ops[RSX_NR_FAM_SEMAPHORE] == 1 &&
          st.native_ops[RSX_NR_FAM_FLIP] == 1, "native counts wrong");
    CHECK(st.fallbacks[RSX_NR_FAM_DRAW][RSX_NR_FB_DISABLED] == 1,
          "draw fallback not counted");
    CHECK(st.fallback_episodes == 1, "episode count %llu",
          st.fallback_episodes);
    CHECK(st.shadow_methods == 5, "shadow methods %llu", st.shadow_methods);
    char line[256];
    u32 ln = rsx_nr_stats_format(&st, line, sizeof(line));
    CHECK(ln > 0 && strstr(line, "native=3") && strstr(line, "eps=1"),
          "stats line malformed: %s", line);
    ln = rsx_nr_shadow_census_format(&it, line, sizeof(line));
    CHECK(ln > 0 && strstr(line, "methods=5") &&
          strstr(line, "native=3") && strstr(line, "eps=1"),
          "shadow census malformed: %s", line);

    /* capacity refusal falls back cleanly (tiny ring) */
    rsx_nr_ring tiny;
    CHECK(rsx_nr_ring_init(&tiny, 16, 256) == 0, "tiny ring init");
    rsx_nr_intercept it2;
    rsx_nr_intercept_init(&it2, &tiny, &tokens,
                          (1u << RSX_NR_FAM_CLEAR), 1);
    CHECK(rsx_nr_try_clear(&it2, 1, 0, 0, 0) == 0,
          "clear accepted despite capacity");
    rsx_nr_intercept_get_stats(&it2, &st);
    CHECK(st.fallbacks[RSX_NR_FAM_CLEAR][RSX_NR_FB_CAPACITY] == 1,
          "capacity fallback not counted");
    CHECK(!rsx_nr_ring_reject_sticky(&tiny), "pre-check tripped the ring");

    /* Fixed-memory shape used by the first live typed family.  The IR oracle
     * deliberately snapshots folded state on the first ordered action, so
     * size this for the full bounded snapshot and prove exactly one present
     * emerges before the ring drains. */
    rsx_nr_ring flip_ring;
    CHECK(rsx_nr_ring_init(&flip_ring, 128, 16384) == 0, "flip ring init");
    rsx_nr_intercept flip_it;
    rsx_nr_intercept_init(&flip_it, &flip_ring, &tokens,
                          (1u << RSX_NR_FAM_FLIP), 1);
    CHECK(rsx_nr_try_flip(&flip_it, 3, 0, 0, 0) == 1,
          "state-independent flip refused by side capacity");
    unsigned flip_presents = 0;
    const rsx_nr_slot* flip_slot;
    while ((flip_slot = rsx_nr_ring_peek(&flip_ring)) != NULL) {
        if (flip_slot->op.kind == RSX_NIR_OP_PRESENT) {
            flip_presents++;
            CHECK(flip_slot->op.u.present.buffer == 3,
                  "typed flip buffer changed");
        }
        rsx_nr_ring_pop(&flip_ring);
    }
    CHECK(flip_presents == 1, "typed flip present count %u", flip_presents);
    CHECK(rsx_nr_ring_depth(&flip_ring) == 0, "typed flip did not drain");

    rsx_nir_stream_free(&got);
    rsx_nir_stream_free(&pure);
    rsx_nr_ring_destroy(&ring);
    rsx_nr_ring_destroy(&tiny);
    rsx_nr_ring_destroy(&flip_ring);
}

/* ---- data-move equivalence: packet path vs typed ----------------------- */

static void test_transfers(void)
{
    rsx_nir_stream sa, sb;
    rsx_nir_stream_init(&sa);
    rsx_nir_stream_init(&sb);

    /* packet path: NV0039 buffer copy + NV3062/NV308A inline + NV3089 blit */
    rsx_nir_adapter ad;
    rsx_nir_adapter_init(&ad, &sa);
    rsx_nir_adapter_method(&ad, 0x2184, 0xFEED0001);   /* DMA in (main)   */
    rsx_nir_adapter_method(&ad, 0x2188, 0xFEED0000);   /* DMA out (local) */
    rsx_nir_adapter_method(&ad, 0x230C, 0x1000);       /* OFFSET_IN       */
    rsx_nir_adapter_method(&ad, 0x2310, 0x2000);       /* OFFSET_OUT      */
    rsx_nir_adapter_method(&ad, 0x2314, 256);          /* PITCH_IN        */
    rsx_nir_adapter_method(&ad, 0x2318, 256);          /* PITCH_OUT       */
    rsx_nir_adapter_method(&ad, 0x231C, 256);          /* LINE_LENGTH_IN  */
    rsx_nir_adapter_method(&ad, 0x2320, 4);            /* LINE_COUNT      */
    rsx_nir_adapter_method(&ad, 0x2324, 0x0101);       /* FORMAT          */
    rsx_nir_adapter_method(&ad, 0x2328, 0);            /* BUFFER_NOTIFY   */

    rsx_nir_adapter_method(&ad, 0x6188, 0xFEED0001);   /* 3062 dst dma    */
    rsx_nir_adapter_method(&ad, 0x6300, 0xB);          /* color fmt       */
    rsx_nir_adapter_method(&ad, 0x6304, 0x00400040);   /* pitch           */
    rsx_nir_adapter_method(&ad, 0x630C, 0x3000);       /* dst offset      */
    rsx_nir_adapter_method(&ad, 0xA304, 0x00020001);   /* point y=2 x=1   */
    rsx_nir_adapter_method(&ad, 0xA308, 0x00010003);   /* size out 3x1    */
    rsx_nir_adapter_method(&ad, 0xA400, 0x11111111);
    rsx_nir_adapter_method(&ad, 0xA404, 0x22222222);
    rsx_nir_adapter_method(&ad, 0xA408, 0x33333333);
    /* the SDK pads inline runs to an even word count with a zero word
     * (SetInlineTransfer paddedSizeInWords); hardware skips operands at
     * window index >= SIZE_OUT.x, so this word must NOT reach the
     * payload */
    rsx_nir_adapter_method(&ad, 0xA40C, 0x00000000);
    /* run ends at the next non-COLOR method */
    rsx_nir_adapter_method(&ad, 0xC184, 0xFEED0001);   /* 3089 src dma    */
    rsx_nir_adapter_method(&ad, 0xC300, 0xA);          /* src color fmt   */
    rsx_nir_adapter_method(&ad, 0xC308, 0x00000000);   /* clip point      */
    rsx_nir_adapter_method(&ad, 0xC30C, 0x00F00140);   /* clip 320x240    */
    rsx_nir_adapter_method(&ad, 0xC310, 0x00000000);   /* out point       */
    rsx_nir_adapter_method(&ad, 0xC314, 0x00F00140);   /* out size        */
    rsx_nir_adapter_method(&ad, 0xC318, 0x00100000);   /* ds_dx 1.0       */
    rsx_nir_adapter_method(&ad, 0xC31C, 0x00100000);   /* dt_dy 1.0       */
    rsx_nir_adapter_method(&ad, 0xC400, 0x00F00140);   /* in size         */
    rsx_nir_adapter_method(&ad, 0xC404, 0x00010280);   /* pitch|origin    */
    rsx_nir_adapter_method(&ad, 0xC408, 0x4000);       /* in offset       */
    rsx_nir_adapter_method(&ad, 0xC40C, 0x00000000);   /* IN_POINT: fire  */
    rsx_nir_adapter_finish(&ad);

    /* typed path: the same three moves */
    rsx_nir_emitter em;
    rsx_nir_emitter_init_stream(&em, &sb);
    typed_stage_defaults(&em);

    rsx_nir_transfer t;
    memset(&t, 0, sizeof(t));
    t.kind = RSX_NIR_XFER_BUFFER;
    t.src_location = RSX_NIR_LOCATION_MAIN;  t.src_offset = 0x1000; t.src_pitch = 256;
    t.dst_location = RSX_NIR_LOCATION_LOCAL; t.dst_offset = 0x2000; t.dst_pitch = 256;
    t.src_format = 1; t.dst_format = 1;
    t.line_length = 256; t.line_count = 4;
    rsx_nir_em_transfer(&em, &t, NULL);

    memset(&t, 0, sizeof(t));
    t.kind = RSX_NIR_XFER_INLINE;
    t.dst_location = RSX_NIR_LOCATION_MAIN;
    t.dst_offset = 0x3000; t.dst_pitch = 0x40; t.dst_format = 0xB;
    t.point_x = 1; t.point_y = 2; t.size_w = 3; t.size_h = 1;
    t.word_count = 3;
    const u32 inline_words[3] = { 0x11111111, 0x22222222, 0x33333333 };
    rsx_nir_em_transfer(&em, &t, inline_words);

    memset(&t, 0, sizeof(t));
    t.kind = RSX_NIR_XFER_SCALED;
    t.src_location = RSX_NIR_LOCATION_MAIN;
    t.src_offset = 0x4000; t.src_pitch = 0x280; t.src_format = 0xA;
    t.dst_location = RSX_NIR_LOCATION_MAIN;
    t.dst_offset = 0x3000; t.dst_pitch = 0x40; t.dst_format = 0xB;
    t.in_w = 0x140; t.in_h = 0xF0;
    t.out_w = 0x140; t.out_h = 0xF0;
    t.clip_w = 0x140; t.clip_h = 0xF0;
    t.ds_dx = 0x00100000; t.dt_dy = 0x00100000;
    t.origin = 1; t.interpolator = 0;
    rsx_nir_em_transfer(&em, &t, NULL);

    char err[256] = {0};
    int rc = rsx_nir_compare(&sa, &sb, err, sizeof(err));
    CHECK(rc == 0, "transfers packet vs typed: %s", err);

    rsx_nir_action* acts = NULL;
    int na = rsx_nir_fold(&sa, &acts);
    CHECK(na == 3, "transfer action count %d", na);
    if (na == 3) {
        CHECK(acts[0].u.transfer.kind == RSX_NIR_XFER_BUFFER, "a0 kind");
        CHECK(acts[1].u.transfer.kind == RSX_NIR_XFER_INLINE &&
              acts[1].u.transfer.point_x == 1 &&
              acts[1].u.transfer.word_count == 3, "a1 inline shape");
        CHECK(acts[2].u.transfer.kind == RSX_NIR_XFER_SCALED &&
              acts[2].u.transfer.src_offset == 0x4000, "a2 scaled shape");
    }
    free(acts);
    rsx_nir_stream_free(&sa);
    rsx_nir_stream_free(&sb);
}

/* ---- SDK SetInlineTransfer shape (conformance) -------------------------
 * The SDK's SetInlineTransfer emitter handles the 64-byte destination-alignment
 * restriction by programming the ALIGNED base as OFFSET_DESTIN and the
 * residue as POINT.x (pixelShift = (dst & 63) >> 2), format Y32 (0xB)
 * with pitch 0x1000, SIZE_OUT/IN = (n,1), and pads the COLOR run to an
 * even word count with a zero word. The folded transfer must carry the
 * unpadded count at the shifted point. */
static void test_sdk_inline_transfer_shape(void)
{
    rsx_nir_stream sa, sb;
    rsx_nir_stream_init(&sa);
    rsx_nir_stream_init(&sb);
    rsx_nir_adapter ad;
    rsx_nir_adapter_init(&ad, &sa);

    const u32 dst = 0x12344;         /* 4-aligned, not 64-aligned        */
    const u32 aligned = dst & ~63u;  /* 0x12340                          */
    const u32 shift = (dst & 63u) >> 2;   /* 1                           */
    rsx_nir_adapter_method(&ad, 0x6188, 0xFEED0000);   /* dst dma local  */
    rsx_nir_adapter_method(&ad, 0x630C, aligned);      /* OFFSET_DESTIN  */
    rsx_nir_adapter_method(&ad, 0x6300, 0xB);          /* Y32            */
    rsx_nir_adapter_method(&ad, 0x6304, 0x10001000);   /* pitch pair     */
    rsx_nir_adapter_method(&ad, 0xA304, shift);        /* POINT y=0 x=1  */
    rsx_nir_adapter_method(&ad, 0xA308, 0x00010003);   /* SIZE_OUT 3x1   */
    rsx_nir_adapter_method(&ad, 0xA30C, 0x00010003);   /* SIZE_IN 3x1    */
    rsx_nir_adapter_method(&ad, 0xA400, 0xCAFE0001);
    rsx_nir_adapter_method(&ad, 0xA404, 0xCAFE0002);
    rsx_nir_adapter_method(&ad, 0xA408, 0xCAFE0003);
    rsx_nir_adapter_method(&ad, 0xA40C, 0);            /* SDK pad word   */
    rsx_nir_adapter_finish(&ad);

    rsx_nir_emitter em;
    rsx_nir_emitter_init_stream(&em, &sb);
    typed_stage_defaults(&em);
    rsx_nir_transfer t;
    memset(&t, 0, sizeof(t));
    t.kind = RSX_NIR_XFER_INLINE;
    t.dst_location = RSX_NIR_LOCATION_LOCAL;
    t.dst_offset = aligned;
    t.dst_pitch = 0x1000;
    t.dst_format = 0xB;
    t.point_x = shift;
    t.point_y = 0;
    t.size_w = 3;
    t.size_h = 1;
    t.word_count = 3;
    const u32 words[3] = { 0xCAFE0001, 0xCAFE0002, 0xCAFE0003 };
    rsx_nir_em_transfer(&em, &t, words);

    char err[256] = {0};
    CHECK(rsx_nir_compare(&sa, &sb, err, sizeof(err)) == 0,
          "SDK inline shape packet vs typed: %s", err);
    rsx_nir_stream_free(&sa);
    rsx_nir_stream_free(&sb);
}

/* ---- FIFO control-word classification (SDK encodings) ------------------
 * CELL_GCM_METHOD_FLAG_JUMP = 0x20000000, _CALL = 0x00000002, _RETURN =
 * 0x00020000 (SDK gcm control-word encodings). The linear parser must stop
 * at each without consuming it — following them needs the live consumer's
 * address space. */
static void test_fifo_control_words(void)
{
    rsx_nir_stream s;
    rsx_nir_stream_init(&s);
    rsx_nir_adapter ad;
    rsx_nir_adapter_init(&ad, &s);

    const u32 words[][3] = {
        { (1u << 18) | 0x0A6C, 0x0203, 0x1000 | 0x20000000u },  /* JUMP   */
        { (1u << 18) | 0x0A6C, 0x0203, 0x1000 | 0x00000002u },  /* CALL   */
        { (1u << 18) | 0x0A6C, 0x0203, 0x00020000u },           /* RETURN */
    };
    for (int i = 0; i < 3; i++) {
        u32 stop = 0;
        u32 used = rsx_nir_adapter_fifo(&ad, words[i], 3, &stop);
        CHECK(used == 2, "control word %d consumed at %u", i, used);
        CHECK(stop == words[i][2], "control word %d stop %08X", i, stop);
    }
    rsx_nir_stream_free(&s);
}

/* ---- SET_REFERENCE / user command / tokens / fallback markers ---------- */

static void test_reference_user_tokens(void)
{
    rsx_nir_stream sa, sb;
    rsx_nir_stream_init(&sa);
    rsx_nir_stream_init(&sb);

    /* Packet path: the exact words emitted by the first three vertical
     * producer gates.  SET_REFERENCE and WAIT_LABEL arrive through the raw
     * NV406E FIFO front end; user command uses the same flattened method
     * stream as the live consumer. */
    rsx_nir_adapter ad;
    rsx_nir_adapter_init(&ad, &sa);
    fifo_buf f;
    memset(&f, 0, sizeof(f));
    fm1(&f, 0x0050, 0xBEEF);
    fm1(&f, 0x0064, 0x3A0);
    fm1(&f, 0x0068, 0x12345678);
    u32 stop = 0;
    u32 used = rsx_nir_adapter_fifo(&ad, f.words, f.n, &stop);
    CHECK(used == f.n, "reference packet consumed %u of %u", used, f.n);
    rsx_nir_adapter_method(&ad, 0xEB00, 7);

    rsx_nir_emitter em;
    rsx_nir_emitter_init_stream(&em, &sb);
    typed_stage_defaults(&em);
    rsx_nir_em_set_reference(&em, 0xBEEF);
    rsx_nir_em_semaphore_acquire(&em, 0x66616661u, 0x3A0, 0x12345678);
    rsx_nir_em_user_command(&em, 7);

    char err[256] = {0};
    int rc = rsx_nir_compare(&sa, &sb, err, sizeof(err));
    CHECK(rc == 0, "reference/wait-label/user packet vs typed: %s", err);

    /* tokens + fallback markers are native-only ordered actions: equal
     * sequences compare equal; any payload difference is detected */
    rsx_nir_stream ta, tb;
    rsx_nir_stream_init(&ta);
    rsx_nir_stream_init(&tb);
    rsx_nir_emitter ea, eb;
    rsx_nir_emitter_init_stream(&ea, &ta);
    rsx_nir_emitter_init_stream(&eb, &tb);
    rsx_nir_em_token_wait(&ea, 3, 41);
    rsx_nir_em_fallback(&ea, RSX_NIR_FALLBACK_ENTER, 2, 1);
    rsx_nir_em_token_signal(&ea, 3, 42);
    rsx_nir_em_token_wait(&eb, 3, 41);
    rsx_nir_em_fallback(&eb, RSX_NIR_FALLBACK_ENTER, 2, 1);
    rsx_nir_em_token_signal(&eb, 3, 42);
    err[0] = 0;
    CHECK(rsx_nir_compare(&ta, &tb, err, sizeof(err)) == 0,
          "identical token streams differ: %s", err);
    rsx_nir_em_token_signal(&eb, 3, 43);
    CHECK(rsx_nir_compare(&ta, &tb, err, sizeof(err)) != 0,
          "extra token signal not detected");
    rsx_nir_stream_free(&ta);
    rsx_nir_stream_free(&tb);
    rsx_nir_stream_free(&sa);
    rsx_nir_stream_free(&sb);
}

/* ---- fixed-capacity stream: allocation-free + sticky overflow ---------- */

static void test_fixed_capacity(void)
{
    static rsx_nir_op ops[8];
    static u32 side[64];
    rsx_nir_stream s;
    rsx_nir_stream_init_fixed(&s, ops, 8, side, 64);

    rsx_nir_op op;
    memset(&op, 0, sizeof(op));
    op.kind = RSX_NIR_OP_BARRIER;
    for (int i = 0; i < 8; i++)
        CHECK(rsx_nir_push(&s, &op) == 0, "fixed push %d refused", i);
    CHECK(s.overflow == 0, "premature overflow");
    CHECK(rsx_nir_push(&s, &op) == -1, "push past capacity accepted");
    CHECK(s.overflow == 1, "overflow flag not sticky");
    CHECK(rsx_nir_push(&s, &op) == -1, "sticky overflow allowed a push");
    CHECK(s.op_count == 8, "op_count corrupted by refused push");

    /* side capacity refusal is sticky too */
    rsx_nir_stream_reset(&s);
    u32 w[64];
    memset(w, 0xAB, sizeof(w));
    CHECK(rsx_nir_side_push(&s, w, 64) == 0, "side fill refused");
    CHECK(rsx_nir_side_push(&s, w, 1) == ~0u, "side overflow accepted");
    CHECK(s.overflow == 1, "side overflow not sticky");

    rsx_nir_stream_free(&s);   /* must not free caller storage (no crash) */
}

/* ---- submission ring: SPSC order, side wrap, stats, tokens ------------- */

/* side-payload location of an op: fills *ofs/*count, returns 1 when the
 * op carries a payload */
static int op_side(const rsx_nir_op* op, u32* ofs, u32* count)
{
    switch (op->kind) {
    case RSX_NIR_OP_DRAW:
        *ofs = op->u.draw.batches_ofs;
        *count = op->u.draw.batch_count * 2;
        break;
    case RSX_NIR_OP_SET_VERTEX_PROGRAM:
        *ofs = op->u.vertex_program.words_ofs;
        *count = op->u.vertex_program.word_count;
        break;
    case RSX_NIR_OP_SET_CONSTANTS:
        *ofs = op->u.constants.words_ofs;
        *count = op->u.constants.slot_count * 4;
        break;
    case RSX_NIR_OP_TRANSFER:
        *ofs = op->u.transfer.words_ofs;
        *count = op->u.transfer.word_count;
        break;
    default:
        *ofs = 0;
        *count = 0;
        return 0;
    }
    return *count != 0;
}

static void op_set_side_ofs(rsx_nir_op* op, u32 ofs)
{
    switch (op->kind) {
    case RSX_NIR_OP_DRAW:               op->u.draw.batches_ofs = ofs; break;
    case RSX_NIR_OP_SET_VERTEX_PROGRAM: op->u.vertex_program.words_ofs = ofs; break;
    case RSX_NIR_OP_SET_CONSTANTS:      op->u.constants.words_ofs = ofs; break;
    case RSX_NIR_OP_TRANSFER:           op->u.transfer.words_ofs = ofs; break;
    default: break;
    }
}

/* copy a popped op (side payload included) into a plain stream so the
 * ring's output can be refolded/compared like any other producer */
static void copy_op_to_stream(const rsx_nr_ring* r, const rsx_nir_op* op,
                              rsx_nir_stream* out)
{
    rsx_nir_op c = *op;
    u32 ofs, count;
    if (op_side(op, &ofs, &count)) {
        u32 nofs = rsx_nir_side_push(out, rsx_nr_ring_side_ptr(r, ofs), count);
        op_set_side_ofs(&c, nofs);
    }
    rsx_nir_push(out, &c);
}

static void drain_ring_to_stream(rsx_nr_ring* r, rsx_nir_stream* out)
{
    const rsx_nr_slot* slot;
    while ((slot = rsx_nr_ring_peek(r)) != NULL) {
        copy_op_to_stream(r, &slot->op, out);
        rsx_nr_ring_pop(r);
    }
}

static void test_ring(void)
{
    rsx_nr_ring bad;
    CHECK(rsx_nr_ring_init(&bad, 63, 1024) != 0, "non-pow2 op cap accepted");
    CHECK(rsx_nr_ring_init(&bad, 64, 1000) != 0, "non-pow2 side cap accepted");

    rsx_nr_ring ring;
    CHECK(rsx_nr_ring_init(&ring, 64, 1024) == 0, "ring init failed");

    /* reference stream: the synthetic scene emitted typed */
    rsx_nir_stream ref;
    rsx_nir_stream_init(&ref);
    rsx_nir_emitter er;
    rsx_nir_emitter_init_stream(&er, &ref);
    typed_stage_defaults(&er);
    build_scene_typed(&er);

    /* ring path: same scene through the ring sink, popped incrementally
     * into a rebuild stream (interleaved push/pop exercises wrap) */
    rsx_nir_stream rebuilt;
    rsx_nir_stream_init(&rebuilt);
    rsx_nir_sink k = rsx_nr_ring_sink(&ring);
    rsx_nir_emitter em;
    rsx_nir_emitter_init(&em, &k);
    typed_stage_defaults(&em);

    /* drain helper pattern: emit the scene, draining continuously so the
     * fixed ring never fills */
    build_scene_typed(&em);
    const rsx_nr_slot* slot;
    while ((slot = rsx_nr_ring_peek(&ring)) != NULL) {
        copy_op_to_stream(&ring, &slot->op, &rebuilt);
        rsx_nr_ring_pop(&ring);
    }
    CHECK(!rsx_nr_ring_reject_sticky(&ring), "ring rejected during scene");
    CHECK(rsx_nr_ring_depth(&ring) == 0, "ring not drained");

    char err[256] = {0};
    int rc = rsx_nir_compare(&ref, &rebuilt, err, sizeof(err));
    CHECK(rc == 0, "ring round trip diverges: %s", err);

    /* wrap stress: repeat the scene many times through the small ring,
     * draining after each action-sized burst; side allocations must stay
     * contiguous and content-intact across wraps */
    for (int pass = 0; pass < 50 && !g_failures; pass++) {
        rsx_nir_stream ref2, got2;
        rsx_nir_stream_init(&ref2);
        rsx_nir_stream_init(&got2);
        rsx_nir_emitter e2, e3;
        rsx_nir_emitter_init_stream(&e2, &ref2);
        typed_stage_defaults(&e2);
        build_scene_typed(&e2);
        rsx_nir_emitter_init(&e3, &k);
        typed_stage_defaults(&e3);
        build_scene_typed(&e3);
        while ((slot = rsx_nr_ring_peek(&ring)) != NULL) {
            copy_op_to_stream(&ring, &slot->op, &got2);
            rsx_nr_ring_pop(&ring);
        }
        err[0] = 0;
        if (rsx_nir_compare(&ref2, &got2, err, sizeof(err)) != 0) {
            CHECK(0, "ring wrap pass %d diverges: %s", pass, err);
        }
        rsx_nir_stream_free(&ref2);
        rsx_nir_stream_free(&got2);
    }
    CHECK(!rsx_nr_ring_reject_sticky(&ring), "ring rejected during wrap");

    /* capacity refusal: a too-large single command must be refused loudly
     * and leave the ring usable */
    CHECK(!rsx_nr_ring_can_accept(&ring, 1, 2048),
          "can_accept oversize side approved");
    u32* p = NULL;
    CHECK(rsx_nr_ring_side_reserve(&ring, 2048, &p) == ~0u,
          "oversize reserve succeeded");
    CHECK(rsx_nr_ring_reject_sticky(&ring), "oversize reserve not sticky");
    rsx_nr_ring_clear_reject(&ring);
    CHECK(rsx_nr_ring_can_accept(&ring, 1, 16), "ring unusable after reject");

    /* tokens: monotonic signal, wrapping compare */
    rsx_nr_tokens tk;
    rsx_nr_tokens_init(&tk);
    CHECK(rsx_nr_tokens_satisfied(&tk, 5, 0), "token 0 not satisfied");
    CHECK(!rsx_nr_tokens_satisfied(&tk, 5, 1), "unsignaled token satisfied");
    rsx_nr_tokens_signal(&tk, 5, 10);
    CHECK(rsx_nr_tokens_satisfied(&tk, 5, 10), "signaled token unsatisfied");
    CHECK(rsx_nr_tokens_satisfied(&tk, 5, 3), "lower want unsatisfied");
    CHECK(!rsx_nr_tokens_satisfied(&tk, 5, 11), "future want satisfied");
    rsx_nr_tokens_signal(&tk, 5, 4);      /* stale signal must not regress */
    CHECK(rsx_nr_tokens_value(&tk, 5) == 10, "token regressed");

    rsx_nir_stream_free(&ref);
    rsx_nir_stream_free(&rebuilt);
    rsx_nr_ring_destroy(&ring);
}

/* ---- backend core: ordered execution + host sync semantics ------------- */

typedef struct exec_rec {
    char kinds[64];
    u32 n;
    u32 labels[256];         /* dma-agnostic label window by offset>>4     */
    u32 ref;
    u32 last_clear_color;
    u32 last_draw_total;
    u32 draw_const0_ok;
    u32 last_present;
    u32 reports;
    int report_result;
    u32 report_defer_calls;
    int report_defer_result;
    u32 user_cause;
    u32 flushes;
    u32 flush_reasons[16];
    u32 flush_reason_count;
    u32 last_sem_kind;
} exec_rec;

static void rec_add(exec_rec* r, char k)
{
    if (r->n < sizeof(r->kinds) - 1)
        r->kinds[r->n++] = k;
}

static int rec_clear(void* u, const rsx_nir_pipeline* st,
                     const rsx_nir_clear* c)
{
    exec_rec* r = u;
    (void)st;
    rec_add(r, 'C');
    r->last_clear_color = c->color_value;
    return 0;
}

static int rec_draw(void* u, const rsx_nir_pipeline* st, const u32* vp_words,
                    u32 vp_word_count, const rsx_nir_draw* d,
                    const u32* batches)
{
    exec_rec* r = u;
    (void)vp_words;
    rec_add(r, 'D');
    r->last_draw_total = d->total_count;
    if (d->batch_count && !batches)
        return -1;
    /* the draw must observe folded state: constant slot 5 written (the
     * scene uploads slots 5..6) and the VP words present */
    r->draw_const0_ok = st->constants_written[5] &&
                        st->constants[5][0] == f32bits(1.0f) &&
                        vp_word_count == 8;
    return 0;
}

static int rec_transfer(void* u, const rsx_nir_pipeline* st,
                        const rsx_nir_transfer* t, const u32* words)
{
    exec_rec* r = u;
    (void)st; (void)t; (void)words;
    rec_add(r, 'T');
    return 0;
}

static int rec_present(void* u, u32 buffer)
{
    exec_rec* r = u;
    rec_add(r, 'P');
    r->last_present = buffer;
    return 0;
}

static void rec_flush(void* u) { ((exec_rec*)u)->flushes++; }

static void rec_flush_reason(void* u, u32 reason)
{
    exec_rec* r = u;
    r->flushes++;
    if (r->flush_reason_count <
        (u32)(sizeof(r->flush_reasons) / sizeof(r->flush_reasons[0])))
        r->flush_reasons[r->flush_reason_count++] = reason;
}

static void rec_sem_write(void* u, u32 dma, u32 offset, u32 value, u32 tex)
{
    exec_rec* r = u;
    (void)dma;
    r->last_sem_kind = tex;
    rec_add(r, 's');
    if ((offset >> 4) < 256)
        r->labels[offset >> 4] = value;
}

static int rec_sem_read(void* u, u32 dma, u32 offset, u32* value)
{
    exec_rec* r = u;
    (void)dma;
    if ((offset >> 4) >= 256)
        return -1;
    *value = r->labels[offset >> 4];
    return 0;
}

static int rec_report(void* u, u32 kind, u32 arg, u32 dma)
{
    exec_rec* r = u;
    (void)kind; (void)arg; (void)dma;
    rec_add(r, 'r');
    r->reports++;
    return r->report_result;
}

static int rec_report_defer(void* u, u32 kind, u32 arg, u32 dma)
{
    exec_rec* r = u;
    (void)kind; (void)arg; (void)dma;
    r->report_defer_calls++;
    return r->report_defer_result;
}

static void rec_ref(void* u, u32 value) { ((exec_rec*)u)->ref = value; }

static void rec_user(void* u, u32 cause)
{
    exec_rec* r = u;
    rec_add(r, 'u');
    r->user_cause = cause;
}

static void test_backend_core(void)
{
    rsx_nr_ring ring;
    rsx_nr_tokens tokens;
    rsx_nr_tokens_init(&tokens);
    CHECK(rsx_nr_ring_init(&ring, 1024, 32768) == 0, "ring init");

    exec_rec rec;
    memset(&rec, 0, sizeof(rec));
    rsx_nr_exec_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = &rec;
    ops.clear = rec_clear;
    ops.draw = rec_draw;
    ops.transfer = rec_transfer;
    ops.present = rec_present;
    ops.flush = rec_flush;
    ops.flush_reason = rec_flush_reason;
    ops.sem_write = rec_sem_write;
    ops.sem_read = rec_sem_read;
    ops.report = rec_report;
    ops.set_reference = rec_ref;
    ops.user_command = rec_user;

    rsx_nr_backend be;
    rsx_nr_backend_init(&be, &ring, &tokens, &ops);
    CHECK(rsx_nr_backend_step(&be) == RSX_NR_STEP_EMPTY, "empty ring stepped");

    /* emit a full synthetic scene through the ring */
    rsx_nir_sink k = rsx_nr_ring_sink(&ring);
    rsx_nir_emitter em;
    rsx_nir_emitter_init(&em, &k);
    typed_stage_defaults(&em);
    build_scene_typed(&em);
    rsx_nir_em_set_reference(&em, 0xCAFE);
    rsx_nir_em_user_command(&em, 42);

    u32 executed = rsx_nr_backend_run(&be, 0);
    CHECK(executed > 0, "backend executed nothing");
    CHECK(rsx_nr_ring_depth(&ring) == 0, "backend left ops queued");
    CHECK(be.stats.exec_errors == 0, "exec errors %llu",
          be.stats.exec_errors);
    /* scene shape: clear, draw, sem release, draw, present (+ ref, user) */
    CHECK(strcmp(rec.kinds, "CDsDPu") == 0, "exec order '%s'", rec.kinds);
    CHECK(rec.draw_const0_ok, "draw did not observe folded VP/constants");
    CHECK(rec.ref == 0xCAFE, "reference %08X", rec.ref);
    CHECK(rec.user_cause == 42, "user cause %u", rec.user_cause);
    CHECK(rec.flushes >= 2,
          "semaphore release + SET_REFERENCE did not flush (%u)",
          rec.flushes);
    CHECK(rec.flush_reason_count == 2u &&
              rec.flush_reasons[0] == RSX_NR_FLUSH_SEMAPHORE &&
              rec.flush_reasons[1] == RSX_NR_FLUSH_REFERENCE,
          "semantic flush order count=%u reasons=%u,%u",
          rec.flush_reason_count, rec.flush_reasons[0],
          rec.flush_reasons[1]);
    /* SDK-conformance: the scene's release is a BACK-END (0x1D70) write,
     * whose hardware store swizzles bytes 0<->2 — the wire value
     * 0x11223344 must land in memory as 0x11443322, matching both the
     * SDK's SetWriteBackEndLabel pre-swap compensation and the live
     * consumer's store transform. */
    CHECK(rec.labels[4] == 0x11443322, "backend BE semaphore store %08X",
          rec.labels[4]);
    /* texture-pipe (0x1D74) releases store verbatim */
    rsx_nir_em_semaphore_release(&em, 0, 0x50, 0x11223344u, 1);
    rsx_nr_backend_run(&be, 0);
    CHECK(rec.labels[5] == 0x11223344, "backend TEX semaphore store %08X",
          rec.labels[5]);
    /* and a second BE release directly (regression pin for the transform) */
    rsx_nir_em_semaphore_release(&em, 0, 0x60, 0xAABBCCDDu, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(rec.labels[6] == 0xAADDCCBBu, "backend BE swizzle %08X",
          rec.labels[6]);
    /* NV406E is a distinct verbatim release kind.  Device-credit adjustment
     * belongs to the embedder after this core callback. */
    rsx_nir_em_semaphore_release(&em, 0, 0x70, 0x11223344u, 2);
    rsx_nr_backend_run(&be, 0);
    CHECK(rec.labels[7] == 0x11223344u && rec.last_sem_kind == 2u,
          "backend NV406E release value=%08X kind=%u",
          rec.labels[7], rec.last_sem_kind);

    const u32 flushes_before_report = rec.flushes;
    rsx_nir_em_report(&em, 0, 0x01000080u, 0x66626660u);
    rsx_nr_backend_run(&be, 0);
    CHECK(rec.reports == 1 && rec.flushes == flushes_before_report + 1,
          "report did not flush/publish reports=%u flushes=%u/%u",
          rec.reports, flushes_before_report, rec.flushes);
    CHECK(rec.flush_reason_count &&
              rec.flush_reasons[rec.flush_reason_count - 1u] ==
                  RSX_NR_FLUSH_REPORT,
          "report flush reason was not preserved");

    /* A successful deferred report is retained by the embedder without a
     * flush or immediate guest publication. A refusal must preserve the
     * established flush + report callback behavior byte-for-byte. */
    be.ops.report_defer = rec_report_defer;
    const u32 reports_before_defer = rec.reports;
    const u32 flushes_before_defer = rec.flushes;
    rsx_nir_em_report(&em, 0, 0x01000088u, 0x66626660u);
    rsx_nr_backend_run(&be, 0);
    CHECK(rec.report_defer_calls == 1u &&
              rec.reports == reports_before_defer &&
              rec.flushes == flushes_before_defer,
          "deferred report used immediate path defer=%u reports=%u/%u "
          "flushes=%u/%u", rec.report_defer_calls, rec.reports,
          reports_before_defer, rec.flushes, flushes_before_defer);

    rec.report_defer_result = 1;
    rsx_nir_em_report(&em, 0, 0x0100008Cu, 0x66626660u);
    rsx_nr_backend_run(&be, 0);
    CHECK(rec.report_defer_calls == 2u &&
              rec.reports == reports_before_defer + 1u &&
              rec.flushes == flushes_before_defer + 1u,
          "defer fallback did not preserve immediate path");
    be.ops.report_defer = NULL;
    rec.report_defer_result = 0;
    rsx_nir_em_barrier(&em, 0xA5u);
    rsx_nr_backend_run(&be, 0);
    CHECK(rec.flush_reason_count &&
              rec.flush_reasons[rec.flush_reason_count - 1u] ==
                  RSX_NR_FLUSH_BARRIER,
          "barrier flush reason was not preserved");
    const u64 errors_before_report_refusal = be.stats.exec_errors;
    const u32 reports_before_refusal = rec.reports;
    rec.report_result = -1;
    rsx_nir_em_report(&em, 0, 0x01000090u, 0xBAD68000u);
    rsx_nr_backend_run(&be, 0);
    CHECK(rec.reports == reports_before_refusal + 1u &&
          be.stats.exec_errors == errors_before_report_refusal + 1u,
          "report refusal was silently claimed reports=%u errors=%llu/%llu",
          rec.reports, be.stats.exec_errors, errors_before_report_refusal);
    rec.report_result = 0;

    /* blocking: an acquire on an unsatisfied label parks WITHOUT popping */
    rsx_nir_em_semaphore_acquire(&em, 0x66616661, 0x40, 0x77);
    rsx_nir_em_present(&em, 3);
    CHECK(rsx_nr_backend_step(&be) == RSX_NR_STEP_EXECUTED ||
          1, "");   /* state flush ops execute first */
    rsx_nr_backend_run(&be, 0);
    CHECK(rsx_nr_ring_depth(&ring) == 2, "acquire did not park (depth %u)",
          rsx_nr_ring_depth(&ring));
    CHECK(rsx_nr_backend_step(&be) == RSX_NR_STEP_BLOCKED_SEMAPHORE,
          "park result wrong");
    rec.labels[4] = 0x77;               /* host releases the label */
    rsx_nr_backend_run(&be, 0);
    CHECK(rsx_nr_ring_depth(&ring) == 0, "acquire never resumed");
    CHECK(rec.last_present == 3, "post-acquire present lost");

    /* blocking: TOKEN_WAIT parks until the producing engine signals */
    rsx_nir_em_token_wait(&em, 9, 5);
    rsx_nir_em_clear(&em, 1, 0xAA, 0, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(rsx_nr_backend_step(&be) == RSX_NR_STEP_BLOCKED_TOKEN,
          "token wait did not park");
    u32 clears_before = (u32)be.stats.executed[RSX_NIR_OP_CLEAR];
    rsx_nr_tokens_signal(&tokens, 9, 5);
    rsx_nr_backend_run(&be, 0);
    CHECK((u32)be.stats.executed[RSX_NIR_OP_CLEAR] == clears_before + 1,
          "post-token clear lost");
    CHECK(be.stats.blocked_token > 0 && be.stats.blocked_semaphore > 0,
          "block accounting empty");

    rsx_nr_ring_destroy(&ring);
}

/* ---- adversarial: SPU-patched (self-modifying) command groups ---------- */

/* Models gs_task's patch-then-release protocol at the parser level: the
 * consumer parks on a jump-to-self stopper; the SPU patches later command
 * bytes (plain PUTs), then releases the stopper (fenced PUT). The decode
 * of the patched buffer must equal the decode of a buffer that was never
 * patched — i.e. the parser reads CURRENT bytes on resume, never a stale
 * snapshot. */
static void test_spu_patched_commands(void)
{
    u32 buf[64];
    u32 n = 0;
    /* packet 1: depth func */
    buf[n++] = (1u << 18) | 0x0A6C;
    buf[n++] = 0x0203;
    const u32 stopper_at = n;
    buf[n++] = 0x20000000u | (stopper_at * 4);   /* jump-to-self stopper  */
    /* the "unpatched hole": junk the SPU will overwrite */
    buf[n++] = 0xDEADBEEFu;
    buf[n++] = 0xDEADBEEFu;
    buf[n++] = 0xDEADBEEFu;
    buf[n++] = 0xDEADBEEFu;
    const u32 total = n;

    rsx_nir_stream sa, sb;
    rsx_nir_stream_init(&sa);
    rsx_nir_stream_init(&sb);
    rsx_nir_adapter ad;
    rsx_nir_adapter_init(&ad, &sa);

    /* leg 1: parse until the stopper parks us */
    u32 stop = 0;
    u32 used = rsx_nir_adapter_fifo(&ad, buf, total, &stop);
    CHECK(used == stopper_at, "parser did not park at the stopper (%u)",
          used);
    CHECK((stop & 0xE0000003u) == 0x20000000u, "stop word %08X", stop);

    /* the SPU patches the hole with a real packet, then releases the
     * stopper by rewriting it as a jump PAST itself (modeled here by the
     * consumer resuming at the patched words, since the linear parser has
     * no address space to follow jumps through) */
    u32 p = stopper_at + 1;
    buf[p++] = (1u << 18) | 0x1D90;              /* clear color           */
    buf[p++] = 0xFF112233u;
    buf[p++] = (1u << 18) | 0x1D94;              /* CLEAR_BUFFERS         */
    buf[p++] = 0xF3;
    used = rsx_nir_adapter_fifo(&ad, buf + stopper_at + 1,
                                total - stopper_at - 1, &stop);
    CHECK(used == total - stopper_at - 1, "resume consumed %u", used);
    rsx_nir_adapter_finish(&ad);

    /* reference: the same logical stream that was never patched */
    u32 ref[8];
    u32 rn = 0;
    ref[rn++] = (1u << 18) | 0x0A6C;
    ref[rn++] = 0x0203;
    ref[rn++] = (1u << 18) | 0x1D90;
    ref[rn++] = 0xFF112233u;
    ref[rn++] = (1u << 18) | 0x1D94;
    ref[rn++] = 0xF3;
    rsx_nir_adapter ad2;
    rsx_nir_adapter_init(&ad2, &sb);
    used = rsx_nir_adapter_fifo(&ad2, ref, rn, &stop);
    CHECK(used == rn, "reference consumed %u", used);
    rsx_nir_adapter_finish(&ad2);

    char err[256] = {0};
    CHECK(rsx_nir_compare(&sa, &sb, err, sizeof(err)) == 0,
          "patched vs unpatched decode: %s", err);
    rsx_nir_stream_free(&sa);
    rsx_nir_stream_free(&sb);
}

/* ---- adversarial: partial publication ---------------------------------- */

static void test_partial_publication(void)
{
    /* a method packet whose args are not yet published: the parser must
     * refuse the truncated tail and resume exactly there once the
     * producer publishes the rest */
    u32 buf[16];
    u32 n = 0;
    buf[n++] = (2u << 18) | 0x1D8C;              /* two args promised     */
    buf[n++] = 0xFFFFFF00u;                      /* only one published    */
    const u32 published = n;
    buf[n++] = 0x11111111u;                      /* arrives later         */
    buf[n++] = (1u << 18) | 0x1D94;
    buf[n++] = 0x01;

    rsx_nir_stream sa, sb;
    rsx_nir_stream_init(&sa);
    rsx_nir_stream_init(&sb);
    rsx_nir_adapter ad;
    rsx_nir_adapter_init(&ad, &sa);
    u32 stop = 0;
    u32 used = rsx_nir_adapter_fifo(&ad, buf, published, &stop);
    CHECK(used == 0, "truncated packet was consumed (%u)", used);
    /* nothing executed: no ops beyond (possibly) none at all */
    CHECK(sa.op_count == 0, "partial publication emitted ops");
    /* full publication: parse from the packet start */
    used = rsx_nir_adapter_fifo(&ad, buf, n, &stop);
    CHECK(used == n, "full parse consumed %u of %u", used, n);
    rsx_nir_adapter_finish(&ad);

    rsx_nir_adapter ad2;
    rsx_nir_adapter_init(&ad2, &sb);
    rsx_nir_adapter_fifo(&ad2, buf, n, &stop);
    rsx_nir_adapter_finish(&ad2);
    char err[256] = {0};
    CHECK(rsx_nir_compare(&sa, &sb, err, sizeof(err)) == 0,
          "resumed vs whole parse: %s", err);

    /* ring-side atomicity: state ops without their action execute no
     * action at the backend */
    rsx_nr_ring ring;
    rsx_nr_tokens tokens;
    rsx_nr_tokens_init(&tokens);
    CHECK(rsx_nr_ring_init(&ring, 256, 8192) == 0, "ring init");
    exec_rec rec;
    memset(&rec, 0, sizeof(rec));
    rsx_nr_exec_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = &rec;
    ops.clear = rec_clear;
    ops.draw = rec_draw;
    ops.present = rec_present;
    rsx_nr_backend be;
    rsx_nr_backend_init(&be, &ring, &tokens, &ops);
    rsx_nir_op op;
    memset(&op, 0, sizeof(op));
    op.kind = RSX_NIR_OP_SET_BLEND;
    op.u.blend.blend_enable = 1;
    rsx_nr_ring_push(&ring, &op);
    rsx_nr_backend_run(&be, 0);
    CHECK(rec.n == 0, "state-only partial command executed an action");
    CHECK(be.st.blend.blend_enable == 1, "state op not folded");
    rsx_nr_ring_destroy(&ring);
    rsx_nir_stream_free(&sa);
    rsx_nir_stream_free(&sb);
}

/* ---- adversarial: reset semantics -------------------------------------- */

static void test_reset_semantics(void)
{
    /* after a stream reset + emitter re-init, the new stream must be
     * self-contained: the first action re-emits every state group, and
     * comparison against a fresh producer of the same commands holds */
    rsx_nir_stream sa, sb;
    rsx_nir_stream_init(&sa);
    rsx_nir_stream_init(&sb);
    rsx_nir_emitter em;
    rsx_nir_emitter_init_stream(&em, &sa);
    typed_stage_defaults(&em);
    build_scene_typed(&em);
    u32 ops_before = sa.op_count;
    CHECK(ops_before > 0, "scene emitted nothing");

    rsx_nir_stream_reset(&sa);
    rsx_nir_emitter_init_stream(&em, &sa);   /* renderer reset            */
    typed_stage_defaults(&em);
    rsx_nir_em_clear(&em, 1, 0xAB, 0, 0);

    rsx_nir_emitter e2;
    rsx_nir_emitter_init_stream(&e2, &sb);
    typed_stage_defaults(&e2);
    rsx_nir_em_clear(&e2, 1, 0xAB, 0, 0);
    char err[256] = {0};
    CHECK(rsx_nir_compare(&sa, &sb, err, sizeof(err)) == 0,
          "post-reset stream not self-contained: %s", err);
    rsx_nir_stream_free(&sa);
    rsx_nir_stream_free(&sb);
}

/* ---- end-to-end: intercept + backend + FIFO-consumer simulator --------- */

static void test_intercept_backend_end_to_end(void)
{
    rsx_nr_ring ring;
    rsx_nr_tokens tokens;
    rsx_nr_tokens_init(&tokens);
    CHECK(rsx_nr_ring_init(&ring, 1024, 32768) == 0, "ring init");
    rsx_nr_intercept it;
    rsx_nr_intercept_init(&it, &ring, &tokens,
                          (1u << RSX_NR_FAM_CLEAR) |
                          (1u << RSX_NR_FAM_FLIP), 1);
    typed_stage_defaults(&it.shadow.em);

    exec_rec rec;
    memset(&rec, 0, sizeof(rec));
    rsx_nr_exec_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = &rec;
    ops.clear = rec_clear;
    ops.draw = rec_draw;
    ops.present = rec_present;
    ops.sem_write = rec_sem_write;
    ops.sem_read = rec_sem_read;
    rsx_nr_backend be;
    rsx_nr_backend_init(&be, &ring, &tokens, &ops);

    /* frame: native clear; FIFO-owned draw (family off); native flip.
     * The backend must execute the clear, park at the drain wait, and
     * present only after the FIFO consumer reports the episode done. */
    CHECK(rsx_nr_try_clear(&it, 0xF3, 0x40, 0xFFFFFF, 0) == 1, "clear");
    u32 batch[2] = { 0, 3 };
    CHECK(rsx_nr_try_draw(&it, 5, 0, batch, 1) == 0, "draw intercepted");
    /* (the FIFO path executes the draw on its own consumer here) */
    CHECK(rsx_nr_try_flip(&it, 0, 0, 0, 0) == 1, "flip");

    rsx_nr_backend_run(&be, 0);
    CHECK(strcmp(rec.kinds, "C") == 0,
          "backend ran past the drain gate: '%s'", rec.kinds);
    CHECK(rsx_nr_backend_step(&be) == RSX_NR_STEP_BLOCKED_TOKEN,
          "not parked on the drain token");

    rsx_nr_intercept_fifo_drained(&it, 1);   /* FIFO consumer catches up  */
    rsx_nr_backend_run(&be, 0);
    CHECK(strcmp(rec.kinds, "CP") == 0, "post-drain execution '%s'",
          rec.kinds);
    CHECK(rec.last_present == 0, "flip buffer %u", rec.last_present);
    CHECK(be.stats.fallback_enters == 1 && be.stats.fallback_exits == 1,
          "fallback marker accounting %llu/%llu",
          be.stats.fallback_enters, be.stats.fallback_exits);

    rsx_nr_ring_destroy(&ring);
}

/* Exact shape of the first live draw bridge: BEGIN and batches are mirrored
 * through FIFO shadow mode; before END reaches the shadow, the typed backend
 * owns one DRAW action; shadowing END then resets the batch accumulator. */
static void test_live_draw_end_bridge_shape(void)
{
    rsx_nr_ring ring;
    rsx_nr_tokens tokens;
    rsx_nr_tokens_init(&tokens);
    CHECK(rsx_nr_ring_init(&ring, 1024, 32768) == 0, "ring init");
    rsx_nr_intercept it;
    rsx_nr_intercept_init(&it, &ring, &tokens,
                          (1u << RSX_NR_FAM_DRAW), 1);

    rsx_nr_intercept_shadow_method(&it, 0x1808, 5); /* BEGIN triangles */
    rsx_nr_intercept_shadow_method(&it, 0x1814, (11u << 24) | 4u);
    CHECK(it.shadow.batch_count == 1 && !it.shadow.draw_indexed &&
          !it.shadow.draw_mixed,
          "shadow did not retain live draw batch");

    exec_rec rec;
    memset(&rec, 0, sizeof(rec));
    rsx_nr_exec_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = &rec;
    ops.draw = rec_draw;
    rsx_nr_backend be;
    rsx_nr_backend_init(&be, &ring, &tokens, &ops);

    CHECK(rsx_nr_try_draw(&it, it.shadow.rsx.current_primitive,
                          it.shadow.draw_indexed, it.shadow.batches,
                          it.shadow.batch_count) == 1,
          "live-shape typed draw refused");
    rsx_nr_backend_run(&be, 0);
    CHECK(strcmp(rec.kinds, "D") == 0, "live-shape execution '%s'",
          rec.kinds);
    CHECK(be.stats.executed[RSX_NIR_OP_DRAW] == 1 &&
          be.stats.exec_errors == 0,
          "live-shape backend accounting wrong");

    rsx_nr_intercept_shadow_method(&it, 0x1808, 0); /* END after ownership */
    CHECK(it.shadow.batch_count == 0,
          "shadow END did not reset live batch accumulator");
    rsx_nr_ring_destroy(&ring);
}

/* ---- optional real-capture leg ----------------------------------------- */

typedef struct rxs_head {
    u32 n_blocks, n_records, reg_words, vp_words;
    u32 const_words;
} rxs_head;

static int run_capture(const char* path)
{
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "capture leg: cannot open %s\n", path);
        return -1;
    }
    u32 header[8];
    if (fread(header, 4, 8, fp) != 8 || memcmp(header, "RXS1", 4) != 0 ||
        (header[1] != 2 && header[1] != 3)) {
        fprintf(stderr, "capture leg: %s is not RXS1 v2/v3\n", path);
        fclose(fp);
        return -1;
    }
    rxs_head h;
    h.n_blocks = header[2];
    h.n_records = header[3];
    h.reg_words = header[4];
    h.vp_words = header[5];
    h.const_words = 0;

    u32 disp_count;
    u32 disp[8][4];
    if (fread(&disp_count, 4, 1, fp) != 1 ||
        fread(disp, 16, 8, fp) != 8 ||
        (header[1] >= 3 && fread(&h.const_words, 4, 1, fp) != 1)) {
        fclose(fp);
        return -1;
    }

    u32* regs = malloc((size_t)h.reg_words * 4);
    u32* vp = malloc((size_t)h.vp_words * 4);
    u32* consts = malloc(h.const_words ? (size_t)h.const_words * 4 : 4);
    u32* blocks = malloc((size_t)h.n_blocks * 16);
    u32* records = malloc((size_t)h.n_records * 8);
    if (!regs || !vp || !consts || !blocks || !records ||
        fread(regs, 4, h.reg_words, fp) != h.reg_words ||
        fread(vp, 4, h.vp_words, fp) != h.vp_words ||
        (h.const_words && fread(consts, 4, h.const_words, fp) != h.const_words) ||
        fread(blocks, 16, h.n_blocks, fp) != h.n_blocks) {
        fprintf(stderr, "capture leg: truncated state sections\n");
        goto fail;
    }
    /* skip the guest-memory data section: block bounds give its size */
    {
        u64 data_size = 0;
        for (u32 i = 0; i < h.n_blocks; i++) {
            u64 end = (u64)blocks[i * 4 + 3] + blocks[i * 4 + 2];
            if (end > data_size)
                data_size = end;
        }
        /* seek from current position */
        if (data_size) {
#ifdef _WIN32
            if (_fseeki64(fp, (long long)data_size, SEEK_CUR)) {
#else
            if (fseek(fp, (long)data_size, SEEK_CUR)) {
#endif
                fprintf(stderr, "capture leg: data section seek failed\n");
                goto fail;
            }
        }
    }
    if (fread(records, 8, h.n_records, fp) != h.n_records) {
        fprintf(stderr, "capture leg: truncated records\n");
        goto fail;
    }
    fclose(fp);
    fp = NULL;

    {
        /* two independent adapter instances over the same stream */
        rsx_nir_stream sa, sb;
        rsx_nir_stream_init(&sa);
        rsx_nir_stream_init(&sb);
        for (int pass = 0; pass < 2; pass++) {
            rsx_nir_adapter* ad = malloc(sizeof(*ad));
            if (!ad)
                goto fail;
            rsx_nir_adapter_init(ad, pass ? &sb : &sa);
            rsx_nir_adapter_seed(ad, regs, h.reg_words, vp, h.vp_words,
                                 consts, h.const_words);
            for (u32 i = 0; i < h.n_records; i++) {
                u32 m = records[i * 2];
                u32 a = records[i * 2 + 1];
                if (m & 0x80000000u)
                    continue;                    /* memory-apply record      */
                rsx_nir_adapter_method(ad, m, a);
            }
            rsx_nir_adapter_finish(ad);          /* flush pending inline run */
            if (!pass) {
                u32 draws = 0, clears = 0, sems = 0, presents = 0, reports = 0;
                u32 transfers = 0;
                const char* const dump_transfers =
                    getenv("YZ_NIR_DUMP_TRANSFERS");
                const char* const dump_transfer_window =
                    getenv("YZ_NIR_DUMP_TRANSFER_WINDOW");
                rsx_nir_cursor c;
                rsx_nir_action act;
                rsx_nir_cursor_init(&c, &sa);
                u32 bad_draw = 0;
                u32 action_index = 0;
                while (rsx_nir_cursor_next(&c, &act)) {
                    if (dump_transfer_window && dump_transfer_window[0] &&
                        strcmp(dump_transfer_window, "0") != 0 &&
                        action_index >= 1695u && action_index <= 1745u) {
                        printf("  action[%u] op=%u kind=%u rt=%u:%08X/%ux%u\n",
                               action_index, act.op_index, act.kind,
                               act.state.surface.color_location[0],
                               act.state.surface.color_offset[0],
                               act.state.surface.clip_w,
                               act.state.surface.clip_h);
                    }
                    switch (act.kind) {
                    case RSX_NIR_OP_DRAW:
                        draws++;
                        if (act.u.draw.batch_count == 0 ||
                            act.u.draw.primitive == 0 ||
                            act.u.draw.primitive > 10)
                            bad_draw++;
                        break;
                    case RSX_NIR_OP_CLEAR: clears++; break;
                    case RSX_NIR_OP_SEMAPHORE_RELEASE:
                    case RSX_NIR_OP_SEMAPHORE_ACQUIRE: sems++; break;
                    case RSX_NIR_OP_PRESENT: presents++; break;
                    case RSX_NIR_OP_REPORT: reports++; break;
                    case RSX_NIR_OP_TRANSFER:
                        transfers++;
                        if (dump_transfers && dump_transfers[0] &&
                            strcmp(dump_transfers, "0") != 0) {
                            const rsx_nir_transfer* const t = &act.u.transfer;
                            printf("  transfer[%u] action=%u kind=%u "
                                   "src=%u:%08X pitch=%u fmt=%u "
                                   "dst=%u:%08X pitch=%u fmt=%u "
                                   "line=%ux%u in=%u,%u+%ux%u "
                                   "out=%u,%u+%ux%u clip=%u,%u+%ux%u "
                                   "step=%08X/%08X origin=%u interp=%u\n",
                                   transfers - 1u, action_index, t->kind,
                                   t->src_location, t->src_offset,
                                   t->src_pitch, t->src_format,
                                   t->dst_location, t->dst_offset,
                                   t->dst_pitch, t->dst_format,
                                   t->line_length, t->line_count,
                                   t->in_x, t->in_y, t->in_w, t->in_h,
                                   t->out_x, t->out_y, t->out_w, t->out_h,
                                   t->clip_x, t->clip_y,
                                   t->clip_w, t->clip_h,
                                   t->ds_dx, t->dt_dy,
                                   t->origin, t->interpolator);
                        }
                        break;
                    default: break;
                    }
                    action_index++;
                }
                printf("capture %s: methods=%u actions=%u draws=%u clears=%u "
                       "sems=%u reports=%u transfers=%u presents=%u "
                       "ops=%u side=%u\n",
                       path, ad->methods_seen, ad->actions_seen, draws,
                       clears, sems, reports, transfers, presents, sa.op_count,
                       sa.side_count);
                /* method-write census by decoder class: how much of the
                 * real stream the register-file model actively decodes
                 * (STATE/EXEC) vs merely stores (TODO). */
                {
                    u64 by_class[3] = {0, 0, 0};
                    for (u32 r = 0; r < RSX_DSP_NUM_REGS; r++)
                        if (ad->rsx.seen[r])
                            by_class[ad->rsx.klass[r] <= 2 ?
                                     ad->rsx.klass[r] : 0] += ad->rsx.seen[r];
                    u64 total = by_class[0] + by_class[1] + by_class[2];
                    printf("capture method census: total=%llu "
                           "decoded(state)=%llu exec=%llu stored-only=%llu "
                           "(%.2f%% decoded)\n",
                           (unsigned long long)total,
                           (unsigned long long)by_class[1],
                           (unsigned long long)by_class[2],
                           (unsigned long long)by_class[0],
                           total ? 100.0 * (double)(by_class[1] + by_class[2]) /
                                       (double)total : 0.0);
                    /* top stored-only methods: the honest gap list */
                    for (int t = 0; t < 8; t++) {
                        u32 best = 0, best_r = 0;
                        for (u32 r = 0; r < RSX_DSP_NUM_REGS; r++)
                            if (ad->rsx.klass[r] == RSX_DSP_CLASS_TODO &&
                                ad->rsx.seen[r] > best) {
                                best = ad->rsx.seen[r];
                                best_r = r;
                            }
                        if (!best)
                            break;
                        printf("  stored-only method 0x%05X x%u\n",
                               best_r << 2, best);
                        ad->rsx.seen[best_r] = 0;
                    }
                }
                CHECK(draws > 0, "capture produced no draws");
                CHECK(bad_draw == 0, "capture produced %u malformed draws",
                      bad_draw);
                CHECK(ad->batch_overflow == 0, "capture batch overflow %u",
                      ad->batch_overflow);
            }
            free(ad);
        }
        char err[256] = {0};
        int rc = rsx_nir_compare(&sa, &sb, err, sizeof(err));
        CHECK(rc == 0, "capture determinism: %s", err);

        /* ring-transport determinism: pump the whole capture stream
         * through a deliberately small ring (thousands of wraps) and
         * require a byte-identical refold */
        {
            rsx_nr_ring ring;
            CHECK(rsx_nr_ring_init(&ring, 1024, 1u << 15) == 0,
                  "capture ring init");
            rsx_nir_stream rb;
            rsx_nir_stream_init(&rb);
            const rsx_nr_slot* slot;
            u32 pumped = 0;
            for (u32 i = 0; i < sa.op_count; i++) {
                const rsx_nir_op* op = &sa.ops[i];
                u32 ofs, cnt;
                op_side(op, &ofs, &cnt);
                if (!rsx_nr_ring_can_accept(&ring, 1, cnt)) {
                    while ((slot = rsx_nr_ring_peek(&ring)) != NULL) {
                        copy_op_to_stream(&ring, &slot->op, &rb);
                        rsx_nr_ring_pop(&ring);
                    }
                    if (!rsx_nr_ring_can_accept(&ring, 1, cnt)) {
                        CHECK(0, "capture op %u exceeds ring capacity "
                              "(side %u)", i, cnt);
                        break;
                    }
                }
                rsx_nir_op c = *op;
                if (cnt) {
                    u32* dst = NULL;
                    u32 nofs = rsx_nr_ring_side_reserve(&ring, cnt, &dst);
                    if (nofs == ~0u || !dst) {
                        CHECK(0, "capture side reserve failed at op %u", i);
                        break;
                    }
                    memcpy(dst, rsx_nir_side(&sa, ofs, cnt),
                           (size_t)cnt * 4);
                    op_set_side_ofs(&c, nofs);
                }
                if (rsx_nr_ring_push(&ring, &c) != 0) {
                    CHECK(0, "capture ring push failed at op %u", i);
                    break;
                }
                pumped++;
            }
            while ((slot = rsx_nr_ring_peek(&ring)) != NULL) {
                copy_op_to_stream(&ring, &slot->op, &rb);
                rsx_nr_ring_pop(&ring);
            }
            CHECK(pumped == sa.op_count, "pumped %u of %u ops", pumped,
                  sa.op_count);
            err[0] = 0;
            rc = rsx_nir_compare(&sa, &rb, err, sizeof(err));
            CHECK(rc == 0, "capture ring transport: %s", err);
            printf("capture ring transport: %u ops, %u side words, "
                   "%llu wrap-pad words, high water %llu\n",
                   sa.op_count, sa.side_count,
                   (unsigned long long)ring.side_pad_words,
                   (unsigned long long)ring.op_high_water);
            rsx_nir_stream_free(&rb);
            rsx_nr_ring_destroy(&ring);
        }

        rsx_nir_stream_free(&sa);
        rsx_nir_stream_free(&sb);
    }

    free(regs); free(vp); free(consts); free(blocks); free(records);
    return 0;

fail:
    if (fp)
        fclose(fp);
    free(regs); free(vp); free(consts); free(blocks); free(records);
    g_failures++;
    return -1;
}

static void test_shadow_terminal_action(void)
{
    rsx_nir_stream s;
    rsx_nir_stream_init(&s);
    rsx_nir_adapter ad;
    rsx_nir_adapter_init(&ad, &s);
    ad.shadow_mode = 1;

    rsx_nir_adapter_method(&ad, M_BEGIN_END, 5);
    rsx_nir_adapter_method(&ad, M_DRAW_ARRAYS,
                           (2u << 24) | 7u); /* first 7, count 3 */
    CHECK(s.op_count == 0, "shadow path emitted before terminal ownership");
    CHECK(rsx_nir_adapter_shadow_action(&ad, M_BEGIN_END, 0) == 1,
          "shadow terminal draw was not emitted");
    CHECK(ad.shadow_mode == 1, "adapter did not return to shadow mode");
    CHECK(s.op_count != 0, "terminal action produced an empty stream");
    if (s.op_count) {
        const rsx_nir_op* draw = &s.ops[s.op_count - 1];
        CHECK(draw->kind == RSX_NIR_OP_DRAW,
              "terminal op kind %u, expected DRAW", draw->kind);
        if (draw->kind == RSX_NIR_OP_DRAW) {
            CHECK(draw->u.draw.primitive == 5 &&
                  draw->u.draw.indexed == 0 &&
                  draw->u.draw.batch_count == 1 &&
                  draw->u.draw.total_count == 3,
                  "terminal draw shape primitive=%u indexed=%u batches=%u total=%u",
                  draw->u.draw.primitive, draw->u.draw.indexed,
                  draw->u.draw.batch_count, draw->u.draw.total_count);
            const u32* batch = rsx_nir_side(
                &s, draw->u.draw.batches_ofs, 2);
            CHECK(batch && batch[0] == 7 && batch[1] == 3,
                  "terminal batch payload mismatch");
        }
    }

    const u32 before = s.op_count;
    rsx_nir_adapter_method(&ad, M_BEGIN_END, 5);
    CHECK(rsx_nir_adapter_shadow_action(&ad, M_BEGIN_END, 0) == 0,
          "empty begin/end incorrectly claimed as a draw");
    CHECK(s.op_count == before,
          "empty begin/end changed the typed stream");

    rsx_nir_adapter_method(&ad, M_CLEAR_COLOR, 0xA0B0C0D0u);
    CHECK(rsx_nir_adapter_shadow_action(&ad, M_CLEAR_BUFFERS, 0xF0u) == 1,
          "shadow terminal clear was not emitted");
    CHECK(ad.shadow_mode == 1, "clear did not restore shadow mode");
    CHECK(s.op_count > before &&
          s.ops[s.op_count - 1].kind == RSX_NIR_OP_CLEAR,
          "terminal clear did not end in a CLEAR op");
    if (s.op_count > before &&
        s.ops[s.op_count - 1].kind == RSX_NIR_OP_CLEAR) {
        const rsx_nir_clear* clear = &s.ops[s.op_count - 1].u.clear;
        CHECK(clear->mask == 0xF0u &&
              clear->color_value == 0xA0B0C0D0u,
              "terminal clear payload mask=%X color=%08X",
              clear->mask, clear->color_value);
    }

    rsx_nir_adapter_method(&ad, 0x2184u, 0xFEED0000u);
    rsx_nir_adapter_method(&ad, 0x2188u, 0xFEED0001u);
    rsx_nir_adapter_method(&ad, 0x230Cu, 0x1000u);
    rsx_nir_adapter_method(&ad, 0x2310u, 0x2000u);
    rsx_nir_adapter_method(&ad, 0x2314u, 16u);
    rsx_nir_adapter_method(&ad, 0x2318u, 16u);
    rsx_nir_adapter_method(&ad, 0x231Cu, 8u);
    rsx_nir_adapter_method(&ad, 0x2320u, 2u);
    rsx_nir_adapter_method(&ad, 0x2324u, 0x0101u);
    CHECK(rsx_nir_adapter_shadow_action(&ad, 0x2328u, 0u) == 1,
          "shadow terminal buffer transfer was not emitted");
    CHECK(ad.shadow_mode == 1 && s.op_count &&
          s.ops[s.op_count - 1].kind == RSX_NIR_OP_TRANSFER,
          "terminal transfer did not restore shadow mode/emit TRANSFER");
    if (s.op_count && s.ops[s.op_count - 1].kind == RSX_NIR_OP_TRANSFER) {
        const rsx_nir_transfer* transfer =
            &s.ops[s.op_count - 1].u.transfer;
        CHECK(transfer->kind == RSX_NIR_XFER_BUFFER &&
              transfer->src_location == RSX_NIR_LOCATION_LOCAL &&
              transfer->dst_location == RSX_NIR_LOCATION_MAIN &&
              transfer->src_offset == 0x1000u &&
              transfer->dst_offset == 0x2000u &&
              transfer->line_length == 8u && transfer->line_count == 2u,
              "terminal buffer transfer payload mismatch");
    }

    rsx_nir_adapter_method(&ad, 0x01A4u, 0x66606660u);
    rsx_nir_adapter_method(&ad, 0x1D6Cu, 0x80u);
    CHECK(rsx_nir_adapter_shadow_action(&ad, 0x1D70u, 0x11223344u) == 1,
          "shadow back-end release was not emitted");
    CHECK(ad.shadow_mode == 1 && s.op_count &&
          s.ops[s.op_count - 1].kind == RSX_NIR_OP_SEMAPHORE_RELEASE,
          "terminal release did not restore shadow mode/emit semaphore");
    if (s.op_count &&
        s.ops[s.op_count - 1].kind == RSX_NIR_OP_SEMAPHORE_RELEASE) {
        const rsx_nir_semaphore* sem =
            &s.ops[s.op_count - 1].u.semaphore;
        CHECK(sem->dma_context == 0x66606660u && sem->offset == 0x80u &&
              sem->value == 0x11223344u && sem->texture_read == 0u,
              "terminal back-end release payload mismatch");
    }
    rsx_nir_adapter_method(&ad, 0x01A8u, 0x66626660u);
    CHECK(rsx_nir_adapter_shadow_action(&ad, 0x1800u, 0x01000080u) == 1,
          "shadow GET_REPORT was not emitted");
    CHECK(ad.shadow_mode == 1 && s.op_count &&
          s.ops[s.op_count - 1].kind == RSX_NIR_OP_REPORT,
          "terminal report did not restore shadow mode/emit REPORT");
    if (s.op_count && s.ops[s.op_count - 1].kind == RSX_NIR_OP_REPORT) {
        const rsx_nir_report* report = &s.ops[s.op_count - 1].u.report;
        CHECK(report->kind == 0u && report->arg == 0x01000080u &&
              report->dma_report == 0x66626660u,
              "terminal report payload mismatch");
    }
    rsx_nir_stream_free(&s);
}

static void test_section_method_support(void)
{
    rsx_nir_stream stream;
    rsx_nir_stream_init(&stream);
    rsx_nir_adapter ad;
    rsx_nir_adapter_init(&ad, &stream);
    CHECK(RSX_REPORT_LOCAL_BASE == 0x10201400u &&
              RSX_REPORT_AREA_SIZE == 0x8000u,
          "local report array does not match reportsReportOffset/slot count");
    CHECK(rsx_report_unmodeled_value(
              RSX_REPORT_TYPE_ZPASS_PIXEL_CNT, 1) == 1u &&
              rsx_report_unmodeled_value(
                  RSX_REPORT_TYPE_ZPASS_PIXEL_CNT, 0) == 0u &&
              rsx_report_unmodeled_value(
                  RSX_REPORT_TYPE_ZCULL_STATS, 1) == 0u &&
              rsx_report_unmodeled_value(
                  RSX_REPORT_TYPE_ZCULL_STATS3, 1) == 0u,
          "unmodeled report fallback did not remain narrowly ZPASS-visible");
    CHECK(ad.fifo_semaphore_dma == 0x66616661u,
          "NV406E reset semaphore DMA %08X", ad.fifo_semaphore_dma);
    CHECK(rsx_nir_adapter_method_supported(&ad, 0x0300u, 1u) &&
              rsx_nir_adapter_method_supported(
                  &ad, 0x003C0u, 0x00010101u) &&
              rsx_nir_adapter_method_supported(
                  &ad, 0x00440u, 0x9AABAA98u) &&
              rsx_nir_adapter_method_supported(
                  &ad, 0x0A000u, 0x31337808u) &&
              !rsx_nir_adapter_method_supported(
                  &ad, 0x003C0u, 0x01010101u) &&
              !rsx_nir_adapter_method_supported(
                  &ad, 0x00440u, 0x9AABAA99u) &&
              !rsx_nir_adapter_method_supported(
                  &ad, 0x0A000u, 0x31337809u),
          "title context image was not admitted/rejected exactly");
    rsx_nir_adapter_method(&ad, 0x1D94u, 0xF0u); /* first render action */
    CHECK(!ad.context_image_open &&
              !rsx_nir_adapter_method_supported(&ad, 0x0300u, 1u) &&
              !rsx_nir_adapter_method_supported(
                  &ad, 0x00440u, 0x9AABAA98u),
          "title context image admission remained open after execution");
    CHECK(rsx_nir_adapter_method_supported(&ad, 0x0050u, 0u),
          "SET_REFERENCE not section-supported");
    CHECK(rsx_nir_adapter_method_supported(&ad, 0x0068u, 0u) &&
              rsx_nir_adapter_method_supported(&ad, 0x006Cu, 0u),
          "NV406E semaphore family not section-supported");
    CHECK(rsx_nir_adapter_method_supported(&ad, 0x1D94u, 0u),
          "CLEAR_SURFACE not section-supported");
    CHECK(rsx_nir_adapter_method_supported(
              &ad, M_RENDER_ENABLE, 0x01000000u) &&
              rsx_nir_adapter_method_supported(
                  &ad, M_RENDER_ENABLE, 0x020045A0u) &&
              !rsx_nir_adapter_method_supported(
                  &ad, M_RENDER_ENABLE, 0x000045A0u) &&
              !rsx_nir_adapter_method_supported(
                  &ad, M_RENDER_ENABLE, 0x030045A0u),
          "conditional-render modes were not fenced exactly");
    rsx_nir_adapter_method(&ad, M_CONTEXT_REPORT, 0x66626660u);
    rsx_nir_adapter_method(&ad, M_RENDER_ENABLE, 0x020045A0u);
    rsx_nir_adapter_method(&ad, M_CONTEXT_REPORT, 0x66626661u);
    rsx_nir_adapter_stage_state(&ad);
    CHECK(ad.em.pending.render_condition.enabled == 1u &&
              ad.em.pending.render_condition.dma_report == 0x66626660u &&
              ad.em.pending.render_condition.offset == 0x45A0u,
          "conditional report binding was retargeted after SET_RENDER_ENABLE");
    rsx_nir_adapter_method(&ad, M_RENDER_ENABLE, 0x01000000u);
    rsx_nir_adapter_stage_state(&ad);
    CHECK(ad.em.pending.render_condition.enabled == 0u,
          "conditional-render disable was not retained");
    CHECK(rsx_nir_adapter_method_supported(&ad, 0xE924u, 0u),
          "typed present companion not section-supported");
    CHECK(!rsx_nir_adapter_method_supported(&ad, 0x0004u, 0u) &&
              !rsx_nir_adapter_method_supported(&ad, 0xFFFFCu, 0u),
          "unknown methods incorrectly section-supported");
    CHECK(rsx_nir_adapter_method_supported(&ad, 0x1804u, 0u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x1804u, 1u),
          "ZCULL statistics enable was not fenced to the inert disabled mode");
    CHECK(rsx_nir_adapter_method_supported(&ad, 0x1D78u, 1u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x1D78u, 0u),
          "ZMIN/MAX non-default mode was not fenced");
    CHECK(rsx_nir_adapter_method_supported(&ad, 0x02B8u, 0u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x02B8u, 1u),
          "window-offset non-default mode was not fenced");
    CHECK(rsx_nir_adapter_method_supported(&ad, 0x0380u, 0u) &&
              rsx_nir_adapter_method_supported(&ad, 0x0380u, 1u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x0380u, 2u) &&
              rsx_nir_adapter_method_supported(&ad, 0x0384u, 0x3F000000u) &&
              rsx_nir_adapter_method_supported(&ad, 0x0388u, 0x3F800000u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x0384u, 0x7F800000u),
          "depth-bounds register family was not represented/fenced exactly");
    rsx_nir_adapter_method(&ad, 0x0384u, 0x3F000000u);
    rsx_nir_adapter_method(&ad, 0x0388u, 0x3F600000u);
    rsx_nir_adapter_method(&ad, 0x0380u, 1u);
    rsx_nir_adapter_stage_state(&ad);
    CHECK(ad.em.pending.depth_stencil.depth_bounds_test_enable == 1u &&
              ad.em.pending.depth_stencil.depth_bounds_min == 0x3F000000u &&
              ad.em.pending.depth_stencil.depth_bounds_max == 0x3F600000u,
          "depth-bounds register state was not retained in typed state");
    CHECK(rsx_nir_adapter_method_supported(&ad, 0x1828u, 0x1B02u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x1828u, 0x1B01u),
          "non-fill polygon mode was not fenced");
    CHECK(rsx_nir_adapter_method_supported(&ad, 0x0A60u, 1u) &&
              rsx_nir_adapter_method_supported(&ad, 0x0A64u, 0u) &&
              rsx_nir_adapter_method_supported(&ad, 0x0A68u, 1u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x0A68u, 2u) &&
              rsx_nir_adapter_method_supported(&ad, 0x0A78u, 0x3FC00000u) &&
              rsx_nir_adapter_method_supported(&ad, 0x0A7Cu, 0xC0000000u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x0A78u, 0x7F800000u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x0A7Cu, 0x7FC00000u),
          "polygon-offset state was not represented/fenced exactly");
    rsx_nir_adapter_method(&ad, 0x0A60u, 1u);
    rsx_nir_adapter_method(&ad, 0x0A64u, 0u);
    rsx_nir_adapter_method(&ad, 0x0A68u, 1u);
    rsx_nir_adapter_method(&ad, 0x0A78u, 0x3FC00000u);
    rsx_nir_adapter_method(&ad, 0x0A7Cu, 0xC0000000u);
    rsx_nir_adapter_stage_state(&ad);
    CHECK(ad.em.pending.raster.polygon_offset_point_enable == 1u &&
              ad.em.pending.raster.polygon_offset_line_enable == 0u &&
              ad.em.pending.raster.polygon_offset_fill_enable == 1u &&
              ad.em.pending.raster.polygon_offset_scale == 0x3FC00000u &&
              ad.em.pending.raster.polygon_offset_bias == 0xC0000000u,
          "polygon-offset register state was not retained in typed raster");
    CHECK(rsx_nir_adapter_method_supported(
              &ad, 0xC198u, 0x313371C3u) &&
              !rsx_nir_adapter_method_supported(
                  &ad, 0xC198u, 0x31337A73u),
          "NV3089 non-SURFACE2D context was not fenced");
    rsx_nir_adapter_method(&ad, 0xC198u, 0x313371C3u);
    CHECK(ad.sif_context_surface == 0x313371C3u,
          "NV3089 surface context was not retained");
    CHECK(rsx_nir_adapter_method_supported(&ad, 0x1D88u, 0x102D0u) &&
              !rsx_nir_adapter_method_supported(
                  &ad, 0x1D88u, 0x00200000u),
          "shader-window validation mismatch");
    CHECK(rsx_nir_adapter_method_supported(&ad, 0x0348u, 0u) &&
              rsx_nir_adapter_method_supported(&ad, 0x0348u, 1u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x0348u, 2u) &&
              rsx_nir_adapter_method_supported(&ad, 0x1FF8u, 0u) &&
              rsx_nir_adapter_method_supported(&ad, 0x1FF8u, 0x100u),
          "two-sided stencil/VP branch routing was not fenced correctly");
    CHECK(rsx_nir_adapter_method_supported(&ad, 0x034Cu, 0xFFu) &&
              rsx_nir_adapter_method_supported(&ad, 0x0350u, 0x0203u) &&
              rsx_nir_adapter_method_supported(&ad, 0x0354u, 0x12u) &&
              rsx_nir_adapter_method_supported(&ad, 0x0358u, 0xFFu) &&
              rsx_nir_adapter_method_supported(&ad, 0x035Cu, 0x1E00u) &&
              rsx_nir_adapter_method_supported(&ad, 0x0360u, 0x1E01u) &&
              rsx_nir_adapter_method_supported(&ad, 0x0364u, 0x1E02u),
          "complete back-stencil family was not section-supported");
    rsx_nir_adapter_method(&ad, 0x034Cu, 0xFFu);
    rsx_nir_adapter_method(&ad, 0x0350u, 0x0203u);
    rsx_nir_adapter_method(&ad, 0x0354u, 0x12u);
    rsx_nir_adapter_method(&ad, 0x0358u, 0xFFu);
    rsx_nir_adapter_method(&ad, 0x035Cu, 0x1E00u);
    rsx_nir_adapter_method(&ad, 0x0360u, 0x1E01u);
    rsx_nir_adapter_method(&ad, 0x0364u, 0x1E02u);
    rsx_nir_adapter_method(&ad, 0x0348u, 1u);
    rsx_nir_adapter_stage_state(&ad);
    CHECK(ad.em.pending.depth_stencil.two_sided_stencil_enable == 1u &&
              ad.em.pending.depth_stencil.back_stencil_write_mask == 0xFFu &&
              ad.em.pending.depth_stencil.back_stencil_func == 0x0203u &&
              ad.em.pending.depth_stencil.back_stencil_ref == 0x12u &&
              ad.em.pending.depth_stencil.back_stencil_mask == 0xFFu &&
              ad.em.pending.depth_stencil.back_stencil_op_fail == 0x1E00u &&
              ad.em.pending.depth_stencil.back_stencil_op_zfail == 0x1E01u &&
              ad.em.pending.depth_stencil.back_stencil_op_zpass == 0x1E02u,
          "back-face stencil register state was not retained in typed state");
    rsx_nir_adapter_method(&ad, 0x0348u, 0u);
    CHECK(rsx_nir_adapter_method_supported(
              &ad, 0x03B0u, 0x00100000u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x03B0u, 0u) &&
              rsx_nir_adapter_method_supported(&ad, 0x0300u, 0u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x0300u, 1u) &&
              rsx_nir_adapter_method_supported(&ad, 0x1EE4u, 0u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x1EE4u, 1u) &&
              rsx_nir_adapter_method_supported(&ad, 0x1EE8u, 0x100u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x1EE8u, 0x101u) &&
              rsx_nir_adapter_method_supported(&ad, 0x1EA4u, 0x10u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x1EA4u, 0x11u) &&
              rsx_nir_adapter_method_supported(&ad, 0x1EA8u, 0x01000100u) &&
              rsx_nir_adapter_method_supported(&ad, 0x1EACu, 0xFF000002u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x1EACu, 0u) &&
              rsx_nir_adapter_method_supported(&ad, 0x17CCu, 1u) &&
              rsx_nir_adapter_method_supported(
                  &ad, 0x1D7Cu, 0xFFFF0000u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x1D7Cu, 1u),
          "default CONTROL0/dither/point/query/AA modes were not fenced exactly");
    CHECK(rsx_nir_adapter_method_supported(&ad, 0x0908u, 0x303u) &&
              rsx_nir_adapter_method_supported(&ad, 0x090Cu, 0u) &&
              !rsx_nir_adapter_method_supported(&ad, 0x0980u, 0u),
          "vertex-texture register family routing mismatch");
    CHECK(rsx_nir_adapter_method_supported(
              &ad, 0xC2FCu, 1u) &&
              !rsx_nir_adapter_method_supported(&ad, 0xC2FCu, 3u),
          "NV3089 non-default color conversion was not fenced");
    rsx_nir_adapter_method(&ad, 0xC2FCu, 1u);
    CHECK(ad.sif_color_conversion == 1u,
          "NV3089 color conversion was not retained");
    rsx_nir_stream_free(&stream);
}

static void test_adapter_copy_rebind(void)
{
    rsx_nir_stream source_stream, section_stream;
    rsx_nir_stream_init(&source_stream);
    rsx_nir_stream_init(&section_stream);
    rsx_nir_adapter source;
    rsx_nir_adapter section;
    rsx_nir_adapter_init(&source, &source_stream);
    source.shadow_mode = 1;

    /* Model the live transactional snapshot. The copied dispatch sink still
     * points at source until the explicit rebind. Dispatch-based DRAW must be
     * emitted into the section stream and must not mutate the shadow owner. */
    section = source;
    section.em.out = rsx_nir_stream_sink(&section_stream);
    section.shadow_mode = 0;
    rsx_nir_adapter_rebind(&section);
    rsx_nir_adapter_method(&section, M_BEGIN_END, 5u);
    rsx_nir_adapter_method(&section, M_DRAW_ARRAYS,
                           (2u << 24) | 7u);
    rsx_nir_adapter_method(&section, M_BEGIN_END, 0u);
    CHECK(section_stream.op_count &&
              section_stream.ops[section_stream.op_count - 1u].kind ==
                  RSX_NIR_OP_DRAW,
          "copied/rebound section adapter did not emit DRAW");
    CHECK(source_stream.op_count == 0u && source.batch_count == 0u,
          "section dispatch escaped into the persistent shadow adapter");

    /* Model committing the section snapshot back to the persistent owner.
     * Its callback pointer must likewise be rebound away from the temporary. */
    const u32 section_ops = section_stream.op_count;
    source = section;
    source.em.out = rsx_nir_stream_sink(&source_stream);
    source.shadow_mode = 0;
    rsx_nir_adapter_rebind(&source);
    rsx_nir_adapter_method(&source, M_CLEAR_COLOR, 0x11223344u);
    rsx_nir_adapter_method(&source, M_CLEAR_BUFFERS, 0xF0u);
    CHECK(source_stream.op_count &&
              source_stream.ops[source_stream.op_count - 1u].kind ==
                  RSX_NIR_OP_CLEAR,
          "committed/rebound persistent adapter did not emit CLEAR");
    CHECK(section_stream.op_count == section_ops,
          "committed adapter dispatch targeted the temporary section");

    rsx_nir_adapter_rebind(0);
    rsx_nir_stream_free(&section_stream);
    rsx_nir_stream_free(&source_stream);
}

static void test_adapter_render_state_checkpoint(void)
{
    rsx_nir_stream live_stream, saved_stream;
    rsx_nir_stream_init(&live_stream);
    rsx_nir_stream_init(&saved_stream);
    rsx_nir_adapter live, saved;
    rsx_nir_adapter_init(&live, &live_stream);
    rsx_nir_adapter_init(&saved, &saved_stream);

    rsx_nir_adapter_method(&live, M_CLEAR_COLOR, 0x11223344u);
    rsx_nir_adapter_method(&live, M_BEGIN_END, 6u);
    rsx_nir_adapter_method(&live, M_DRAW_ARRAYS, (2u << 24) | 7u);
    live.fifo_semaphore_dma = 0x66604200u;
    live.fifo_semaphore_offset = 0x100u;
    live.m2mf_offset_in = 0x200u;
    live.methods_seen = 17u;
    rsx_nir_adapter_copy_render_state(&saved, &live);

    /* Model hidden movie traffic: graphics state must later roll back, while
     * FIFO synchronization and transfer staging remain at their newest
     * values. */
    rsx_nir_adapter_method(&live, M_CLEAR_COLOR, 0xAABBCCDDu);
    rsx_nir_adapter_method(&live, M_BEGIN_END, 0u);
    live.fifo_semaphore_dma = 0x66616661u;
    live.fifo_semaphore_offset = 0xFE0u;
    live.m2mf_offset_in = 0x300u;
    live.methods_seen = 29u;
    const rsx_nir_sink live_sink = live.em.out;
    rsx_nir_adapter_copy_render_state(&live, &saved);

    CHECK(live.rsx.regs[M_CLEAR_COLOR >> 2] == 0x11223344u &&
              live.rsx.in_begin_end == 1u &&
              live.rsx.current_primitive == 6u &&
              live.batch_count == 1u,
          "render checkpoint did not restore pre-movie graphics state");
    CHECK(live.fifo_semaphore_dma == 0x66616661u &&
              live.fifo_semaphore_offset == 0xFE0u &&
              live.m2mf_offset_in == 0x300u &&
              live.methods_seen == 29u,
          "render checkpoint rolled back synchronization/transfer metadata");
    CHECK(live.em.out.push == live_sink.push &&
              live.em.out.user == live_sink.user,
          "render checkpoint replaced the destination sink");

    rsx_nir_adapter_method(&live, M_BEGIN_END, 0u);
    CHECK(live_stream.op_count &&
              live_stream.ops[live_stream.op_count - 1u].kind ==
                  RSX_NIR_OP_DRAW,
          "restored pre-movie draw did not remain executable");
    CHECK(saved_stream.op_count == 0u,
          "render checkpoint dispatch escaped into the saved adapter");

    rsx_nir_stream_free(&saved_stream);
    rsx_nir_stream_free(&live_stream);
}

static void test_dispatch_state_sync_and_fifo_registers(void)
{
    rsx_dispatch source, destination;
    rsx_dispatch_sink source_sink = {0}, destination_sink = {0};
    static int source_owner, destination_owner;
    source_sink.user = &source_owner;
    destination_sink.user = &destination_owner;
    rsx_dispatch_init(&source, &source_sink);
    rsx_dispatch_init(&destination, &destination_sink);
    rsx_dispatch_method(&source, M_CLEAR_COLOR, 0xAABBCCDDu);
    rsx_dispatch_method(&source, M_BEGIN_END, 6u);
    source.vp[17] = 0x11223344u;
    source.constants[91][2] = 0x55667788u;
    const u8 destination_class = destination.klass[M_CLEAR_COLOR >> 2];
    rsx_dispatch_copy_architectural_state(&destination, &source);
    CHECK(destination.regs[M_CLEAR_COLOR >> 2] == 0xAABBCCDDu &&
              destination.vp[17] == 0x11223344u &&
              destination.constants[91][2] == 0x55667788u &&
              destination.in_begin_end == 1 &&
              destination.current_primitive == 6u,
          "architectural state copy omitted live render state");
    CHECK(destination.sink.user == &destination_owner &&
              destination.klass[M_CLEAR_COLOR >> 2] == destination_class,
          "architectural state copy replaced destination metadata");

    rsx_nir_stream stream;
    rsx_nir_stream_init(&stream);
    rsx_nir_adapter adapter;
    rsx_nir_adapter_init(&adapter, &stream);
    rsx_dispatch legacy;
    rsx_dispatch_init(&legacy, 0);
    static const u32 fifo_methods[] = {0x0050u, 0x0060u, 0x0064u,
                                       0x0068u, 0x006Cu};
    for (u32 i = 0; i < (u32)(sizeof(fifo_methods) /
                              sizeof(fifo_methods[0])); ++i) {
        const u32 value = 0xCAFE0000u | i;
        rsx_dispatch_method(&legacy, fifo_methods[i], value);
        rsx_nir_adapter_method(&adapter, fifo_methods[i], value);
        CHECK(adapter.rsx.regs[fifo_methods[i] >> 2] ==
                  legacy.regs[fifo_methods[i] >> 2],
              "FIFO method 0x%X diverged in architectural register state",
              fifo_methods[i]);
    }
    rsx_nir_stream_free(&stream);
}

int main(int argc, char** argv)
{
    test_fifo_vs_typed();
    test_state_persistence_and_dedup();
    test_divergence_detected();
    test_ordering();
    test_fifo_front_end();
    test_flip_intercept();
    test_intercept_mixed_mode();
    test_transfers();
    test_sdk_inline_transfer_shape();
    test_fifo_control_words();
    test_reference_user_tokens();
    test_fixed_capacity();
    test_ring();
    test_backend_core();
    test_spu_patched_commands();
    test_partial_publication();
    test_reset_semantics();
    test_intercept_backend_end_to_end();
    test_live_draw_end_bridge_shape();
    test_shadow_terminal_action();
    test_section_method_support();
    test_adapter_copy_rebind();
    test_adapter_render_state_checkpoint();
    test_dispatch_state_sync_and_fifo_registers();

    const char* rxs = argc > 1 ? argv[1] : getenv("YZ_NIR_RXS");
    if (rxs && rxs[0])
        run_capture(rxs);
    else
        printf("capture leg: SKIP (no .rxs supplied via argv[1] or "
               "YZ_NIR_RXS)\n");

    if (g_failures) {
        fprintf(stderr, "rsx_nir equivalence: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("rsx_nir equivalence: PASS\n");
    return 0;
}
