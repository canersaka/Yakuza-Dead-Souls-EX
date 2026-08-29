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
#include "../rsx_nr_frame_owner.h"
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
#include <d3dcompiler.h>
#include <dxgi1_4.h>

static int g_failures;
static u32 g_present_handoffs;
static void* g_last_present_texture;
static u32 g_watched_pages[2];
static u32 g_last_watched_offset[2];
static u8 g_watched_host_page[2][1024]; /* LOCAL_SIZE / host page */
static u32 g_published_writes;
static rsx_nr_d3d12* g_churn_sink;
static int g_churn_unrelated_vertex_page;
static u64 g_content_shader_calls;
static u64 g_content_pso_loads;
static u64 g_content_pso_stores;

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
#define RTF32_OFFSET 0x00330000u
#define ZETA_OFFSET  0x00320000u
#define COLOR_ALIAS_OFFSET 0x00410000u /* outside the guest arena          */
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
    g_last_present_texture = texture;
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
    g_watched_host_page[space][page_offset >> 12] = 1u;
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
    if (g_churn_unrelated_vertex_page && g_churn_sink && !space &&
        offset == VTX_OFFSET && min_bytes >= RSX_GUEST_PAGE_SIZE) {
        /* Model a producer continually publishing unrelated scratch bytes in
         * the same generation page as a stable vertex span. */
        const u32 churn_offset = VTX_OFFSET + RSX_GUEST_PAGE_SIZE / 2u;
        g_local[churn_offset] ^= 1u;
        rsx_nr_d3d12_note_guest_write(
            g_churn_sink, 0u, churn_offset, 1u);
    }
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

static void write_ret_fp(void)
{
    /* MOV r0, COL0; RET; END. Opcode bit 6 is SRC1 bit 31, so the
     * second instruction's full opcode is 0x45 rather than DP3. */
    u8* p = g_local + FP_OFFSET;
    put_fp_word(p + 0, (0x01u << 24) | (0xFu << 9) | (1u << 13));
    put_fp_word(p + 4, 1u | ((0u << 9) | (1u << 11) |
                             (2u << 13) | (3u << 15)) |
                              (7u << 18));
    put_fp_word(p + 8, 0);
    put_fp_word(p + 12, 0);
    put_fp_word(p + 16, (0x05u << 24) | (1u << 30) | 1u);
    put_fp_word(p + 20, 7u << 18);
    put_fp_word(p + 24, 1u << 31);
    put_fp_word(p + 28, 0);
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

static void write_solid_texture_bytes(u8 r, u8 g, u8 b, u8 a)
{
    u8* p = g_local + TEX_OFFSET;
    for (u32 i = 0; i < 4; i++) {
        p[i * 4 + 0] = a;            /* RSX A8R8G8B8 guest order          */
        p[i * 4 + 1] = r;
        p[i * 4 + 2] = g;
        p[i * 4 + 3] = b;
    }
}

static void write_solid_texture(rsx_guest_pages* pages,
                                u8 r, u8 g, u8 b, u8 a)
{
    write_solid_texture_bytes(r, g, b, a);
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

static void stage_d8r8g8b8_cube_texture0(rsx_nir_emitter* em)
{
    rsx_nir_texture texture;
    memset(&texture, 0, sizeof(texture));
    texture.enabled = 1;
    texture.offset = TEX_OFFSET;
    texture.location = RSX_NIR_LOCATION_LOCAL;
    texture.format = 0x9Eu;          /* swizzled D8R8G8B8                 */
    texture.dimension = 2;
    texture.cubemap = 1;
    texture.mipmaps = 1;
    texture.width = 2;
    texture.height = 2;
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

static void stage_color_border_texture0(rsx_nir_emitter* em)
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
    texture.wrap = 0x00040404u;      /* BORDER on every coordinate        */
    texture.border_color = 0xFFFFFFFFu;
    texture.remap = 0xAAE4u;
    texture.filter = (1u << 16) | (1u << 24);
    rsx_nir_em_texture(em, 0, &texture);
}

static void stage_private_rgba_rt_texture0_at(rsx_nir_emitter* em,
                                               u32 offset)
{
    rsx_nir_texture texture;
    memset(&texture, 0, sizeof(texture));
    texture.enabled = 1;
    texture.offset = offset;
    texture.location = RSX_NIR_LOCATION_LOCAL;
    texture.format = 0xA5u;          /* LINEAR | A8R8G8B8                */
    texture.dimension = 2;
    texture.mipmaps = 1;
    texture.width = RT_W;
    texture.height = RT_H;
    texture.pitch = RT_W * 4u;
    texture.wrap = 0x00030303u;
    /* Deliberately not the guest identity remap.  A GPU render-target alias
     * is already in native component order and must not be remapped as if it
     * were freshly decoded guest texels. */
    texture.remap = 0u;
    texture.filter = (1u << 16) | (1u << 24);
    rsx_nir_em_texture(em, 0, &texture);
}

static void stage_private_rgba_rt_texture0(rsx_nir_emitter* em)
{
    stage_private_rgba_rt_texture0_at(em, RT565_OFFSET);
}

static void stage_external_color_texture0(rsx_nir_emitter* em)
{
    rsx_nir_texture texture;
    memset(&texture, 0, sizeof(texture));
    texture.enabled = 1;
    texture.offset = COLOR_ALIAS_OFFSET;
    texture.location = RSX_NIR_LOCATION_LOCAL;
    texture.format = 0xA5u;          /* LINEAR | A8R8G8B8                */
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
static void write_triangle_z_bytes(float x0, float y0, float x1, float y1,
                                   float x2, float y2, float z)
{
    u8* p = g_local + VTX_OFFSET;
    const float v[12] = { x0, y0, z, 1.0f,
                          x1, y1, z, 1.0f,
                          x2, y2, z, 1.0f };
    for (int i = 0; i < 12; i++)
        put_be32(p + i * 4, fbits(v[i]));
}

static void write_triangle_z(rsx_guest_pages* pages, float x0, float y0,
                             float x1, float y1, float x2, float y2,
                             float z)
{
    write_triangle_z_bytes(x0, y0, x1, y1, x2, y2, z);
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

/* The production semantic report bridge publishes zero for the modeled
 * ZPASS/ZCULL counters. Archived captures do not contain the separate report
 * aperture, so resolve valid conditional-render addresses to that same
 * deterministic value instead of turning every conditional draw into a
 * missing-callback execution error. */
static int cap_render_condition_read(void* user, u32 dma, u32 offset,
                                     u32* value)
{
    (void)user;
    if (!value || (dma != 0x66626660u && dma != 0xBAD68000u) ||
        offset > 0x00FFFFF0u)
        return -1;
    *value = 0u;
    return 0;
}

static void* test_compile_shader(
    void* user, u32 stage, const char* source, u32 source_length,
    u32 compiler_flags, int* cache_hit, int* compiled)
{
    (void)user;
    ID3DBlob* blob = NULL;
    ID3DBlob* error = NULL;
    if (cache_hit)
        *cache_hit = 0;
    if (compiled)
        *compiled = 0;
    g_content_shader_calls++;
    const HRESULT hr = D3DCompile(
        source, source_length, "content-cache-test", NULL, NULL, "main",
        stage == 'V' ? "vs_5_0" : "ps_5_0", compiler_flags, 0,
        &blob, &error);
    if (error)
        error->lpVtbl->Release(error);
    if (FAILED(hr))
        return NULL;
    if (compiled)
        *compiled = 1;
    return blob;
}

static int test_pso_load(
    void* user, u64 key, u64 vertex_hash, u64 pixel_hash,
    void** data, u32* size)
{
    (void)user; (void)key; (void)vertex_hash; (void)pixel_hash;
    g_content_pso_loads++;
    *data = NULL;
    *size = 0;
    return -1;
}

static int test_pso_store(
    void* user, u64 key, u64 vertex_hash, u64 pixel_hash,
    const void* data, u32 size)
{
    (void)user; (void)key; (void)vertex_hash; (void)pixel_hash;
    CHECK(data && size, "empty driver PSO cache blob");
    g_content_pso_stores++;
    return 0;
}

static void test_pso_free(void* user, void* data)
{
    (void)user;
    free(data);
}

typedef struct cap_data {
    u32 n_blocks, n_records, reg_words, vp_words, const_words;
    u32 disp_count;
    u32 disp[8][4];                  /* width, height, pitch, local offset */
    u32* regs;
    u32* vp;
    u32* consts;
    u32* blocks;                     /* {location, offset, size, data_off} */
    u8* data;
    u64 data_size;
    u32* records;
} cap_data;

typedef struct cap_frame_fifo {
    u32* words;
    u32 word_count;
} cap_frame_fifo;

static int cap_frame_read32(void* user, u32 io, u32* value)
{
    cap_frame_fifo* const fifo = user;
    if (!fifo || !value || (io & 3u) || (io >> 2) >= fifo->word_count)
        return -1;
    *value = fifo->words[io >> 2];
    return 0;
}

enum {
    CAP_METHOD_COUNT = 0x40000,
    CAP_FAILURE_COUNT = 64,
    CAP_TARGET_COUNT = 64,
    CAP_TRANSFER_COUNT = 64,
};

typedef struct cap_method_manifest {
    u64 count;
    u64 supported;
    u64 unsupported;
    u32 first_arg;
    u32 last_arg;
    u32 first_unsupported_arg;
} cap_method_manifest;

typedef struct cap_action_failure {
    u32 kind;
    int rc;
    u64 ordinal;
    u32 color_location, color_offset, color_format, color_target;
    u32 depth_location, depth_offset, depth_format;
    u32 clip_w, clip_h;
    u32 fp_location, fp_offset, fp_control;
    u32 vp_start, primitive, indexed, batch_count, total_count;
    u32 clear_mask, clear_color, clear_depth_stencil;
    rsx_nir_transfer transfer;
} cap_action_failure;

typedef struct cap_target_manifest {
    u32 location, offset, format, width, height;
    u64 writes;
} cap_target_manifest;

typedef struct cap_transfer_manifest {
    rsx_nir_transfer transfer;
    u64 count;
    u64 first_ordinal;
    u64 last_ordinal;
} cap_transfer_manifest;

typedef struct cap_manifest {
    cap_method_manifest* methods;
    u64 method_records;
    u64 data_records;
    u32 unique_methods;
    u32 unique_unsupported_methods;
    cap_action_failure failures[CAP_FAILURE_COUNT];
    u32 failure_count;
    u64 failure_overflow;
    cap_target_manifest targets[CAP_TARGET_COUNT];
    u32 target_count;
    u64 target_overflow;
    cap_transfer_manifest transfers[CAP_TRANSFER_COUNT];
    u32 transfer_count;
    u64 transfer_overflow;
    u32 last_target;
    u32 last_present_buffer;
    int saw_present;
    u64 action_ordinal;
} cap_manifest;

typedef struct cap_exec_trace {
    rsx_nr_exec_ops inner;
    cap_manifest* manifest;
} cap_exec_trace;

static void cap_note_target(cap_manifest* m, const rsx_nir_pipeline* st)
{
    if (!m || !st || !st->surface.clip_w || !st->surface.clip_h)
        return;
    for (u32 i = 0; i < m->target_count; ++i) {
        cap_target_manifest* const target = &m->targets[i];
        if (target->location == st->surface.color_location[0] &&
            target->offset == st->surface.color_offset[0] &&
            target->format == st->surface.color_format &&
            target->width == st->surface.clip_w &&
            target->height == st->surface.clip_h) {
            target->writes++;
            m->last_target = i;
            return;
        }
    }
    if (m->target_count >= CAP_TARGET_COUNT) {
        m->target_overflow++;
        return;
    }
    cap_target_manifest* const target = &m->targets[m->target_count];
    target->location = st->surface.color_location[0];
    target->offset = st->surface.color_offset[0];
    target->format = st->surface.color_format;
    target->width = st->surface.clip_w;
    target->height = st->surface.clip_h;
    target->writes = 1;
    m->last_target = m->target_count++;
}

static int cap_transfer_equal(const rsx_nir_transfer* a,
                              const rsx_nir_transfer* b)
{
    return a->kind == b->kind &&
           a->src_location == b->src_location &&
           a->dst_location == b->dst_location &&
           a->src_offset == b->src_offset &&
           a->dst_offset == b->dst_offset &&
           a->src_pitch == b->src_pitch &&
           a->dst_pitch == b->dst_pitch &&
           a->src_format == b->src_format &&
           a->dst_format == b->dst_format &&
           a->line_length == b->line_length &&
           a->line_count == b->line_count &&
           a->point_x == b->point_x && a->point_y == b->point_y &&
           a->word_count == b->word_count &&
           a->in_x == b->in_x && a->in_y == b->in_y &&
           a->in_w == b->in_w && a->in_h == b->in_h &&
           a->out_x == b->out_x && a->out_y == b->out_y &&
           a->out_w == b->out_w && a->out_h == b->out_h &&
           a->clip_x == b->clip_x && a->clip_y == b->clip_y &&
           a->clip_w == b->clip_w && a->clip_h == b->clip_h &&
           a->ds_dx == b->ds_dx && a->dt_dy == b->dt_dy &&
           a->origin == b->origin &&
           a->interpolator == b->interpolator;
}

static void cap_note_transfer(cap_manifest* m,
                              const rsx_nir_transfer* transfer)
{
    if (!m || !transfer)
        return;
    for (u32 i = 0; i < m->transfer_count; ++i) {
        cap_transfer_manifest* const entry = &m->transfers[i];
        if (!cap_transfer_equal(&entry->transfer, transfer))
            continue;
        entry->count++;
        entry->last_ordinal = m->action_ordinal;
        return;
    }
    if (m->transfer_count >= CAP_TRANSFER_COUNT) {
        m->transfer_overflow++;
        return;
    }
    cap_transfer_manifest* const entry = &m->transfers[m->transfer_count++];
    entry->transfer = *transfer;
    entry->count = 1u;
    entry->first_ordinal = m->action_ordinal;
    entry->last_ordinal = m->action_ordinal;
}

static void cap_note_failure(cap_exec_trace* t, u32 kind, int rc,
                             const rsx_nir_pipeline* st,
                             const rsx_nir_clear* clear,
                             const rsx_nir_draw* draw,
                             const rsx_nir_transfer* transfer)
{
    cap_manifest* const m = t->manifest;
    if (!m)
        return;
    if (m->failure_count >= CAP_FAILURE_COUNT) {
        m->failure_overflow++;
        return;
    }
    cap_action_failure* const failure = &m->failures[m->failure_count++];
    memset(failure, 0, sizeof(*failure));
    failure->kind = kind;
    failure->rc = rc;
    failure->ordinal = m->action_ordinal;
    if (st) {
        failure->color_location = st->surface.color_location[0];
        failure->color_offset = st->surface.color_offset[0];
        failure->color_format = st->surface.color_format;
        failure->color_target = st->surface.color_target;
        failure->depth_location = st->surface.zeta_location;
        failure->depth_offset = st->surface.zeta_offset;
        failure->depth_format = st->surface.depth_format;
        failure->clip_w = st->surface.clip_w;
        failure->clip_h = st->surface.clip_h;
        failure->fp_location = st->fragment_program.location;
        failure->fp_offset = st->fragment_program.offset;
        failure->fp_control = st->fragment_program.control;
        failure->vp_start = st->vertex_program.start_slot;
    }
    if (clear) {
        failure->clear_mask = clear->mask;
        failure->clear_color = clear->color_value;
        failure->clear_depth_stencil =
            (clear->depth_value << 8) | (clear->stencil_value & 0xFFu);
    }
    if (draw) {
        failure->primitive = draw->primitive;
        failure->indexed = draw->indexed;
        failure->batch_count = draw->batch_count;
        failure->total_count = draw->total_count;
    }
    if (transfer)
        failure->transfer = *transfer;
}

static int cap_trace_clear(void* user, const rsx_nir_pipeline* st,
                           const rsx_nir_clear* clear)
{
    cap_exec_trace* const t = user;
    t->manifest->action_ordinal++;
    const int rc = t->inner.clear
        ? t->inner.clear(t->inner.user, st, clear) : -1;
    if (rc)
        cap_note_failure(t, RSX_NIR_OP_CLEAR, rc, st, clear, NULL, NULL);
    else if (clear->mask & 0xF0u)
        cap_note_target(t->manifest, st);
    return rc;
}

static int cap_trace_draw(void* user, const rsx_nir_pipeline* st,
                          const u32* vp_words, u32 vp_word_count,
                          const rsx_nir_draw* draw, const u32* batches)
{
    cap_exec_trace* const t = user;
    t->manifest->action_ordinal++;
    const int rc = t->inner.draw
        ? t->inner.draw(t->inner.user, st, vp_words, vp_word_count,
                        draw, batches) : -1;
    if (rc)
        cap_note_failure(t, RSX_NIR_OP_DRAW, rc, st, NULL, draw, NULL);
    else
        cap_note_target(t->manifest, st);
    return rc;
}

static int cap_trace_transfer(void* user, const rsx_nir_pipeline* st,
                              const rsx_nir_transfer* transfer,
                              const u32* words)
{
    cap_exec_trace* const t = user;
    t->manifest->action_ordinal++;
    const int rc = t->inner.transfer
        ? t->inner.transfer(t->inner.user, st, transfer, words) : -1;
    if (rc)
        cap_note_failure(t, RSX_NIR_OP_TRANSFER, rc, st, NULL, NULL,
                         transfer);
    else
        cap_note_transfer(t->manifest, transfer);
    return rc;
}

static int cap_trace_present(void* user, u32 buffer)
{
    cap_exec_trace* const t = user;
    t->manifest->action_ordinal++;
    t->manifest->last_present_buffer = buffer;
    t->manifest->saw_present = 1;
    return t->inner.present
        ? t->inner.present(t->inner.user, buffer) : -1;
}

static void cap_trace_flush(void* user)
{
    cap_exec_trace* const t = user;
    if (t->inner.flush)
        t->inner.flush(t->inner.user);
}

static int cap_manifest_init(cap_manifest* m)
{
    memset(m, 0, sizeof(*m));
    m->methods = calloc(CAP_METHOD_COUNT, sizeof(*m->methods));
    return m->methods ? 0 : -1;
}

static void cap_manifest_free(cap_manifest* m)
{
    free(m->methods);
    memset(m, 0, sizeof(*m));
}

static int cap_load(const char* path, cap_data* c)
{
    memset(c, 0, sizeof(*c));
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "capture: cannot open %s\n", path);
        return -1;
    }
    u32 header[8];
    if (fread(header, 4, 8, fp) != 8 || memcmp(header, "RXS1", 4) != 0 ||
        (header[1] != 2 && header[1] != 3) ||
        fread(&c->disp_count, 4, 1, fp) != 1 ||
        fread(c->disp, 16, 8, fp) != 8 || c->disp_count > 8u ||
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
                           const cap_target_manifest* target,
                           const char* name)
{
    if (!target || !target->width || !target->height)
        return -1;
    const u32 width = target->width;
    const u32 height = target->height;
    const size_t bytes = (size_t)width * height * 4u;
    u8* const pixels = malloc(bytes);
    if (!pixels || rsx_nr_d3d12_read_rt(
            sink, target->location, target->offset, width, height,
            pixels) != 0) {
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

static int cap_dump_depth_raw(rsx_nr_d3d12* sink, const char* path,
                              u32 space, u32 offset, u32 format,
                              u32 width, u32 height)
{
    const size_t count = (size_t)width * height;
    float* const depth = malloc(count * sizeof(*depth));
    if (!depth || rsx_nr_d3d12_read_depth(
            sink, space, offset, format, width, height, depth) != 0) {
        free(depth);
        return -1;
    }
    FILE* const fp = fopen(path, "wb");
    int ok = fp != NULL;
    if (ok)
        ok = fwrite(depth, sizeof(*depth), count, fp) == count;
    if (fp && fclose(fp) != 0)
        ok = 0;
    free(depth);
    return ok ? 0 : -1;
}

static int cap_run_once(cap_data* c, u64* rt_hash, char* stats_line,
                        size_t stats_size, int dump_outputs,
                        cap_manifest* manifest)
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
        /* The earliest SDK-context capture predates memory-block export but
         * contains a real inline transfer to offset 0x00800000. Give that
         * incomplete archive a bounded 16 MiB window in both RSX spaces;
         * zero initialization is sufficient to validate ordered transfer
         * execution, while any access beyond the window remains an explicit
         * failure. Complete captures continue to size from their snapshots. */
        if (!c->n_blocks && g_cap.size[s] < 0x01000000u)
            g_cap.size[s] = 0x01000000u;
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
    for (u32 i = 0; i < c->disp_count; ++i) {
        rsx_nr_d3d12_set_display_buffer(
            sink, i, RSX_NIR_LOCATION_LOCAL, c->disp[i][3],
            c->disp[i][0], c->disp[i][1]);
    }
    rsx_nr_d3d12_set_render_condition_reader(
        sink, cap_render_condition_read, NULL);
    const char* allow_flow_txl = getenv("YZ_NR_CAPTURE_FLOW_TXL_ORACLE");
    const char* const strict_text = getenv("YZ_NR_CAPTURE_STRICT_FRAME");
    const int strict_frame = strict_text && strict_text[0] &&
        strcmp(strict_text, "0") != 0;
    if (strict_frame || (allow_flow_txl && allow_flow_txl[0] &&
        strcmp(allow_flow_txl, "0") != 0))
        CHECK(rsx_nr_d3d12_set_coherent_section_mode(sink, 1) == 0,
              "capture coherent-section mode refused before execution");

    rsx_nr_ring ring;
    rsx_nr_tokens tokens;
    rsx_nr_tokens_init(&tokens);
    if (rsx_nr_ring_init(&ring, 4096, 1u << 19))
        return -1;
    rsx_nr_exec_ops ops;
    memset(&ops, 0, sizeof(ops));
    cap_exec_trace trace;
    memset(&trace, 0, sizeof(trace));
    trace.manifest = manifest;
    rsx_nr_d3d12_get_exec_ops(sink, &trace.inner);
    ops = trace.inner;
    ops.user = &trace;
    ops.clear = cap_trace_clear;
    ops.draw = cap_trace_draw;
    ops.transfer = cap_trace_transfer;
    ops.present = cap_trace_present;
    ops.flush = cap_trace_flush;
    rsx_nr_backend be;
    rsx_nr_backend_init(&be, &ring, &tokens, &ops);

    rsx_nir_adapter* ad = malloc(sizeof(*ad));
    if (!ad)
        return -1;
    rsx_nir_sink k = rsx_nr_ring_sink(&ring);
    rsx_nir_adapter_init_sink(ad, &k);
    rsx_nir_adapter_seed(ad, c->regs, c->reg_words, c->vp, c->vp_words,
                         c->consts, c->const_words);

    cap_frame_fifo frame_fifo;
    memset(&frame_fifo, 0, sizeof(frame_fifo));
    rsx_nr_frame_owner frame_owner;
    memset(&frame_owner, 0, sizeof(frame_owner));
    u32 frame_get = 0x1000u;
    if (strict_frame) {
        frame_fifo.word_count = 0x800000u / 4u;
        frame_fifo.words = calloc(frame_fifo.word_count, sizeof(u32));
        if (!frame_fifo.words)
            return -1;
        rsx_nr_frame_owner_init(
            &frame_owner, ad, &be, &ring, cap_frame_read32, &frame_fifo,
            NULL, NULL, NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL);
    }

    u32 completed_draws = 0;
    u32 stop_after_draw = 0;
    {
        const char* const stop = getenv("YZ_NR_CAPTURE_STOP_AFTER_DRAW");
        if (stop && stop[0])
            stop_after_draw = (u32)strtoul(stop, NULL, 0);
    }
    int ring_fault = 0;
    for (u32 i = 0; i < c->n_records; i++) {
        u32 m = c->records[i * 2];
        u32 a = c->records[i * 2 + 1];
        if (m & 0x80000000u) {
            /* drain so preceding draws see pre-apply bytes, then apply */
            while (rsx_nr_backend_step(&be) == RSX_NR_STEP_EXECUTED)
                ;
            cap_apply_block(c, rsx_nr_d3d12_pages(sink), a);
            manifest->data_records++;
            continue;
        }
        manifest->method_records++;
        if (m < CAP_METHOD_COUNT) {
            cap_method_manifest* const method = &manifest->methods[m];
            const int supported =
                rsx_nir_adapter_method_supported(ad, m, a);
            if (!method->count) {
                method->first_arg = a;
                method->first_unsupported_arg = supported ? 0u : a;
                manifest->unique_methods++;
                if (!supported)
                    manifest->unique_unsupported_methods++;
            } else if (!supported && !method->unsupported) {
                method->first_unsupported_arg = a;
                manifest->unique_unsupported_methods++;
            }
            method->count++;
            method->last_arg = a;
            if (supported)
                method->supported++;
            else
                method->unsupported++;
        }
        if (strict_frame) {
            if (frame_get > 0x7FFFF8u)
                frame_get = 0x1000u;
            frame_fifo.words[frame_get >> 2] = (1u << 18) | m;
            frame_fifo.words[(frame_get + 4u) >> 2] = a;
            u32 next_get = frame_get;
            u32 next_ret = ~0u;
            const rsx_nr_frame_step_result result =
                rsx_nr_frame_owner_step(
                    &frame_owner, frame_get, frame_get + 8u, ~0u,
                    &next_get, &next_ret);
            if (result != RSX_NR_FRAME_ADVANCED ||
                next_get != frame_get + 8u || next_ret != ~0u) {
                fprintf(stderr,
                        "strict-frame fault result=%u record=%u "
                        "owner-kind=%u get=%08X method=%05X arg=%08X "
                        "index=%u backend-errors=%llu\n",
                        result, i, frame_owner.failure.kind,
                        frame_owner.failure.get,
                        frame_owner.failure.method,
                        frame_owner.failure.argument,
                        frame_owner.failure.argument_index,
                        be.stats.exec_errors);
                if (manifest->failure_count) {
                    const cap_action_failure* const failure =
                        &manifest->failures[manifest->failure_count - 1u];
                    fprintf(stderr,
                            "strict-frame action kind=%u rc=%d ordinal=%llu "
                            "rt=%u:%08X/fmt=%u/target=%u/%ux%u "
                            "z=%u:%08X/fmt=%u fp=%u:%08X/%08X "
                            "vp=%u draw=%u/%u/%u/%u\n",
                            failure->kind, failure->rc, failure->ordinal,
                            failure->color_location,
                            failure->color_offset,
                            failure->color_format,
                            failure->color_target,
                            failure->clip_w, failure->clip_h,
                            failure->depth_location,
                            failure->depth_offset,
                            failure->depth_format,
                            failure->fp_location,
                            failure->fp_offset,
                            failure->fp_control,
                            failure->vp_start, failure->primitive,
                            failure->indexed, failure->batch_count,
                            failure->total_count);
                    if (failure->kind == RSX_NIR_OP_TRANSFER) {
                        const rsx_nir_transfer* const transfer =
                            &failure->transfer;
                        fprintf(stderr,
                                "strict-frame transfer kind=%u "
                                "src=%u:%08X pitch=%u fmt=%u "
                                "dst=%u:%08X pitch=%u fmt=%u "
                                "point=%u,%u size=%ux%u words=%u\n",
                                transfer->kind,
                                transfer->src_location,
                                transfer->src_offset,
                                transfer->src_pitch,
                                transfer->src_format,
                                transfer->dst_location,
                                transfer->dst_offset,
                                transfer->dst_pitch,
                                transfer->dst_format,
                                transfer->point_x,
                                transfer->point_y,
                                transfer->size_w,
                                transfer->size_h,
                                transfer->word_count);
                    }
                }
                {
                    rsx_nr_d3d12_stats failure_stats;
                    rsx_nr_d3d12_get_stats(sink, &failure_stats);
                    fprintf(stderr,
                            "strict-frame backend failure topo=%llu rt=%llu "
                            "plan=%llu pso=%llu idx=%llu fp=%llu tex=%llu "
                            "compile=%llu residency=%llu texfail=%llu\n",
                            failure_stats.unsup_draw_topology,
                            failure_stats.unsup_draw_rt,
                            failure_stats.unsup_draw_plan,
                            failure_stats.unsup_draw_pso,
                            failure_stats.unsup_draw_index,
                            failure_stats.unsup_draw_fp,
                            failure_stats.unsup_draw_texture,
                            failure_stats.compile_failures,
                            failure_stats.residency_failures,
                            failure_stats.texture_failures);
                }
                ring_fault = 1;
                break;
            }
            frame_get = next_get;
        } else {
            rsx_nir_adapter_method(ad, m, a);
        }
        if (m == 0x1808u && a == 0u)
            completed_draws++;
        if (rsx_nr_ring_reject_sticky(&ring)) {
            ring_fault = 1;
            break;
        }
        /* keep the ring shallow */
        while (rsx_nr_ring_depth(&ring) > 2048 &&
               rsx_nr_backend_step(&be) == RSX_NR_STEP_EXECUTED)
            ;
        if (stop_after_draw && completed_draws >= stop_after_draw)
            break;
    }
    rsx_nir_adapter_finish(ad);
    while (rsx_nr_backend_step(&be) == RSX_NR_STEP_EXECUTED)
        ;

    rsx_nr_d3d12_stats st;
    rsx_nr_d3d12_get_stats(sink, &st);
    snprintf(stats_line, stats_size,
             "methods=%llu unique=%u unsupported_methods=%u data=%llu "
             "clears=%llu draws=%llu (restart=%llu) batches=%llu "
             "presents=%llu xfers=%llu gpu_xfers=%llu/%llu "
             "pso=%llu(+%lluh) unsup_draw=%llu "
             "[topo=%llu rt=%llu plan=%llu pso=%llu idx=%llu fp=%llu "
             "tex=%llu] real_fp=%llu tex_draw=%llu "
             "tex_cache=%llu(+%lluh,%llur) tex_fail=%llu alias=%llu "
             "depth_snap=%llu/%llu "
             "unsup_clear=%llu unsup_xfer=%llu compile_fail=%llu "
             "exec_err=%llu submits=%llu "
             "topo_id=[3:%llu 7:%llu 8:%llu 9:%llu 10:%llu] "
             "rtfmt=[1:%llu 2:%llu 3:%llu 6:%llu 7:%llu 9:%llu 10:%llu "
             "11:%llu 12:%llu 13:%llu 14:%llu 15:%llu 16:%llu] "
             "targets=%u target_overflow=%llu failure_keys=%u "
             "failure_overflow=%llu",
             manifest->method_records, manifest->unique_methods,
             manifest->unique_unsupported_methods, manifest->data_records,
             st.clears, st.draws, st.restart_draws, st.draw_batches,
             st.presents, st.transfers, st.transfer_gpu_readbacks,
             st.transfer_gpu_uploads, st.pso_builds, st.pso_hits,
             st.unsupported_draws, st.unsup_draw_topology, st.unsup_draw_rt,
             st.unsup_draw_plan, st.unsup_draw_pso, st.unsup_draw_index,
             st.unsup_draw_fp, st.unsup_draw_texture, st.real_fp_draws,
             st.texture_draws, st.texture_builds, st.texture_hits,
             st.texture_refreshes, st.texture_failures, st.rt_alias_binds,
             st.depth_snapshot_builds, st.depth_snapshot_resolves,
             st.unsupported_clears, st.unsupported_transfers,
             st.compile_failures, be.stats.exec_errors,
             st.queue_submissions,
             st.unsup_topology_id[3], st.unsup_topology_id[7],
             st.unsup_topology_id[8], st.unsup_topology_id[9],
             st.unsup_topology_id[10], st.unsup_rt_format[1],
             st.unsup_rt_format[2], st.unsup_rt_format[3],
             st.unsup_rt_format[6], st.unsup_rt_format[7],
             st.unsup_rt_format[9], st.unsup_rt_format[10],
             st.unsup_rt_format[11], st.unsup_rt_format[12],
             st.unsup_rt_format[13], st.unsup_rt_format[14],
             st.unsup_rt_format[15], st.unsup_rt_format[16],
             manifest->target_count, manifest->target_overflow,
             manifest->failure_count, manifest->failure_overflow);

    *rt_hash = 0;
    int presented_readback = -2;
    u32 oracle_offset = 0;
    int have_oracle_offset = 0;
    {
        const char* const oracle = getenv("YZ_NR_CAPTURE_ORACLE_RT_OFFSET");
        if (oracle && oracle[0]) {
            oracle_offset = (u32)strtoul(oracle, NULL, 0);
            have_oracle_offset = 1;
        }
    }
    if (have_oracle_offset) {
        for (u32 i = 0;; ++i) {
            cap_target_manifest target = {0};
            if (rsx_nr_d3d12_rt_info(
                    sink, i, &target.location, &target.offset,
                    &target.format, &target.width, &target.height) != 0)
                break;
            if (target.offset != oracle_offset)
                continue;
            const size_t bytes =
                (size_t)target.width * target.height * 4u;
            u8* px = malloc(bytes);
            if (px)
                presented_readback = rsx_nr_d3d12_read_rt(
                    sink, target.location, target.offset,
                    target.width, target.height, px);
            if (px && presented_readback == 0)
                *rt_hash = fnv64(px, bytes);
            free(px);
            break;
        }
    } else if (manifest->saw_present &&
        manifest->last_present_buffer < c->disp_count) {
        const u32* const display = c->disp[manifest->last_present_buffer];
        const size_t bytes = (size_t)display[0] * display[1] * 4u;
        u8* px = malloc(bytes);
        if (px)
            presented_readback = rsx_nr_d3d12_read_rt(
                sink, RSX_NIR_LOCATION_LOCAL, display[3],
                display[0], display[1], px);
        if (px && presented_readback == 0)
            *rt_hash = fnv64(px, bytes);
        free(px);
    }
    if (!*rt_hash)
        fprintf(stderr,
                "capture: presented readback unavailable saw=%d buffer=%u "
                "display-count=%u rc=%d\n",
                manifest->saw_present, manifest->last_present_buffer,
                c->disp_count, presented_readback);

    if (dump_outputs) {
        const char* dump_dir = getenv("YZ_NR_CAPTURE_DUMP_DIR");
        int dumped = 0;
        if (dump_dir && dump_dir[0]) {
            for (u32 i = 0;; ++i) {
                cap_target_manifest target = {0};
                if (rsx_nr_d3d12_rt_info(
                        sink, i, &target.location, &target.offset,
                        &target.format, &target.width, &target.height) != 0)
                    break;
                if (have_oracle_offset &&
                    target.offset != oracle_offset)
                    continue;
                char name[160];
                snprintf(name, sizeof(name),
                         "native_rt_%02u_%u_%08X_%ux%u_f%u.ppm",
                         i, target.location, target.offset,
                         target.width, target.height, target.format);
                dumped |= cap_dump_rt_ppm(
                    sink, dump_dir, &target, name) == 0;
            }
        }
        if (dump_dir && dump_dir[0] && !dumped) {
            fprintf(stderr, "capture: failed to dump oracle render targets\n");
            ring_fault = 1;
        }
    }
    {
        const char* const path = getenv("YZ_NR_CAPTURE_DEPTH_DUMP");
        if (path && path[0]) {
            const char* const offset_text =
                getenv("YZ_NR_CAPTURE_DEPTH_OFFSET");
            const char* const width_text =
                getenv("YZ_NR_CAPTURE_DEPTH_WIDTH");
            const char* const height_text =
                getenv("YZ_NR_CAPTURE_DEPTH_HEIGHT");
            const u32 offset = offset_text
                ? (u32)strtoul(offset_text, NULL, 0) : 0x00B40000u;
            const u32 width = width_text
                ? (u32)strtoul(width_text, NULL, 0) : 1024u;
            const u32 height = height_text
                ? (u32)strtoul(height_text, NULL, 0) : 768u;
            if (cap_dump_depth_raw(
                    sink, path, RSX_NIR_LOCATION_LOCAL, offset, 2u,
                    width, height) != 0) {
                fprintf(stderr,
                        "capture: failed to dump depth %u:%08X %ux%u\n",
                        RSX_NIR_LOCATION_LOCAL, offset, width, height);
                ring_fault = 1;
            }
        }
    }

    free(frame_fifo.words);
    free(ad);
    rsx_nr_ring_destroy(&ring);
    rsx_nr_d3d12_destroy(sink);
    free(g_cap.arena[0]);
    free(g_cap.arena[1]);
    memset(&g_cap, 0, sizeof(g_cap));
    return ring_fault ? -1 : 0;
}

static void cap_write_manifest(const char* capture_path,
                               const cap_manifest* manifest)
{
    const char* const dir = getenv("YZ_NR_CAPTURE_MANIFEST_DIR");
    if (!dir || !dir[0] || !manifest || !manifest->methods)
        return;
    const char* base = strrchr(capture_path, '\\');
    const char* slash = strrchr(capture_path, '/');
    if (!base || (slash && slash > base))
        base = slash;
    base = base ? base + 1 : capture_path;
    char stem[256];
    size_t stem_length = strlen(base);
    if (stem_length >= sizeof(stem))
        stem_length = sizeof(stem) - 1u;
    memcpy(stem, base, stem_length);
    stem[stem_length] = 0;
    for (size_t i = 0; i < stem_length; ++i)
        if (!((stem[i] >= 'a' && stem[i] <= 'z') ||
              (stem[i] >= 'A' && stem[i] <= 'Z') ||
              (stem[i] >= '0' && stem[i] <= '9') || stem[i] == '-' ||
              stem[i] == '_'))
            stem[i] = '_';
    char path[1024];
    if (snprintf(path, sizeof(path), "%s\\%s.full-native-manifest.csv",
                 dir, stem) <= 0)
        return;
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "capture: cannot write manifest %s\n", path);
        return;
    }
    fprintf(fp,
            "record,capture,method_or_kind,count,supported,unsupported,"
            "first_arg,last_arg,first_unsupported_arg,details\n");
    fprintf(fp,
            "summary,%s,methods,%llu,%u,%u,0x%08X,0x%08X,0x%08X,"
            "data=%llu;failures=%u;targets=%u;transfers=%u\n",
            base, manifest->method_records, manifest->unique_methods,
            manifest->unique_unsupported_methods, 0u, 0u, 0u,
            manifest->data_records, manifest->failure_count,
            manifest->target_count, manifest->transfer_count);
    for (u32 method = 0; method < CAP_METHOD_COUNT; ++method) {
        const cap_method_manifest* const entry =
            &manifest->methods[method];
        if (!entry->count)
            continue;
        fprintf(fp,
                "method,%s,0x%05X,%llu,%llu,%llu,0x%08X,0x%08X,"
                "0x%08X,\n",
                base, method, entry->count, entry->supported,
                entry->unsupported, entry->first_arg, entry->last_arg,
                entry->first_unsupported_arg);
    }
    for (u32 i = 0; i < manifest->failure_count; ++i) {
        const cap_action_failure* const f = &manifest->failures[i];
        fprintf(fp,
                "failure,%s,%u,1,0,1,0x%08X,0x%08X,0x%08X,"
                "ordinal=%llu;rc=%d;color=%u:0x%08X/f%u/t%u/%ux%u;"
                "depth=%u:0x%08X/f%u;fp=%u:0x%08X/c0x%08X;vp=%u;"
                "draw=%u/%u/%u/%u;clear=0x%08X/0x%08X/0x%08X;"
                "xfer=%u,src=%u:0x%08X,p%u,f%u,dst=%u:0x%08X,p%u,f%u,"
                "line=%ux%u,words=%u,in=%ux%u,out=%u,%u+%ux%u,"
                "step=0x%08X/0x%08X\n",
                base, f->kind, f->clear_mask, f->clear_color,
                f->clear_depth_stencil, f->ordinal, f->rc,
                f->color_location, f->color_offset, f->color_format,
                f->color_target, f->clip_w, f->clip_h,
                f->depth_location, f->depth_offset, f->depth_format,
                f->fp_location, f->fp_offset, f->fp_control, f->vp_start,
                f->primitive, f->indexed, f->batch_count, f->total_count,
                f->clear_mask, f->clear_color, f->clear_depth_stencil,
                f->transfer.kind, f->transfer.src_location,
                f->transfer.src_offset, f->transfer.src_pitch,
                f->transfer.src_format, f->transfer.dst_location,
                f->transfer.dst_offset, f->transfer.dst_pitch,
                f->transfer.dst_format, f->transfer.line_length,
                f->transfer.line_count, f->transfer.word_count,
                f->transfer.in_w, f->transfer.in_h, f->transfer.out_x,
                f->transfer.out_y, f->transfer.out_w, f->transfer.out_h,
                f->transfer.ds_dx, f->transfer.dt_dy);
    }
    for (u32 i = 0; i < manifest->target_count; ++i) {
        const cap_target_manifest* const target = &manifest->targets[i];
        fprintf(fp,
                "target,%s,%u,%llu,0,0,0x%08X,0x%08X,0x%08X,"
                "location=%u;format=%u;width=%u;height=%u\n",
                base, i, target->writes, target->offset, target->width,
                target->height, target->location, target->format,
                target->width, target->height);
    }
    for (u32 i = 0; i < manifest->transfer_count; ++i) {
        const cap_transfer_manifest* const entry = &manifest->transfers[i];
        const rsx_nir_transfer* const t = &entry->transfer;
        fprintf(fp,
                "transfer,%s,%u,%llu,%llu,0,0x%08X,0x%08X,0x%08X,"
                "ordinal=%llu..%llu;kind=%u;src=%u:0x%08X,p%u,f%u;"
                "dst=%u:0x%08X,p%u,f%u;line=%ux%u;point=%u,%u;"
                "words=%u;in=%u,%u+%ux%u;out=%u,%u+%ux%u;"
                "clip=%u,%u+%ux%u;step=0x%08X/0x%08X;"
                "origin=%u;interp=%u\n",
                base, i, entry->count, entry->count,
                t->src_offset, t->dst_offset, t->kind,
                entry->first_ordinal, entry->last_ordinal, t->kind,
                t->src_location, t->src_offset, t->src_pitch,
                t->src_format, t->dst_location, t->dst_offset,
                t->dst_pitch, t->dst_format, t->line_length,
                t->line_count, t->point_x, t->point_y, t->word_count,
                t->in_x, t->in_y, t->in_w, t->in_h, t->out_x,
                t->out_y, t->out_w, t->out_h, t->clip_x, t->clip_y,
                t->clip_w, t->clip_h, t->ds_dx, t->dt_dy,
                t->origin, t->interpolator);
    }
    fclose(fp);
}

static void run_capture_backend(const char* path)
{
    cap_data c;
    if (cap_load(path, &c)) {
        CHECK(0, "capture %s failed to load", path);
        return;
    }
    char line1[2048], line2[2048];
    u64 h1 = 0, h2 = 0;
    cap_manifest manifest1 = {0}, manifest2 = {0};
    if (cap_manifest_init(&manifest1) || cap_manifest_init(&manifest2)) {
        CHECK(0, "capture manifest allocation failed");
        cap_manifest_free(&manifest1);
        cap_manifest_free(&manifest2);
        cap_free(&c);
        return;
    }
    int r1 = cap_run_once(&c, &h1, line1, sizeof(line1), 1, &manifest1);
    if (r1 == -2) {
        printf("capture backend leg: SKIP (no WARP device)\n");
        cap_manifest_free(&manifest1);
        cap_manifest_free(&manifest2);
        cap_free(&c);
        return;
    }
    CHECK(r1 == 0, "capture backend run 1 faulted");
    if (r1 != 0) {
        cap_manifest_free(&manifest1);
        cap_manifest_free(&manifest2);
        cap_free(&c);
        return;
    }
    {
        const char* const single_run = getenv("YZ_NR_CAPTURE_SINGLE_RUN");
        if (single_run && single_run[0] && strcmp(single_run, "0") != 0) {
            CHECK(h1 != 0u,
                  "capture presented target could not be read back");
            cap_write_manifest(path, &manifest1);
            printf("capture backend %s:\n  %s\n  rt_hash=%016llX\n",
                   path, line1, (unsigned long long)h1);
            cap_manifest_free(&manifest1);
            cap_manifest_free(&manifest2);
            cap_free(&c);
            return;
        }
    }
    int r2 = cap_run_once(&c, &h2, line2, sizeof(line2), 0, &manifest2);
    CHECK(r2 == 0, "capture backend run 2 faulted");
    CHECK(strcmp(line1, line2) == 0, "capture stats nondeterministic:\n  %s\n  %s",
          line1, line2);
    CHECK(h1 != 0u && h2 != 0u,
          "capture presented target could not be read back");
    CHECK(h1 == h2, "capture RT hash nondeterministic %016llX/%016llX",
          (unsigned long long)h1, (unsigned long long)h2);
    cap_write_manifest(path, &manifest1);
    printf("capture backend %s:\n  %s\n  rt_hash=%016llX\n", path, line1,
           (unsigned long long)h1);
    cap_manifest_free(&manifest1);
    cap_manifest_free(&manifest2);
    cap_free(&c);
}

typedef struct broker_color_test {
    ID3D12Resource* resource;
    ID3D12Resource* color_alias;
    ID3D12Resource* depth;
    u32 calls;
    u32 color_lookup_calls;
    u32 color_create_calls;
    u32 depth_calls;
    u32 depth_lookup_calls;
    u32 depth_create_calls;
    u32 depth_resolve_calls;
    int depth_resolve_fail;
} broker_color_test;

static int borrow_rgba_for_logical_565(
    void* user, u32 space, u32 offset, u32 width, u32 height,
    int create, void** resource, u32* dxgi_format,
    u32* resource_width, u32* resource_height)
{
    broker_color_test* broker = (broker_color_test*)user;
    if (!broker || !resource || !dxgi_format || !resource_width ||
        !resource_height || space || width != RT_W || height != RT_H)
        return -1;
    ID3D12Resource* selected = NULL;
    if (offset == RT565_OFFSET && create) {
        selected = broker->resource;
        broker->color_create_calls++;
    } else if (offset == COLOR_ALIAS_OFFSET && !create) {
        selected = broker->color_alias;
        broker->color_lookup_calls++;
    }
    if (!selected)
        return -1;
    selected->lpVtbl->AddRef(selected);
    *resource = selected;
    *dxgi_format = (u32)DXGI_FORMAT_R8G8B8A8_UNORM;
    *resource_width = RT_W;
    *resource_height = RT_H;
    broker->calls++;
    return 0;
}

static int borrow_depth_for_alias_test(
    void* user, u32 space, u32 offset, u32 depth_format,
    u32 width, u32 height, int create, void** resource, u32* resource_format,
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
    if (create)
        broker->depth_create_calls++;
    else
        broker->depth_lookup_calls++;
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
    ID3D12Resource* color_alias_resource = NULL;
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

    hr = device->lpVtbl->CreateCommittedResource(
        device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource,
        (void**)&color_alias_resource);
    CHECK(SUCCEEDED(hr) && color_alias_resource,
          "could not create established-only color alias resource");
    if (FAILED(hr) || !color_alias_resource)
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

    broker_color_test broker;
    memset(&broker, 0, sizeof(broker));
    broker.resource = resource;
    broker.color_alias = color_alias_resource;
    broker.depth = depth_resource;
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

    rsx_nir_texture external_depth;
    memset(&external_depth, 0, sizeof(external_depth));
    external_depth.enabled = 1;
    external_depth.offset = ZETA_OFFSET;
    external_depth.location = RSX_NIR_LOCATION_LOCAL;
    external_depth.format = 0xB0u;
    external_depth.dimension = 2u;
    external_depth.mipmaps = 1u;
    external_depth.width = RT_W;
    external_depth.height = RT_H;
    external_depth.pitch = RT_W * 4u;
    CHECK(rsx_nr_d3d12_validate_depth_sample_alias(
              sink, &external_depth) == 0,
          "lookup-only established depth alias was refused");
    CHECK(broker.depth_lookup_calls == 1u &&
              broker.depth_create_calls == 0u,
          "depth alias validation lookup=%u create=%u",
          broker.depth_lookup_calls, broker.depth_create_calls);

    /* A producer owned entirely by the established renderer never enters
     * this backend as a native depth target. Its first native appearance can
     * be a sampled DEPTH24_D8 binding. Discover that existing resource with a
     * lookup-only broker request; preflight must not create a second target. */
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
          "lookup-only borrowed zeta sample had %llu execution errors",
          be.stats.exec_errors);
    CHECK(broker.depth_lookup_calls >= 1u &&
              broker.depth_create_calls == 0u &&
              broker.depth_resolve_calls >= 1u,
          "external depth was not lazily imported lookup=%u create=%u resolve=%u",
          broker.depth_lookup_calls, broker.depth_create_calls,
          broker.depth_resolve_calls);

    write_test_fp();
    stage_frame_state(&em);
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

    /* An established-only color producer may first appear to native code as
     * a sampled dependency. COLOR_ALIAS_OFFSET is outside guest memory and
     * has never been a native target, so successful preflight/execution
     * proves an exact lookup-only broker import rather than stale guest
     * decoding or target creation. Pixel content is intentionally not an
     * oracle here; the resource is opaque established-renderer output. */
    write_tex_fp();
    stage_frame_state(&em);
    rsx_nir_em_surface(&em, &s565);
    stage_external_color_texture0(&em);
    write_triangle(rsx_nr_d3d12_pages(sink),
                   -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f);
    {
        const u32 batch[2] = {0u, 3u};
        rsx_nir_em_draw(&em, 5u, 0u, batch, 1u);
    }
    rsx_nr_backend_run(&be, 0u);
    CHECK(be.stats.exec_errors == 0u,
          "established-only color alias draw had %llu execution errors",
          be.stats.exec_errors);
    CHECK(broker.color_lookup_calls == 1u,
          "color dependency import lookup count=%u",
          broker.color_lookup_calls);
    CHECK(broker.color_create_calls >= 1u,
          "native target broker create was not exercised");

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
    if (color_alias_resource)
        color_alias_resource->lpVtbl->Release(color_alias_resource);
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

    CHECK(rsx_nr_d3d12_flush_report_dependency(sink) == 0,
          "report dependency did not retire the leased shared list");
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

static void test_submit_attribution_gate(void)
{
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
    rsx_nir_clear clear = {0xF0u, 0xFF112233u, 0xFFFFFFu, 0u};

    _putenv_s("YZ_NR_SUBMIT_ATTRIBUTION", "");
    _putenv_s("YZ_NR_RSX_TAIL_BREAKDOWN", "");
    rsx_nr_d3d12* sink = rsx_nr_d3d12_create(
        NULL, LOCAL_SIZE, MAIN_SIZE, arena_ptr, arena_wptr, NULL);
    CHECK(sink != NULL, "default-off attribution sink creation failed");
    if (sink) {
        rsx_nr_exec_ops ops;
        rsx_nr_d3d12_stats stats;
        memset(&ops, 0, sizeof(ops));
        rsx_nr_d3d12_get_exec_ops(sink, &ops);
        CHECK(ops.clear(ops.user, &state, &clear) == 0,
              "default-off attribution clear failed");
        ops.flush_reason(ops.user, RSX_NR_FLUSH_REFERENCE);
        rsx_nr_d3d12_get_stats(sink, &stats);
        CHECK(stats.submit_attribution_qpc_frequency == 0u &&
                  stats.submit_cause[
                      RSX_NR_D3D12_SUBMIT_REFERENCE_PUBLICATION].submissions ==
                      0u,
              "default-off attribution performed clocks/accounting");
        rsx_nr_d3d12_destroy(sink);
    }

    _putenv_s("YZ_NR_RSX_TAIL_BREAKDOWN", "1");
    sink = rsx_nr_d3d12_create(
        NULL, LOCAL_SIZE, MAIN_SIZE, arena_ptr, arena_wptr, NULL);
    CHECK(sink != NULL, "enabled tail-breakdown sink creation failed");
    if (sink) {
        rsx_nr_exec_ops ops;
        rsx_nr_d3d12_stats stats;
        memset(&ops, 0, sizeof(ops));
        rsx_nr_d3d12_get_exec_ops(sink, &ops);
        CHECK(ops.clear(ops.user, &state, &clear) == 0,
              "enabled tail-breakdown clear failed");
        ops.flush_reason(ops.user, RSX_NR_FLUSH_REFERENCE);
        CHECK(ops.present(ops.user, 0u) == 0,
              "enabled tail-breakdown present failed");
        CHECK(rsx_nr_d3d12_finalize_tail_breakdown(sink) == 0,
              "tail-breakdown shutdown resolve failed");
        rsx_nr_d3d12_get_stats(sink, &stats);
        rsx_nr_d3d12_tail_bucket bucket;
        memset(&bucket, 0, sizeof(bucket));
        CHECK(stats.submit_attribution_qpc_frequency != 0u &&
                  stats.tail_gpu_frequency != 0u &&
                  stats.tail_gpu_intervals_recorded == 1u &&
                  stats.tail_gpu_intervals_dropped == 0u &&
                  rsx_nr_d3d12_tail_bucket_count(sink) == 1u &&
                  rsx_nr_d3d12_get_tail_bucket(sink, 0u, &bucket) == 0 &&
                  bucket.present_sequence == 1u &&
                  bucket.submit_count[
                      RSX_NR_D3D12_SUBMIT_REFERENCE_PUBLICATION] == 1u &&
                  bucket.submit_gpu_intervals[
                      RSX_NR_D3D12_SUBMIT_REFERENCE_PUBLICATION] == 1u &&
                  stats.submit_cause[
                      RSX_NR_D3D12_SUBMIT_REFERENCE_PUBLICATION]
                          .gpu_intervals == 1u,
              "tail-breakdown gate qpc=%llu gpu=%llu intervals=%llu/%llu",
              stats.submit_attribution_qpc_frequency,
              stats.tail_gpu_frequency,
              stats.tail_gpu_intervals_recorded,
              stats.tail_gpu_intervals_dropped);
        rsx_nr_d3d12_destroy(sink);
    }
    _putenv_s("YZ_NR_RSX_TAIL_BREAKDOWN", "");

    _putenv_s("YZ_NR_SUBMIT_ATTRIBUTION", "1");
    sink = rsx_nr_d3d12_create(
        NULL, LOCAL_SIZE, MAIN_SIZE, arena_ptr, arena_wptr, NULL);
    CHECK(sink != NULL, "enabled attribution sink creation failed");
    if (sink) {
        rsx_nr_exec_ops ops;
        rsx_nr_d3d12_stats stats;
        memset(&ops, 0, sizeof(ops));
        rsx_nr_d3d12_get_exec_ops(sink, &ops);
        CHECK(ops.clear(ops.user, &state, &clear) == 0,
              "enabled attribution clear failed");
        ops.flush_reason(ops.user, RSX_NR_FLUSH_REFERENCE);
        rsx_nr_d3d12_get_stats(sink, &stats);
        CHECK(stats.submit_attribution_qpc_frequency != 0u &&
                  stats.submit_cause[
                      RSX_NR_D3D12_SUBMIT_REFERENCE_PUBLICATION].submissions ==
                      1u &&
                  stats.submit_cause[
                      RSX_NR_D3D12_SUBMIT_REFERENCE_PUBLICATION].draws == 0u,
              "enabled reference attribution frequency=%llu submits=%llu",
              stats.submit_attribution_qpc_frequency,
              stats.submit_cause[
                  RSX_NR_D3D12_SUBMIT_REFERENCE_PUBLICATION].submissions);
        rsx_nr_d3d12_destroy(sink);
    }
    _putenv_s("YZ_NR_SUBMIT_ATTRIBUTION", "");
}

static void test_private_rt_registry_capacity(void)
{
    rsx_nr_d3d12* sink = rsx_nr_d3d12_create(
        NULL, LOCAL_SIZE, MAIN_SIZE, arena_ptr, arena_wptr, NULL);
    if (!sink) {
        CHECK(0, "private RT capacity sink creation failed");
        return;
    }

    rsx_nir_pipeline state;
    memset(&state, 0, sizeof(state));
    state.surface.color_format = 8u;
    state.surface.color_target = 1u;
    state.surface.color_location[0] = RSX_NIR_LOCATION_LOCAL;
    state.surface.color_pitch[0] = 16u;
    state.surface.clip_w = 4u;
    state.surface.clip_h = 4u;
    rsx_nir_clear clear = {0xF0u, 0u, 0u, 0u};

    for (u32 i = 0; i < 64u; ++i) {
        state.surface.color_offset[0] = i * 0x1000u;
        CHECK(rsx_nr_d3d12_preflight_clear(sink, &state, &clear) == 0,
              "private RT identity %u was refused", i);
    }
    rsx_nr_d3d12_stats before, after;
    rsx_nr_d3d12_get_stats(sink, &before);
    CHECK(before.rt_builds == 64u,
          "private RT registry built %llu/64 targets", before.rt_builds);

    state.surface.color_offset[0] = 64u * 0x1000u;
    CHECK(rsx_nr_d3d12_preflight_clear(sink, &state, &clear) != 0,
          "private RT registry did not refuse its bounded 65th identity");
    rsx_nr_d3d12_get_stats(sink, &after);
    CHECK(after.rt_builds == before.rt_builds,
          "bounded RT refusal partially allocated a target");
    rsx_nr_d3d12_destroy(sink);
}

static void test_display_chooses_latest_surface_identity(void)
{
    rsx_nr_d3d12* sink = rsx_nr_d3d12_create(
        NULL, LOCAL_SIZE, MAIN_SIZE, arena_ptr, arena_wptr, NULL);
    if (!sink) {
        CHECK(0, "display alias sink creation failed");
        return;
    }
    CHECK(rsx_nr_d3d12_set_live_output(
              sink, 0, test_present_handoff, NULL) == 0,
          "display alias live-output setup failed");
    rsx_nr_d3d12_rt_provenance provenance = {0};
    CHECK(rsx_nr_d3d12_get_rt_provenance(sink, 0, &provenance) != 0,
          "scanout provenance was not default-off");
    CHECK(rsx_nr_d3d12_set_scanout_provenance(sink, 1) == 0,
          "scanout provenance setup failed");
    rsx_nr_d3d12_set_display_buffer(
        sink, 0, RSX_NIR_LOCATION_LOCAL, RT_OFFSET, RT_W, RT_H);

    rsx_nr_exec_ops ops;
    memset(&ops, 0, sizeof(ops));
    rsx_nr_d3d12_get_exec_ops(sink, &ops);
    rsx_nir_pipeline state;
    memset(&state, 0, sizeof(state));
    state.surface.color_target = 1u;
    state.surface.color_location[0] = RSX_NIR_LOCATION_LOCAL;
    state.surface.color_offset[0] = RT_OFFSET;
    state.surface.color_pitch[0] = RT_W * 4u;
    state.surface.clip_w = RT_W;
    state.surface.clip_h = RT_H;
    rsx_nir_clear clear = {0xF0u, 0xFFFF0000u, 0u, 0u};

    /* The title's format-8 identity is allocated first. */
    state.surface.color_format = 8u;
    CHECK(rsx_nr_d3d12_preflight_clear(sink, &state, &clear) == 0 &&
              ops.clear(ops.user, &state, &clear) == 0 &&
              ops.present(ops.user, 0u) == 0,
          "first display identity failed");
    void* const title_texture = g_last_present_texture;

    /* The world reuses the display address and dimensions with format 5.
     * It must win scanout even though its target occupies a later slot. */
    state.surface.color_format = 5u;
    clear.color_value = 0xFF0000FFu;
    CHECK(rsx_nr_d3d12_preflight_clear(sink, &state, &clear) == 0 &&
              ops.clear(ops.user, &state, &clear) == 0 &&
              ops.present(ops.user, 0u) == 0,
          "second display identity failed");
    void* const world_texture = g_last_present_texture;
    CHECK(title_texture && world_texture && title_texture != world_texture,
          "display alias did not create two exact surface identities");
    CHECK(rsx_nr_d3d12_read_rt(
              sink, RSX_NIR_LOCATION_LOCAL, RT_OFFSET,
              RT_W, RT_H, g_pix) == 0 &&
              pix_is(1u, 1u, 0xFFu, 0x00u, 0x00u),
          "latest display identity did not provide world pixels");

    /* Recency follows successful writes, not allocation/table order. */
    state.surface.color_format = 8u;
    clear.color_value = 0xFF00FF00u;
    CHECK(ops.clear(ops.user, &state, &clear) == 0 &&
              ops.present(ops.user, 0u) == 0,
          "rewritten first display identity failed");
    CHECK(g_last_present_texture == title_texture,
          "scanout did not return to the newly written older identity");
    CHECK(rsx_nr_d3d12_read_rt(
              sink, RSX_NIR_LOCATION_LOCAL, RT_OFFSET,
              RT_W, RT_H, g_pix) == 0 &&
              pix_is(1u, 1u, 0x00u, 0xFFu, 0x00u),
          "display recency did not follow the successful rewrite");

    /* A depth-only clear still carries a color-surface declaration and may
     * allocate its exact format identity during preflight.  It writes no
     * color bytes, so it must neither supersede the last genuine color writer
     * nor make a never-written alias eligible for presentation. */
    state.surface.color_format = 5u;
    state.surface.depth_format = 2u;
    state.surface.zeta_location = RSX_NIR_LOCATION_LOCAL;
    state.surface.zeta_offset = ZETA_OFFSET;
    state.surface.zeta_pitch = RT_W * 4u;
    clear.mask = 0x01u;
    clear.depth_value = 0x7FFFFFu;
    CHECK(rsx_nr_d3d12_preflight_clear(sink, &state, &clear) == 0 &&
              ops.clear(ops.user, &state, &clear) == 0 &&
              ops.present(ops.user, 0u) == 0,
          "depth-only display-alias clear failed");
    CHECK(g_last_present_texture == title_texture,
          "depth-only clear incorrectly replaced the color scanout writer");
    CHECK(rsx_nr_d3d12_read_rt(
              sink, RSX_NIR_LOCATION_LOCAL, RT_OFFSET,
              RT_W, RT_H, g_pix) == 0 &&
              pix_is(1u, 1u, 0x00u, 0xFFu, 0x00u),
          "depth-only clear changed the selected color scanout");

    rsx_nr_d3d12_stats stats;
    rsx_nr_d3d12_get_stats(sink, &stats);
    CHECK(stats.rt_builds == 2u,
          "display alias test built %llu identities instead of two",
          stats.rt_builds);
    CHECK(rsx_nr_d3d12_get_rt_provenance(sink, 0, &provenance) == 0 &&
              provenance.format == 8u &&
              provenance.color_clear_writes == 2u &&
              provenance.draw_writes == 0u &&
              provenance.present_count == 3u,
          "title provenance mismatch fmt=%u clear=%llu draw=%llu present=%llu",
          provenance.format, provenance.color_clear_writes,
          provenance.draw_writes, provenance.present_count);
    CHECK(rsx_nr_d3d12_get_rt_provenance(sink, 1, &provenance) == 0 &&
              provenance.format == 5u &&
              provenance.color_clear_writes == 1u &&
              provenance.draw_writes == 0u &&
              provenance.present_count == 1u,
          "world provenance mismatch fmt=%u clear=%llu draw=%llu present=%llu",
          provenance.format, provenance.color_clear_writes,
          provenance.draw_writes, provenance.present_count);
    CHECK(rsx_nr_d3d12_get_rt_provenance(sink, 2, &provenance) != 0,
          "scanout provenance exceeded the live RT table");
    rsx_nr_d3d12_destroy(sink);
}

/* Strict live native rendering uses private RGBA targets without the legacy
 * resource broker.  Prove that a format-8 target written by one pass is
 * rebound by GPU identity in the following pass.  Guest bytes at the source
 * address deliberately remain zero, so a missed alias produces black. */
static void test_private_rgba_rt_alias(void)
{
    rsx_nr_d3d12* sink = rsx_nr_d3d12_create(
        NULL, LOCAL_SIZE, MAIN_SIZE, arena_ptr, arena_wptr, NULL);
    if (!sink) {
        CHECK(0, "private RGBA alias sink creation failed");
        return;
    }
    CHECK(rsx_nr_d3d12_set_live_output(
              sink, 1, test_present_handoff, NULL) == 0,
          "private RGBA live-output setup failed");

    rsx_nr_ring ring;
    memset(&ring, 0, sizeof(ring));
    if (rsx_nr_ring_init(&ring, 128u, 4096u)) {
        CHECK(0, "private RGBA alias ring init failed");
        rsx_nr_d3d12_destroy(sink);
        return;
    }
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

    memset(g_local + RT565_OFFSET, 0, RT_W * RT_H * 4u);
    rsx_nir_surface source;
    memset(&source, 0, sizeof(source));
    source.color_format = 8u;
    source.depth_format = 2u;
    source.raster_type = 1u;
    source.clip_w = RT_W;
    source.clip_h = RT_H;
    source.color_offset[0] = RT565_OFFSET;
    source.color_pitch[0] = RT_W * 4u;
    source.color_location[0] = RSX_NIR_LOCATION_LOCAL;
    source.color_target = 1u;
    rsx_nir_em_surface(&em, &source);
    rsx_nir_em_clear(&em, 0xF0u, 0xFF00FF00u, 0u, 0u);

    write_tex_fp();
    stage_frame_state(&em);
    stage_private_rgba_rt_texture0(&em);
    {
        const u32 native_vp[4] = {
            0x401F9C00u, 0x0040000Du, 0x81000000u, 0x0001FF81u
        };
        const u32 batch[2] = {0u, 3u};
        rsx_nir_em_vertex_program(
            &em, 0u, native_vp, 4u, 1u, 3u, 0u);
        rsx_nir_em_clear(&em, 0xF3u, 0xFF000000u, 0xFFFFFFu, 0u);
        write_triangle(rsx_nr_d3d12_pages(sink),
                       -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f);
        rsx_nir_em_draw(&em, 5u, 0u, batch, 1u);
    }
    rsx_nr_backend_run(&be, 0u);
    CHECK(be.stats.exec_errors == 0u,
          "private RGBA alias execution faulted (%llu)",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(
              sink, RSX_NIR_LOCATION_LOCAL, RT_OFFSET,
              RT_W, RT_H, g_pix) == 0,
          "private RGBA alias destination readback failed");
    CHECK(pix_is(2u, 61u, 0x00u, 0xFFu, 0x00u),
          "private RGBA alias sampled guest black (%02X %02X %02X)",
          pix(2u, 61u)[0], pix(2u, 61u)[1], pix(2u, 61u)[2]);
    rsx_nr_d3d12_stats stats;
    rsx_nr_d3d12_get_stats(sink, &stats);
    CHECK(stats.rt_alias_binds >= 1u,
          "private RGBA target was not rebound as a GPU alias");

    rsx_nr_ring_destroy(&ring);
    rsx_nr_d3d12_destroy(sink);
}

/* One guest address can carry successive logical color formats in a live
 * frame.  Private full-native targets cannot share a D3D12 resource across
 * incompatible RSX identities, so target-as-texture lookup must follow the
 * latest successful writer rather than the first allocated table slot. */
static void test_private_rt_alias_chooses_latest_writer(void)
{
    rsx_nr_d3d12* sink = rsx_nr_d3d12_create(
        NULL, LOCAL_SIZE, MAIN_SIZE, arena_ptr, arena_wptr, NULL);
    if (!sink) {
        CHECK(0, "latest private alias sink creation failed");
        return;
    }
    CHECK(rsx_nr_d3d12_set_live_output(
              sink, 1, test_present_handoff, NULL) == 0,
          "latest private alias live-output setup failed");

    rsx_nr_ring ring;
    memset(&ring, 0, sizeof(ring));
    if (rsx_nr_ring_init(&ring, 128u, 4096u)) {
        CHECK(0, "latest private alias ring init failed");
        rsx_nr_d3d12_destroy(sink);
        return;
    }
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

    rsx_nir_surface source;
    memset(&source, 0, sizeof(source));
    source.depth_format = 2u;
    source.raster_type = 1u;
    source.clip_w = RT_W;
    source.clip_h = RT_H;
    source.color_offset[0] = RT565_OFFSET;
    source.color_pitch[0] = RT_W * 4u;
    source.color_location[0] = RSX_NIR_LOCATION_LOCAL;
    source.color_target = 1u;

    /* Allocate/write format 8 first, then a format-5 sibling at the same
     * address. The subsequent A8R8G8B8 texture view must see red, not the
     * older green identity. */
    source.color_format = 8u;
    rsx_nir_em_surface(&em, &source);
    rsx_nir_em_clear(&em, 0xF0u, 0xFF00FF00u, 0u, 0u);
    source.color_format = 5u;
    rsx_nir_em_surface(&em, &source);
    rsx_nir_em_clear(&em, 0xF0u, 0xFFFF0000u, 0u, 0u);

    write_tex_fp();
    stage_frame_state(&em);
    stage_private_rgba_rt_texture0_at(&em, RT565_OFFSET);
    {
        const u32 native_vp[4] = {
            0x401F9C00u, 0x0040000Du, 0x81000000u, 0x0001FF81u
        };
        const u32 batch[2] = {0u, 3u};
        rsx_nir_em_vertex_program(
            &em, 0u, native_vp, 4u, 1u, 3u, 0u);
        rsx_nir_em_clear(&em, 0xF3u, 0xFF000000u, 0xFFFFFFu, 0u);
        write_triangle(rsx_nr_d3d12_pages(sink),
                       -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f);
        rsx_nir_em_draw(&em, 5u, 0u, batch, 1u);
    }
    rsx_nr_backend_run(&be, 0u);
    CHECK(be.stats.exec_errors == 0u,
          "latest private alias first execution faulted (%llu)",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(
              sink, RSX_NIR_LOCATION_LOCAL, RT_OFFSET,
              RT_W, RT_H, g_pix) == 0 &&
              pix_is(2u, 61u, 0xFFu, 0x00u, 0x00u),
          "private alias sampled first allocation instead of latest writer");

    /* Recency must move back to the older table slot after a real rewrite. */
    source.color_format = 8u;
    rsx_nir_em_surface(&em, &source);
    rsx_nir_em_clear(&em, 0xF0u, 0xFF0000FFu, 0u, 0u);
    stage_frame_state(&em);
    stage_private_rgba_rt_texture0_at(&em, RT565_OFFSET);
    {
        const u32 batch[2] = {0u, 3u};
        rsx_nir_em_clear(&em, 0xF3u, 0xFF000000u, 0xFFFFFFu, 0u);
        rsx_nir_em_draw(&em, 5u, 0u, batch, 1u);
    }
    rsx_nr_backend_run(&be, 0u);
    CHECK(be.stats.exec_errors == 0u,
          "latest private alias rewrite execution faulted (%llu)",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(
              sink, RSX_NIR_LOCATION_LOCAL, RT_OFFSET,
              RT_W, RT_H, g_pix) == 0 &&
              pix_is(2u, 61u, 0x00u, 0x00u, 0xFFu),
          "private alias recency did not follow rewritten older slot");

    rsx_nr_ring_destroy(&ring);
    rsx_nr_d3d12_destroy(sink);
}

typedef struct image4_test_arena {
    u8* local;
    u8* main;
    u32 size;
} image4_test_arena;

static const u8* image4_arena_ptr(
    void* user, u32 space, u32 offset, u32 min_bytes)
{
    image4_test_arena* arena = (image4_test_arena*)user;
    if (!arena || offset > arena->size || min_bytes > arena->size - offset)
        return NULL;
    return (space ? arena->main : arena->local) + offset;
}

static u8* image4_arena_wptr(
    void* user, u32 space, u32 offset, u32 min_bytes)
{
    return (u8*)image4_arena_ptr(user, space, offset, min_bytes);
}

static void test_image4_gpu_mlaa_path(void)
{
    enum {
        IMAGE4_LOCAL = 0x01140000u,
        IMAGE4_MAIN = 0x01772D00u,
        IMAGE4_W = 1024u,
        IMAGE4_H = 768u,
        IMAGE4_PITCH = 4096u,
        IMAGE4_BYTES = IMAGE4_PITCH * IMAGE4_H
    };
    image4_test_arena arena;
    arena.size = 32u << 20;
    fprintf(stderr, "[image4-test] allocating-arena\n");
    arena.local = (u8*)calloc(1u, arena.size);
    arena.main = (u8*)calloc(1u, arena.size);
    fprintf(stderr, "[image4-test] arena-ready\n");
    CHECK(arena.local != NULL && arena.main != NULL,
          "image4 guest arena allocation failed");
    if (!arena.local || !arena.main) {
        free(arena.local);
        free(arena.main);
        return;
    }
    rsx_nr_d3d12* sink = rsx_nr_d3d12_create(
        NULL, LOCAL_SIZE, MAIN_SIZE,
        image4_arena_ptr, image4_arena_wptr, &arena);
    CHECK(sink != NULL, "image4 WARP sink creation failed");
    if (!sink) {
        free(arena.local);
        free(arena.main);
        return;
    }
    fprintf(stderr, "[image4-test] sink-ready\n");
    CHECK(rsx_nr_d3d12_set_live_output(sink, 1, NULL, NULL) == 0,
          "image4 RGBA target configuration failed");
    CHECK(rsx_nr_d3d12_set_image4_gpu_mlaa(sink, 1) == 0,
          "image4 compute enable failed");

    rsx_nr_ring ring;
    rsx_nr_tokens tokens;
    rsx_nr_tokens_init(&tokens);
    CHECK(rsx_nr_ring_init(&ring, 128u, 256u) == 0,
          "image4 ring init failed");
    rsx_nr_exec_ops ops;
    memset(&ops, 0, sizeof(ops));
    rsx_nr_d3d12_get_exec_ops(sink, &ops);
    rsx_nr_backend backend;
    rsx_nr_backend_init(&backend, &ring, &tokens, &ops);
    rsx_nir_sink ring_sink = rsx_nr_ring_sink(&ring);
    rsx_nir_emitter emitter;
    rsx_nir_emitter_init(&emitter, &ring_sink);

    rsx_nir_surface surface;
    memset(&surface, 0, sizeof(surface));
    surface.color_format = 5u;
    surface.raster_type = 1u;
    surface.clip_w = IMAGE4_W;
    surface.clip_h = IMAGE4_H;
    surface.color_offset[0] = IMAGE4_LOCAL;
    surface.color_pitch[0] = IMAGE4_PITCH;
    surface.color_location[0] = RSX_NIR_LOCATION_LOCAL;
    surface.color_target = 1u;
    rsx_nir_em_surface(&emitter, &surface);
    rsx_nir_scissor scissor = {0u, 0u, IMAGE4_W, IMAGE4_H};
    rsx_nir_em_scissor(&emitter, &scissor);
    rsx_nir_raster raster;
    memset(&raster, 0, sizeof(raster));
    raster.color_mask = 0x01010101u;
    rsx_nir_em_raster(&emitter, &raster);
    rsx_nir_em_clear(&emitter, 0xF0u, 0xFF000000u, 0u, 0u);
    rsx_nr_backend_run(&backend, 0u);
    fprintf(stderr, "[image4-test] target-ready\n");
    CHECK(backend.stats.exec_errors == 0u,
          "image4 target creation failed (%llu)",
          backend.stats.exec_errors);

    /* Deterministic hard edge in guest A,R,G,B order. The normal reverse
     * transfer seeds the native RT; only the following recognized round is
     * eligible to avoid its readback and matching upload. */
    for (u32 y = 0; y < IMAGE4_H; ++y) {
        u8* row = arena.main + IMAGE4_MAIN + (size_t)y * IMAGE4_PITCH;
        for (u32 x = 0; x < IMAGE4_W; ++x) {
            const u8 value = x < IMAGE4_W / 2u ? 0u : 255u;
            row[x * 4u + 0u] = 255u;
            row[x * 4u + 1u] = value;
            row[x * 4u + 2u] = value;
            row[x * 4u + 3u] = value;
        }
    }
    rsx_nir_transfer transfer;
    memset(&transfer, 0, sizeof(transfer));
    transfer.kind = RSX_NIR_XFER_SCALED;
    transfer.src_location = RSX_NIR_LOCATION_MAIN;
    transfer.src_offset = IMAGE4_MAIN;
    transfer.src_pitch = IMAGE4_PITCH;
    transfer.src_format = 3u;
    transfer.dst_location = RSX_NIR_LOCATION_LOCAL;
    transfer.dst_offset = IMAGE4_LOCAL;
    transfer.dst_pitch = IMAGE4_PITCH;
    transfer.dst_format = 10u;
    transfer.in_w = transfer.out_w = transfer.clip_w = IMAGE4_W;
    transfer.in_h = transfer.out_h = transfer.clip_h = IMAGE4_H;
    transfer.ds_dx = transfer.dt_dy = 0x00100000u;
    transfer.origin = transfer.interpolator = 1u;
    rsx_nir_em_transfer(&emitter, &transfer, NULL);
    rsx_nr_backend_run(&backend, 0u);
    fprintf(stderr, "[image4-test] seed-uploaded\n");

    rsx_nr_d3d12_stats before, after;
    rsx_nr_d3d12_get_stats(sink, &before);
    transfer.src_location = RSX_NIR_LOCATION_LOCAL;
    transfer.src_offset = IMAGE4_LOCAL;
    transfer.dst_location = RSX_NIR_LOCATION_MAIN;
    transfer.dst_offset = IMAGE4_MAIN;
    CHECK(rsx_nr_d3d12_arm_image4_gpu_mlaa(sink, 1) == 0,
          "exact image4 producer admission failed");
    rsx_nir_em_transfer(&emitter, &transfer, NULL);
    rsx_nr_backend_run(&backend, 0u);
    fprintf(stderr, "[image4-test] forward-held\n");
    u32 generation = 0u;
    u64 writer_fence = 0u;
    int materialized = 1;
    CHECK(rsx_nr_d3d12_image4_gpu_mlaa_pending(
              sink, &generation, &writer_fence, &materialized) == 1 &&
              generation != 0u && !materialized,
          "exact forward transfer did not retain the GPU target");
    CHECK(rsx_nr_d3d12_execute_image4_gpu_mlaa(
              sink, generation, 10u, 89u) == 0,
          "image4 compute dispatch failed");
    fprintf(stderr, "[image4-test] compute-complete\n");
    transfer.src_location = RSX_NIR_LOCATION_MAIN;
    transfer.src_offset = IMAGE4_MAIN;
    transfer.dst_location = RSX_NIR_LOCATION_LOCAL;
    transfer.dst_offset = IMAGE4_LOCAL;
    rsx_nir_em_transfer(&emitter, &transfer, NULL);
    rsx_nr_backend_run(&backend, 0u);
    fprintf(stderr, "[image4-test] restore-consumed\n");
    rsx_nr_d3d12_get_stats(sink, &after);
    CHECK(after.transfer_gpu_readbacks == before.transfer_gpu_readbacks &&
              after.transfer_gpu_uploads == before.transfer_gpu_uploads,
          "accepted image4 round performed readback/upload (%llu/%llu)",
          after.transfer_gpu_readbacks - before.transfer_gpu_readbacks,
          after.transfer_gpu_uploads - before.transfer_gpu_uploads);
    CHECK(after.image4_mlaa_dispatches ==
              before.image4_mlaa_dispatches + 1u &&
              after.image4_mlaa_avoided_readbacks ==
              before.image4_mlaa_avoided_readbacks + 1u &&
              after.image4_mlaa_avoided_uploads ==
              before.image4_mlaa_avoided_uploads + 1u &&
              after.image4_mlaa_avoided_bytes ==
              before.image4_mlaa_avoided_bytes + 2ull * IMAGE4_BYTES,
          "image4 avoided-transfer accounting mismatch");
    CHECK(after.image4_mlaa_gpu_samples ==
              before.image4_mlaa_gpu_samples + 1u &&
              after.image4_mlaa_gpu_ticks >= before.image4_mlaa_gpu_ticks &&
              after.image4_mlaa_gpu_frequency != 0u &&
              after.image4_mlaa_qpc_frequency != 0u &&
              after.image4_mlaa_fence_wait_ticks >=
                  before.image4_mlaa_fence_wait_ticks,
          "image4 bounded GPU/fence timing aggregate missing");

    u8* pixels = (u8*)malloc(IMAGE4_BYTES);
    CHECK(pixels != NULL, "image4 output allocation failed");
    if (pixels && rsx_nr_d3d12_read_rt(
            sink, RSX_NIR_LOCATION_LOCAL, IMAGE4_LOCAL,
            IMAGE4_W, IMAGE4_H, pixels) == 0) {
        const u8* left = pixels + ((size_t)384u * IMAGE4_W + 100u) * 4u;
        const u8* edge_l = pixels +
            ((size_t)384u * IMAGE4_W + 511u) * 4u;
        const u8* edge_r = pixels +
            ((size_t)384u * IMAGE4_W + 512u) * 4u;
        const u8* right = pixels +
            ((size_t)384u * IMAGE4_W + 900u) * 4u;
        CHECK(left[0] == 0u && left[1] == 0u && left[2] == 0u &&
                  right[0] == 255u && right[1] == 255u && right[2] == 255u,
              "image4 changed pixels away from the edge");
        CHECK(edge_l[0] >= 63u && edge_l[0] <= 64u &&
                  edge_r[0] >= 191u && edge_r[0] <= 192u,
              "image4 edge blend mismatch left=%u right=%u",
              edge_l[0], edge_r[0]);
    } else {
        CHECK(0, "image4 output readback failed");
    }
    free(pixels);
    rsx_nr_ring_destroy(&ring);
    rsx_nr_d3d12_destroy(sink);
    free(arena.local);
    free(arena.main);
}

int main(int argc, char** argv)
{
    const char* const image4_only = getenv("YZ_NR_IMAGE4_ONLY");
    if (image4_only && image4_only[0] == '1' && image4_only[1] == '\0') {
        test_image4_gpu_mlaa_path();
        if (g_failures)
            return 1;
        printf("rsx_nr_backend_d3d12 image4: PASS\n");
        return 0;
    }
    /* Large archived-frame gates run in their own fresh WARP process. This
     * avoids carrying thousands of unrelated unit-test submissions and
     * resources into the deterministic two-pass capture comparison. */
    const char* const capture_only = getenv("YZ_NR_CAPTURE_ONLY");
    if (capture_only && capture_only[0] &&
        strcmp(capture_only, "0") != 0) {
        const char* const capture_path =
            argc > 1 ? argv[1] : getenv("YZ_NIR_RXS");
        if (!capture_path || !capture_path[0]) {
            fprintf(stderr,
                    "capture-only requires argv[1] or YZ_NIR_RXS\n");
            return 2;
        }
        run_capture_backend(capture_path);
        if (g_failures) {
            fprintf(stderr, "rsx_nr_backend_d3d12: %d FAILURE(S)\n",
                    g_failures);
            return 1;
        }
        printf("rsx_nr_backend_d3d12: PASS\n");
        return 0;
    }

    write_test_fp();
    rsx_nr_d3d12* sink = rsx_nr_d3d12_create(NULL, LOCAL_SIZE, MAIN_SIZE,
                                             arena_ptr, arena_wptr, NULL);
    if (!sink) {
        fprintf(stderr, "no WARP D3D12 device: SKIP\n");
        return 2;
    }
    CHECK(rsx_nr_d3d12_set_content_cache(
              sink, test_compile_shader, test_pso_load,
              test_pso_store, test_pso_free, NULL) == 0,
          "content-cache callback configuration failed");
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

    /* A zero CLEAR_SURFACE mask is an ordered no-op and must not require
     * seeded target state or count as an unsupported clear. */
    {
        rsx_nir_pipeline empty_state;
        rsx_nir_clear no_clear = {0};
        memset(&empty_state, 0, sizeof(empty_state));
        CHECK(rsx_nr_d3d12_preflight_clear(
                  sink, &empty_state, &no_clear) == 0,
              "zero-mask clear preflight refused");
        rsx_nir_em_clear(&em, 0u, 0u, 0u, 0u);
        rsx_nr_backend_run(&be, 0);
        CHECK(be.stats.exec_errors == 0,
              "zero-mask clear execution failed");
    }

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
        /* Retained RSX registers that cannot affect this triangle's D3D12
         * descriptor must not manufacture another driver pipeline. */
        rsx_nir_pipeline inert = be.st;
        rsx_nr_d3d12_stats before_inert, after_inert;
        rsx_nr_d3d12_get_stats(sink, &before_inert);
        inert.raster.mrt_color_mask ^= 0xFFFFFFFFu;
        inert.raster.polygon_offset_point_enable = 1u;
        inert.raster.polygon_offset_line_enable = 1u;
        inert.raster.polygon_offset_scale = 0x41200000u;
        inert.raster.polygon_offset_bias = 0xC0A00000u;
        inert.blend.sfactor ^= 0x00010001u;
        inert.blend.dfactor ^= 0x00020002u;
        inert.blend.equation ^= 0x00030003u;
        inert.depth_stencil.stencil_func ^= 0x7u;
        inert.depth_stencil.stencil_mask ^= 0xA5u;
        inert.depth_stencil.stencil_write_mask ^= 0x5Au;
        inert.depth_stencil.stencil_op_fail ^= 0x1E01u;
        CHECK(rsx_nr_d3d12_preflight_draw(
                  sink, &inert, native_vp, 4u,
                  &preflight_draw, batch) == 0,
              "inert-state draw preflight refused");
        rsx_nr_d3d12_get_stats(sink, &after_inert);
        CHECK(after_inert.pso_builds == before_inert.pso_builds &&
                  after_inert.pso_hits == before_inert.pso_hits + 1u,
              "inert RSX state rebuilt a PSO builds=%llu/%llu hits=%llu/%llu",
              before_inert.pso_builds, after_inert.pso_builds,
              before_inert.pso_hits, after_inert.pso_hits);
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

    /* Producer-time immutable snapshot: preparation must consume the exact
     * vertex generation visible while GET is withheld.  A producer rewrite
     * after preparation but before command execution must affect only the
     * next island, never this already admitted draw. */
    {
        rsx_nir_op snapshot_ops[1];
        u32 snapshot_side[2];
        rsx_nir_stream snapshot_stream;
        rsx_nir_stream_init_fixed(
            &snapshot_stream, snapshot_ops, 1u, snapshot_side, 2u);
        rsx_nir_op op;
        memset(&op, 0, sizeof(op));
        op.kind = RSX_NIR_OP_DRAW;
        op.u.draw = preflight_draw;
        op.u.draw.batches_ofs =
            rsx_nir_side_push(&snapshot_stream, batch, 2u);
        CHECK(rsx_nir_push(&snapshot_stream, &op) == 0,
              "snapshot test stream overflowed");
        write_triangle(
            rsx_nr_d3d12_pages(sink), -1.0f, -1.0f, 1.0f, -1.0f,
            -1.0f, 1.0f);
        rsx_nir_em_clear(&em, 0xF3u, 0xFF0000FFu, 0xFFFFFFu, 0u);
        rsx_nr_backend_run(&be, 0u);
        rsx_nr_d3d12_stats snapshot_before, snapshot_after;
        rsx_nr_d3d12_get_stats(sink, &snapshot_before);
        CHECK(rsx_nr_d3d12_record_snapshot_draw(
                  sink, &be, &snapshot_stream, 0u) == 0 &&
                  rsx_nr_d3d12_prepare_snapshot_island(
                      sink, &be, &snapshot_stream) == 0 &&
                  snapshot_stream.ops[0].u.draw.snapshot_id == 1u,
              "immutable draw snapshot preparation failed");
        write_triangle(
            rsx_nr_d3d12_pages(sink), 1.0f, 1.0f, -1.0f, 1.0f,
            1.0f, -1.0f);
        CHECK(rsx_nr_backend_stream_step(
                  &be, &snapshot_stream, 0u) == RSX_NR_STEP_EXECUTED,
              "prepared draw did not execute");
        rsx_nr_d3d12_finish_snapshot_island(sink, &be, 1);
        CHECK(rsx_nr_d3d12_read_rt(
                  sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
              "snapshot RT readback failed");
        CHECK(pix_is(2, 61, 0xFF, 0x00, 0xFF) &&
                  pix_is(61, 2, 0xFF, 0x00, 0x00),
              "prepared draw observed post-preparation guest bytes");
        rsx_nr_d3d12_get_stats(sink, &snapshot_after);
        CHECK(snapshot_after.snapshot_islands ==
                  snapshot_before.snapshot_islands + 1u &&
              snapshot_after.snapshot_draws_prepared ==
                  snapshot_before.snapshot_draws_prepared + 1u &&
              snapshot_after.snapshot_draws_executed ==
                  snapshot_before.snapshot_draws_executed + 1u &&
              snapshot_after.snapshot_prepare_failures ==
                  snapshot_before.snapshot_prepare_failures,
              "snapshot lifecycle accounting was not exact");
        /* Restore the ordinary bottom-left source generation used by the
         * following legacy-reference legs. */
        write_triangle(
            rsx_nr_d3d12_pages(sink), -1.0f, -1.0f, 1.0f, -1.0f,
            -1.0f, 1.0f);
        rsx_nir_em_clear(&em, 0xF3u, 0xFF0000FFu, 0xFFFFFFu, 0u);
        rsx_nr_backend_run(&be, 0u);
    }

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
    {
        rsx_nir_op conditional_ops[1];
        u32 conditional_side[2];
        rsx_nir_stream conditional_stream;
        rsx_nir_stream_init_fixed(
            &conditional_stream, conditional_ops, 1u,
            conditional_side, 2u);
        rsx_nir_op conditional_op;
        memset(&conditional_op, 0, sizeof(conditional_op));
        conditional_op.kind = RSX_NIR_OP_DRAW;
        conditional_op.u.draw = preflight_draw;
        conditional_op.u.draw.batches_ofs =
            rsx_nir_side_push(&conditional_stream, batch, 2u);
        CHECK(rsx_nir_push(&conditional_stream, &conditional_op) == 0,
              "conditional snapshot stream overflowed");
        rsx_nr_d3d12_stats before_dependency, after_dependency;
        rsx_nr_d3d12_get_stats(sink, &before_dependency);
        CHECK(rsx_nr_d3d12_record_snapshot_draw(
                  sink, &be, &conditional_stream, 0u) == 0 &&
                  conditional_stream.ops[0].u.draw.snapshot_id == 0u,
              "report-dependent draw captured pre-fence resources");
        rsx_nr_d3d12_get_stats(sink, &after_dependency);
        CHECK(after_dependency.snapshot_draws_prepared ==
                  before_dependency.snapshot_draws_prepared,
              "conditional draw changed snapshot preparation state");
        rsx_nr_d3d12_finish_snapshot_island(sink, &be, 0);
    }

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
                  g_watched_host_page[0][VTX_OFFSET >> 12],
              "array draw did not register only its exact vertex page "
              "local=%llu main=%llu watch=%u offset=%X",
              resident.resident_pages[0], resident.resident_pages[1],
              g_watched_pages[0], g_last_watched_offset[0]);
    }

    /* One RSX BEGIN/END can contain several DRAW_INDEX_ARRAY methods. A
     * triangle strip continues across those batch boundaries; only an
     * explicit restart index cuts it. The first two indices are deliberately
     * in batch 0 and their completing third index is in batch 1, followed by
     * a restart and a second triangle. Issuing one host draw per batch loses
     * the first triangle and was the live multi-batch shadow-pass defect. */
    {
        u8* indices = g_main + IDX_OFFSET;
        const u16 values[7] = {0u, 1u, 2u, 0xFFFFu, 0u, 2u, 3u};
        for (u32 i = 0; i < 7u; ++i) {
            indices[i * 2u] = (u8)(values[i] >> 8);
            indices[i * 2u + 1u] = (u8)values[i];
        }
        rsx_guest_pages_note_write(
            rsx_nr_d3d12_pages(sink), 1, IDX_OFFSET, 14u);
        stage_frame_state(&em);
        stage_vertex_bindings(&em, 0u);
        rsx_nir_index_binding split_ib;
        memset(&split_ib, 0, sizeof(split_ib));
        split_ib.offset = IDX_OFFSET;
        split_ib.location = RSX_NIR_LOCATION_MAIN;
        split_ib.restart_enable = 1u;
        split_ib.restart_index = 0xFFFFu;
        rsx_nir_em_index_binding(&em, &split_ib);
        rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);
        write_quad(rsx_nr_d3d12_pages(sink));
        const u32 split_strip_batches[4] = {0u, 2u, 2u, 5u};
        rsx_nir_em_draw(&em, 6, 1, split_strip_batches, 2);
        rsx_nr_backend_run(&be, 0);
        CHECK(be.stats.exec_errors == 0,
              "split triangle-strip draw faulted");
        CHECK(rsx_nr_d3d12_read_rt(
                  sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
              "split triangle-strip RT readback failed");
        CHECK(pix_is(61u, 61u, 0xFF, 0x00, 0xFF) &&
                  pix_is(2u, 2u, 0xFF, 0x00, 0xFF),
              "split triangle-strip did not preserve batch/restart semantics");
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

    /* A deliberately silent writer models a live publication route that
     * bypassed generation notification. The default mirror would retain the
     * previous top-right triangle. The narrow diagnostic must refresh only
     * the draw's already-derived vertex span and recover the new geometry. */
    {
        rsx_nr_d3d12_stats before_force, after_force;
        write_triangle_z_bytes(-1.0f, -1.0f, 1.0f, -1.0f,
                               -1.0f, 1.0f, 0.5f);
        CHECK(rsx_nr_d3d12_set_force_draw_input_refresh(sink, 1) == 0,
              "failed to enable forced exact-span refresh");
        rsx_nr_d3d12_begin_draw_input_refresh_section(sink);
        rsx_nr_d3d12_get_stats(sink, &before_force);
        stage_frame_state(&em);
        rsx_nir_em_clear(&em, 0xF3, 0xFF00FF00u, 0xFFFFFF, 0);
        rsx_nir_em_draw(&em, 5, 0, batch, 1);
        rsx_nir_em_present(&em, 0);
        rsx_nr_backend_run(&be, 0);
        rsx_nir_em_draw(&em, 5, 0, batch, 1);
        rsx_nr_backend_run(&be, 0);
        CHECK(rsx_nr_d3d12_set_force_draw_input_refresh(sink, 0) == 0,
              "failed to disable forced exact-span refresh");
        rsx_nr_d3d12_get_stats(sink, &after_force);
        CHECK(be.stats.exec_errors == 0,
              "forced exact-span refresh draw faulted");
        CHECK(after_force.forced_draw_input_refreshes ==
                  before_force.forced_draw_input_refreshes + 1u,
              "same-section exact page was not refreshed exactly once");
        CHECK(rsx_nr_d3d12_read_rt(
                  sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
              "forced exact-span refresh readback failed");
        CHECK(pix_is(2, 61, 0xFF, 0x00, 0xFF) &&
                  pix_is(61, 2, 0x00, 0xFF, 0x00),
              "forced exact-span refresh did not recover silent write");
    }

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

    /* A dynamic producer may reuse another portion of the same coarse page
     * for the next draw.  Coarse generation can therefore change on every
     * mirror upload while this draw's exact vertex bytes remain stable.  The
     * ordered exact-span patch must execute this draw once without falsely
     * acknowledging the whole page or exhausting stabilization retries. */
    {
        rsx_nr_d3d12_stats before_churn, after_churn;
        stage_frame_state(&em);
        stage_vertex_bindings(&em, 0u);
        write_triangle(rsx_nr_d3d12_pages(sink),
                       -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f);
        rsx_nr_d3d12_get_stats(sink, &before_churn);
        g_churn_sink = sink;
        g_churn_unrelated_vertex_page = 1;
        rsx_nir_em_draw(&em, 5, 0, batch, 1);
        rsx_nr_backend_run(&be, 0);
        g_churn_unrelated_vertex_page = 0;
        g_churn_sink = NULL;
        rsx_nr_d3d12_get_stats(sink, &after_churn);
        CHECK(be.stats.exec_errors == 0 &&
                  after_churn.draws == before_churn.draws + 1u,
              "same-page unrelated churn rejected exact draw");
        CHECK(after_churn.residency_failures ==
                  before_churn.residency_failures &&
                  after_churn.mirror_exact_patches >
                      before_churn.mirror_exact_patches,
              "same-page churn did not use exact ordered patch "
              "residency=%llu/%llu exact=%llu/%llu",
              before_churn.residency_failures,
              after_churn.residency_failures,
              before_churn.mirror_exact_patches,
              after_churn.mirror_exact_patches);
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
        CHECK(g_watched_host_page[0][TEX_OFFSET >> 12],
          "guest texture did not arm its exact host write page "
          "watches=%u last=%X", g_watched_pages[0],
          g_last_watched_offset[0]);

    write_solid_texture_bytes(255, 0, 0, 255);
    /* Model the host VM-write hook: only an exact page armed by the backend
     * is forwarded to its generation tracker after the bytes publish. */
    if (g_watched_host_page[0][TEX_OFFSET >> 12])
        rsx_nr_d3d12_note_guest_write(sink, 0, TEX_OFFSET, 16u);
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

    /* D8R8G8B8 is a four-byte color texture with a discard byte in place of
     * alpha.  The gun route uses it for a cubemap; admit the exact RSX family
     * instead of failing an otherwise fully native dependency island. */
    for (u32 pixel = 0; pixel < 6u * 4u; ++pixel) {
        g_local[TEX_OFFSET + pixel * 4u + 0u] = 0xFFu;
        g_local[TEX_OFFSET + pixel * 4u + 1u] = 0x00u;
        g_local[TEX_OFFSET + pixel * 4u + 2u] = 0xFFu;
        g_local[TEX_OFFSET + pixel * 4u + 3u] = 0x00u;
    }
    rsx_guest_pages_note_write(
        rsx_nr_d3d12_pages(sink), 0, TEX_OFFSET, 6u * 4u * 4u);
    write_tex_fp();
    stage_frame_state(&em);
    stage_d8r8g8b8_cube_texture0(&em);
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0,
          "D8R8G8B8 cubemap exec errors %llu", be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(
              sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "D8R8G8B8 cubemap readback failed");
    CHECK(pix_is(2, 61, 0x00, 0xFF, 0x00),
          "D8R8G8B8 cubemap pixel %02X %02X %02X",
          pix(2, 61)[0], pix(2, 61)[1], pix(2, 61)[2]);
    write_solid_texture(rsx_nr_d3d12_pages(sink), 255, 0, 0, 255);

    /* ---- color BORDER parity with the established renderer -----------
     * The guest requests an opaque-white border, but the established D3D12
     * path leaves its sampler border at zero.  The test VS emits an
     * out-of-range TC0=(-1,-1), so this real TEX draw catches a white edge
     * leaking into screen-space post effects. */
    write_tex_fp();
    stage_frame_state(&em);
    stage_color_border_texture0(&em);
    rsx_nir_em_clear(&em, 0xF3, 0xFF0000FFu, 0xFFFFFF, 0);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0,
          "color border sample exec errors %llu", be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "color border RT readback failed");
    CHECK(pix_is(2, 61, 0x00, 0x00, 0x00),
          "color border sample was not legacy-zero %02X %02X %02X",
          pix(2, 61)[0], pix(2, 61)[1], pix(2, 61)[2]);
    write_tex_fp();

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

    /* ---- F_X32 render target -----------------------------------------
     * The earlier world archive contains one float-only color pass. D3D12's
     * R32_FLOAT RTV is an exact representation; the pixel shader's x output
     * is the sole stored component. Execute and submit the draw without
     * converting it through an RGBA target. */
    rsx_nr_d3d12_stats f32_before, f32_after;
    rsx_nr_d3d12_get_stats(sink, &f32_before);
    write_test_fp();
    stage_frame_state(&em);
    rsx_nir_surface sf32;
    memset(&sf32, 0, sizeof(sf32));
    sf32.color_format = 13u;
    sf32.depth_format = 2u;
    sf32.raster_type = 1u;
    sf32.clip_w = RT_W;
    sf32.clip_h = RT_H;
    sf32.color_offset[0] = RTF32_OFFSET;
    sf32.color_pitch[0] = RT_W * 4u;
    sf32.color_location[0] = RSX_NIR_LOCATION_LOCAL;
    sf32.color_target = 1u;
    rsx_nir_em_surface(&em, &sf32);
    write_triangle(rsx_nr_d3d12_pages(sink),
                   -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f);
    rsx_nir_em_draw(&em, 5u, 0u, batch, 1u);
    rsx_nr_backend_run(&be, 0);
    ops.flush(ops.user);
    rsx_nr_d3d12_get_stats(sink, &f32_after);
    CHECK(be.stats.exec_errors == 0u &&
              f32_after.draws == f32_before.draws + 1u,
          "F_X32 draw failed errors=%llu draws=%llu/%llu",
          be.stats.exec_errors, f32_before.draws, f32_after.draws);

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
     * sampled value comes from the prior GPU depth resource. Strict native
     * sampling resolves the private D24 resource to the established R32
     * representation exactly once per depth-writing generation. */
    rsx_nr_d3d12_stats depth_snapshot_before;
    rsx_nr_d3d12_get_stats(sink, &depth_snapshot_before);
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
    rsx_nr_d3d12_stats depth_snapshot_first;
    rsx_nr_d3d12_get_stats(sink, &depth_snapshot_first);
    CHECK(depth_snapshot_first.depth_snapshot_builds ==
              depth_snapshot_before.depth_snapshot_builds + 1u &&
          depth_snapshot_first.depth_snapshot_resolves ==
              depth_snapshot_before.depth_snapshot_resolves + 1u,
          "first zeta snapshot builds=%llu/%llu resolves=%llu/%llu",
          depth_snapshot_before.depth_snapshot_builds,
          depth_snapshot_first.depth_snapshot_builds,
          depth_snapshot_before.depth_snapshot_resolves,
          depth_snapshot_first.depth_snapshot_resolves);

    /* Sampling again without a producer write must reuse the same resolved
     * generation rather than adding a second compute dispatch. */
    stage_frame_state(&em);
    stage_depth_texture0(&em);
    rsx_nir_em_clear(&em, 0xF0, 0xFF0000FFu, 0, 0);
    write_triangle(rsx_nr_d3d12_pages(sink), -1.0f, -1.0f, 1.0f, -1.0f,
                   -1.0f, 1.0f);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    rsx_nr_d3d12_stats depth_snapshot_reused;
    rsx_nr_d3d12_get_stats(sink, &depth_snapshot_reused);
    CHECK(be.stats.exec_errors == 0 &&
              depth_snapshot_reused.depth_snapshot_builds ==
                  depth_snapshot_first.depth_snapshot_builds &&
              depth_snapshot_reused.depth_snapshot_resolves ==
                  depth_snapshot_first.depth_snapshot_resolves,
          "unchanged zeta snapshot was rebuilt/resolved builds=%llu/%llu "
          "resolves=%llu/%llu",
          depth_snapshot_first.depth_snapshot_builds,
          depth_snapshot_reused.depth_snapshot_builds,
          depth_snapshot_first.depth_snapshot_resolves,
          depth_snapshot_reused.depth_snapshot_resolves);

    /* A subsequent depth clear creates a new producer generation. The R32
     * resource is retained, but its contents must be resolved exactly once. */
    stage_frame_state(&em);
    rsx_nir_em_clear(&em, 0x01, 0, 0x800000u, 0);
    rsx_nr_backend_run(&be, 0);
    write_tex_fp();
    stage_frame_state(&em);
    stage_depth_texture0(&em);
    rsx_nir_em_clear(&em, 0xF0, 0xFF0000FFu, 0, 0);
    write_triangle(rsx_nr_d3d12_pages(sink), -1.0f, -1.0f, 1.0f, -1.0f,
                   -1.0f, 1.0f);
    rsx_nir_em_draw(&em, 5, 0, batch, 1);
    rsx_nir_em_present(&em, 0);
    rsx_nr_backend_run(&be, 0);
    CHECK(be.stats.exec_errors == 0, "second zeta sample exec errors %llu",
          be.stats.exec_errors);
    CHECK(rsx_nr_d3d12_read_rt(sink, 0, RT_OFFSET, RT_W, RT_H, g_pix) == 0,
          "second zeta RT readback failed");
    CHECK(pix_is(2, 61, 0x80, 0x80, 0x80),
          "second zeta sample pixel %02X %02X %02X",
          pix(2, 61)[0], pix(2, 61)[1], pix(2, 61)[2]);
    rsx_nr_d3d12_stats depth_snapshot_second;
    rsx_nr_d3d12_get_stats(sink, &depth_snapshot_second);
    CHECK(depth_snapshot_second.depth_snapshot_builds ==
              depth_snapshot_first.depth_snapshot_builds &&
          depth_snapshot_second.depth_snapshot_resolves ==
              depth_snapshot_first.depth_snapshot_resolves + 1u,
          "changed zeta snapshot builds=%llu/%llu resolves=%llu/%llu",
          depth_snapshot_first.depth_snapshot_builds,
          depth_snapshot_second.depth_snapshot_builds,
          depth_snapshot_first.depth_snapshot_resolves,
          depth_snapshot_second.depth_snapshot_resolves);

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

        /* NV3089 and NV3062 use distinct format-enum domains. The Hana
         * frame's source format 3 and destination format 10 are both raw
         * A8R8G8B8, while NV3062 SET_PITCH stores pitch in its high half. */
        const u32 scaled_src = 0x12000u, scaled_dst = 0x8000u;
        static const u8 row0[16] = {
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
            0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10
        };
        static const u8 row1[16] = {
            0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
            0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20
        };
        memcpy(g_local + scaled_src, row0, sizeof(row0));
        memcpy(g_local + scaled_src + 32u, row1, sizeof(row1));
        memset(g_main + scaled_dst, 0, 48u);
        memset(&transfer, 0, sizeof(transfer));
        transfer.kind = RSX_NIR_XFER_SCALED;
        transfer.src_location = RSX_NIR_LOCATION_LOCAL;
        transfer.src_offset = scaled_src;
        transfer.src_pitch = 32u;
        transfer.src_format = 3u;
        transfer.dst_location = RSX_NIR_LOCATION_MAIN;
        transfer.dst_offset = scaled_dst;
        transfer.dst_pitch = 32u;
        transfer.dst_format = 10u;
        transfer.in_w = transfer.out_w = transfer.clip_w = 4u;
        transfer.in_h = transfer.out_h = transfer.clip_h = 2u;
        transfer.ds_dx = transfer.dt_dy = 0x00100000u;
        transfer.origin = transfer.interpolator = 1u;
        CHECK(rsx_nr_d3d12_preflight_transfer(
                  sink, &be.st, &transfer, NULL) == 0,
              "representation-identical scaled transfer preflight refused");
        g_published_writes = 0;
        rsx_nir_em_transfer(&em, &transfer, NULL);
        rsx_nr_backend_run(&be, 0);
        CHECK(be.stats.exec_errors == 2,
              "representation-identical transfer added exec error (%llu)",
              be.stats.exec_errors);
        CHECK(memcmp(g_main + scaled_dst, row0, sizeof(row0)) == 0 &&
                  memcmp(g_main + scaled_dst + 32u, row1, sizeof(row1)) == 0,
              "representation-identical transfer bytes mismatch");
        CHECK(g_published_writes == 2 &&
                  g_published_offset[0] == scaled_dst &&
                  g_published_size[0] == sizeof(row0) &&
                  g_published_offset[1] == scaled_dst + 32u &&
                  g_published_size[1] == sizeof(row1),
              "scaled transfer publication mismatch");

        /* A strict-native surface is authoritative on the GPU. Exercise the
         * exact Hana round trip: GPU target -> main guest bytes, then main
         * guest bytes -> the same native target. This must use A,R,G,B guest
         * order even though the offline target is physically BGRA. */
        stage_frame_state(&em);
        rsx_nir_em_clear(&em, 0xF0u, 0x7F123456u, 0u, 0u);
        rsx_nr_backend_run(&be, 0u);
        const u32 roundtrip = 0xA000u;
        memset(g_main + roundtrip, 0, RT_W * RT_H * 4u);
        memset(&transfer, 0, sizeof(transfer));
        transfer.kind = RSX_NIR_XFER_SCALED;
        transfer.src_location = RSX_NIR_LOCATION_LOCAL;
        transfer.src_offset = RT_OFFSET;
        transfer.src_pitch = RT_W * 4u;
        transfer.src_format = 3u;
        transfer.dst_location = RSX_NIR_LOCATION_MAIN;
        transfer.dst_offset = roundtrip;
        transfer.dst_pitch = RT_W * 4u;
        transfer.dst_format = 10u;
        transfer.in_w = transfer.out_w = transfer.clip_w = RT_W;
        transfer.in_h = transfer.out_h = transfer.clip_h = RT_H;
        transfer.ds_dx = transfer.dt_dy = 0x00100000u;
        transfer.origin = transfer.interpolator = 1u;
        rsx_nir_em_transfer(&em, &transfer, NULL);
        rsx_nr_backend_run(&be, 0u);
        CHECK(be.stats.exec_errors == 2 &&
                  g_main[roundtrip + 0u] == 0x7Fu &&
                  g_main[roundtrip + 1u] == 0x12u &&
                  g_main[roundtrip + 2u] == 0x34u &&
                  g_main[roundtrip + 3u] == 0x56u,
              "native target readback did not publish guest A,R,G,B bytes");

        g_main[roundtrip + 0u] = 0xD4u;
        g_main[roundtrip + 1u] = 0xA1u;
        g_main[roundtrip + 2u] = 0xB2u;
        g_main[roundtrip + 3u] = 0xC3u;
        transfer.src_location = RSX_NIR_LOCATION_MAIN;
        transfer.src_offset = roundtrip;
        transfer.dst_location = RSX_NIR_LOCATION_LOCAL;
        transfer.dst_offset = RT_OFFSET;
        rsx_nir_em_transfer(&em, &transfer, NULL);
        rsx_nr_backend_run(&be, 0u);
        CHECK(be.stats.exec_errors == 2,
              "native target upload added exec error (%llu)",
              be.stats.exec_errors);
        CHECK(rsx_nr_d3d12_read_rt(
                  sink, RSX_NIR_LOCATION_LOCAL, RT_OFFSET,
                  RT_W, RT_H, g_pix) == 0 &&
                  pix_is(0u, 0u, 0xC3u, 0xB2u, 0xA1u),
              "guest A,R,G,B upload did not update the native target "
              "(%02X %02X %02X %02X)",
              pix(0u, 0u)[0], pix(0u, 0u)[1],
              pix(0u, 0u)[2], pix(0u, 0u)[3]);
        rsx_nr_d3d12_stats transfer_stats;
        rsx_nr_d3d12_get_stats(sink, &transfer_stats);
        CHECK(transfer_stats.transfer_gpu_readbacks >= 1u &&
                  transfer_stats.transfer_gpu_uploads >= 1u,
              "native transfer route was not exercised read=%llu upload=%llu",
              transfer_stats.transfer_gpu_readbacks,
              transfer_stats.transfer_gpu_uploads);
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

    /* ---- shader-unused descriptor state is canonical -----------------
     * Titles freely mutate dormant texture units. Those registers cannot
     * consume sampler-table identities when the current fragment program
     * does not reference the unit; otherwise unrelated state exhausts the
     * bounded D3D12 sampler heap and forces an ordered mid-frame retirement. */
    {
        rsx_nr_d3d12_stats before_unused, after_unused;
        rsx_nr_d3d12_get_stats(sink, &before_unused);
        rsx_nir_texture unused;
        memset(&unused, 0, sizeof(unused));
        unused.enabled = 1u;
        unused.location = RSX_NIR_LOCATION_LOCAL;
        unused.dimension = 2u;
        unused.mipmaps = 1u;
        unused.width = 2u;
        unused.height = 2u;
        unused.pitch = 8u;
        for (u32 i = 0; i < 256u; ++i) {
            unused.offset = TEX_OFFSET + (i & 15u) * 16u;
            unused.wrap = (i & 7u) | ((i & 7u) << 8) |
                          ((i & 7u) << 16);
            unused.filter = ((i & 7u) << 16) |
                            (((i + 1u) & 7u) << 24);
            unused.control0 = i * 0x10101u;
            rsx_nir_em_texture(&em, 15u, &unused);
            rsx_nir_em_draw(&em, 5u, 0u, batch, 1u);
            rsx_nr_backend_run(&be, 0);
        }
        rsx_nr_d3d12_get_stats(sink, &after_unused);
        CHECK(after_unused.queue_submissions ==
                  before_unused.queue_submissions,
              "unused texture state forced %llu submissions",
              after_unused.queue_submissions -
                  before_unused.queue_submissions);
        CHECK(after_unused.descriptor_table_builds <=
                  before_unused.descriptor_table_builds + 1u &&
              after_unused.descriptor_table_hits >=
                  before_unused.descriptor_table_hits + 255u,
              "unused texture state escaped descriptor canonicalization");
        memset(&unused, 0, sizeof(unused));
        rsx_nir_em_texture(&em, 15u, &unused);
        rsx_nr_backend_run(&be, 0);
    }

    /* ---- bounded upload-arena rollover -------------------------------
     * One admitted live section can exceed the 32-MiB upload arena even
     * though every draw fits individually.  Retire an ordered prefix before
     * the first draw that would overflow, then continue without fallback. */
    {
        rsx_nr_d3d12_stats before_rollover, after_rollover;
        rsx_nr_d3d12_get_stats(sink, &before_rollover);
        for (u32 i = 0; i < 4096u; ++i) {
            rsx_nir_em_draw(&em, 5u, 0u, batch, 1u);
            rsx_nr_backend_run(&be, 0);
        }
        rsx_nr_d3d12_get_stats(sink, &after_rollover);
        CHECK(be.stats.exec_errors == 2,
              "upload rollover introduced exec error (%llu)",
              be.stats.exec_errors);
        CHECK(after_rollover.draws == before_rollover.draws + 4096u,
              "upload rollover lost draws (%llu/%llu)",
              after_rollover.draws, before_rollover.draws);
        CHECK(after_rollover.upload_rollovers >
                  before_rollover.upload_rollovers &&
              after_rollover.queue_submissions >
                  before_rollover.queue_submissions,
              "long section did not retire its full upload arena");
    }

    /* ---- top-level fragment RET ---------------------------------------
     * The live Frontier shader uses the opcode-hi form 0x45. It must reach
     * the compiler and execute as an early return, not be refused as a
     * generic branch bit or decoded as DP3. */
    {
        rsx_nr_d3d12_stats before_ret, after_ret;
        rsx_nr_d3d12_get_stats(sink, &before_ret);
        write_ret_fp();
        rsx_nir_em_draw(&em, 5u, 0u, batch, 1u);
        rsx_nr_backend_run(&be, 0u);
        rsx_nr_d3d12_get_stats(sink, &after_ret);
        CHECK(after_ret.draws == before_ret.draws + 1u &&
                  after_ret.unsupported_draws ==
                      before_ret.unsupported_draws &&
                  after_ret.compile_failures == before_ret.compile_failures,
              "top-level RET did not execute natively");
        write_test_fp();
    }

    /* ---- first exact fragment-program refusal identity ----------------
     * Failure-only diagnostics must preserve the exact unsupported word and
     * remain absent from the accepted draw path. */
    {
        rsx_nr_d3d12_stats before_fp_failure, after_fp_failure;
        rsx_nr_d3d12_get_stats(sink, &before_fp_failure);
        u8* const p = g_local + FP_OFFSET;
        put_fp_word(p + 0u, (0x07u << 24) | (0xFu << 9) | 1u);
        put_fp_word(p + 4u, 1u | ((0u << 9) | (1u << 11) |
                                  (2u << 13) | (3u << 15)) |
                                   (7u << 18));
        put_fp_word(p + 8u, 0u);
        put_fp_word(p + 12u, 0u);
        rsx_nir_em_draw(&em, 5u, 0u, batch, 1u);
        rsx_nr_backend_run(&be, 0u);
        rsx_nr_d3d12_get_stats(sink, &after_fp_failure);
        CHECK(after_fp_failure.unsupported_draws ==
                  before_fp_failure.unsupported_draws + 1u &&
              after_fp_failure.unsup_draw_fp ==
                  before_fp_failure.unsup_draw_fp + 1u,
              "unsupported FP was not classified exactly");
        CHECK(after_fp_failure.first_fp_failure_stage == 2u &&
                  after_fp_failure.first_fp_failure_result == 0 &&
                  after_fp_failure.first_fp_failure_location == 0u &&
                  after_fp_failure.first_fp_failure_offset == FP_OFFSET &&
                  after_fp_failure.first_fp_failure_size == 16u &&
                  after_fp_failure.first_fp_failure_unsupported_count == 1u &&
                  after_fp_failure.first_fp_failure_instruction_offset == 0u &&
                  after_fp_failure.first_fp_failure_opcode == 0x07u &&
                  after_fp_failure.first_fp_failure_reason == 1u &&
                  after_fp_failure.first_fp_failure_words[0] ==
                      ((0x07u << 24) | (0xFu << 9) | 1u) &&
                  after_fp_failure.first_fp_failure_byte_hash != 0u,
              "first FP refusal identity was incomplete");
        write_test_fp();
    }

    /* ---- sink accounting ----------------------------------------------- */
    rsx_nr_d3d12_stats st;
    rsx_nr_d3d12_get_stats(sink, &st);
    CHECK(st.texture_cache_capacity == 8192u &&
              st.pso_cache_capacity == 16384u,
          "production cache capacities texture=%u pso=%u",
          st.texture_cache_capacity, st.pso_cache_capacity);
    CHECK(st.unsupported_clears == 1, "partial clear not counted (%llu)",
          st.unsupported_clears);
    CHECK(st.clears == 34 && st.draws == 4901 && st.presents == 26,
          "sink counts clears=%llu draws=%llu presents=%llu", st.clears,
          st.draws, st.presents);
    CHECK(st.conditional_draws_skipped == 1u,
          "conditional skips=%llu expected=1",
          st.conditional_draws_skipped);
    CHECK(st.queue_submissions < st.clears + st.draws + st.presents,
          "draw/clear actions were not submission-batched (%llu submissions "
          "for %llu actions)", st.queue_submissions,
          st.clears + st.draws + st.presents);
    CHECK(st.unsupported_draws == 2 && st.unsup_draw_index == 1 &&
              st.unsup_draw_fp == 1 && st.compile_failures == 0,
          "unsupported=%llu compile_failures=%llu", st.unsupported_draws,
          st.compile_failures);
    CHECK(st.pso_builds >= 1 && st.pso_hits >= 1,
          "pso cache builds=%llu hits=%llu", st.pso_builds, st.pso_hits);
    CHECK(g_content_shader_calls ==
              st.vertex_shader_builds + st.pixel_shader_builds &&
              g_content_pso_loads == st.pso_builds &&
              g_content_pso_stores == st.pso_builds,
          "content cache shader=%llu builds=%llu/%llu pso=%llu/%llu/%llu",
          g_content_shader_calls, st.vertex_shader_builds,
          st.pixel_shader_builds, g_content_pso_loads,
          g_content_pso_stores, st.pso_builds);
    CHECK(st.real_fp_draws == st.draws,
          "real fragment programs=%llu draws=%llu", st.real_fp_draws,
          st.draws);
    CHECK(st.texture_draws == 12 && st.texture_builds == 3 &&
              st.texture_refreshes == 3 && st.texture_failures == 0,
          "textures draws=%llu builds=%llu refresh=%llu failures=%llu",
          st.texture_draws, st.texture_builds, st.texture_refreshes,
          st.texture_failures);
    CHECK(st.rt_alias_binds >= 1, "R5G6B5 alias not counted");
    CHECK(st.depth_snapshot_builds == 1u &&
              st.depth_snapshot_resolves == 2u,
          "depth snapshots builds=%llu resolves=%llu",
          st.depth_snapshot_builds, st.depth_snapshot_resolves);
    CHECK(g_present_handoffs == 26,
          "native scanout handoffs=%u expected=26", g_present_handoffs);

    rsx_nr_ring_destroy(&ring);
    rsx_nr_d3d12_destroy(sink);

    test_image4_gpu_mlaa_path();
    test_submit_attribution_gate();
    test_broker_actual_color_format();
    test_shared_timeline();
    test_private_rt_registry_capacity();
    test_display_chooses_latest_surface_identity();
    test_private_rgba_rt_alias();
    test_private_rt_alias_chooses_latest_writer();

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
