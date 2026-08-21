/*
 * Native-render D3D12 sink test (WARP): the typed command path executes
 * real rasterization offline — ring -> backend core -> D3D12 sink with
 * vertex pulling over the guest-memory mirror.
 *
 * Legs:
 *   1. clear only: RT readback matches the clear color exactly.
 *   2. array draw: a half-screen triangle pulled from big-endian guest
 *      vertex bytes covers exactly its half (inside = solid PS color,
 *      outside = clear color).
 *   3. dirty-page re-upload: guest vertex bytes change + note_write; the
 *      next draw must see the new geometry (opposite half covered).
 *   4. indexed draw: same triangle through an in-shader BE u16 index
 *      fetch.
 *
 * Exits 2 ("skip") when no WARP D3D12 device exists.
 */

#ifndef _WIN32
int main(void) { return 2; }
#else

#include "../rsx_nr_backend_d3d12.h"
#include "../rsx_nir_emitter.h"

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

#define LOCAL_SIZE (1u << 20)
#define MAIN_SIZE  (1u << 16)
#define RT_OFFSET  0x00300000u   /* outside the arena on purpose: the RT  */
#define RT_W 64u                 /* is a GPU object keyed by (space,ofs)  */
#define RT_H 64u
#define VTX_OFFSET 0x2000u
#define IDX_OFFSET 0x4000u

static u8 g_local[LOCAL_SIZE];
static u8 g_main[MAIN_SIZE];

static const u8* arena_ptr(void* user, u32 space, u32 offset, u32 min_bytes)
{
    (void)user;
    u8* base = space ? g_main : g_local;
    u32 size = space ? MAIN_SIZE : LOCAL_SIZE;
    if (offset > size || min_bytes > size - offset)
        return NULL;
    return base + offset;
}

static u8* arena_wptr(void* user, u32 space, u32 offset, u32 min_bytes)
{
    return (u8*)arena_ptr(user, space, offset, min_bytes);
}

static void put_be32(u8* p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static u32 fbits(float f)
{
    u32 v;
    memcpy(&v, &f, 4);
    return v;
}

/* three float4 clip-space vertices, big-endian, at VTX_OFFSET */
static void write_triangle(rsx_guest_pages* pages, float x0, float y0,
                           float x1, float y1, float x2, float y2)
{
    u8* p = g_local + VTX_OFFSET;
    const float v[12] = { x0, y0, 0.5f, 1.0f,
                          x1, y1, 0.5f, 1.0f,
                          x2, y2, 0.5f, 1.0f };
    for (int i = 0; i < 12; i++)
        put_be32(p + i * 4, fbits(v[i]));
    rsx_guest_pages_note_write(pages, 0, VTX_OFFSET, 48);
}

static void stage_frame_state(rsx_nir_emitter* em)
{
    rsx_nir_surface s;
    memset(&s, 0, sizeof(s));
    s.color_format = 5;
    s.depth_format = 2;
    s.raster_type = 1;
    s.clip_w = RT_W;
    s.clip_h = RT_H;
    s.color_offset[0] = RT_OFFSET;
    s.color_pitch[0] = RT_W * 4;
    s.color_location[0] = RSX_NIR_LOCATION_LOCAL;
    s.color_target = 1;
    rsx_nir_em_surface(em, &s);

    rsx_nir_viewport v;
    memset(&v, 0, sizeof(v));
    v.w = RT_W;
    v.h = RT_H;
    rsx_nir_em_viewport(em, &v);

    rsx_nir_vertex_bindings vb;
    memset(&vb, 0, sizeof(vb));
    for (u32 i = 0; i < RSX_NIR_NUM_VERTEX_ATTR; i++)
        vb.attr[i].def[3] = 1.0f;
    vb.attr[0].type = 2;             /* FLOAT                              */
    vb.attr[0].size = 4;
    vb.attr[0].stride = 16;
    vb.attr[0].offset = VTX_OFFSET;
    vb.attr[0].location = RSX_NIR_LOCATION_LOCAL;
    rsx_nir_em_vertex_bindings(em, &vb);
}

static u8 g_pix[RT_W * RT_H * 4];

/* pixel at (x, y): b/g/r/a bytes */
static const u8* pix(u32 x, u32 y)
{
    return g_pix + ((size_t)y * RT_W + x) * 4;
}

static int pix_is(u32 x, u32 y, u8 bb, u8 gg, u8 rr)
{
    const u8* p = pix(x, y);
    return p[0] == bb && p[1] == gg && p[2] == rr;
}

int main(void)
{
    rsx_nr_d3d12* sink = rsx_nr_d3d12_create(NULL, LOCAL_SIZE, MAIN_SIZE,
                                             arena_ptr, arena_wptr, NULL);
    if (!sink) {
        fprintf(stderr, "no WARP D3D12 device: SKIP\n");
        return 2;
    }

    rsx_nr_ring ring;
    rsx_nr_tokens tokens;
    rsx_nr_tokens_init(&tokens);
    if (rsx_nr_ring_init(&ring, 1024, 65536)) {
        fprintf(stderr, "ring init failed\n");
        return 1;
    }
    rsx_nr_exec_ops ops;
    memset(&ops, 0, sizeof(ops));
    rsx_nr_d3d12_get_exec_ops(sink, &ops);
    rsx_nr_backend be;
    rsx_nr_backend_init(&be, &ring, &tokens, &ops);

    rsx_nir_sink k = rsx_nr_ring_sink(&ring);
    rsx_nir_emitter em;
    rsx_nir_emitter_init(&em, &k);

    /* ---- leg 1: clear to blue ------------------------------------------ */
    stage_frame_state(&em);
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0, "clear leg exec errors %llu",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "RT readback failed");
    CHECK(pix_is(1, 1, 0xFF, 0x00, 0x00), "clear pixel %02X %02X %02X",
          pix(1, 1)[0], pix(1, 1)[1], pix(1, 1)[2]);
    CHECK(pix_is(62, 62, 0xFF, 0x00, 0x00), "clear pixel far corner");

    /* ---- leg 2: bottom-left half triangle ------------------------------ */
    write_triangle(rsx_nr_d3d12_pages(sink), -1.0f, -1.0f, 1.0f, -1.0f,
                   -1.0f, 1.0f);
    u32 batch[2] = { 0, 3 };
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0, "draw leg exec errors %llu",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "RT readback failed");
    /* NDC (-1,-1) = screen bottom-left; the triangle covers the bottom-left
     * half. Inside: (2, 61); outside: (61, 2) stays blue. */
    CHECK(pix_is(2, 61, 0xFF, 0x00, 0xFF), "inside pixel %02X %02X %02X",
          pix(2, 61)[0], pix(2, 61)[1], pix(2, 61)[2]);
    CHECK(pix_is(61, 2, 0xFF, 0x00, 0x00), "outside pixel %02X %02X %02X",
          pix(61, 2)[0], pix(61, 2)[1], pix(61, 2)[2]);

    /* ---- leg 3: dirty-page re-upload flips the covered half ------------ */
    stage_frame_state(&em);
    rsx_nir_em_clear(&em, 0xF3, 0xFF00FF00u, 0xFFFFFF, 0);   /* green      */
    write_triangle(rsx_nr_d3d12_pages(sink), 1.0f, 1.0f, -1.0f, 1.0f,
                   1.0f, -1.0f);                             /* top-right  */
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0, "dirty leg exec errors %llu",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "RT readback failed");
    CHECK(pix_is(61, 2, 0xFF, 0x00, 0xFF), "new-half pixel %02X %02X %02X",
          pix(61, 2)[0], pix(61, 2)[1], pix(61, 2)[2]);
    CHECK(pix_is(2, 61, 0x00, 0xFF, 0x00), "old-half pixel %02X %02X %02X",
          pix(2, 61)[0], pix(2, 61)[1], pix(2, 61)[2]);

    /* ---- leg 4: indexed draw through BE u16 in-shader index fetch ------ */
    {
        u8* ip = g_main + IDX_OFFSET;
        ip[0] = 0; ip[1] = 0;        /* 0 */
        ip[2] = 0; ip[3] = 1;        /* 1 */
        ip[4] = 0; ip[5] = 2;        /* 2 */
        rsx_guest_pages_note_write(rsx_nr_d3d12_pages(sink), 1, IDX_OFFSET, 6);
    }
    stage_frame_state(&em);
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);   /* blue       */
    write_triangle(rsx_nr_d3d12_pages(sink), -1.0f, -1.0f, 1.0f, -1.0f,
                   -1.0f, 1.0f);
    rsx_nir_index_binding ib;
    memset(&ib, 0, sizeof(ib));
    ib.offset = IDX_OFFSET;
    ib.location = RSX_NIR_LOCATION_MAIN;
    ib.is_u32 = 0;
    rsx_nir_em_index_binding(&em, &ib);
    rsx_nir_em_draw(&em, 5, 1, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0, "indexed leg exec errors %llu",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "RT readback failed");
    CHECK(pix_is(2, 61, 0xFF, 0x00, 0xFF), "indexed inside pixel");
    CHECK(pix_is(61, 2, 0xFF, 0x00, 0x00), "indexed outside pixel");

    /* ---- sink accounting ----------------------------------------------- */
    rsx_nr_d3d12_stats st;
    rsx_nr_d3d12_get_stats(sink, &st);
    CHECK(st.clears == 3 && st.draws == 3 && st.presents == 4,
          "sink counts clears=%llu draws=%llu presents=%llu", st.clears,
          st.draws, st.presents);
    CHECK(st.unsupported_draws == 0 && st.compile_failures == 0,
          "unsupported=%llu compile_failures=%llu", st.unsupported_draws,
          st.compile_failures);
    CHECK(st.pso_builds >= 1 && st.pso_hits >= 1,
          "pso cache builds=%llu hits=%llu", st.pso_builds, st.pso_hits);

    rsx_nr_ring_destroy(&ring);
    rsx_nr_d3d12_destroy(sink);

    if (g_failures) {
        fprintf(stderr, "rsx_nr_backend_d3d12: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("rsx_nr_backend_d3d12: PASS\n");
    return 0;
}

#endif /* _WIN32 */
