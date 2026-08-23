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
#define FP_OFFSET  0x6000u

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

static u32 fbits(float f);

/* Store one host-order NV40 fragment word in the guest byte order consumed
 * by rsx_fp_read_word (big-endian with 16-bit halves exchanged). */
static void put_fp_word(u8* p, u32 word)
{
    put_be32(p, (word << 16) | (word >> 16));
}

static void write_test_fp(void)
{
    /* MOV r0, COL0; END, unconditional.  The passthrough test VS supplies
     * magenta COL0, so the existing pixel oracle now proves a real guest FP
     * rather than a hard-coded D3D12 stand-in. */
    u8* p = g_local + FP_OFFSET;
    put_fp_word(p + 0, (0x01u << 24) | (0xFu << 9) | (1u << 13) | 1u);
    put_fp_word(p + 4, 1u | ((0u << 9) | (1u << 11) |
                             (2u << 13) | (3u << 15)) |
                              (7u << 18));
    put_fp_word(p + 8, 0);
    put_fp_word(p + 12, 0);
}

static void write_const_fp(float r, float g, float b, float a)
{
    u8* p = g_local + FP_OFFSET;
    put_fp_word(p + 0, (0x01u << 24) | (0xFu << 9) | 1u);
    put_fp_word(p + 4, 2u | ((0u << 9) | (1u << 11) |
                             (2u << 13) | (3u << 15)) |
                              (7u << 18));
    put_fp_word(p + 8, 0);
    put_fp_word(p + 12, 0);
    put_fp_word(p + 16, fbits(r));
    put_fp_word(p + 20, fbits(g));
    put_fp_word(p + 24, fbits(b));
    put_fp_word(p + 28, fbits(a));
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

    rsx_nir_raster r;
    memset(&r, 0, sizeof(r));
    r.color_mask = 0x01010101u;
    rsx_nir_em_raster(em, &r);

    rsx_nir_fragment_program fp;
    memset(&fp, 0, sizeof(fp));
    fp.offset = FP_OFFSET;
    fp.location = RSX_NIR_LOCATION_LOCAL;
    fp.control = 0x40u;              /* r0 is the color export             */
    rsx_nir_em_fragment_program(em, &fp);

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

/* ---- optional real-capture execution leg -------------------------------
 * Feed an exported .rxs stream through adapter -> ring -> backend with the
 * D3D12 sink: real captured vertex programs run through the pull
 * decompiler on WARP; guest memory comes from the capture's own data
 * blocks, applied at their recorded stream positions through the dirty-
 * page tracker (the real mirror re-upload flow at capture scale).
 * Refusals (non-A8R8G8B8 surfaces, restart/fan/quad draws, FP stand-in)
 * are measured coverage, not failures; the leg fails only on parse
 * errors, ring faults, or nondeterminism between two identical runs. */

#include "../rsx_nir_adapter.h"

typedef struct cap_mem {
    u8* arena[2];
    u32 size[2];
} cap_mem;

static cap_mem g_cap;

static const u8* cap_ptr(void* user, u32 space, u32 offset, u32 min_bytes)
{
    (void)user;
    if (space > 1 || !g_cap.arena[space] || offset > g_cap.size[space] ||
        min_bytes > g_cap.size[space] - offset)
        return NULL;
    return g_cap.arena[space] + offset;
}

static u8* cap_wptr(void* user, u32 space, u32 offset, u32 min_bytes)
{
    return (u8*)cap_ptr(user, space, offset, min_bytes);
}

typedef struct cap_data {
    u32 n_blocks, n_records, reg_words, vp_words, const_words;
    u32* regs;
    u32* vp;
    u32* consts;
    u32* blocks;                     /* {location, offset, size, data_off} */
    u8* data;
    u64 data_size;
    u32* records;
} cap_data;

static int cap_load(const char* path, cap_data* c)
{
    memset(c, 0, sizeof(*c));
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "capture: cannot open %s\n", path);
        return -1;
    }
    u32 header[8];
    u32 disp_count;
    u32 disp[8][4];
    if (fread(header, 4, 8, fp) != 8 || memcmp(header, "RXS1", 4) != 0 ||
        (header[1] != 2 && header[1] != 3) ||
        fread(&disp_count, 4, 1, fp) != 1 || fread(disp, 16, 8, fp) != 8 ||
        (header[1] >= 3 && fread(&c->const_words, 4, 1, fp) != 1)) {
        fclose(fp);
        return -1;
    }
    c->n_blocks = header[2];
    c->n_records = header[3];
    c->reg_words = header[4];
    c->vp_words = header[5];
    c->regs = malloc((size_t)c->reg_words * 4 + 4);
    c->vp = malloc((size_t)c->vp_words * 4 + 4);
    c->consts = malloc((size_t)c->const_words * 4 + 4);
    c->blocks = malloc((size_t)c->n_blocks * 16 + 4);
    c->records = malloc((size_t)c->n_records * 8 + 4);
    if (!c->regs || !c->vp || !c->consts || !c->blocks || !c->records ||
        fread(c->regs, 4, c->reg_words, fp) != c->reg_words ||
        fread(c->vp, 4, c->vp_words, fp) != c->vp_words ||
        (c->const_words &&
         fread(c->consts, 4, c->const_words, fp) != c->const_words) ||
        fread(c->blocks, 16, c->n_blocks, fp) != c->n_blocks) {
        fclose(fp);
        return -1;
    }
    for (u32 i = 0; i < c->n_blocks; i++) {
        u64 end = (u64)c->blocks[i * 4 + 3] + c->blocks[i * 4 + 2];
        if (end > c->data_size)
            c->data_size = end;
    }
    c->data = malloc(c->data_size ? (size_t)c->data_size : 1);
    if (!c->data || (c->data_size &&
                     fread(c->data, 1, (size_t)c->data_size, fp) !=
                         (size_t)c->data_size) ||
        fread(c->records, 8, c->n_records, fp) != c->n_records) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static void cap_free(cap_data* c)
{
    free(c->regs); free(c->vp); free(c->consts);
    free(c->blocks); free(c->data); free(c->records);
}

static void cap_apply_block(cap_data* c, rsx_guest_pages* pages, u32 index)
{
    if (index >= c->n_blocks)
        return;
    const u32 loc = c->blocks[index * 4];
    const u32 ofs = c->blocks[index * 4 + 1];
    const u32 size = c->blocks[index * 4 + 2];
    const u32 dofs = c->blocks[index * 4 + 3];
    if (loc > 1 || !g_cap.arena[loc] || ofs > g_cap.size[loc] ||
        size > g_cap.size[loc] - ofs)
        return;
    memcpy(g_cap.arena[loc] + ofs, c->data + dofs, size);
    rsx_guest_pages_note_write(pages, loc, ofs, size);
}

static u64 fnv64(const u8* p, size_t n)
{
    u64 h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

static int cap_run_once(cap_data* c, u64* rt_hash, char* stats_line,
                        size_t stats_size)
{
    /* arena sizes: max block end per location, 64K aligned */
    u32 need[2] = { 0, 0 };
    for (u32 i = 0; i < c->n_blocks; i++) {
        u32 loc = c->blocks[i * 4];
        u64 end = (u64)c->blocks[i * 4 + 1] + c->blocks[i * 4 + 2];
        if (loc <= 1 && end > need[loc])
            need[loc] = (u32)end;
    }
    for (int s = 0; s < 2; s++) {
        g_cap.size[s] = (need[s] + 0xFFFFu) & ~0xFFFFu;
        if (!g_cap.size[s])
            g_cap.size[s] = 0x10000;
        g_cap.arena[s] = calloc(1, g_cap.size[s]);
        if (!g_cap.arena[s])
            return -1;
    }

    rsx_nr_d3d12* sink = rsx_nr_d3d12_create(NULL, g_cap.size[0],
                                             g_cap.size[1], cap_ptr,
                                             cap_wptr, NULL);
    if (!sink) {
        free(g_cap.arena[0]);
        free(g_cap.arena[1]);
        memset(&g_cap, 0, sizeof(g_cap));
        return -2;                   /* no device                          */
    }

    rsx_nr_ring ring;
    rsx_nr_tokens tokens;
    rsx_nr_tokens_init(&tokens);
    if (rsx_nr_ring_init(&ring, 4096, 1u << 19))
        return -1;
    rsx_nr_exec_ops ops;
    memset(&ops, 0, sizeof(ops));
    rsx_nr_d3d12_get_exec_ops(sink, &ops);
    rsx_nr_backend be;
    rsx_nr_backend_init(&be, &ring, &tokens, &ops);

    rsx_nir_adapter* ad = malloc(sizeof(*ad));
    if (!ad)
        return -1;
    rsx_nir_sink k = rsx_nr_ring_sink(&ring);
    rsx_nir_adapter_init_sink(ad, &k);
    rsx_nir_adapter_seed(ad, c->regs, c->reg_words, c->vp, c->vp_words,
                         c->consts, c->const_words);

    u32 rt_space = 0, rt_offset = 0, rt_w = 0, rt_h = 0;
    int ring_fault = 0;
    for (u32 i = 0; i < c->n_records; i++) {
        u32 m = c->records[i * 2];
        u32 a = c->records[i * 2 + 1];
        if (m & 0x80000000u) {
            /* drain so preceding draws see pre-apply bytes, then apply */
            while (rsx_nr_backend_step(&be) == RSX_NR_STEP_EXECUTED)
                ;
            cap_apply_block(c, rsx_nr_d3d12_pages(sink), a);
            continue;
        }
        rsx_nir_adapter_method(ad, m, a);
        if (rsx_nr_ring_reject_sticky(&ring)) {
            ring_fault = 1;
            break;
        }
        /* keep the ring shallow */
        while (rsx_nr_ring_depth(&ring) > 2048 &&
               rsx_nr_backend_step(&be) == RSX_NR_STEP_EXECUTED)
            ;
        /* remember the last color target the stream configured */
        if ((be.st.surface.color_format == 4 ||
             be.st.surface.color_format == 5 ||
             be.st.surface.color_format == 8) && be.st.surface.clip_w) {
            rt_space = be.st.surface.color_location[0];
            rt_offset = be.st.surface.color_offset[0];
            rt_w = be.st.surface.clip_w;
            rt_h = be.st.surface.clip_h;
        }
    }
    rsx_nir_adapter_finish(ad);
    while (rsx_nr_backend_step(&be) == RSX_NR_STEP_EXECUTED)
        ;

    rsx_nr_d3d12_stats st;
    rsx_nr_d3d12_get_stats(sink, &st);
    snprintf(stats_line, stats_size,
             "clears=%llu draws=%llu (restart=%llu) batches=%llu "
             "presents=%llu xfers=%llu pso=%llu(+%lluh) unsup_draw=%llu "
             "[topo=%llu rt=%llu plan=%llu pso=%llu idx=%llu] "
             "unsup_clear=%llu unsup_xfer=%llu compile_fail=%llu "
             "exec_err=%llu",
             st.clears, st.draws, st.restart_draws, st.draw_batches,
             st.presents, st.transfers, st.pso_builds, st.pso_hits,
             st.unsupported_draws, st.unsup_draw_topology, st.unsup_draw_rt,
             st.unsup_draw_plan, st.unsup_draw_pso, st.unsup_draw_index,
             st.unsupported_clears, st.unsupported_transfers,
             st.compile_failures, be.stats.exec_errors);

    *rt_hash = 0;
    if (rt_w && rt_h) {
        u8* px = malloc((size_t)rt_w * rt_h * 4);
        if (px && rsx_nr_d3d12_read_rt(sink, rt_space, rt_offset, rt_w, rt_h,
                                       px) == 0)
            *rt_hash = fnv64(px, (size_t)rt_w * rt_h * 4);
        free(px);
    }

    free(ad);
    rsx_nr_ring_destroy(&ring);
    rsx_nr_d3d12_destroy(sink);
    free(g_cap.arena[0]);
    free(g_cap.arena[1]);
    memset(&g_cap, 0, sizeof(g_cap));
    return ring_fault ? -1 : 0;
}

static void run_capture_backend(const char* path)
{
    cap_data c;
    if (cap_load(path, &c)) {
        CHECK(0, "capture %s failed to load", path);
        return;
    }
    char line1[512], line2[512];
    u64 h1 = 0, h2 = 0;
    int r1 = cap_run_once(&c, &h1, line1, sizeof(line1));
    if (r1 == -2) {
        printf("capture backend leg: SKIP (no WARP device)\n");
        cap_free(&c);
        return;
    }
    CHECK(r1 == 0, "capture backend run 1 faulted");
    int r2 = cap_run_once(&c, &h2, line2, sizeof(line2));
    CHECK(r2 == 0, "capture backend run 2 faulted");
    CHECK(strcmp(line1, line2) == 0, "capture stats nondeterministic:\n  %s\n  %s",
          line1, line2);
    CHECK(h1 == h2, "capture RT hash nondeterministic %016llX/%016llX",
          (unsigned long long)h1, (unsigned long long)h2);
    printf("capture backend %s:\n  %s\n  rt_hash=%016llX\n", path, line1,
           (unsigned long long)h1);
    cap_free(&c);
}

int main(int argc, char** argv)
{
    write_test_fp();
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

    /* ---- real FP constants + structural PSO reuse ---------------------
     * Two byte-distinct inline-CONST payloads must share one structural PSO
     * while b1 changes the rendered result. */
    write_const_fp(0.0f, 1.0f, 0.0f, 1.0f);
    stage_frame_state(&em);
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0, "green constant FP exec errors %llu",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "RT readback failed");
    CHECK(pix_is(2, 61, 0x00, 0xFF, 0x00), "green constant FP pixel");
    rsx_nr_d3d12_stats before_constant_change;
    rsx_nr_d3d12_get_stats(sink, &before_constant_change);

    write_const_fp(1.0f, 0.0f, 0.0f, 1.0f);
    stage_frame_state(&em);
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0, "red constant FP exec errors %llu",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "RT readback failed");
    CHECK(pix_is(2, 61, 0x00, 0x00, 0xFF), "red constant FP pixel");
    rsx_nr_d3d12_stats after_constant_change;
    rsx_nr_d3d12_get_stats(sink, &after_constant_change);
    CHECK(after_constant_change.pso_builds ==
              before_constant_change.pso_builds &&
          after_constant_change.pso_hits > before_constant_change.pso_hits,
          "inline constant rebuilt PSO builds=%llu/%llu hits=%llu/%llu",
          before_constant_change.pso_builds, after_constant_change.pso_builds,
          before_constant_change.pso_hits, after_constant_change.pso_hits);

    /* ---- scissored clear ------------------------------------------------
     * CLEAR_SURFACE clears only within the scissor box
     * (cellGcmSetClearSurface). Blue full clear, then a green clear
     * confined to the top-left 32x32 scissor: inside green, outside
     * stays blue. */
    stage_frame_state(&em);
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);   /* blue     */
    rsx_nir_scissor scz;
    memset(&scz, 0, sizeof(scz));
    scz.w = 32;
    scz.h = 32;
    rsx_nir_em_scissor(&em, &scz);
    rsx_nir_em_clear(&em, 0xF3, 0xFF00FF00u, 0xFFFFFF, 0);   /* green    */
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0, "scissor clear leg exec errors %llu",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "RT readback failed");
    CHECK(pix_is(2, 2, 0x00, 0xFF, 0x00), "inside-scissor pixel %02X %02X %02X",
          pix(2, 2)[0], pix(2, 2)[1], pix(2, 2)[2]);
    CHECK(pix_is(40, 40, 0xFF, 0x00, 0x00), "outside-scissor pixel "
          "%02X %02X %02X", pix(40, 40)[0], pix(40, 40)[1], pix(40, 40)[2]);
    scz.w = RT_W;                    /* restore full scissor for later legs */
    scz.h = RT_H;
    rsx_nir_em_scissor(&em, &scz);

    /* ---- partial-channel clear refused (counted, never approximated) ---
     * CELL_GCM_CLEAR_R/G/B/A are individually maskable on hardware
     * (distinct CELL_GCM_CLEAR_* mask bits); D3D12 clears whole
     * targets, so a partial color mask must be refused to the core. */
    stage_frame_state(&em);
    rsx_nir_em_clear(&em, 0x13, 0xFF000000u, 0, 0);   /* Z+S+R only      */
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 1, "partial clear not surfaced (%llu)",
          be.stats.exec_errors);

    /* ---- sink accounting ----------------------------------------------- */
    rsx_nr_d3d12_stats st;
    rsx_nr_d3d12_get_stats(sink, &st);
    CHECK(st.unsupported_clears == 1, "partial clear not counted (%llu)",
          st.unsupported_clears);
    CHECK(st.clears == 7 && st.draws == 5 && st.presents == 7,
          "sink counts clears=%llu draws=%llu presents=%llu", st.clears,
          st.draws, st.presents);
    CHECK(st.unsupported_draws == 0 && st.compile_failures == 0,
          "unsupported=%llu compile_failures=%llu", st.unsupported_draws,
          st.compile_failures);
    CHECK(st.pso_builds >= 1 && st.pso_hits >= 1,
          "pso cache builds=%llu hits=%llu", st.pso_builds, st.pso_hits);
    CHECK(st.real_fp_draws == st.draws,
          "real fragment programs=%llu draws=%llu", st.real_fp_draws,
          st.draws);

    rsx_nr_ring_destroy(&ring);
    rsx_nr_d3d12_destroy(sink);

    /* optional real-capture execution leg (large local untracked oracle:
     * absent capture = SKIP so CTest stays hermetic) */
    const char* rxs = argc > 1 ? argv[1] : getenv("YZ_NIR_RXS");
    if (rxs && rxs[0])
        run_capture_backend(rxs);
    else
        printf("capture backend leg: SKIP (no .rxs via argv[1]/YZ_NIR_RXS)\n");

    if (g_failures) {
        fprintf(stderr, "rsx_nr_backend_d3d12: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("rsx_nr_backend_d3d12: PASS\n");
    return 0;
}

#endif /* _WIN32 */
