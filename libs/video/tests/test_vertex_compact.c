#include "../rsx_vertex_compact.h"
#include "../rsx_commands.h"
#include "../rsx_vp_decompiler.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define M_VTXBUF_OFFSET    0x1680u
#define M_VERTEX_DATA_BASE 0x1738u
#define M_VB_ELEMENT_BASE  0x173Cu
#define M_VTXFMT           0x1740u
#define M_VTX_ATTR_4F      0x1C00u
#define M_FREQUENCY_DIV    0x1FC0u
#define M_FRONT_FACE       0x1834u

typedef struct test_memory {
    u8 bytes[4096];
    u32 resolver_calls;
} test_memory;

static int failures = 0;

#define CHECK(condition, label) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); \
        failures++; \
    } \
} while (0)

static int nearf(float a, float b)
{
    return fabsf(a - b) < 0.0002f;
}

static const u8* test_guest_ptr(
    void* user, u32 location, u32 offset, u32 min_bytes)
{
    test_memory* memory = (test_memory*)user;
    memory->resolver_calls++;
    if (location != 0 || (u64)offset + min_bytes > sizeof(memory->bytes))
        return NULL;
    return memory->bytes + offset;
}

static void put_be16(u8* destination, u16 value)
{
    destination[0] = (u8)(value >> 8);
    destination[1] = (u8)value;
}

static void put_be32(u8* destination, u32 value)
{
    destination[0] = (u8)(value >> 24);
    destination[1] = (u8)(value >> 16);
    destination[2] = (u8)(value >> 8);
    destination[3] = (u8)value;
}

static void put_be_float(u8* destination, float value)
{
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    put_be32(destination, bits);
}

static u32 float_bits(float value)
{
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static int float4_bits_equal(const float left[4], const float right[4])
{
    for (u32 component = 0; component < 4; component++)
        if (float_bits(left[component]) != float_bits(right[component]))
            return 0;
    return 1;
}

static void test_dispatch_reset_defaults(void)
{
    rsx_dispatch rsx;
    rsx_dispatch_init(&rsx, NULL);
    CHECK(rsx_dsp_reg(&rsx, M_FRONT_FACE) == 0x0901u,
          "NV40 reset front-face winding is counter-clockwise");
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

static void test_layout(void)
{
    rsx_vertex_layout_plan layout;
    rsx_vertex_layout_plan_init(&layout, (1u << 0) | (1u << 3) | (1u << 7));
    CHECK(layout.count == 3, "layout count");
    CHECK(layout.stride == 48, "layout stride");
    CHECK(layout.attrs[0] == 0 && layout.attrs[1] == 3 &&
          layout.attrs[2] == 7, "deterministic ascending attributes");
    CHECK(layout.slot_by_attr[0] == 0 &&
          layout.slot_by_attr[3] == 1 &&
          layout.slot_by_attr[7] == 2 &&
          layout.slot_by_attr[6] == -1, "semantic slot map");
}

static void test_formats(void)
{
    float value[4];
    u8 data[16] = {0};

    put_be_float(data + 0, 1.25f);
    put_be_float(data + 4, -2.5f);
    CHECK(rsx_vertex_decode_element(
        RSX_VTX_TYPE_FLOAT, 2, data, value), "float decode accepted");
    CHECK(nearf(value[0], 1.25f) && nearf(value[1], -2.5f) &&
          value[2] == 0.0f && value[3] == 1.0f, "float endian/default lanes");

    put_be16(data + 0, 0x3C00u);
    put_be16(data + 2, 0xC000u);
    CHECK(rsx_vertex_decode_element(
        RSX_VTX_TYPE_HALF, 2, data, value), "half decode accepted");
    CHECK(nearf(value[0], 1.0f) && nearf(value[1], -2.0f),
          "half endian conversion");

    data[0] = 0; data[1] = 127; data[2] = 255; data[3] = 64;
    CHECK(rsx_vertex_decode_element(
        RSX_VTX_TYPE_UNORM8, 4, data, value), "unorm8 decode accepted");
    CHECK(nearf(value[0], 0.0f) && nearf(value[1], 127.0f / 255.0f) &&
          nearf(value[2], 1.0f) && nearf(value[3], 64.0f / 255.0f),
          "unorm8 conversion");

    data[0] = 1; data[1] = 17; data[2] = 129; data[3] = 255;
    CHECK(rsx_vertex_decode_element(
        RSX_VTX_TYPE_UINT8, 4, data, value), "uint8 decode accepted");
    CHECK(value[0] == 1.0f && value[1] == 17.0f &&
          value[2] == 129.0f && value[3] == 255.0f,
          "uint8 conversion");

    put_be16(data + 0, (u16)(s16)-32767);
    put_be16(data + 2, 0);
    put_be16(data + 4, 32767);
    CHECK(rsx_vertex_decode_element(
        RSX_VTX_TYPE_SNORM16, 3, data, value), "snorm16 decode accepted");
    CHECK(nearf(value[0], -1.0f) && value[1] == 0.0f &&
          nearf(value[2], 1.0f), "snorm16 endian conversion");

    put_be16(data + 0, (u16)(s16)-1234);
    put_be16(data + 2, (u16)2345);
    CHECK(rsx_vertex_decode_element(
        RSX_VTX_TYPE_SINT16, 2, data, value), "sint16 decode accepted");
    CHECK(value[0] == -1234.0f && value[1] == 2345.0f,
          "sint16 endian conversion");

    /* Preserve legacy handling for an invalid component count: the fetch
     * bounds cover the encoded span, while conversion clamps to float4. */
    data[0] = 1; data[1] = 2; data[2] = 3; data[3] = 4; data[4] = 5;
    CHECK(rsx_vertex_decode_element(
        RSX_VTX_TYPE_UINT8, 5, data, value),
        "oversized legacy component count accepted");
    CHECK(value[0] == 1.0f && value[1] == 2.0f &&
          value[2] == 3.0f && value[3] == 4.0f,
          "oversized legacy component count clamps to float4");

    {
        const s32 x = 1, y = -1, z = 511;
        const u32 packed =
            ((u32)x & 0x7FFu) |
            (((u32)y & 0x7FFu) << 11) |
            (((u32)z & 0x3FFu) << 22);
        put_be32(data, packed);
        CHECK(rsx_vertex_decode_element(
            RSX_VTX_TYPE_CMP32, 1, data, value), "cmp32 decode accepted");
        CHECK(nearf(value[0], 1.0f / 1023.0f) &&
              nearf(value[1], -1.0f / 1023.0f) &&
              nearf(value[2], 1.0f), "cmp32 endian/sign conversion");
    }
}

static void fetch_one(
    rsx_dispatch* rsx, test_memory* memory, u32 mask,
    rsx_vertex_ref ref, float* output, int* result)
{
    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_plan fetch;
    rsx_vertex_layout_plan_init(&layout, mask);
    rsx_vertex_fetch_plan_init(
        &fetch, rsx, &layout, test_guest_ptr, memory);
    rsx_vertex_fetch_plan_prepare(&fetch, &ref, 1);
    *result = rsx_vertex_fetch_one(
        &fetch, &ref, (u8*)output);
}

static void test_defaults_and_bounds(void)
{
    rsx_dispatch rsx;
    test_memory memory = {0};
    rsx_vertex_ref ref = {0, 0};
    float output[4] = {0};
    int result = 0;
    memset(&rsx, 0, sizeof(rsx));

    set_default(&rsx, 0, 9.0f, 8.0f, 7.0f, 6.0f);
    fetch_one(&rsx, &memory, 1u << 0, ref, output, &result);
    CHECK(result && output[0] == 9.0f && output[3] == 6.0f,
          "shader-read unbound ATTR0 receives RSX default");

    memset(&rsx, 0, sizeof(rsx));
    fetch_one(&rsx, &memory, 1u << 3, ref, output, &result);
    CHECK(result && output[0] == 1.0f && output[1] == 1.0f &&
          output[2] == 1.0f && output[3] == 1.0f,
          "legacy ATTR3 missing-color default preserved");

    memset(&rsx, 0, sizeof(rsx));
    set_attr(&rsx, 0, RSX_VTX_TYPE_FLOAT, 4, 16, 1, 0, 4090);
    fetch_one(&rsx, &memory, 1u << 0, ref, output, &result);
    CHECK(!result, "invalid enabled ATTR0 bounds remain fatal");

    memset(&rsx, 0, sizeof(rsx));
    set_default(&rsx, 1, 3.0f, 4.0f, 5.0f, 6.0f);
    set_attr(&rsx, 1, RSX_VTX_TYPE_FLOAT, 4, 16, 1, 0, 4090);
    fetch_one(&rsx, &memory, 1u << 1, ref, output, &result);
    CHECK(result && output[0] == 3.0f && output[3] == 6.0f,
          "invalid non-position bounds retain legacy default fallback");
}

static void test_stride_base_frequency_and_span(void)
{
    rsx_dispatch rsx;
    test_memory memory = {0};
    float output[4] = {0};
    int result = 0;
    memset(&rsx, 0, sizeof(rsx));

    set_attr(&rsx, 0, RSX_VTX_TYPE_FLOAT, 2, 0, 1, 0, 0);
    put_be_float(memory.bytes + 0, 1.0f);
    put_be_float(memory.bytes + 4, 2.0f);
    put_be_float(memory.bytes + 8, 3.0f);
    put_be_float(memory.bytes + 12, 4.0f);
    fetch_one(
        &rsx, &memory, 1u << 0, (rsx_vertex_ref){1, 0},
        output, &result);
    CHECK(result && output[0] == 3.0f && output[1] == 4.0f,
          "stride zero uses tightly packed element size");

    memset(&rsx, 0, sizeof(rsx));
    memset(&memory, 0, sizeof(memory));
    rsx.regs[M_VERTEX_DATA_BASE >> 2] = 4;
    rsx.regs[M_VB_ELEMENT_BASE >> 2] = 2;
    set_attr(&rsx, 0, RSX_VTX_TYPE_FLOAT, 1, 4, 1, 0, 0);
    put_be_float(memory.bytes + 16, 42.0f);
    fetch_one(
        &rsx, &memory, 1u << 0, (rsx_vertex_ref){1, 2},
        output, &result);
    CHECK(result && output[0] == 42.0f,
          "base offset and base index applied once");

    memset(&rsx, 0, sizeof(rsx));
    memset(&memory, 0, sizeof(memory));
    set_attr(&rsx, 0, RSX_VTX_TYPE_FLOAT, 1, 4, 2, 0, 0);
    put_be_float(memory.bytes + 4, 11.0f);
    put_be_float(memory.bytes + 8, 22.0f);
    fetch_one(
        &rsx, &memory, 1u << 0, (rsx_vertex_ref){5, 0},
        output, &result);
    CHECK(result && output[0] == 22.0f,
          "frequency divider division behavior");
    rsx.regs[M_FREQUENCY_DIV >> 2] = 1u;
    fetch_one(
        &rsx, &memory, 1u << 0, (rsx_vertex_ref){5, 0},
        output, &result);
    CHECK(result && output[0] == 11.0f,
          "frequency divider modulo behavior");

    memset(&rsx, 0, sizeof(rsx));
    memset(&memory, 0, sizeof(memory));
    set_attr(&rsx, 0, RSX_VTX_TYPE_FLOAT, 1, 4, 1, 0, 0);
    put_be_float(memory.bytes + 0, 1.0f);
    put_be_float(memory.bytes + 4, 2.0f);
    put_be_float(memory.bytes + 8, 3.0f);
    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_plan fetch;
    rsx_vertex_ref refs[3] = {{0, 0}, {1, 0}, {2, 0}};
    rsx_vertex_layout_plan_init(&layout, 1u);
    rsx_vertex_fetch_plan_init(
        &fetch, &rsx, &layout, test_guest_ptr, &memory);
    rsx_vertex_fetch_plan_prepare(&fetch, refs, 3);
    CHECK(memory.resolver_calls == 1,
          "stable bounds resolved once for the whole draw");
    for (u32 i = 0; i < 3; i++)
        CHECK(rsx_vertex_fetch_one(
            &fetch, &refs[i], (u8*)output), "stable-span fetch");
    CHECK(memory.resolver_calls == 1,
          "stable base avoids per-vertex guest resolution");
}

static u32 vp_src(u32 type)
{
    return type & 3u;
}

static void vp_instruction(
    u8* bytes, u32 vec_op, u32 sca_op, u32 input,
    u32 src0, u32 src1, u32 src2, int vec_write, int sca_write, int end)
{
    u32 d0 = 0;
    u32 d1 =
        ((src0 >> 9) & 0xFFu) |
        ((input & 0xFu) << 8) |
        ((vec_op & 0x1Fu) << 22) |
        ((sca_op & 0x1Fu) << 27);
    u32 d2 =
        ((src2 >> 11) & 0x3Fu) |
        ((src1 & 0x1FFFFu) << 6) |
        ((src0 & 0x1FFu) << 23);
    u32 d3 =
        (end ? 1u : 0u) |
        (vec_write ? 1u << 16 : 0u) |
        (sca_write ? 1u << 20 : 0u) |
        ((src2 & 0x7FFu) << 21);
    memcpy(bytes + 0, &d0, 4);
    memcpy(bytes + 4, &d1, 4);
    memcpy(bytes + 8, &d2, 4);
    memcpy(bytes + 12, &d3, 4);
}

static void test_vp_masks(void)
{
    u8 program[32] = {0};
    rsx_vp_input_analysis analysis;
    vp_instruction(
        program, 1, 0, 5, vp_src(2), 0, 0, 1, 0, 0);
    vp_instruction(
        program + 16, 0, 1, 3, 0, 0, vp_src(2), 0, 1, 1);
    CHECK(rsx_vp_analyze_inputs(program, sizeof(program), &analysis) == 2,
          "VP analysis instruction count");
    CHECK(analysis.exact &&
          analysis.input_mask == ((1u << 5) | (1u << 3)),
          "VP vector/scalar input mask");

    vp_instruction(
        program, 3, 0, 7, 0, vp_src(2), 0, 1, 0, 1);
    CHECK(rsx_vp_analyze_inputs(program, 16, &analysis) == 1 &&
          analysis.exact && analysis.input_mask == 0,
          "ADD does not mark unused src1");

    vp_instruction(
        program, 23, 0, 2, vp_src(2), 0, 0, 1, 0, 1);
    CHECK(rsx_vp_analyze_inputs(program, 16, &analysis) == 1 &&
          !analysis.exact && analysis.input_mask == 0xFFFFu,
          "unknown VP operation falls back to all inputs");

    vp_instruction(
        program, 1, 0, 5, vp_src(2), 0, 0, 1, 0, 1);
    {
        char hlsl[256 * 1024];
        CHECK(rsx_vp_decompile_compact_ex(
            program, 16, 0, 1u << 5, hlsl, sizeof(hlsl)) == 1,
            "compact VP decompile");
        CHECK(strstr(hlsl, "float4 a5:ATTR5;") != NULL &&
              strstr(hlsl, "float4 a4:ATTR4;") == NULL &&
              strstr(hlsl, "v[5]=input.a5;") != NULL,
              "compact HLSL preserves selected semantic identity");
    }

    vp_instruction(
        program, 2, 0, 2, vp_src(2), vp_src(1), 0, 1, 0, 0);
    vp_instruction(
        program + 16, 4, 0, 8,
        vp_src(1), vp_src(2), vp_src(1), 1, 0, 1);
    {
        char hlsl[256 * 1024];
        const u32 normal_uv_mask = (1u << 2) | (1u << 8);
        CHECK(rsx_vp_analyze_inputs(
                  program, sizeof(program), &analysis) == 2 &&
              analysis.exact && analysis.input_mask == normal_uv_mask,
              "VP analysis retains normal and UV shader inputs");
        CHECK(rsx_vp_decompile_compact_ex(
                  program, sizeof(program), 0, normal_uv_mask,
                  hlsl, sizeof(hlsl)) == 2,
              "normal/UV compact VP decompile");
        CHECK(strstr(hlsl, "float4 a2:ATTR2;") != NULL &&
              strstr(hlsl, "float4 a8:ATTR8;") != NULL &&
              strstr(hlsl, "v[2]=input.a2;") != NULL &&
              strstr(hlsl, "v[8]=input.a8;") != NULL,
              "normal and UV keep their original semantic indices");
    }
}

static void test_vp_opcode_source_tables(void)
{
    static const struct {
        u8 opcode;
        u8 sources;
    } vector_cases[] = {
        {0x00, 0}, {0x01, 1}, {0x02, 3}, {0x03, 5},
        {0x04, 7}, {0x05, 3}, {0x06, 3}, {0x07, 3},
        {0x08, 3}, {0x09, 3}, {0x0A, 3}, {0x0B, 3},
        {0x0C, 3}, {0x0D, 1}, {0x0E, 1}, {0x0F, 1},
        {0x10, 3}, {0x11, 0}, {0x12, 3}, {0x13, 3},
        {0x14, 3}, {0x15, 0}, {0x16, 1}, {0x19, 1}
    };
    static const u8 scalar_cases[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
        0x06, 0x07, 0x0D, 0x0E, 0x0F, 0x10
    };
    u8 instruction[16];
    rsx_vp_input_analysis analysis;

    for (u32 opcode_index = 0;
         opcode_index < sizeof(vector_cases) / sizeof(vector_cases[0]);
         opcode_index++) {
        for (u32 candidate = 0; candidate < 3; candidate++) {
            const u32 source[3] = {
                candidate == 0 ? vp_src(2) : vp_src(1),
                candidate == 1 ? vp_src(2) : vp_src(1),
                candidate == 2 ? vp_src(2) : vp_src(1)
            };
            memset(instruction, 0, sizeof(instruction));
            vp_instruction(
                instruction, vector_cases[opcode_index].opcode, 0, 9,
                source[0], source[1], source[2], 1, 0, 1);
            const u32 expected =
                vector_cases[opcode_index].sources & (1u << candidate)
                    ? 1u << 9 : 0u;
            const int ok =
                rsx_vp_analyze_inputs(
                    instruction, sizeof(instruction), &analysis) == 1 &&
                analysis.exact && analysis.input_mask == expected;
            if (!ok) {
                fprintf(stderr,
                        "FAIL: vector source table op=0x%02X source=%u "
                        "expected=0x%X got=0x%X exact=%d\n",
                        vector_cases[opcode_index].opcode, candidate,
                        expected, analysis.input_mask, analysis.exact);
                failures++;
            }
        }
    }

    for (u32 opcode_index = 0;
         opcode_index < sizeof(scalar_cases) / sizeof(scalar_cases[0]);
         opcode_index++) {
        for (u32 candidate = 0; candidate < 3; candidate++) {
            const u32 source[3] = {
                candidate == 0 ? vp_src(2) : vp_src(1),
                candidate == 1 ? vp_src(2) : vp_src(1),
                candidate == 2 ? vp_src(2) : vp_src(1)
            };
            memset(instruction, 0, sizeof(instruction));
            vp_instruction(
                instruction, 0, scalar_cases[opcode_index], 11,
                source[0], source[1], source[2], 0, 1, 1);
            const u32 expected =
                scalar_cases[opcode_index] && candidate == 2
                    ? 1u << 11 : 0u;
            const int ok =
                rsx_vp_analyze_inputs(
                    instruction, sizeof(instruction), &analysis) == 1 &&
                analysis.exact && analysis.input_mask == expected;
            if (!ok) {
                fprintf(stderr,
                        "FAIL: scalar source table op=0x%02X source=%u "
                        "expected=0x%X got=0x%X exact=%d\n",
                        scalar_cases[opcode_index], candidate,
                        expected, analysis.input_mask, analysis.exact);
                failures++;
            }
        }
    }

    memset(instruction, 0, sizeof(instruction));
    vp_instruction(
        instruction, 0, 0x08, 4,
        vp_src(1), vp_src(1), vp_src(2), 0, 1, 1);
    CHECK(rsx_vp_analyze_inputs(
              instruction, sizeof(instruction), &analysis) == 1 &&
          !analysis.exact && analysis.input_mask == 0xFFFFu,
          "scalar flow control falls back to all inputs");
}

static void test_compact_matches_full_legacy_layout(void)
{
    rsx_dispatch rsx;
    test_memory memory = {0};
    memset(&rsx, 0, sizeof(rsx));

    /* Character-like inputs cover the formats most likely to affect the
     * orphanage faces: position, compressed normal, half tangent/UV,
     * normalized color, and integer bone indices. */
    set_attr(&rsx, 0, RSX_VTX_TYPE_FLOAT,   3, 32, 1, 0,  64);
    set_attr(&rsx, 2, RSX_VTX_TYPE_CMP32,   1,  4, 1, 0, 512);
    set_attr(&rsx, 6, RSX_VTX_TYPE_HALF,    4,  8, 1, 0, 640);
    set_attr(&rsx, 8, RSX_VTX_TYPE_HALF,    2,  4, 1, 0, 768);
    set_attr(&rsx, 3, RSX_VTX_TYPE_UNORM8,  4,  4, 2, 0, 896);
    set_attr(&rsx, 7, RSX_VTX_TYPE_UINT8,   4,  4, 2, 0, 960);
    rsx.regs[M_VERTEX_DATA_BASE >> 2] = 16;

    for (u32 element = 0; element < 16; element++) {
        u8* position = memory.bytes + 16 + 64 + element * 32;
        put_be_float(position + 0, (float)element + 0.25f);
        put_be_float(position + 4, -(float)element - 0.5f);
        put_be_float(position + 8, (float)element * 2.0f);

        const s32 nx = (s32)((element * 97u) & 0x7FFu) - 1024;
        const s32 ny = (s32)((element * 53u) & 0x7FFu) - 1024;
        const s32 nz = (s32)((element * 29u) & 0x3FFu) - 512;
        const u32 packed =
            ((u32)nx & 0x7FFu) |
            (((u32)ny & 0x7FFu) << 11) |
            (((u32)nz & 0x3FFu) << 22);
        put_be32(memory.bytes + 16 + 512 + element * 4, packed);

        u8* tangent = memory.bytes + 16 + 640 + element * 8;
        put_be16(tangent + 0, 0x3C00u);
        put_be16(tangent + 2, (u16)(0xBC00u + (element & 1u)));
        put_be16(tangent + 4, 0x3800u);
        put_be16(tangent + 6, 0x4000u);

        u8* uv = memory.bytes + 16 + 768 + element * 4;
        put_be16(uv + 0, (u16)(0x3400u + element));
        put_be16(uv + 2, (u16)(0x3800u + element));
    }
    for (u32 element = 0; element < 8; element++) {
        u8* color = memory.bytes + 16 + 896 + element * 4;
        u8* bones = memory.bytes + 16 + 960 + element * 4;
        for (u32 lane = 0; lane < 4; lane++) {
            color[lane] = (u8)(element * 23u + lane * 17u);
            bones[lane] = (u8)(element * 4u + lane);
        }
    }

    const rsx_vertex_ref refs[] = {
        {0, 0}, {1, 0}, {4, 0}, {7, 0}, {8, 0}, {11, 0}
    };
    const u32 compact_mask =
        (1u << 0) | (1u << 2) | (1u << 3) |
        (1u << 6) | (1u << 7) | (1u << 8);
    rsx_vertex_layout_plan full_layout, compact_layout;
    rsx_vertex_fetch_plan full_fetch, compact_fetch;
    rsx_vertex_layout_plan_init(&full_layout, 0xFFFFu);
    rsx_vertex_layout_plan_init(&compact_layout, compact_mask);
    rsx_vertex_fetch_plan_init(
        &full_fetch, &rsx, &full_layout, test_guest_ptr, &memory);
    rsx_vertex_fetch_plan_init(
        &compact_fetch, &rsx, &compact_layout, test_guest_ptr, &memory);
    rsx_vertex_fetch_plan_prepare(
        &full_fetch, refs, (u32)(sizeof(refs) / sizeof(refs[0])));
    rsx_vertex_fetch_plan_prepare(
        &compact_fetch, refs, (u32)(sizeof(refs) / sizeof(refs[0])));

    for (u32 vertex = 0; vertex < sizeof(refs) / sizeof(refs[0]); vertex++) {
        float full[16][4];
        float compact[16][4];
        memset(full, 0xCD, sizeof(full));
        memset(compact, 0xCD, sizeof(compact));
        CHECK(rsx_vertex_fetch_one(
                  &full_fetch, &refs[vertex], (u8*)full),
              "full legacy-layout character fetch accepted");
        CHECK(rsx_vertex_fetch_one(
                  &compact_fetch, &refs[vertex], (u8*)compact),
              "compact character fetch accepted");
        for (u32 slot = 0; slot < compact_layout.count; slot++) {
            const u32 attr = compact_layout.attrs[slot];
            CHECK(float4_bits_equal(compact[slot], full[attr]),
                  "compact attribute matches full legacy-layout bits");
        }
    }
}

static void test_vertex_reference_remap(void)
{
    rsx_vertex_ref refs[] = {
        {7, 0}, {2, 0}, {7, 0}, {7, 4}, {2, 0}, {9, 4}, {7, 4}
    };
    const u32 expected_map[] = {0, 1, 0, 2, 1, 3, 2};
    const rsx_vertex_ref expected_unique[] = {
        {7, 0}, {2, 0}, {7, 4}, {9, 4}
    };
    rsx_vertex_remap remap = {0};
    u32 unique_count = 0;
    CHECK(rsx_vertex_remap_build(
              &remap, refs,
              (u32)(sizeof(refs) / sizeof(refs[0])),
              &unique_count),
          "vertex reference remap builds");
    CHECK(unique_count == 4, "vertex reference remap unique count");
    for (u32 i = 0; i < unique_count; i++) {
        CHECK(refs[i].vertex_id == expected_unique[i].vertex_id &&
              refs[i].base_index == expected_unique[i].base_index,
              "vertex reference remap preserves first-use order");
    }
    for (u32 i = 0; i < sizeof(expected_map) / sizeof(expected_map[0]); i++)
        CHECK(rsx_vertex_remap_index(&remap, i) == expected_map[i],
              "vertex reference remap reconstructs occurrences");

    {
        int indexed = 0;
        CHECK(rsx_vertex_topology_plan(
                  RSX_PRIMITIVE_TRIANGLES, unique_count < 7, &indexed) &&
              indexed,
              "duplicate triangle list remains supported and indexed");
        CHECK(rsx_vertex_topology_plan(
                  RSX_PRIMITIVE_TRIANGLES, 0, &indexed) && !indexed,
              "ordinary triangle list remains unindexed");
        CHECK(rsx_vertex_topology_plan(
                  RSX_PRIMITIVE_TRIANGLE_STRIP, 0, &indexed) && indexed &&
              rsx_vertex_topology_plan(
                  RSX_PRIMITIVE_TRIANGLE_FAN, 0, &indexed) && indexed &&
              rsx_vertex_topology_plan(
                  RSX_PRIMITIVE_QUADS, 0, &indexed) && indexed,
              "converted triangle primitives remain indexed");
        CHECK(!rsx_vertex_topology_plan(
                  RSX_PRIMITIVE_LINES, 0, &indexed),
              "unsupported primitive remains rejected");
    }

    rsx_vertex_ref second[] = {{3, 1}, {3, 1}, {4, 1}};
    CHECK(rsx_vertex_remap_build(&remap, second, 3, &unique_count),
          "vertex reference remap reuses storage");
    CHECK(unique_count == 2 &&
          rsx_vertex_remap_index(&remap, 0) == 0 &&
          rsx_vertex_remap_index(&remap, 1) == 0 &&
          rsx_vertex_remap_index(&remap, 2) == 1,
          "vertex reference remap resets its generation");
    rsx_vertex_remap_destroy(&remap);
}

int main(void)
{
    test_dispatch_reset_defaults();
    test_layout();
    test_formats();
    test_compact_matches_full_legacy_layout();
    test_defaults_and_bounds();
    test_stride_base_frequency_and_span();
    test_vp_masks();
    test_vp_opcode_source_tables();
    test_vertex_reference_remap();
    if (failures) {
        fprintf(stderr, "test_vertex_compact: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_vertex_compact: ALL PASS\n");
    return 0;
}
