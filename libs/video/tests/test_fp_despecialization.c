/*
 * Failing-before regression proof for fragment constant de-specialization.
 *
 * Before RSX_FP_BUFFERED_CONSTANTS_API exists this test intentionally fails:
 * it demonstrates that changing only an inline CONST payload or alpha-ref
 * value changes generated HLSL and the legacy byte identity.  The same source
 * switches to the public buffered API once it exists and proves that the
 * structural HLSL/identity collapse while uploaded data remains distinct.
 */

#include "../rsx_fp_decompiler.h"
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <d3dcompiler.h>
#endif

#define OPC(x)      ((u32)(x) << 24)
#define OUTMASK_ALL (0xFu << 9)
#define END         (1u << 0)
#define SWZ_IDENT   ((0u<<9)|(1u<<11)|(2u<<13)|(3u<<15))
#define T_CONST     2u
#define UNCOND      ((1u<<18) | (1u<<19) | (1u<<20))

static void put_word(u8* p, u32 h)
{
    const u32 be = (h << 16) | (h >> 16);
    p[0] = (u8)(be >> 24);
    p[1] = (u8)(be >> 16);
    p[2] = (u8)(be >> 8);
    p[3] = (u8)be;
}

static void put_float(u8* p, float value)
{
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    put_word(p, bits);
}

static void make_const_program(u8 program[32], float value)
{
    memset(program, 0, 32);
    put_word(program + 0, OPC(0x01) | OUTMASK_ALL | END);
    put_word(program + 4, T_CONST | SWZ_IDENT | UNCOND);
    put_float(program + 16, value);
    put_float(program + 20, 0.25f);
    put_float(program + 24, -0.0f);
    put_word(program + 28, 0x7FC01234u); /* preserve a NaN payload exactly */
}

static u64 hash_bytes(const void* data, size_t size)
{
    const u8* bytes = (const u8*)data;
    u64 hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static unsigned count_text(const char* text, const char* needle)
{
    unsigned count = 0;
    const size_t length = strlen(needle);
    for (const char* at = strstr(text, needle); at;
         at = strstr(at + length, needle))
        ++count;
    return count;
}

int main(void)
{
    u8 first[32], second[32];
    char hlsl_first[16384], hlsl_second[16384];
    make_const_program(first, 0.5f);
    make_const_program(second, 0.75f);

#if !defined(RSX_FP_BUFFERED_CONSTANTS_API)
    rsx_fp_decompile(first, sizeof(first), RSX_FP_CTRL_AUTO,
                     hlsl_first, sizeof(hlsl_first));
    rsx_fp_decompile(second, sizeof(second), RSX_FP_CTRL_AUTO,
                     hlsl_second, sizeof(hlsl_second));

    char alpha_first[16384], alpha_second[16384];
    memcpy(alpha_first, hlsl_first, sizeof(alpha_first));
    memcpy(alpha_second, hlsl_first, sizeof(alpha_second));
    rsx_fp_apply_alpha_test(
        alpha_first, sizeof(alpha_first), 0x0204u, 64.0f / 255.0f);
    rsx_fp_apply_alpha_test(
        alpha_second, sizeof(alpha_second), 0x0204u, 192.0f / 255.0f);

    printf("[FAIL-BEFORE] inline-only values: hlsl_equal=%d "
           "legacy_identity_equal=%d\n",
           strcmp(hlsl_first, hlsl_second) == 0,
           hash_bytes(first, sizeof(first)) ==
               hash_bytes(second, sizeof(second)));
    printf("[FAIL-BEFORE] alpha-ref-only values: hlsl_equal=%d\n",
           strcmp(alpha_first, alpha_second) == 0);
    return 1;
#else
    rsx_fp_constant_block constants_first, constants_second;
    u32 slots_first = 0, slots_second = 0;
    if (rsx_fp_collect_constants(
            first, sizeof(first), &constants_first) != 1 ||
        rsx_fp_collect_constants(
            second, sizeof(second), &constants_second) != 1)
        return 2;
    if (rsx_fp_decompile_buffered_ex(
            first, sizeof(first), RSX_FP_CTRL_AUTO, 0,
            hlsl_first, sizeof(hlsl_first), &slots_first) != 1 ||
        rsx_fp_decompile_buffered_ex(
            second, sizeof(second), RSX_FP_CTRL_AUTO, 0,
            hlsl_second, sizeof(hlsl_second), &slots_second) != 1)
        return 3;
    if (strcmp(hlsl_first, hlsl_second) != 0 ||
        slots_first != 1 || slots_second != 1 ||
        rsx_fp_structural_hash(first, sizeof(first),
                               1469598103934665603ull) !=
            rsx_fp_structural_hash(second, sizeof(second),
                                   1469598103934665603ull) ||
        memcmp(&constants_first, &constants_second,
               sizeof(constants_first)) == 0) {
        printf("[FAIL] inline constants did not collapse structurally\n");
        return 4;
    }

    char alpha_first[16384], alpha_second[16384];
    memcpy(alpha_first, hlsl_first, sizeof(alpha_first));
    memcpy(alpha_second, hlsl_second, sizeof(alpha_second));
    if (rsx_fp_apply_alpha_test_buffered(
            alpha_first, sizeof(alpha_first), 0x0204u) != 1 ||
        rsx_fp_apply_alpha_test_buffered(
            alpha_second, sizeof(alpha_second), 0x0204u) != 1 ||
        strcmp(alpha_first, alpha_second) != 0 ||
        rsx_fp_alpha_ref(64, 8) == rsx_fp_alpha_ref(192, 8)) {
        printf("[FAIL] alpha references did not collapse structurally\n");
        return 5;
    }

    /* Two instructions consume two slots in instruction order. Verify exact
     * bits, including signed zero, infinity, a NaN payload and a denormal. */
    u8 ordered[64] = {0};
    put_word(ordered + 0, OPC(0x01) | OUTMASK_ALL);
    put_word(
        ordered + 4,
        T_CONST | SWZ_IDENT | UNCOND | (1u << 17) | (1u << 29));
    put_word(ordered + 16, 0x80000000u);
    put_word(ordered + 20, 0x7F800000u);
    put_word(ordered + 24, 0x7FC01234u);
    put_word(ordered + 28, 0x00000001u);
    put_word(ordered + 32, OPC(0x01) | OUTMASK_ALL | END);
    put_word(ordered + 36, T_CONST | SWZ_IDENT | UNCOND);
    put_float(ordered + 48, 1.0f);
    put_float(ordered + 52, 2.0f);
    put_float(ordered + 56, 3.0f);
    put_float(ordered + 60, 4.0f);
    rsx_fp_constant_block ordered_constants;
    u32 ordered_slots = 0;
    if (rsx_fp_collect_constants(
            ordered, sizeof(ordered), &ordered_constants) != 2 ||
        ordered_constants.count != 2 ||
        ordered_constants.values[0][0] != 0x80000000u ||
        ordered_constants.values[0][1] != 0x7F800000u ||
        ordered_constants.values[0][2] != 0x7FC01234u ||
        ordered_constants.values[0][3] != 0x00000001u ||
        rsx_fp_decompile_buffered_ex(
            ordered, sizeof(ordered), RSX_FP_CTRL_AUTO, 0,
            hlsl_first, sizeof(hlsl_first), &ordered_slots) != 2 ||
        ordered_slots != 2 ||
        !strstr(hlsl_first, "fp_constants[0]") ||
        !strstr(hlsl_first, "fp_constants[1]") ||
        !strstr(hlsl_first, "-(abs(")) {
        printf("[FAIL] ordered/special-value constant extraction\n");
        return 6;
    }

    /* All CONST operands in one instruction share its one inline slot. */
    u8 shared[32] = {0};
    put_word(shared + 0, OPC(0x04) | OUTMASK_ALL | END);
    put_word(shared + 4, T_CONST | SWZ_IDENT | UNCOND);
    put_word(shared + 8, T_CONST | SWZ_IDENT);
    put_float(shared + 16, 8.0f);
    put_float(shared + 20, 9.0f);
    put_float(shared + 24, 10.0f);
    put_float(shared + 28, 11.0f);
    u32 shared_slots = 0;
    if (rsx_fp_decompile_buffered_ex(
            shared, sizeof(shared), RSX_FP_CTRL_AUTO, 0,
            hlsl_second, sizeof(hlsl_second), &shared_slots) != 1 ||
        shared_slots != 1 ||
        count_text(hlsl_second, "fp_constants[0]") < 2) {
        printf("[FAIL] multiple CONST operands did not share one slot\n");
        return 7;
    }

    /* Opcode/compare-mode changes remain structural distinctions. */
    u8 structurally_different[32];
    memcpy(structurally_different, first, sizeof(structurally_different));
    put_word(
        structurally_different + 0,
        OPC(0x02) | OUTMASK_ALL | END);
    if (rsx_fp_structural_hash(
            first, sizeof(first), 1469598103934665603ull) ==
            rsx_fp_structural_hash(
                structurally_different, sizeof(structurally_different),
                1469598103934665603ull)) {
        printf("[FAIL] genuine opcode distinction collapsed\n");
        return 8;
    }
    char alpha_compare[16384];
    if (rsx_fp_decompile_buffered_ex(
            first, sizeof(first), RSX_FP_CTRL_AUTO, 0,
            alpha_compare, sizeof(alpha_compare), NULL) != 1 ||
        rsx_fp_apply_alpha_test_buffered(
            alpha_compare, sizeof(alpha_compare), 0x0201u) != 1 ||
        strcmp(alpha_first, alpha_compare) == 0) {
        printf("[FAIL] genuine alpha compare distinction collapsed\n");
        return 9;
    }

    /* UNORM8, half, and float alpha normalization. */
    const u32 float_half_bits = 0x3F400000u; /* 0.75f */
    if (rsx_fp_alpha_ref(64u, 8u) != 64.0f / 255.0f ||
        rsx_fp_alpha_ref(0x3800u, 14u) != 0.5f ||
        rsx_fp_alpha_ref(float_half_bits, 15u) != 0.75f) {
        printf("[FAIL] alpha normalization\n");
        return 10;
    }

    char texcoord_hlsl[16384];
    memcpy(texcoord_hlsl, hlsl_first, sizeof(texcoord_hlsl));
    if (rsx_fp_apply_texcoord_control(
            texcoord_hlsl, sizeof(texcoord_hlsl), 0x05u) != 1 ||
        !strstr(texcoord_hlsl,
                "input.tc0 = float4(input.tc0.xy, 0.0, input.position.w)") ||
        !strstr(texcoord_hlsl,
                "input.tc2 = float4(input.tc2.xy, 0.0, input.position.w)") ||
        strstr(texcoord_hlsl,
               "input.tc1 = float4(input.tc1.xy, 0.0, input.position.w)") ||
        rsx_fp_apply_texcoord_control(
            texcoord_hlsl, sizeof(texcoord_hlsl), 1u << 8) != -1) {
        printf("[FAIL] texcoord-control structural input transform\n");
        return 11;
    }
    if (rsx_fp_apply_shader_window(
            texcoord_hlsl, sizeof(texcoord_hlsl), 0x000102D0u) != 1 ||
        !strstr(texcoord_hlsl,
                "input.position.x = input.position.x - 0.5") ||
        !strstr(texcoord_hlsl,
                "input.position.y = input.position.y - 0.5") ||
        rsx_fp_apply_shader_window(
            texcoord_hlsl, sizeof(texcoord_hlsl), 0x00200000u) != -1) {
        printf("[FAIL] shader-window WPOS transform\n");
        return 11;
    }

    /* Aligned allocation, wrap request, reset/retry, and oversize. */
    u32 ring_offset = 99, ring_allocation = 99;
    if (rsx_fp_constant_ring_plan(
            256, 1024, 17, &ring_offset, &ring_allocation) != 1 ||
        ring_offset != 256 || ring_allocation != 256 ||
        rsx_fp_constant_ring_plan(
            896, 1024, 17, &ring_offset, &ring_allocation) != 0 ||
        rsx_fp_constant_ring_plan(
            0, 1024, 17, &ring_offset, &ring_allocation) != 1 ||
        ring_offset != 0 ||
        rsx_fp_constant_ring_plan(
            0, 1024, 1025, &ring_offset, &ring_allocation) != -1) {
        printf("[FAIL] constant-ring alignment/wrap/oversize\n");
        return 11;
    }

    /* The live vertex-CB lane has 4,096 fixed blocks.  The last block fits,
     * the 4,097th requests a fence/reset, and retrying at zero succeeds. */
    {
        const u32 vertex_block = 8448u;
        const u32 vertex_capacity = vertex_block * 4096u;
        u32 vertex_offset = 99u;
    if (rsx_vertex_constant_ring_plan(
                vertex_block * 4095u, vertex_capacity,
                vertex_block, &vertex_offset) != 1 ||
            vertex_offset != vertex_block * 4095u ||
            rsx_vertex_constant_ring_plan(
                vertex_capacity, vertex_capacity,
                vertex_block, &vertex_offset) != 0 ||
            rsx_vertex_constant_ring_plan(
                0, vertex_capacity, vertex_block, &vertex_offset) != 1 ||
            vertex_offset != 0 ||
            rsx_vertex_constant_ring_plan(
                0, vertex_capacity, vertex_capacity + 256u,
                &vertex_offset) != -1) {
            printf("[FAIL] vertex constant-ring 4097th-draw recycle\n");
            return 12;
        }
    }

    /* Opcode bit 6 lives in SRC1 bit 31. RET is a top-level predicated early
     * return, not DP3 with a generic "branch" modifier. Flow-control fields
     * also must not be mistaken for a trailing inline-CONST payload. */
    {
        u8 ret_program[48] = {0};
        put_word(ret_program + 0u,
                 OPC(0x01) | OUTMASK_ALL | (1u << 7));
        put_word(ret_program + 4u, T_CONST | SWZ_IDENT | UNCOND);
        put_float(ret_program + 16u, 0.25f);
        put_float(ret_program + 20u, 0.5f);
        put_float(ret_program + 24u, 0.75f);
        put_float(ret_program + 28u, 1.0f);

        u8 ret_tail[16] = {0};
        put_word(ret_tail + 0u, OPC(0x05) | (1u << 30) | END);
        put_word(ret_tail + 4u,
                 (1u << 18) | (1u << 31)); /* LT against cc1.xxxx */
        put_word(ret_tail + 8u,
                 (1u << 31) | T_CONST);    /* opcode_hi plus alias bits */
        memcpy(ret_program + 32u, ret_tail, sizeof(ret_tail));

        rsx_fp_constant_block ret_constants;
        u32 ret_slots = 99u;
        if (rsx_fp_program_size(ret_program, sizeof(ret_program)) != 48u ||
            rsx_fp_collect_constants(
                ret_program, sizeof(ret_program), &ret_constants) != 1 ||
            rsx_fp_decompile_buffered_ex(
                ret_program, sizeof(ret_program), 0u, 0u,
                hlsl_first, sizeof(hlsl_first), &ret_slots) != 2 ||
            ret_slots != 1u ||
            !strstr(hlsl_first,
                    "if (any(cc1.xxxx < (float4)0)) return h[0];") ||
            strstr(hlsl_first, "unsupported flow-control")) {
            printf("[FAIL] predicated RET lowering/program walk\n");
            return 13;
        }

        /* A single RET with source-alias CONST bits is exactly 16 bytes and
         * has no literal payload. */
        put_word(ret_tail + 4u, UNCOND | T_CONST);
        if (rsx_fp_program_size(ret_tail, sizeof(ret_tail)) != 16u ||
            rsx_fp_collect_constants(
                ret_tail, sizeof(ret_tail), &ret_constants) != 0 ||
            rsx_fp_decompile_buffered_ex(
                ret_tail, sizeof(ret_tail), 0x40u, 0u,
                hlsl_first, sizeof(hlsl_first), &ret_slots) != 1 ||
            ret_slots != 0u ||
            !strstr(hlsl_first, "return r[0];")) {
            printf("[FAIL] unconditional RET flow fields consumed data\n");
            return 14;
        }
    }

#if defined(_WIN32)
    ID3DBlob* shader = NULL;
    ID3DBlob* errors = NULL;
    const HRESULT compile_result = D3DCompile(
            texcoord_hlsl, strlen(texcoord_hlsl), "fp-buffered-test",
        NULL, NULL, "main", "ps_5_0", 0, 0, &shader, &errors);
    if (FAILED(compile_result)) {
        printf(
            "[FAIL] buffered HLSL compile: %s\n",
            errors ? (const char*)errors->lpVtbl->GetBufferPointer(errors)
                   : "no diagnostic");
        if (errors) errors->lpVtbl->Release(errors);
        return 12;
    }
    if (errors) errors->lpVtbl->Release(errors);
    if (shader) shader->lpVtbl->Release(shader);
#endif

    printf("[PASS] inline constants and alpha refs collapse structurally; "
           "runtime payload/order/bits, compare modes and ring behavior "
           "validated\n");
    return 0;
#endif
}
