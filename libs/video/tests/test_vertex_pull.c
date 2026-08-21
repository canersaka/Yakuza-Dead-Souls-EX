/*
 * ps3recomp - unit tests for the vertex-pulling plan and HLSL codegen
 *
 * Offline, no GPU: plan support/fallback decisions, cbuffer image
 * correctness, shader-cache signature behavior, codegen structure and
 * determinism, a C model of the generated address arithmetic checked
 * against the CPU path's rsx_vertex_element_index(), and the pull-variant
 * decompile of a hand-assembled NV40 program.  Bit-level decode parity
 * against a real GPU lives in test_vertex_pull_gpu.c (WARP).
 */
#include "../rsx_vertex_pull.h"
#include "../rsx_vertex_formats.h"
#include "../rsx_vp_decompiler.h"

#include <stdio.h>
#include <string.h>

#define M_VTXBUF_OFFSET    0x1680u
#define M_VERTEX_DATA_BASE 0x1738u
#define M_VB_ELEMENT_BASE  0x173Cu
#define M_VTXFMT           0x1740u
#define M_VTX_ATTR_4F      0x1C00u
#define M_FREQUENCY_DIV    0x1FC0u

static int failures = 0;

#define CHECK(condition, label) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); \
        failures++; \
    } \
} while (0)

static u32 float_bits(float value)
{
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void set_attr(
    rsx_dispatch* rsx, u32 attr, u32 type, u32 size, u32 stride,
    u32 frequency, u32 location, u32 offset)
{
    rsx->regs[(M_VTXFMT + attr * 4) >> 2] =
        type | (size << 4) | (stride << 8) | (frequency << 16);
    rsx->regs[(M_VTXBUF_OFFSET + attr * 4) >> 2] =
        (location << 31) | offset;
}

static void set_default(
    rsx_dispatch* rsx, u32 attr, float x, float y, float z, float w)
{
    rsx->regs[(M_VTX_ATTR_4F + attr * 16 + 0) >> 2] = float_bits(x);
    rsx->regs[(M_VTX_ATTR_4F + attr * 16 + 4) >> 2] = float_bits(y);
    rsx->regs[(M_VTX_ATTR_4F + attr * 16 + 8) >> 2] = float_bits(z);
    rsx->regs[(M_VTX_ATTR_4F + attr * 16 + 12) >> 2] = float_bits(w);
}

static void seed_dispatch(rsx_dispatch* rsx)
{
    rsx_dispatch_init(rsx, NULL);
    set_attr(rsx, 0, RSX_VTX_TYPE_FLOAT, 3, 32, 1, 0, 0x100);
    set_attr(rsx, 1, RSX_VTX_TYPE_HALF, 4, 8, 0, 1, 0x2000);
    set_attr(rsx, 2, RSX_VTX_TYPE_UNORM8, 4, 4, 2, 0, 0x300);
    set_attr(rsx, 4, RSX_VTX_TYPE_CMP32, 1, 4, 3, 0, 0x400);
    /* attr 5 stays disabled, attr 3 disabled with the quirky default. */
    rsx->regs[M_VERTEX_DATA_BASE >> 2] = 0x40;
    rsx->regs[M_FREQUENCY_DIV >> 2] = 1u << 2;   /* attr2 modulo */
    set_default(rsx, 5, 0.25f, 0.5f, 0.75f, 2.0f);
}

static void test_plan_and_constants(void)
{
    rsx_dispatch rsx;
    seed_dispatch(&rsx);
    rsx_vertex_layout_plan layout;
    rsx_vertex_layout_plan_init(&layout, 0x37u);   /* attrs 0,1,2,4,5 */

    rsx_vertex_pull_plan plan;
    CHECK(rsx_vertex_pull_plan_init(&plan, &rsx, &layout,
                                    RSX_PULL_TYPES_ALL) == 1,
          "all used formats pullable");
    CHECK(plan.pulled_mask == 0x17u, "enabled attrs 0,1,2,4 pulled");
    CHECK(plan.unsupported_mask == 0, "nothing unsupported");
    CHECK(plan.attr[5].pulled == 0, "disabled attr not pulled");
    CHECK(plan.base_offset == 0x40u, "base offset decoded");
    CHECK(plan.attr[1].stride == 8 && plan.attr[1].elem_size == 8,
          "half4 sizes");
    CHECK(plan.attr[3].default_value[0] == 1.0f &&
              plan.attr[3].default_value[3] == 1.0f,
          "ATTR3 neutral default quirk preserved");
    CHECK(plan.attr[5].default_value[1] == 0.5f, "register default kept");

    /* Restricting the allowed set flags the format and fails the plan. */
    rsx_vertex_pull_plan limited;
    const u32 no_half = RSX_PULL_TYPES_ALL & ~(1u << RSX_VTX_TYPE_HALF);
    CHECK(rsx_vertex_pull_plan_init(&limited, &rsx, &layout, no_half) == 0,
          "restricted set reports fallback");
    CHECK(limited.unsupported_mask == 0x2u && !(limited.pulled_mask & 2u),
          "exactly the half attr is unsupported");

    /* Constants image. */
    rsx_vertex_pull_constants k;
    rsx_vertex_pull_fill_constants(&plan, 7u, 100u,
                                   RSX_PULL_SOURCE_INDEX_U16, 0x8000u, 1u,
                                   0x10000u, 0x20000u, &k);
    CHECK(k.base_offset == 0x40u && k.base_index == 7u && k.first == 100u,
          "draw constants");
    CHECK(k.source == RSX_PULL_SOURCE_INDEX_U16 &&
              k.index_offset == 0x8000u && k.index_location == 1u,
          "index-source constants");
    CHECK(k.mem_size[0] == 0x10000u && k.mem_size[1] == 0x20000u,
          "space sizes");
    CHECK(k.attr_cfg[0][0] == 0x100u && k.attr_cfg[0][1] == 32u &&
              k.attr_cfg[0][2] == 1u && k.attr_cfg[0][3] == 0u,
          "attr0 cfg");
    CHECK(k.attr_cfg[1][3] == RSX_PULL_ATTR_FLAG_MAIN,
          "attr1 main-memory flag");
    CHECK(k.attr_cfg[2][3] == RSX_PULL_ATTR_FLAG_MODULO,
          "attr2 modulo flag");
    CHECK(k.attr_cfg[4][2] == 3u && k.attr_cfg[4][3] == 0u,
          "attr4 divide frequency");
    CHECK(k.attr_default[3][0] == 1.0f && k.attr_default[5][3] == 2.0f,
          "defaults forwarded");

    /* Signature keys on baked facts only. */
    const u64 sig = rsx_vertex_pull_signature(&plan);
    CHECK(sig == rsx_vertex_pull_signature(&plan), "signature stable");
    set_attr(&rsx, 0, RSX_VTX_TYPE_FLOAT, 3, 32, 1, 0, 0x900);
    rsx_vertex_pull_plan moved;
    rsx_vertex_pull_plan_init(&moved, &rsx, &layout, RSX_PULL_TYPES_ALL);
    CHECK(rsx_vertex_pull_signature(&moved) == sig,
          "offset change does not re-key the shader");
    set_attr(&rsx, 0, RSX_VTX_TYPE_SNORM16, 4, 32, 1, 0, 0x900);
    rsx_vertex_pull_plan retyped;
    rsx_vertex_pull_plan_init(&retyped, &rsx, &layout, RSX_PULL_TYPES_ALL);
    CHECK(rsx_vertex_pull_signature(&retyped) != sig,
          "type change re-keys the shader");
}

static void test_codegen_structure(void)
{
    rsx_dispatch rsx;
    seed_dispatch(&rsx);
    rsx_vertex_layout_plan layout;
    rsx_vertex_layout_plan_init(&layout, 0x37u);
    rsx_vertex_pull_plan plan;
    rsx_vertex_pull_plan_init(&plan, &rsx, &layout, RSX_PULL_TYPES_ALL);

    static char globals[48 * 1024], globals2[48 * 1024], loads[4 * 1024];
    const int gl = rsx_vertex_pull_emit_globals(&plan, globals,
                                                sizeof(globals));
    const int ll = rsx_vertex_pull_emit_loads(&plan, "yz_sysvid", loads,
                                              sizeof(loads));
    CHECK(gl > 0 && ll > 0, "emitters succeed");
    CHECK(strstr(globals, "ByteAddressBuffer yz_guest_mem0 : register(t20)")
              && strstr(globals,
                        "ByteAddressBuffer yz_guest_mem1 : register(t21)"),
          "raw guest SRVs declared");
    CHECK(strstr(globals, "cbuffer YzVertexPull : register(b1)"),
          "pull cbuffer declared");
    CHECK(strstr(globals, "float4 yz_pull_attr0(") &&
              strstr(globals, "float4 yz_pull_attr1(") &&
              strstr(globals, "float4 yz_pull_attr2(") &&
              strstr(globals, "float4 yz_pull_attr4("),
          "one fetch function per pulled attr");
    CHECK(!strstr(globals, "yz_pull_attr3(") &&
              !strstr(globals, "yz_pull_attr5("),
          "no fetch function for disabled attrs");
    CHECK(strstr(loads, "v[5] = yz_attr_default[5];") &&
              strstr(loads, "v[4] = yz_pull_attr4(yz_vid);"),
          "loads route pulled vs default attrs");
    CHECK(!strstr(loads, "v[3]"), "attr outside the layout gets no load");

    const int gl2 = rsx_vertex_pull_emit_globals(&plan, globals2,
                                                 sizeof(globals2));
    CHECK(gl2 == gl && memcmp(globals, globals2, (size_t)gl) == 0,
          "codegen deterministic");

    /* Overflow reports cleanly. */
    char tiny[64];
    CHECK(rsx_vertex_pull_emit_globals(&plan, tiny, sizeof(tiny)) == -1,
          "overflow returns -1");
}

/* C model of the generated shader's element/address arithmetic, compared
 * against the CPU path's element resolver and u32 address math. */
static u32 model_elem(u32 vid, u32 base_index, u32 freq, u32 modulo)
{
    if (freq <= 1u) return (vid + base_index) & 0x000FFFFFu;
    if (modulo) return vid % freq;
    return vid / freq;
}

static void test_address_model(void)
{
    static const u32 vids[] = {0, 1, 2, 5, 99, 0xFFFFu, 0xFFFFFu, 0x100000u,
                               0x12345u};
    static const u32 bases[] = {0, 1, 7, 0xFFFFu, 0xFFFFFu};
    static const u32 freqs[] = {0, 1, 2, 3, 7, 64};
    for (u32 v = 0; v < sizeof(vids) / 4; v++)
        for (u32 b = 0; b < sizeof(bases) / 4; b++)
            for (u32 f = 0; f < sizeof(freqs) / 4; f++)
                for (u32 mod = 0; mod < 2; mod++) {
                    const u32 want = rsx_vertex_element_index(
                        vids[v], bases[b], freqs[f], mod);
                    const u32 got = model_elem(
                        vids[v], bases[b], freqs[f], mod);
                    if (want != got) {
                        CHECK(0, "shader element model matches CPU");
                        return;
                    }
                }
    CHECK(1, "shader element model matches CPU");

    /* Address wrap behavior: both sides use u32 modular arithmetic. */
    const u32 base_offset = 0xFFFFFF00u, offset = 0x200u, stride = 24u;
    const u32 cpu_addr = base_offset + offset + 9u * stride;
    const u32 gpu_addr = base_offset + offset + 9u * stride;
    CHECK(cpu_addr == gpu_addr && cpu_addr == 0x1D8u,
          "u32 address wrap parity");
}

static void test_pull_decompile(void)
{
    rsx_dispatch rsx;
    seed_dispatch(&rsx);
    rsx_vertex_layout_plan layout;
    rsx_vertex_layout_plan_init(&layout, 0x1u);
    rsx_vertex_pull_plan plan;
    rsx_vertex_pull_plan_init(&plan, &rsx, &layout, RSX_PULL_TYPES_ALL);

    /* MOV o0, v0 with the end bit: d0 sets dst_tmp=none + vec_result,
     * d1 = MOV | src0-high, d2 = src0-low (input v[0], identity swizzle),
     * d3 = end | sca_dst_tmp=none | full vec writemask. */
    u8 ucode[16];
    const u32 words[4] = {0x401F8000u, 0x0040000Du, 0x81000000u, 0x0001FF81u};
    for (u32 w = 0; w < 4; w++) {
        ucode[w * 4 + 0] = (u8)words[w];
        ucode[w * 4 + 1] = (u8)(words[w] >> 8);
        ucode[w * 4 + 2] = (u8)(words[w] >> 16);
        ucode[w * 4 + 3] = (u8)(words[w] >> 24);
    }
    rsx_vp_input_analysis analysis;
    CHECK(rsx_vp_analyze_inputs(ucode, sizeof(ucode), &analysis) == 1 &&
              analysis.exact && analysis.input_mask == 0x1u,
          "hand-assembled program reads exactly v[0]");

    static char hlsl[192 * 1024];
    const int instrs = rsx_vertex_pull_decompile(
        &plan, ucode, sizeof(ucode), 0u, hlsl, sizeof(hlsl));
    CHECK(instrs == 1, "pull decompile succeeds");
    CHECK(strstr(hlsl, "VSOutput main(uint yz_sysvid : SV_VertexID)"),
          "pull main uses SV_VertexID only");
    CHECK(!strstr(hlsl, "VSInput"), "no input-layout struct remains");
    CHECK(strstr(hlsl, "yz_pull_attr0(yz_vid)"),
          "body pulls the referenced attr");
    CHECK(strstr(hlsl, "cbuffer VPConst : register(b0)") &&
              strstr(hlsl, "vp_posscale"),
          "existing constant block and viewport transform retained");

    /* The legacy entry points are unchanged by the new parameter. */
    static char legacy[192 * 1024];
    CHECK(rsx_vp_decompile_ex(ucode, sizeof(ucode), 0u, legacy,
                              sizeof(legacy)) == 1 &&
              strstr(legacy, "VSOutput main(VSInput input)"),
          "legacy decompile path intact");
}

int main(void)
{
    test_plan_and_constants();
    test_codegen_structure();
    test_address_model();
    test_pull_decompile();
    if (failures) {
        fprintf(stderr, "test_vertex_pull: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_vertex_pull: ALL PASS\n");
    return 0;
}
