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
#include "../rsx_vp_decompiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

static int g_failures;
static u32 g_present_handoffs;
static u32 g_watched_pages[2];
static u32 g_last_watched_offset[2];
static u32 g_published_writes;
static u32 g_published_space[4], g_published_offset[4], g_published_size[4];
static u32 g_render_condition_value;
static int g_render_condition_fail;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n");                                           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define LOCAL_SIZE (4u << 20) /* includes the live-style 0x00320000 zeta */
#define MAIN_SIZE  (1u << 16)
#define RT_OFFSET  0x00300000u   /* outside the arena on purpose: the RT  */
#define RT565_OFFSET 0x00310000u
#define ZETA_OFFSET  0x00320000u
#define RT_W 64u                 /* is a GPU object keyed by (space,ofs)  */
#define RT_H 64u
#define VTX_OFFSET 0x2000u
#define IDX_OFFSET 0x4000u
#define FP_OFFSET  0x6000u
#define TEX_OFFSET 0x8000u
#define VTEX_OFFSET 0xA000u

static int test_present_handoff(void* user, void* texture, u32 format,
                                u32 width, u32 height, u32 buffer_id)
{
    (void)user;
    if (!texture || !format || width != RT_W || height != RT_H || buffer_id)
        return -1;
    g_present_handoffs++;
    return 0;
}

static int test_watch_page(void* user, u32 space, u32 page_offset)
{
    (void)user;
    if (space >= 2u || (page_offset & (RSX_GUEST_PAGE_SIZE - 1u)))
        return -1;
    g_watched_pages[space]++;
    g_last_watched_offset[space] = page_offset;
    return 0;
}

static void test_publish_write(void* user, u32 space, u32 offset, u32 size)
{
    if (g_published_writes < 4u) {
        g_published_space[g_published_writes] = space;
        g_published_offset[g_published_writes] = offset;
        g_published_size[g_published_writes] = size;
    }
    g_published_writes++;
    rsx_nr_d3d12_note_guest_write((rsx_nr_d3d12*)user, space, offset, size);
}

static int test_render_condition_read(void* user, u32 dma, u32 offset,
                                      u32* value)
{
    (void)user;
    if (g_render_condition_fail || !value || dma != 0x66626660u ||
        offset != 0x45A0u)
        return -1;
    *value = g_render_condition_value;
    return 0;
}

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

static void write_tex_fp(void)
{
    /* TEX r0, TC0, unit 0; END.  The passthrough test VS emits an
     * out-of-range TC0=(-1,-1), while ordinary texture tests use CLAMP and
     * solid data.  This also lets the depth-border leg exercise the real
     * D3D12 sampler instead of only inspecting a decoded descriptor. */
    u8* p = g_local + FP_OFFSET;
    put_fp_word(p + 0, (0x17u << 24) | (0xFu << 9) |
                         (0x4u << 13) | 1u);
    put_fp_word(p + 4, 1u | ((0u << 9) | (1u << 11) |
                             (2u << 13) | (3u << 15)) |
                              (7u << 18));
    put_fp_word(p + 8, 0);
    put_fp_word(p + 12, 0);
}

static void write_solid_texture(rsx_guest_pages* pages,
                                u8 r, u8 g, u8 b, u8 a)
{
    u8* p = g_local + TEX_OFFSET;
    for (u32 i = 0; i < 4; i++) {
        p[i * 4 + 0] = a;            /* RSX A8R8G8B8 guest order          */
        p[i * 4 + 1] = r;
        p[i * 4 + 2] = g;
        p[i * 4 + 3] = b;
    }
    rsx_guest_pages_note_write(pages, 0, TEX_OFFSET, 16);
}

static void stage_texture0(rsx_nir_emitter* em)
{
    rsx_nir_texture texture;
    memset(&texture, 0, sizeof(texture));
    texture.enabled = 1;
    texture.offset = TEX_OFFSET;
    texture.location = RSX_NIR_LOCATION_LOCAL;
    texture.format = 0xA5u;          /* LINEAR | A8R8G8B8                */
    texture.dimension = 2;
    texture.mipmaps = 1;
    texture.width = 2;
    texture.height = 2;
    texture.pitch = 8;
    texture.wrap = 0x00030303u;
    texture.remap = 0xAAE4u;
    texture.filter = (1u << 16) | (1u << 24);
    rsx_nir_em_texture(em, 0, &texture);
}

static void write_vertex_texture0(rsx_guest_pages* pages,
                                  float x, float y, float z, float w)
{
    const float value[4] = {x, y, z, w};
    for (u32 component = 0; component < 4u; ++component)
        put_be32(g_local + VTEX_OFFSET + component * 4u,
                 fbits(value[component]));
    rsx_guest_pages_note_write(pages, 0, VTEX_OFFSET, 16u);
}

static rsx_nir_texture vertex_texture0_descriptor(void)
{
    rsx_nir_texture texture;
    memset(&texture, 0, sizeof(texture));
    texture.enabled = 1u;
    texture.offset = VTEX_OFFSET;
    texture.location = RSX_NIR_LOCATION_LOCAL;
    texture.format = 0xBBu;          /* LINEAR | W32Z32Y32X32_FLOAT       */
    texture.dimension = 2u;
    texture.mipmaps = 1u;
    texture.width = 1u;
    texture.height = 1u;
    texture.pitch = 16u;
    return texture;
}

static void stage_vertex_texture0(rsx_nir_emitter* em)
{
    const rsx_nir_texture texture = vertex_texture0_descriptor();
    rsx_nir_em_vertex_texture(em, 0, &texture);
}

static void stage_rt565_texture0(rsx_nir_emitter* em)
{
    rsx_nir_texture texture;
    memset(&texture, 0, sizeof(texture));
    texture.enabled = 1;
    texture.offset = RT565_OFFSET;
    texture.location = RSX_NIR_LOCATION_LOCAL;
    texture.format = 0xA4u;          /* LINEAR | R5G6B5                   */
    texture.dimension = 2;
    texture.mipmaps = 1;
    texture.width = RT_W;
    texture.height = RT_H;
    texture.pitch = RT_W * 2u;
    texture.wrap = 0x00030303u;
    texture.remap = 0xAAE4u;
    texture.filter = (1u << 16) | (1u << 24);
    rsx_nir_em_texture(em, 0, &texture);
}

static void stage_depth_texture0(rsx_nir_emitter* em)
{
    rsx_nir_texture texture;
    memset(&texture, 0, sizeof(texture));
    texture.enabled = 1;
    texture.offset = ZETA_OFFSET;
    texture.location = RSX_NIR_LOCATION_LOCAL;
    texture.format = 0xB0u;          /* LINEAR | DEPTH24_D8               */
    texture.dimension = 2;
    texture.mipmaps = 1;
    texture.width = RT_W;
    texture.height = RT_H;
    texture.pitch = RT_W * 4u;
    texture.wrap = 0x00030303u;
    texture.remap = 0xAAE4u;
    texture.filter = (1u << 16) | (1u << 24);
    rsx_nir_em_texture(em, 0, &texture);
}

static void stage_depth_border_texture0(rsx_nir_emitter* em)
{
    rsx_nir_texture texture;
    memset(&texture, 0, sizeof(texture));
    texture.enabled = 1;
    texture.offset = ZETA_OFFSET;
    texture.location = RSX_NIR_LOCATION_LOCAL;
    texture.format = 0xB0u;          /* LINEAR | DEPTH24_D8               */
    texture.dimension = 2;
    texture.mipmaps = 1;
    texture.width = RT_W;
    texture.height = RT_H;
    texture.pitch = RT_W * 4u;
    texture.wrap = 0x00040404u;      /* BORDER on every coordinate        */
    texture.border_color = 0xFFFFFFFFu;
    texture.remap = 0xAAE4u;
    texture.filter = (1u << 16) | (1u << 24);
    rsx_nir_em_texture(em, 0, &texture);
}

static u32 fbits(float f)
{
    u32 v;
    memcpy(&v, &f, 4);
    return v;
}

/* three float4 clip-space vertices, big-endian, at VTX_OFFSET */
static void write_triangle_z(rsx_guest_pages* pages, float x0, float y0,
                             float x1, float y1, float x2, float y2,
                             float z)
{
    u8* p = g_local + VTX_OFFSET;
    const float v[12] = { x0, y0, z, 1.0f,
                          x1, y1, z, 1.0f,
                          x2, y2, z, 1.0f };
    for (int i = 0; i < 12; i++)
        put_be32(p + i * 4, fbits(v[i]));
    rsx_guest_pages_note_write(pages, 0, VTX_OFFSET, 48);
}

static void write_triangle(rsx_guest_pages* pages, float x0, float y0,
                           float x1, float y1, float x2, float y2)
{
    write_triangle_z(pages, x0, y0, x1, y1, x2, y2, 0.5f);
}

static void write_quad(rsx_guest_pages* pages)
{
    u8* p = g_local + VTX_OFFSET;
    const float v[16] = {
        -1.0f, -1.0f, 0.5f, 1.0f,
         1.0f, -1.0f, 0.5f, 1.0f,
         1.0f,  1.0f, 0.5f, 1.0f,
        -1.0f,  1.0f, 0.5f, 1.0f
    };
    for (int i = 0; i < 16; i++)
        put_be32(p + i * 4, fbits(v[i]));
    rsx_guest_pages_note_write(pages, 0, VTX_OFFSET, 64);
}

static void stage_vertex_bindings(rsx_nir_emitter* em, u32 base_index)
{
    rsx_nir_vertex_bindings vb;
    memset(&vb, 0, sizeof(vb));
    for (u32 i = 0; i < RSX_NIR_NUM_VERTEX_ATTR; i++)
        vb.attr[i].def[3] = 1.0f;
    vb.attr[0].type = 2;             /* FLOAT                              */
    vb.attr[0].size = 4;
    vb.attr[0].stride = 16;
    vb.attr[0].offset = VTX_OFFSET;
    vb.attr[0].location = RSX_NIR_LOCATION_LOCAL;
    vb.base_index = base_index;
    rsx_nir_em_vertex_bindings(em, &vb);
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
    s.zeta_offset = ZETA_OFFSET;
    s.zeta_pitch = RT_W * 4u;
    s.zeta_location = RSX_NIR_LOCATION_LOCAL;
    rsx_nir_em_surface(em, &s);

    rsx_nir_viewport v;
    memset(&v, 0, sizeof(v));
    /* The VP epilogue owns guest viewport mapping. Keep a deliberately
     * narrowed raster declaration so the pixel oracle catches any backend
     * that applies x/y/w/h a second time as a D3D viewport. */
    v.x = 7;
    v.y = 5;
    v.w = RT_W - 14;
    v.h = RT_H - 10;
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

    stage_vertex_bindings(em, 0u);
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

static int cap_dump_rt_ppm(rsx_nr_d3d12* sink, const char* dir,
                           u32 offset, const char* name)
{
    const u32 width = 1024u, height = 768u;
    u8* pixels = malloc((size_t)width * height * 4u);
    if (!pixels)
        return -1;
    if (rsx_nr_d3d12_read_rt(
            sink, 0u, offset, width, height, pixels) != 0) {
        free(pixels);
        return -1;
    }
    char path[1024];
    const int length = snprintf(path, sizeof(path), "%s\\%s", dir, name);
    if (length <= 0 || length >= (int)sizeof(path)) {
        free(pixels);
        return -1;
    }
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        free(pixels);
        return -1;
    }
    int ok = fprintf(fp, "P6\n%u %u\n255\n", width, height) > 0;
    for (u32 y = 0; ok && y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u8* bgra = pixels + ((size_t)y * width + x) * 4u;
            const u8 rgb[3] = { bgra[2], bgra[1], bgra[0] };
            if (fwrite(rgb, 1, sizeof(rgb), fp) != sizeof(rgb)) {
                ok = 0;
                break;
            }
        }
    }
    if (fclose(fp) != 0)
        ok = 0;
    free(pixels);
    return ok ? 0 : -1;
}

static int cap_run_once(cap_data* c, u64* rt_hash, char* stats_line,
                        size_t stats_size, int dump_outputs)
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
    const char* allow_flow_txl = getenv("YZ_NR_CAPTURE_FLOW_TXL_ORACLE");
    if (allow_flow_txl && allow_flow_txl[0] &&
        strcmp(allow_flow_txl, "0") != 0)
        CHECK(rsx_nr_d3d12_set_coherent_section_mode(sink, 1) == 0,
              "capture coherent-section mode refused before execution");

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
             "[topo=%llu rt=%llu plan=%llu pso=%llu idx=%llu fp=%llu "
             "tex=%llu] real_fp=%llu tex_draw=%llu "
             "tex_cache=%llu(+%lluh,%llur) tex_fail=%llu alias=%llu "
             "unsup_clear=%llu unsup_xfer=%llu compile_fail=%llu "
             "exec_err=%llu topo_id=[3:%llu 7:%llu 8:%llu 9:%llu 10:%llu] "
             "rtfmt=[1:%llu 2:%llu 3:%llu 6:%llu 7:%llu 9:%llu 10:%llu "
             "11:%llu 12:%llu 13:%llu 14:%llu 15:%llu 16:%llu]",
             st.clears, st.draws, st.restart_draws, st.draw_batches,
             st.presents, st.transfers, st.pso_builds, st.pso_hits,
             st.unsupported_draws, st.unsup_draw_topology, st.unsup_draw_rt,
             st.unsup_draw_plan, st.unsup_draw_pso, st.unsup_draw_index,
             st.unsup_draw_fp, st.unsup_draw_texture, st.real_fp_draws,
             st.texture_draws, st.texture_builds, st.texture_hits,
             st.texture_refreshes, st.texture_failures, st.rt_alias_binds,
             st.unsupported_clears, st.unsupported_transfers,
             st.compile_failures, be.stats.exec_errors,
             st.unsup_topology_id[3], st.unsup_topology_id[7],
             st.unsup_topology_id[8], st.unsup_topology_id[9],
             st.unsup_topology_id[10], st.unsup_rt_format[1],
             st.unsup_rt_format[2], st.unsup_rt_format[3],
             st.unsup_rt_format[6], st.unsup_rt_format[7],
             st.unsup_rt_format[9], st.unsup_rt_format[10],
             st.unsup_rt_format[11], st.unsup_rt_format[12],
             st.unsup_rt_format[13], st.unsup_rt_format[14],
             st.unsup_rt_format[15], st.unsup_rt_format[16]);

    *rt_hash = 0;
    if (rt_w && rt_h) {
        u8* px = malloc((size_t)rt_w * rt_h * 4);
        if (px && rsx_nr_d3d12_read_rt(sink, rt_space, rt_offset, rt_w, rt_h,
                                       px) == 0)
            *rt_hash = fnv64(px, (size_t)rt_w * rt_h * 4);
        free(px);
    }

    if (dump_outputs) {
        const char* dump_dir = getenv("YZ_NR_CAPTURE_DUMP_DIR");
        if (dump_dir && dump_dir[0] &&
            (cap_dump_rt_ppm(sink, dump_dir, 0x01800000u,
                             "native_world_01800000.ppm") != 0 ||
             cap_dump_rt_ppm(sink, dump_dir, 0x00E40000u,
                             "native_final_00E40000.ppm") != 0)) {
            fprintf(stderr, "capture: failed to dump oracle render targets\n");
            ring_fault = 1;
        }
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
    int r1 = cap_run_once(&c, &h1, line1, sizeof(line1), 1);
    if (r1 == -2) {
        printf("capture backend leg: SKIP (no WARP device)\n");
        cap_free(&c);
        return;
    }
    CHECK(r1 == 0, "capture backend run 1 faulted");
    int r2 = cap_run_once(&c, &h2, line2, sizeof(line2), 0);
    CHECK(r2 == 0, "capture backend run 2 faulted");
    CHECK(strcmp(line1, line2) == 0, "capture stats nondeterministic:\n  %s\n  %s",
          line1, line2);
    CHECK(h1 == h2, "capture RT hash nondeterministic %016llX/%016llX",
          (unsigned long long)h1, (unsigned long long)h2);
    printf("capture backend %s:\n  %s\n  rt_hash=%016llX\n", path, line1,
           (unsigned long long)h1);
    cap_free(&c);
}

typedef struct broker_color_test {
    ID3D12Resource* resource;
    ID3D12Resource* depth;
    u32 calls;
    u32 depth_calls;
    u32 depth_resolve_calls;
    int depth_resolve_fail;
} broker_color_test;

static int borrow_rgba_for_logical_565(
    void* user, u32 space, u32 offset, u32 width, u32 height,
    void** resource, u32* dxgi_format)
{
    broker_color_test* broker = (broker_color_test*)user;
    if (!broker || !broker->resource || !resource || !dxgi_format || space ||
        offset != RT565_OFFSET || width != RT_W || height != RT_H)
        return -1;
    broker->resource->lpVtbl->AddRef(broker->resource);
    *resource = broker->resource;
    *dxgi_format = (u32)DXGI_FORMAT_R8G8B8A8_UNORM;
    broker->calls++;
    return 0;
}

static int borrow_depth_for_alias_test(
    void* user, u32 space, u32 offset, u32 depth_format,
    u32 width, u32 height, void** resource, u32* resource_format,
    u32* dsv_format, u32* srv_format, void** sample_resource,
    u32* sample_srv_format, int* publication_required)
{
    broker_color_test* broker = (broker_color_test*)user;
    if (!broker || !broker->depth || !resource || !resource_format ||
        !dsv_format || !srv_format || !sample_resource ||
        !sample_srv_format || !publication_required || space ||
        offset != ZETA_OFFSET || depth_format != 2u ||
        width != RT_W || height != RT_H)
        return -1;
    broker->depth->lpVtbl->AddRef(broker->depth);
    broker->depth->lpVtbl->AddRef(broker->depth);
    *resource = broker->depth;
    *sample_resource = broker->depth;
    *resource_format = (u32)DXGI_FORMAT_R32G8X24_TYPELESS;
    *dsv_format = (u32)DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    *srv_format = (u32)DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    *sample_srv_format = (u32)DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    *publication_required = 0;
    broker->depth_calls++;
    return 0;
}

static int resolve_depth_for_alias_test(
    void* user, u32 space, u32 offset, u32 width, u32 height)
{
    broker_color_test* broker = (broker_color_test*)user;
    if (!broker || space || offset != ZETA_OFFSET ||
        width != RT_W || height != RT_H)
        return -1;
    broker->depth_resolve_calls++;
    return broker->depth_resolve_fail ? -1 : 0;
}

/* The live surface broker canonicalizes logical Cell GCM color format 3 to
 * an RGBA8 D3D resource.  Exercise that exact mismatch through a real WARP
 * draw: resource identity remains keyed by the guest format, while the RTV
 * and PSO must use the broker resource's actual format. */
static void test_broker_actual_color_format(void)
{
    IDXGIFactory4* factory = NULL;
    IDXGIAdapter* adapter = NULL;
    ID3D12Device* device = NULL;
    ID3D12Resource* resource = NULL;
    ID3D12Resource* depth_resource = NULL;
    rsx_nr_d3d12* sink = NULL;
    rsx_nr_ring ring;
    memset(&ring, 0, sizeof(ring));
    int ring_live = 0;

    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory);
    if (SUCCEEDED(hr))
        hr = factory->lpVtbl->EnumWarpAdapter(
            factory, &IID_IDXGIAdapter, (void**)&adapter);
    if (SUCCEEDED(hr))
        hr = D3D12CreateDevice((IUnknown*)adapter, D3D_FEATURE_LEVEL_11_0,
                               &IID_ID3D12Device, (void**)&device);
    CHECK(SUCCEEDED(hr) && device,
          "could not create explicit WARP device for broker-format test");
    if (FAILED(hr) || !device)
        goto done;

    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = RT_W;
    rd.Height = RT_H;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    hr = device->lpVtbl->CreateCommittedResource(
        device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource,
        (void**)&resource);
    CHECK(SUCCEEDED(hr) && resource,
          "could not create broker RGBA8 render target");
    if (FAILED(hr) || !resource)
        goto done;

    rd.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depth_clear = {0};
    depth_clear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    depth_clear.DepthStencil.Depth = 1.0f;
    hr = device->lpVtbl->CreateCommittedResource(
        device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear,
        &IID_ID3D12Resource, (void**)&depth_resource);
    CHECK(SUCCEEDED(hr) && depth_resource,
          "could not create broker depth resource");
    if (FAILED(hr) || !depth_resource)
        goto done;

    sink = rsx_nr_d3d12_create(
        device, LOCAL_SIZE, MAIN_SIZE, arena_ptr, arena_wptr, NULL);
    CHECK(sink != NULL, "could not create broker-format native sink");
    if (!sink)
        goto done;

    broker_color_test broker = {
        resource, depth_resource, 0u, 0u, 0u, 0
    };
    rsx_nr_d3d12_set_resource_broker(
        sink, borrow_rgba_for_logical_565, borrow_depth_for_alias_test,
        resolve_depth_for_alias_test, &broker);
    const int ring_result = rsx_nr_ring_init(&ring, 128u, 4096u);
    CHECK(ring_result == 0,
          "broker-format ring init failed");
    if (ring_result != 0)
        goto done;
    ring_live = 1;

    rsx_nr_tokens tokens;
    rsx_nr_tokens_init(&tokens);
    rsx_nr_exec_ops ops;
    memset(&ops, 0, sizeof(ops));
    rsx_nr_d3d12_get_exec_ops(sink, &ops);
    rsx_nr_backend be;
    rsx_nr_backend_init(&be, &ring, &tokens, &ops);
    rsx_nir_sink k = rsx_nr_ring_sink(&ring);
    rsx_nir_emitter em;
    rsx_nir_emitter_init(&em, &k);

    write_test_fp();
    stage_frame_state(&em);
    rsx_nir_surface s565;
    memset(&s565, 0, sizeof(s565));
    s565.color_format = 3;
    s565.depth_format = 2;
    s565.raster_type = 1;
    s565.clip_w = RT_W;
    s565.clip_h = RT_H;
    s565.color_offset[0] = RT565_OFFSET;
    s565.color_pitch[0] = RT_W * 2u;
    s565.color_location[0] = RSX_NIR_LOCATION_LOCAL;
    s565.color_target = 1;
    s565.zeta_offset = ZETA_OFFSET;
    s565.zeta_pitch = RT_W * 4u;
    s565.zeta_location = RSX_NIR_LOCATION_LOCAL;
    rsx_nir_em_surface(&em, &s565);
    write_triangle(rsx_nr_d3d12_pages(sink),
                   -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f);
    rsx_nir_em_clear(&em, 0xF3u, 0xFF0000FFu, 0xFFFFFFu, 0u);
    {
        const u32 batch[2] = {0u, 3u};
        rsx_nir_em_draw(&em, 5u, 0u, batch, 1u);
    }
    rsx_nr_backend_run(&be, 0u);
    CHECK(be.stats.exec_errors == 0u,
          "logical-565/actual-RGBA draw had %llu execution errors",
          be.stats.exec_errors);
    CHECK(broker.calls >= 2u,
          "logical-565/actual-RGBA broker was not exercised (%u calls)",
          broker.calls);
    CHECK(rsx_nr_d3d12_read_rt(
              sink, 0u, RT565_OFFSET, RT_W, RT_H, g_pix) == 0,
          "broker RGBA target readback failed");
    CHECK(pix_is(2u, 61u, 0xFFu, 0x00u, 0xFFu),
          "broker RGBA target did not execute the logical-565 draw");

    /* The live renderer keeps depth maps as shared typeless D3D resources.
     * A later color pass may sample a non-current one directly on the shared
     * ordered list. This must not fall back to stale guest bytes merely
     * because the resource came through the broker. */
    rsx_nir_em_surface(&em, &s565);
    rsx_nir_em_clear(&em, 0x01u, 0u, 0x400000u, 0u);
    rsx_nr_backend_run(&be, 0u);
    CHECK(be.stats.exec_errors == 0u && broker.depth_calls >= 1u,
          "borrowed zeta clear failed errors=%llu calls=%u",
          be.stats.exec_errors, broker.depth_calls);

    write_tex_fp();
    stage_frame_state(&em);
    rsx_nir_em_surface(&em, &s565);
    stage_depth_texture0(&em);
    rsx_nir_em_clear(&em, 0xF0u, 0xFF0000FFu, 0u, 0u);
    write_triangle(rsx_nr_d3d12_pages(sink),
                   -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f);
    {
        const u32 batch[2] = {0u, 3u};
        rsx_nir_em_draw(&em, 5u, 0u, batch, 1u);
    }
    rsx_nr_backend_run(&be, 0u);
    CHECK(be.stats.exec_errors == 0u,
          "borrowed zeta sample had %llu execution errors",
          be.stats.exec_errors);
    CHECK(broker.depth_resolve_calls >= 1u,
          "borrowed zeta bypassed the resolved-sample contract");
    CHECK(rsx_nr_d3d12_read_rt(
              sink, 0u, RT565_OFFSET, RT_W, RT_H, g_pix) == 0,
          "borrowed zeta target readback failed");
    CHECK(pix_is(2u, 61u, 0x40u, 0x40u, 0x40u),
          "borrowed zeta sample pixel %02X %02X %02X",
          pix(2u, 61u)[0], pix(2u, 61u)[1], pix(2u, 61u)[2]);

    /* A clear-only/no-write live zeta has no resolved snapshot. Preflight
     * already proved guest depth decoding, so resolver refusal must stay a
     * successful native draw and bind that deterministic legacy fallback. */
    for (u32 y = 0; y < RT_H; ++y) {
        u8* const row = g_local + ZETA_OFFSET + y * RT_W * 4u;
        for (u32 x = 0; x < RT_W; ++x) {
            row[x * 4u + 0u] = 0xBFu;
            row[x * 4u + 1u] = 0xFFu;
            row[x * 4u + 2u] = 0xFFu;
            row[x * 4u + 3u] = 0u;
        }
    }
    rsx_guest_pages_note_write(
        rsx_nr_d3d12_pages(sink), 0u, ZETA_OFFSET,
        RT_W * RT_H * 4u);
    broker.depth_resolve_fail = 1;
    write_tex_fp();
    stage_frame_state(&em);
    rsx_nir_em_surface(&em, &s565);
    stage_depth_texture0(&em);
    rsx_nir_em_clear(&em, 0xF0u, 0xFF0000FFu, 0u, 0u);
    write_triangle(rsx_nr_d3d12_pages(sink),
                   -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f);
    {
        const u32 batch[2] = {0u, 3u};
        rsx_nir_em_draw(&em, 5u, 0u, batch, 1u);
    }
    rsx_nr_backend_run(&be, 0u);
    CHECK(be.stats.exec_errors == 0u,
          "borrowed zeta guest fallback had %llu execution errors",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(
              sink, 0u, RT565_OFFSET, RT_W, RT_H, g_pix) == 0,
          "borrowed zeta fallback target readback failed");
    CHECK(pix(2u, 61u)[0] >= 0xBEu && pix(2u, 61u)[0] <= 0xC0u &&
              pix(2u, 61u)[1] == pix(2u, 61u)[0] &&
              pix(2u, 61u)[2] == pix(2u, 61u)[0],
          "borrowed zeta fallback pixel %02X %02X %02X",
          pix(2u, 61u)[0], pix(2u, 61u)[1], pix(2u, 61u)[2]);
    {
        const u32 native_vp[4] = {
            0x401F9C00u, 0x0040000Du, 0x81000000u, 0x0001FF81u
        };
        const u32 batch[2] = {0u, 3u};
        const rsx_nir_draw draw = {5u, 0u, 1u, 0u, 3u};
        rsx_nir_pipeline self_alias = be.st;
        self_alias.depth_stencil.depth_test_enable = 1u;
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &self_alias, native_vp, 4u, &draw, batch) ==
                  -RSX_NR_DRAW_PF_TEXTURE_INVALID,
              "active borrowed DSV/SRV self-alias escaped preflight");
    }
    write_test_fp();

done:
    if (ring_live)
        rsx_nr_ring_destroy(&ring);
    if (sink)
        rsx_nr_d3d12_destroy(sink);
    if (resource)
        resource->lpVtbl->Release(resource);
    if (depth_resource)
        depth_resource->lpVtbl->Release(depth_resource);
    if (device)
        device->lpVtbl->Release(device);
    if (adapter)
        adapter->lpVtbl->Release(adapter);
    if (factory)
        factory->lpVtbl->Release(factory);
}

typedef struct shared_timeline_test {
    ID3D12CommandQueue* queue;
    ID3D12CommandAllocator* alloc;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence* fence;
    HANDLE event;
    u64 fence_value;
    u64 generation;
    u32 acquires;
    u32 releases;
    u32 flushes;
    int leased;
    int fail_acquire_once;
} shared_timeline_test;

static int shared_test_acquire(
    void* user, void** command_list, unsigned long long* generation,
    unsigned long long* recording_fence,
    unsigned long long* completed_fence)
{
    shared_timeline_test* host = (shared_timeline_test*)user;
    if (!host || host->leased || !command_list || !generation ||
        !recording_fence || !completed_fence)
        return -1;
    if (host->fail_acquire_once) {
        host->fail_acquire_once = 0;
        return -1;
    }
    host->leased = 1;
    host->acquires++;
    *command_list = host->list;
    *generation = host->generation;
    *recording_fence = host->fence_value + 1u;
    *completed_fence =
        host->fence->lpVtbl->GetCompletedValue(host->fence);
    return 0;
}

static void shared_test_release(void* user)
{
    shared_timeline_test* host = (shared_timeline_test*)user;
    if (!host || !host->leased) {
        g_failures++;
        return;
    }
    host->leased = 0;
    host->releases++;
}

static int shared_test_flush(void* user)
{
    shared_timeline_test* host = (shared_timeline_test*)user;
    if (!host || host->leased ||
        FAILED(host->list->lpVtbl->Close(host->list)))
        return -1;
    ID3D12CommandList* lists[1] = {(ID3D12CommandList*)host->list};
    host->queue->lpVtbl->ExecuteCommandLists(host->queue, 1, lists);
    const u64 value = ++host->fence_value;
    if (FAILED(host->queue->lpVtbl->Signal(
            host->queue, host->fence, value)))
        return -1;
    if (host->fence->lpVtbl->GetCompletedValue(host->fence) < value) {
        if (FAILED(host->fence->lpVtbl->SetEventOnCompletion(
                host->fence, value, host->event)) ||
            WaitForSingleObject(host->event, 10000u) != WAIT_OBJECT_0)
            return -1;
    }
    if (FAILED(host->alloc->lpVtbl->Reset(host->alloc)) ||
        FAILED(host->list->lpVtbl->Reset(
            host->list, host->alloc, NULL)))
        return -1;
    host->generation++;
    host->flushes++;
    return 0;
}

/* Two independently admitted native sections may append to one established
 * host command-list generation with no native queue submission between them.
 * A legacy boundary then advances the generation; the next native section
 * must observe the retired fence before reusing uploads/descriptors. */
static void test_shared_timeline(void)
{
    IDXGIFactory4* factory = NULL;
    IDXGIAdapter* adapter = NULL;
    ID3D12Device* device = NULL;
    shared_timeline_test host;
    memset(&host, 0, sizeof(host));
    rsx_nr_d3d12* sink = NULL;

    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory);
    if (SUCCEEDED(hr))
        hr = factory->lpVtbl->EnumWarpAdapter(
            factory, &IID_IDXGIAdapter, (void**)&adapter);
    if (SUCCEEDED(hr))
        hr = D3D12CreateDevice((IUnknown*)adapter, D3D_FEATURE_LEVEL_11_0,
                               &IID_ID3D12Device, (void**)&device);
    D3D12_COMMAND_QUEUE_DESC qd = {0};
    if (SUCCEEDED(hr))
        hr = device->lpVtbl->CreateCommandQueue(
            device, &qd, &IID_ID3D12CommandQueue, (void**)&host.queue);
    if (SUCCEEDED(hr))
        hr = device->lpVtbl->CreateCommandAllocator(
            device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void**)&host.alloc);
    if (SUCCEEDED(hr))
        hr = device->lpVtbl->CreateCommandList(
            device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, host.alloc, NULL,
            &IID_ID3D12GraphicsCommandList, (void**)&host.list);
    if (SUCCEEDED(hr))
        hr = device->lpVtbl->CreateFence(
            device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
            (void**)&host.fence);
    host.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    host.generation = 1u;
    CHECK(SUCCEEDED(hr) && host.event,
          "could not create shared-timeline WARP host");
    if (FAILED(hr) || !host.event)
        goto done;

    sink = rsx_nr_d3d12_create(
        device, LOCAL_SIZE, MAIN_SIZE, arena_ptr, arena_wptr, NULL);
    CHECK(sink != NULL, "could not create shared-timeline sink");
    if (!sink)
        goto done;
    CHECK(rsx_nr_d3d12_set_shared_timeline(
              sink, shared_test_acquire, shared_test_release,
              shared_test_flush, &host) == 0,
          "shared timeline configuration failed");
    CHECK(rsx_nr_d3d12_shared_timeline_enabled(sink),
          "shared timeline was not enabled");

    rsx_nr_exec_ops ops;
    memset(&ops, 0, sizeof(ops));
    rsx_nr_d3d12_get_exec_ops(sink, &ops);
    rsx_nir_pipeline state;
    memset(&state, 0, sizeof(state));
    state.surface.color_format = 5u;
    state.surface.depth_format = 2u;
    state.surface.raster_type = 1u;
    state.surface.clip_w = RT_W;
    state.surface.clip_h = RT_H;
    state.surface.color_offset[0] = RT_OFFSET;
    state.surface.color_pitch[0] = RT_W * 4u;
    state.surface.color_location[0] = RSX_NIR_LOCATION_LOCAL;
    state.surface.color_target = 1u;
    state.scissor.w = RT_W;
    state.scissor.h = RT_H;
    rsx_nir_clear clear = {0xF0u, 0xFFFF0000u, 0xFFFFFFu, 0u};

    host.fail_acquire_once = 1;
    CHECK(ops.clear(ops.user, &state, &clear) != 0 &&
              rsx_nr_d3d12_shared_timeline_enabled(sink),
          "transient host ownership permanently faulted shared timeline");
    CHECK(ops.clear(ops.user, &state, &clear) == 0,
          "first shared clear failed");
    clear.color_value = 0xFF00FF00u;
    CHECK(ops.clear(ops.user, &state, &clear) == 0,
          "second shared clear failed");
    rsx_nr_d3d12_stats stats;
    rsx_nr_d3d12_get_stats(sink, &stats);
    CHECK(host.flushes == 0u && stats.queue_submissions == 0u &&
              stats.shared_timeline_forced_submissions == 0u,
          "consecutive shared sections submitted host=%u native=%llu",
          host.flushes, stats.queue_submissions);

    CHECK(shared_test_flush(&host) == 0,
          "simulated legacy boundary failed");
    clear.color_value = 0xFF0000FFu;
    CHECK(ops.clear(ops.user, &state, &clear) == 0,
          "post-generation shared clear failed");
    rsx_nr_d3d12_get_stats(sink, &stats);
    CHECK(stats.shared_timeline_generations == 1u,
          "generation changes=%llu expected=1",
          stats.shared_timeline_generations);

    ops.flush(ops.user);
    rsx_nr_d3d12_get_stats(sink, &stats);
    CHECK(host.flushes == 2u && stats.queue_submissions == 1u &&
              stats.shared_timeline_forced_submissions == 1u,
          "forced shared retirement host=%u submissions=%llu forced=%llu",
          host.flushes, stats.queue_submissions,
          stats.shared_timeline_forced_submissions);
    CHECK(rsx_nr_d3d12_read_rt(
              sink, 0u, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "shared timeline readback failed");
    CHECK(pix_is(1u, 1u, 0xFFu, 0x00u, 0x00u),
          "shared timeline lost command order");
    CHECK(host.acquires == host.releases && !host.leased,
          "timeline lease imbalance %u/%u leased=%d",
          host.acquires, host.releases, host.leased);

done:
    if (sink)
        rsx_nr_d3d12_destroy(sink);
    if (host.event)
        CloseHandle(host.event);
    if (host.fence)
        host.fence->lpVtbl->Release(host.fence);
    if (host.list)
        host.list->lpVtbl->Release(host.list);
    if (host.alloc)
        host.alloc->lpVtbl->Release(host.alloc);
    if (host.queue)
        host.queue->lpVtbl->Release(host.queue);
    if (device)
        device->lpVtbl->Release(device);
    if (adapter)
        adapter->lpVtbl->Release(adapter);
    if (factory)
        factory->lpVtbl->Release(factory);
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
    CHECK(rsx_nr_d3d12_set_live_output(
              sink, 0, test_present_handoff, NULL) == 0,
          "present handoff configuration failed");
    rsx_nr_d3d12_set_watch_page(sink, test_watch_page, NULL);
    rsx_nr_d3d12_set_publish_write(sink, test_publish_write, sink);
    rsx_nr_d3d12_set_display_buffer(
        sink, 0, RSX_NIR_LOCATION_LOCAL, RT_OFFSET, RT_W, RT_H);

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

    /* ---- transactional preflight is execution-free --------------------
     * A complete section is eligible only after every action passes these
     * checks.  Cache/resource preparation is allowed, but the preflight
     * phase must not submit, draw, clear, transfer, publish, or present. */
    write_triangle(rsx_nr_d3d12_pages(sink), -1.0f, -1.0f, 1.0f, -1.0f,
                   -1.0f, 1.0f);
    u32 batch[2] = { 0, 3 };
    rsx_nir_clear preflight_clear = {
        0xF3u, 0xFF0000FFu, 0xFFFFFFu, 0u
    };
    rsx_nir_draw preflight_draw = { 5u, 0u, 1u, 0u, 3u };
    const u32 native_vp[4] = {
        0x401F9C00u, 0x0040000Du, 0x81000000u, 0x0001FF81u
    }; /* MOV o0, v0; END */
    const u32 vertex_texture_vp[8] = {
        0x401F9C00u, 0x0040000Du, 0x81000000u, 0x0001FF80u,
        0x401F9C00u, 0x0640000Du, 0x81000000u, 0x0001FF85u
    }; /* MOV o0,v0; TXL o1,v0,unit0; END */
    rsx_nr_d3d12_stats before_preflight, after_preflight;
    rsx_nr_d3d12_get_stats(sink, &before_preflight);
    const u32 handoffs_before = g_present_handoffs;
    const u32 publishes_before = g_published_writes;
    {
        rsx_nr_d3d12_stats before_program, after_program;
        u32 native_sections = 0u, legacy_sections = 0u;
        u32 fragment_texture_mask = ~0u;
        rsx_nr_d3d12_get_stats(sink, &before_program);
        CHECK(rsx_nr_d3d12_validate_draw_program_usage(
                  sink, &be.st, native_vp, 4u,
                  &fragment_texture_mask) == 0,
              "valid side-effect-free program preflight refused");
        CHECK(fragment_texture_mask == 0u,
              "MOV fragment program reported texture mask %04X",
              fragment_texture_mask);
        write_tex_fp();
        rsx_nir_pipeline texture_usage_state = be.st;
        texture_usage_state.textures[0].enabled = 1u;
        CHECK(rsx_nr_d3d12_validate_draw_program_usage(
                  sink, &texture_usage_state, native_vp, 4u,
                  &fragment_texture_mask) == 0 &&
              fragment_texture_mask == 1u,
              "TEX fragment program usage mask=%04X",
              fragment_texture_mask);
        write_test_fp();
        {
            const u32 invalid_flow_vp[4] = {
                0u, 0x08u << 27, 0u, 1u
            };
            CHECK(rsx_nr_d3d12_validate_draw_program(
                      sink, &be.st, invalid_flow_vp, 4u) ==
                      -RSX_NR_DRAW_PF_VERTEX_PROGRAM,
                  "unsupported program escaped side-effect-free preflight");
            /* Model one complete two-draw section: the first program is
             * supported but the later one is not. Admission is all-or-none,
             * so neither draw may enter the native backend. */
            if (rsx_nr_d3d12_validate_draw_program(
                    sink, &be.st, native_vp, 4u) == 0 &&
                rsx_nr_d3d12_validate_draw_program(
                    sink, &be.st, invalid_flow_vp, 4u) == 0)
                native_sections++;
            else
                legacy_sections++;
        }
        rsx_nr_d3d12_get_stats(sink, &after_program);
        CHECK(native_sections == 0u && legacy_sections == 1u,
              "unsupported section was not wholly legacy");
        CHECK(memcmp(&before_program, &after_program,
                     sizeof(before_program)) == 0,
              "program preflight changed backend statistics");
        CHECK(g_present_handoffs == handoffs_before &&
                  g_published_writes == publishes_before,
              "program preflight caused an external side effect");
    }
    CHECK(rsx_nr_d3d12_preflight_clear(
              sink, &be.st, &preflight_clear) == 0,
          "valid clear preflight refused");
    CHECK(rsx_nr_d3d12_preflight_draw(
              sink, &be.st, native_vp, 4u,
              &preflight_draw, batch) == 0,
          "valid draw preflight refused");
    {
        rsx_nir_pipeline conditional = be.st;
        conditional.render_condition.enabled = 1u;
        conditional.render_condition.dma_report = 0x66626660u;
        conditional.render_condition.offset = 0x45A0u;
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &conditional, native_vp, 4u,
                  &preflight_draw, batch) ==
                  -RSX_NR_DRAW_PF_RENDER_CONDITION,
              "conditional draw was accepted without an exact report reader");
        rsx_nr_d3d12_set_render_condition_reader(
            sink, test_render_condition_read, NULL);
        g_render_condition_value = 0u;
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &conditional, native_vp, 4u,
                  &preflight_draw, batch) == 0,
              "false but resolvable conditional draw was refused");
        g_render_condition_fail = 1;
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &conditional, native_vp, 4u,
                  &preflight_draw, batch) ==
                  -RSX_NR_DRAW_PF_RENDER_CONDITION,
              "unresolvable conditional draw escaped preflight");
        g_render_condition_fail = 0;
    }
    {
        rsx_nir_pipeline polygon_offset = be.st;
        rsx_nr_d3d12_stats before_offset, after_offset;
        polygon_offset.raster.polygon_offset_fill_enable = 1u;
        polygon_offset.raster.polygon_offset_scale = 0x3FC00000u; /* 1.5 */
        polygon_offset.raster.polygon_offset_bias = 0xC0000000u;  /* -2 */
        rsx_nr_d3d12_get_stats(sink, &before_offset);
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &polygon_offset, native_vp, 4u,
                  &preflight_draw, batch) == 0,
              "polygon-offset draw preflight refused");
        rsx_nr_d3d12_get_stats(sink, &after_offset);
        CHECK(after_offset.pso_builds == before_offset.pso_builds + 1u,
              "polygon-offset raster state did not produce a distinct PSO");
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &polygon_offset, native_vp, 4u,
                  &preflight_draw, batch) == 0,
              "repeated polygon-offset draw preflight refused");
        rsx_nr_d3d12_get_stats(sink, &after_offset);
        CHECK(after_offset.pso_hits > before_offset.pso_hits,
              "polygon-offset PSO was not reused");
    }
    {
        rsx_nir_pipeline two_sided = be.st;
        two_sided.depth_stencil.stencil_test_enable = 1u;
        two_sided.depth_stencil.stencil_func = 0x0203u;
        two_sided.depth_stencil.stencil_ref = 0x12u;
        two_sided.depth_stencil.stencil_mask = 0xFFu;
        two_sided.depth_stencil.stencil_write_mask = 0xFFu;
        two_sided.depth_stencil.stencil_op_fail = 0x1E00u;
        two_sided.depth_stencil.stencil_op_zfail = 0x1E01u;
        two_sided.depth_stencil.stencil_op_zpass = 0x1E02u;
        two_sided.depth_stencil.two_sided_stencil_enable = 1u;
        two_sided.depth_stencil.back_stencil_func = 0x0204u;
        two_sided.depth_stencil.back_stencil_ref = 0x12u;
        two_sided.depth_stencil.back_stencil_mask = 0xFFu;
        two_sided.depth_stencil.back_stencil_write_mask = 0xFFu;
        two_sided.depth_stencil.back_stencil_op_fail = 0x1E02u;
        two_sided.depth_stencil.back_stencil_op_zfail = 0x1E01u;
        two_sided.depth_stencil.back_stencil_op_zpass = 0x1E00u;
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &two_sided, native_vp, 4u,
                  &preflight_draw, batch) == 0,
              "representable two-sided stencil draw preflight refused");
        {
            rsx_nr_d3d12_stats before_ref, after_ref;
            rsx_nr_d3d12_get_stats(sink, &before_ref);
            two_sided.depth_stencil.stencil_ref = 0x34u;
            two_sided.depth_stencil.back_stencil_ref = 0x34u;
            CHECK(rsx_nr_d3d12_preflight_draw(
                      sink, &two_sided, native_vp, 4u,
                      &preflight_draw, batch) == 0,
                  "dynamic stencil-reference draw preflight refused");
            rsx_nr_d3d12_get_stats(sink, &after_ref);
            CHECK(after_ref.pso_builds == before_ref.pso_builds &&
                      after_ref.pso_hits == before_ref.pso_hits + 1u,
                  "dynamic stencil reference rebuilt a PSO");
        }
        two_sided.depth_stencil.back_stencil_ref = 0x13u;
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &two_sided, native_vp, 4u,
                  &preflight_draw, batch) ==
                  -RSX_NR_DRAW_PF_STENCIL_STATE,
              "unequal front/back stencil reference escaped preflight");
    }
    {
        rsx_nir_pipeline depth_bounds = be.st;
        depth_bounds.depth_stencil.depth_bounds_test_enable = 1u;
        depth_bounds.depth_stencil.depth_bounds_min = 0x3E800000u; /* .25 */
        depth_bounds.depth_stencil.depth_bounds_max = 0x3F400000u; /* .75 */
        const int depth_bounds_result = rsx_nr_d3d12_preflight_draw(
            sink, &depth_bounds, native_vp, 4u, &preflight_draw, batch);
        CHECK(depth_bounds_result ==
                  (rsx_nr_d3d12_depth_bounds_supported(sink)
                       ? 0 : -RSX_NR_DRAW_PF_DEPTH_BOUNDS),
              "depth-bounds feature gate mismatch result=%d support=%d",
              depth_bounds_result,
              rsx_nr_d3d12_depth_bounds_supported(sink));
        if (rsx_nr_d3d12_depth_bounds_supported(sink)) {
            rsx_nr_d3d12_stats before_bounds, after_bounds;
            rsx_nr_d3d12_get_stats(sink, &before_bounds);
            depth_bounds.depth_stencil.depth_bounds_min = 0x3E000000u; /* .125 */
            depth_bounds.depth_stencil.depth_bounds_max = 0x3F200000u; /* .625 */
            CHECK(rsx_nr_d3d12_preflight_draw(
                      sink, &depth_bounds, native_vp, 4u,
                      &preflight_draw, batch) == 0,
                  "second dynamic depth-bounds draw preflight refused");
            rsx_nr_d3d12_get_stats(sink, &after_bounds);
            CHECK(after_bounds.pso_builds == before_bounds.pso_builds &&
                      after_bounds.pso_hits == before_bounds.pso_hits + 1u,
                  "dynamic depth bounds rebuilt a PSO");
        }
        depth_bounds.depth_stencil.depth_bounds_min = 0x3F600000u; /* .875 */
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &depth_bounds, native_vp, 4u,
                  &preflight_draw, batch) ==
                  -RSX_NR_DRAW_PF_DEPTH_BOUNDS,
              "reversed depth bounds escaped preflight");
    }
    {
        rsx_nir_pipeline unsupported = be.st;
        unsupported.surface.color_target = 0x13u;
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &unsupported, native_vp, 4u,
                  &preflight_draw, batch) ==
                  -RSX_NR_DRAW_PF_SURFACE_TARGET,
              "draw preflight did not preserve exact refusal reason");
    }
    {
        const u32 flow_vp[4] = {
            0u, 0x08u << 27, 0u, 1u
        };
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &be.st, flow_vp, 4u,
                  &preflight_draw, batch) ==
                  -RSX_NR_DRAW_PF_VERTEX_PROGRAM,
              "flow-control VP escaped transactional draw preflight");
    }
    {
        rsx_nir_pipeline vertex_texture = be.st;
        vertex_texture.vertex_textures[0].enabled = 1u;
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &vertex_texture, native_vp, 4u,
                  &preflight_draw, batch) ==
                  -RSX_NR_DRAW_PF_VERTEX_TEXTURE,
              "enabled vertex texture escaped draw preflight");
    }
    {
        rsx_nir_pipeline vertex_texture = be.st;
        write_vertex_texture0(rsx_nr_d3d12_pages(sink),
                              0.0f, 1.0f, 0.0f, 1.0f);
        vertex_texture.vertex_textures[0] = vertex_texture0_descriptor();
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &vertex_texture, vertex_texture_vp, 8u,
                  &preflight_draw, batch) == 0,
              "valid TXL vertex texture preflight refused");
        vertex_texture.vertex_textures[0].format = 0xA5u;
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &vertex_texture, vertex_texture_vp, 8u,
                  &preflight_draw, batch) ==
                  -RSX_NR_DRAW_PF_VERTEX_TEXTURE,
              "ordinary color format escaped vertex-texture preflight");
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &be.st, vertex_texture_vp, 8u,
                  &preflight_draw, batch) ==
                  -RSX_NR_DRAW_PF_VERTEX_PROGRAM,
              "TXL program escaped preflight without its bound unit");
    }
    CHECK(rsx_nr_d3d12_preflight_present(sink, 0) == 0,
          "valid present preflight refused");
    preflight_clear.mask = 0x10u;
    CHECK(rsx_nr_d3d12_preflight_clear(
              sink, &be.st, &preflight_clear) != 0,
          "partial-channel clear preflight accepted");
    rsx_nir_transfer invalid_transfer;
    memset(&invalid_transfer, 0, sizeof(invalid_transfer));
    CHECK(rsx_nr_d3d12_preflight_transfer(
              sink, &be.st, &invalid_transfer, NULL) != 0,
          "empty transfer preflight accepted");
    rsx_nr_d3d12_get_stats(sink, &after_preflight);
    CHECK(after_preflight.queue_submissions ==
              before_preflight.queue_submissions &&
          after_preflight.clears == before_preflight.clears &&
          after_preflight.draws == before_preflight.draws &&
          after_preflight.transfers == before_preflight.transfers &&
          after_preflight.presents == before_preflight.presents &&
          g_present_handoffs == handoffs_before &&
          g_published_writes == publishes_before,
          "preflight executed work submit=%llu/%llu clear=%llu/%llu "
          "draw=%llu/%llu xfer=%llu/%llu present=%llu/%llu",
          after_preflight.queue_submissions,
          before_preflight.queue_submissions,
          after_preflight.clears, before_preflight.clears,
          after_preflight.draws, before_preflight.draws,
          after_preflight.transfers, before_preflight.transfers,
          after_preflight.presents, before_preflight.presents);

    /* ---- conditional draw: false consumes without recording ---------- */
    rsx_nir_render_condition condition;
    memset(&condition, 0, sizeof(condition));
    condition.enabled = 1u;
    condition.dma_report = 0x66626660u;
    condition.offset = 0x45A0u;
    rsx_nir_em_render_condition(&em, &condition);
    g_render_condition_value = 0u;
    rsx_nr_d3d12_stats before_conditional, after_conditional;
    rsx_nr_d3d12_get_stats(sink, &before_conditional);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nr_backend_run(&be, 0);
    rsx_nr_d3d12_get_stats(sink, &after_conditional);
    CHECK(be.stats.exec_errors == 0 &&
              after_conditional.draws == before_conditional.draws &&
              after_conditional.conditional_draws_skipped ==
                  before_conditional.conditional_draws_skipped + 1u,
          "false condition rendered or faulted draws=%llu/%llu skip=%llu/%llu",
          before_conditional.draws, after_conditional.draws,
          before_conditional.conditional_draws_skipped,
          after_conditional.conditional_draws_skipped);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0 &&
              pix_is(2, 61, 0xFF, 0x00, 0x00),
          "false conditional draw changed the target");

    /* ---- leg 2: bottom-left half triangle; true condition executes ---- */
    g_render_condition_value = 1u;
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

    /* A stale nonzero INDEX_BASE may remain while the producer switches to
     * DRAW_ARRAYS.  Array fetches must ignore it exactly as the legacy
     * decoder does; indexed fetches below continue to apply it. */
    stage_frame_state(&em);
    stage_vertex_bindings(&em, 0x40u);
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0,
          "array draw with stale base index faulted");
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "stale-base array RT readback failed");
    CHECK(pix_is(2, 61, 0xFF, 0x00, 0xFF) &&
              pix_is(61, 2, 0xFF, 0x00, 0x00),
          "array draw incorrectly applied stale index base");
    {
        rsx_nr_d3d12_stats resident;
        rsx_nr_d3d12_get_stats(sink, &resident);
        CHECK(resident.resident_pages[0] == 1u &&
                  resident.resident_pages[1] == 0u &&
                  g_watched_pages[0] == 1u &&
                  g_last_watched_offset[0] == VTX_OFFSET,
              "array draw did not register only its exact vertex page "
              "local=%llu main=%llu watch=%u offset=%X",
              resident.resident_pages[0], resident.resident_pages[1],
              g_watched_pages[0], g_last_watched_offset[0]);
    }

    /* The legacy renderer deliberately leaves D3D depth clipping disabled.
     * Fullscreen and post-process programs can therefore write clip-space Z
     * outside [0,w] when depth testing is disabled.  Native rendering must
     * not silently discard those pixels. */
    stage_frame_state(&em);
    stage_vertex_bindings(&em, 0u);
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);
    write_triangle_z(rsx_nr_d3d12_pages(sink),
                     -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 2.0f);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0,
          "unclipped out-of-range Z draw faulted");
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "unclipped out-of-range Z readback failed");
    CHECK(pix_is(2, 61, 0xFF, 0x00, 0xFF) &&
              pix_is(61, 2, 0xFF, 0x00, 0x00),
          "native depth clipping discarded legacy-visible pixels");

    memset(&condition, 0, sizeof(condition));
    rsx_nir_em_render_condition(&em, &condition);

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

    /* ---- one command-list generation reuses one mirror slice ----------
     * Six dirty draws deliberately exceed the backend's three physical
     * staging slices while remaining in one unsubmitted command list. The
     * mirror must append within that submission fence rather than rotate a
     * slice per draw and reject the fourth upload. */
    {
        rsx_nr_d3d12_stats before_append, after_append;
        rsx_nr_d3d12_get_stats(sink, &before_append);
        stage_frame_state(&em);
        rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);
        for (u32 i = 0; i < 6u; ++i) {
            const float edge = i & 1u ? 0.875f : 1.0f;
            write_triangle(rsx_nr_d3d12_pages(sink),
                           -1.0f, -1.0f, edge, -1.0f, -1.0f, edge);
            rsx_nir_em_draw(&em, 5, 0, batch, 1);
            rsx_nr_backend_run(&be, 0);
            CHECK(be.stats.exec_errors == 0,
                  "same-list mirror append failed at dirty draw %u", i);
        }
        rsx_nr_d3d12_get_stats(sink, &after_append);
        CHECK(after_append.draws == before_append.draws + 6u &&
                  after_append.residency_failures ==
                      before_append.residency_failures &&
                  after_append.mirror_rollovers ==
                      before_append.mirror_rollovers,
              "same-list mirror append rotated or failed draws=%llu/%llu "
              "residency=%llu/%llu rollover=%llu/%llu",
              before_append.draws, after_append.draws,
              before_append.residency_failures,
              after_append.residency_failures,
              before_append.mirror_rollovers,
              after_append.mirror_rollovers);
        rsx_nir_em_present(&em, 0);
        rsx_nr_backend_run(&be, 0);
        CHECK(be.stats.exec_errors == 0,
              "same-list mirror append present failed");
    }

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
    {
        rsx_nr_d3d12_stats resident;
        rsx_nr_d3d12_get_stats(sink, &resident);
        CHECK(resident.resident_pages[0] == 1u &&
                  resident.resident_pages[1] == 1u &&
                  g_watched_pages[1] == 1u &&
                  g_last_watched_offset[1] == IDX_OFFSET,
              "indexed draw residency was not sparse/exact "
              "local=%llu main=%llu watch=%u offset=%X",
              resident.resident_pages[0], resident.resident_pages[1],
              g_watched_pages[1], g_last_watched_offset[1]);
    }

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

    /* ---- real texture + exact dirty-page refresh ----------------------
     * Execute an actual TEX instruction through a dynamic SRV/sampler.
     * Rewriting the same guest range must rebuild the persistent GPU
     * resource while retaining the structurally identical PSO. */
    write_tex_fp();
    write_solid_texture(rsx_nr_d3d12_pages(sink), 0, 255, 0, 255);
    stage_frame_state(&em);
    stage_texture0(&em);
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0, "green texture exec errors %llu",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "RT readback failed");
    CHECK(pix_is(2, 61, 0x00, 0xFF, 0x00), "green texture pixel");
    rsx_nr_d3d12_stats before_texture_change;
    rsx_nr_d3d12_get_stats(sink, &before_texture_change);

    write_solid_texture(rsx_nr_d3d12_pages(sink), 255, 0, 0, 255);
    stage_frame_state(&em);
    stage_texture0(&em);
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0, "red texture exec errors %llu",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "RT readback failed");
    CHECK(pix_is(2, 61, 0x00, 0x00, 0xFF), "red texture pixel");
    rsx_nr_d3d12_stats after_texture_change;
    rsx_nr_d3d12_get_stats(sink, &after_texture_change);
    CHECK(after_texture_change.texture_builds ==
              before_texture_change.texture_builds &&
          after_texture_change.texture_refreshes ==
              before_texture_change.texture_refreshes + 1,
          "texture refresh builds=%llu/%llu refresh=%llu/%llu",
          before_texture_change.texture_builds,
          after_texture_change.texture_builds,
          before_texture_change.texture_refreshes,
          after_texture_change.texture_refreshes);
    CHECK(after_texture_change.pso_builds ==
              before_texture_change.pso_builds &&
          after_texture_change.pso_hits > before_texture_change.pso_hits,
          "texture data change rebuilt PSO builds=%llu/%llu hits=%llu/%llu",
          before_texture_change.pso_builds, after_texture_change.pso_builds,
          before_texture_change.pso_hits, after_texture_change.pso_hits);

    /* ---- immutable descriptors + fence-retired texture resources -----
     * Record two differently textured draws into the same open command
     * list, refreshing the same guest texture key between them.  The first
     * draw must keep both its descriptor contents and old resource alive
     * until submission instead of observing the second draw's replacement. */
    stage_frame_state(&em);
    stage_texture0(&em);             /* cached red texture from above      */
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);
    write_triangle(rsx_nr_d3d12_pages(sink),
                   -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nr_backend_run(&be, 0);      /* record, deliberately do not submit */

    write_solid_texture(rsx_nr_d3d12_pages(sink), 0, 255, 0, 255);
    write_triangle(rsx_nr_d3d12_pages(sink),
                   1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f);
    stage_frame_state(&em);
    stage_texture0(&em);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0,
          "same-list texture replacement exec errors %llu",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "same-list texture replacement readback failed");
    CHECK(pix_is(2, 61, 0x00, 0x00, 0xFF),
          "first same-list draw lost its red descriptor/resource");
    CHECK(pix_is(61, 2, 0x00, 0xFF, 0x00),
          "second same-list draw did not use refreshed green texture");

    /* ---- R5G6B5 target sampled as an exact native alias --------------
     * The capture's only remaining surface format is GCM color format 3.
     * Clear a real B5G6R5 target, then sample its GPU contents into the
     * BGRA target.  RT565_OFFSET is outside guest memory, so success cannot
     * come from a stale guest decode. */
    rsx_nir_surface s565;
    memset(&s565, 0, sizeof(s565));
    s565.color_format = 3;
    s565.depth_format = 2;
    s565.raster_type = 1;
    s565.clip_w = RT_W;
    s565.clip_h = RT_H;
    s565.color_offset[0] = RT565_OFFSET;
    s565.color_pitch[0] = RT_W * 2u;
    s565.color_location[0] = RSX_NIR_LOCATION_LOCAL;
    s565.color_target = 1;
    rsx_nir_em_surface(&em, &s565);
    rsx_nir_em_clear(&em, 0xF3, 0xFF00FF00u, 0xFFFFFF, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0, "R5G6B5 clear exec errors %llu",
          be.stats.exec_errors);

    write_tex_fp();
    stage_frame_state(&em);
    stage_rt565_texture0(&em);
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);
    write_triangle(rsx_nr_d3d12_pages(sink),
                   -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0, "R5G6B5 alias exec errors %llu",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "RT readback failed");
    CHECK(pix_is(2, 61, 0x00, 0xFF, 0x00), "R5G6B5 alias pixel");

    /* ---- exact QUADS host-index expansion -----------------------------
     * Primitive 8 is the only non-native topology present in the archived
     * world captures. Two native triangles must cover the same full quad. */
    write_test_fp();
    stage_frame_state(&em);
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);
    write_quad(rsx_nr_d3d12_pages(sink));
    u32 quad_batch[2] = { 0, 4 };
    rsx_nir_em_draw(&em, 8, 0, quad_batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0, "quad expansion exec errors %llu",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "RT readback failed");
    CHECK(pix_is(2, 2, 0xFF, 0x00, 0xFF) &&
              pix_is(61, 61, 0xFF, 0x00, 0xFF),
          "quad expansion did not cover target");

    /* ---- guest-identity zeta + GPU depth sampling ---------------------
     * Clear the distinct ZETA_OFFSET resource to 0.25, then sample it in a
     * depth-disabled pass. The address is outside guest memory, proving the
     * sampled value comes from the prior GPU depth resource. */
    stage_frame_state(&em);
    rsx_nir_em_clear(&em, 0x01, 0, 0x400000u, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0, "zeta clear exec errors %llu",
          be.stats.exec_errors);

    write_tex_fp();
    stage_frame_state(&em);
    stage_depth_texture0(&em);
    rsx_nir_em_clear(&em, 0xF0, 0xFF0000FFu, 0, 0);
    write_triangle(rsx_nr_d3d12_pages(sink), -1.0f, -1.0f, 1.0f, -1.0f,
                   -1.0f, 1.0f);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0, "zeta sample exec errors %llu",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "RT readback failed");
    CHECK(pix_is(2, 61, 0x40, 0x40, 0x40),
          "zeta sample pixel %02X %02X %02X",
          pix(2, 61)[0], pix(2, 61)[1], pix(2, 61)[2]);

    /* ---- depth BORDER parity with the established renderer -----------
     * Yakuza's shadow textures request BORDER and write opaque white into
     * the guest border register.  Legacy leaves the D3D depth-sampler border
     * at zero.  The offline passthrough VS supplies TC0=(-1,-1), proving the
     * native GPU path observes the same zero border instead of white. */
    write_tex_fp();
    stage_frame_state(&em);
    stage_depth_border_texture0(&em);
    rsx_nir_em_clear(&em, 0xF0, 0xFF0000FFu, 0, 0);
    write_triangle(rsx_nr_d3d12_pages(sink), -1.0f, -1.0f, 1.0f, -1.0f,
                   -1.0f, 1.0f);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0,
          "zeta border sample exec errors %llu", be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "RT readback failed");
    CHECK(pix_is(2, 61, 0x00, 0x00, 0x00),
          "zeta border sample was not legacy-zero %02X %02X %02X",
          pix(2, 61)[0], pix(2, 61)[1], pix(2, 61)[2]);
    write_test_fp();

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

    /* ---- refusal is render-atomic across all draw batches -------------
     * Force the host-index path, make batch 0 valid, and make batch 1 point
     * beyond the guest arena. The blue target must remain untouched: a
     * native refusal is safe for ordered legacy fallback only if no earlier
     * batch has already rendered. */
    stage_frame_state(&em);
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFFu, 0);
    ib.restart_enable = 1;
    ib.restart_index = 0xFFFFu;
    rsx_nir_em_index_binding(&em, &ib);
    u32 atomic_refusal_batches[4] = { 0u, 3u, 0x8000u, 3u };
    rsx_nir_em_draw(&em, 5, 1, atomic_refusal_batches, 2);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 1,
          "atomic draw refusal not surfaced (%llu)", be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "RT readback failed");
    CHECK(pix_is(2, 61, 0xFF, 0x00, 0x00),
          "refused draw partially rendered batch 0: %02X %02X %02X",
          pix(2, 61)[0], pix(2, 61)[1], pix(2, 61)[2]);

    /* ---- partial-channel clear refused (counted, never approximated) ---
     * CELL_GCM_CLEAR_R/G/B/A are individually maskable on hardware
     * (distinct CELL_GCM_CLEAR_* mask bits); D3D12 clears whole
     * targets, so a partial color mask must be refused to the core. */
    stage_frame_state(&em);
    rsx_nir_em_clear(&em, 0x13, 0xFF000000u, 0, 0);   /* Z+S+R only      */
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 2, "partial clear not surfaced (%llu)",
          be.stats.exec_errors);

    /* ---- exact post-publication transfer route -----------------------
     * A pitched two-line copy must notify only the bytes actually written,
     * not the untouched pitch envelope between them. The callback then
     * publishes those exact spans into the mirror generation tracker. */
    {
        const u32 src = 0x5000u, dst = 0x6000u;
        memset(g_local + src, 0, 32);
        memset(g_main + dst, 0, 32);
        memcpy(g_local + src, "ABCDEFGH", 8);
        memcpy(g_local + src + 16, "IJKLMNOP", 8);
        g_published_writes = 0;
        rsx_nir_transfer transfer;
        memset(&transfer, 0, sizeof(transfer));
        transfer.kind = RSX_NIR_XFER_BUFFER;
        transfer.src_location = RSX_NIR_LOCATION_LOCAL;
        transfer.dst_location = RSX_NIR_LOCATION_MAIN;
        transfer.src_offset = src;
        transfer.dst_offset = dst;
        transfer.src_pitch = 16;
        transfer.dst_pitch = 16;
        transfer.line_length = 8;
        transfer.line_count = 2;
        rsx_nir_em_transfer(&em, &transfer, NULL);
        rsx_nr_backend_run(&be, 0);
        CHECK(be.stats.exec_errors == 2,
              "exact transfer added exec error (%llu)",
              be.stats.exec_errors);
        CHECK(memcmp(g_main + dst, "ABCDEFGH", 8) == 0 &&
              memcmp(g_main + dst + 16, "IJKLMNOP", 8) == 0,
              "pitched transfer bytes mismatch");
        CHECK(g_published_writes == 2 &&
              g_published_space[0] == RSX_NIR_LOCATION_MAIN &&
              g_published_offset[0] == dst && g_published_size[0] == 8 &&
              g_published_space[1] == RSX_NIR_LOCATION_MAIN &&
              g_published_offset[1] == dst + 16 &&
              g_published_size[1] == 8,
              "published transfer spans count=%u first=%u:%X+%u "
              "second=%u:%X+%u", g_published_writes,
              g_published_space[0], g_published_offset[0],
              g_published_size[0], g_published_space[1],
              g_published_offset[1], g_published_size[1]);
    }

    /* ---- vertex TXL: exact float format + big-endian conversion -------
     * A two-instruction guest VP preserves clip position and samples a
     * one-texel W32Z32Y32X32 vertex texture into COL0. The ordinary FP
     * exports COL0, so the green oracle covers shader declaration, root
     * binding, fixed vertex sampler, cache resolution and byte conversion. */
    write_test_fp();
    stage_frame_state(&em);
    stage_vertex_texture0(&em);
    rsx_nir_em_vertex_program(
        &em, 0u, vertex_texture_vp, 8u, 1u, 3u, 0u);
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFFu, 0);
    write_vertex_texture0(rsx_nr_d3d12_pages(sink),
                          0.0f, 1.0f, 0.0f, 1.0f);
    write_triangle(rsx_nr_d3d12_pages(sink),
                   -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 2,
          "vertex-texture draw exec errors %llu", be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "vertex-texture RT readback failed");
    CHECK(pix_is(2, 61, 0x00, 0xFF, 0x00),
          "vertex TXL pixel %02X %02X %02X",
          pix(2, 61)[0], pix(2, 61)[1], pix(2, 61)[2]);

    /* ---- exact forward BRI/BRB + dynamic branch bits ------------------
     * BRB b0 selects white; the fallthrough writes black then an always BRI
     * skips the white instruction. Position is still pulled from ATTR0.
     * Reusing one PSO while changing only branch_bits proves that the live
     * register is draw data in b0 rather than shader/PSO identity. */
    {
        const u32 branch_vp[24] = {
            0x401F9C00u, 0x0040000Du, 0x81000000u, 0x0001FF80u,
            0x001F9C00u, 0x88000000u, 0x00000000u, 0x90000000u,
            0x401F9C00u, 0x04400000u, 0x00000000u, 0x0001E004u,
            0x001F9C00u, 0x48000000u, 0x00000000u, 0xA0000000u,
            0x401F9C00u, 0x05400000u, 0x00000000u, 0x0001E004u,
            0x001F9C00u, 0x00000000u, 0x00000000u, 0x00000001u
        };
        rsx_nir_texture disabled_vertex_texture;
        memset(&disabled_vertex_texture, 0, sizeof(disabled_vertex_texture));
        rsx_nir_em_vertex_texture(&em, 0u, &disabled_vertex_texture);
        rsx_nir_em_vertex_program(
            &em, 0u, branch_vp, 24u, 1u, 3u, 0u);
        rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFFu, 0);
        rsx_nir_em_draw(&em, 5, 0, batch, 1);
        rsx_nir_em_present(&em, 0);
        rsx_nr_backend_run(&be, 0);
        CHECK(rsx_nr_d3d12_read_rt(
                  sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
              "BRB false RT readback failed");
        CHECK(pix_is(2, 61, 0x00, 0x00, 0x00),
              "BRB false pixel %02X %02X %02X",
              pix(2, 61)[0], pix(2, 61)[1], pix(2, 61)[2]);

        rsx_nr_d3d12_stats before_branch_change, after_branch_change;
        rsx_nr_d3d12_get_stats(sink, &before_branch_change);
        rsx_nir_em_vertex_program(
            &em, 0u, branch_vp, 24u, 1u, 3u, 1u);
        rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFFu, 0);
        rsx_nir_em_draw(&em, 5, 0, batch, 1);
        rsx_nir_em_present(&em, 0);
        rsx_nr_backend_run(&be, 0);
        rsx_nr_d3d12_get_stats(sink, &after_branch_change);
        CHECK(after_branch_change.pso_builds == before_branch_change.pso_builds &&
              after_branch_change.pso_hits == before_branch_change.pso_hits + 1u,
              "dynamic branch bits rebuilt the PSO");
        CHECK(rsx_nr_d3d12_read_rt(
                  sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
              "BRB true RT readback failed");
        CHECK(pix_is(2, 61, 0xFF, 0xFF, 0xFF),
              "BRB true pixel %02X %02X %02X",
              pix(2, 61)[0], pix(2, 61)[1], pix(2, 61)[2]);
    }

    /* ---- consecutive supported sections share one submission ---------
     * Separate backend drains model separate transactionally admitted
     * sections. No dependency or legacy boundary occurs between them, so
     * both must remain on one open native command list. */
    {
        rsx_nr_d3d12_stats before_sections, after_sections;
        rsx_nr_d3d12_get_stats(sink, &before_sections);
        rsx_nir_em_draw(&em, 5u, 0u, batch, 1u);
        rsx_nr_backend_run(&be, 0);
        rsx_nir_em_draw(&em, 5u, 0u, batch, 1u);
        rsx_nr_backend_run(&be, 0);
        rsx_nr_d3d12_get_stats(sink, &after_sections);
        CHECK(after_sections.draws == before_sections.draws + 2u &&
                  after_sections.queue_submissions ==
                      before_sections.queue_submissions,
              "consecutive native sections did not share a submission");
        CHECK(after_sections.descriptor_table_builds ==
                  before_sections.descriptor_table_builds + 1u &&
              after_sections.descriptor_table_hits >=
                  before_sections.descriptor_table_hits + 1u,
              "consecutive section descriptor reuse failed");
    }

    /* ---- exact descriptor-table reuse --------------------------------
     * The sampler heap contains 128 immutable tables. More than 128 draws
     * with identical resource/view/sampler identity must reuse one table and
     * must not synchronously submit merely to recycle descriptor capacity. */
    {
        rsx_nr_d3d12_stats before_reuse, after_reuse;
        rsx_nr_d3d12_get_stats(sink, &before_reuse);
        for (u32 i = 0; i < 512u; ++i) {
            rsx_nir_em_draw(&em, 5u, 0u, batch, 1u);
            rsx_nr_backend_run(&be, 0);
        }
        rsx_nr_d3d12_get_stats(sink, &after_reuse);
        CHECK(after_reuse.queue_submissions ==
                  before_reuse.queue_submissions,
              "descriptor reuse forced %llu submissions",
              after_reuse.queue_submissions -
                  before_reuse.queue_submissions);
        CHECK(after_reuse.descriptor_table_builds ==
                  before_reuse.descriptor_table_builds &&
              after_reuse.descriptor_table_hits >=
                  before_reuse.descriptor_table_hits + 512u,
              "descriptor reuse builds=%llu hits=%llu",
              after_reuse.descriptor_table_builds -
                  before_reuse.descriptor_table_builds,
              after_reuse.descriptor_table_hits -
                  before_reuse.descriptor_table_hits);
        CHECK(rsx_nr_d3d12_read_rt(
                  sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
              "descriptor-reuse RT readback failed");
        CHECK(pix_is(2, 61, 0xFF, 0xFF, 0xFF),
              "descriptor-reuse pixel %02X %02X %02X",
              pix(2, 61)[0], pix(2, 61)[1], pix(2, 61)[2]);
    }

    /* ---- sink accounting ----------------------------------------------- */
    rsx_nr_d3d12_stats st;
    rsx_nr_d3d12_get_stats(sink, &st);
    CHECK(st.unsupported_clears == 1, "partial clear not counted (%llu)",
          st.unsupported_clears);
    CHECK(st.clears == 23 && st.draws == 538 && st.presents == 21,
          "sink counts clears=%llu draws=%llu presents=%llu", st.clears,
          st.draws, st.presents);
    CHECK(st.conditional_draws_skipped == 1u,
          "conditional skips=%llu expected=1",
          st.conditional_draws_skipped);
    CHECK(st.queue_submissions < st.clears + st.draws + st.presents,
          "draw/clear actions were not submission-batched (%llu submissions "
          "for %llu actions)", st.queue_submissions,
          st.clears + st.draws + st.presents);
    CHECK(st.unsupported_draws == 1 && st.unsup_draw_index == 1 &&
              st.compile_failures == 0,
          "unsupported=%llu compile_failures=%llu", st.unsupported_draws,
          st.compile_failures);
    CHECK(st.pso_builds >= 1 && st.pso_hits >= 1,
          "pso cache builds=%llu hits=%llu", st.pso_builds, st.pso_hits);
    CHECK(st.real_fp_draws == st.draws,
          "real fragment programs=%llu draws=%llu", st.real_fp_draws,
          st.draws);
    CHECK(st.texture_draws == 8 && st.texture_builds == 2 &&
              st.texture_refreshes == 2 && st.texture_failures == 0,
          "textures draws=%llu builds=%llu refresh=%llu failures=%llu",
          st.texture_draws, st.texture_builds, st.texture_refreshes,
          st.texture_failures);
    CHECK(st.rt_alias_binds >= 1, "R5G6B5 alias not counted");
    CHECK(g_present_handoffs == 21,
          "native scanout handoffs=%u expected=21", g_present_handoffs);

    rsx_nr_ring_destroy(&ring);
    rsx_nr_d3d12_destroy(sink);

    test_broker_actual_color_format();
    test_shared_timeline();

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
