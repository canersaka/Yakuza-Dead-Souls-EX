/*
 * Standalone unit test for the RSX fragment-program decompiler.
 *
 * Build (no D3D12 dependency):
 *   gcc -std=c11 -Wall -Wextra -I../../../include \
 *       test_fp_decompiler.c ../rsx_fp_decompiler.c -o test_fp.exe
 *
 * Exercises the decode path (word swap, source swizzle/type, opcode → HLSL,
 * dest mask, inline constants) by hand-assembling tiny programs and checking
 * the generated HLSL contains the expected expressions.
 */

#include "../rsx_fp_decompiler.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

/* Store host word H into guest bytes so that rsx_fp_read_word(p) == H.
 * rsx_fp_read_word does be32-load then 16-bit half-swap, so we invert: the
 * guest stores big-endian of swap16(H). */
static void put_word(u8* p, u32 h)
{
    u32 be = (h << 16) | (h >> 16);
    p[0] = (u8)(be >> 24); p[1] = (u8)(be >> 16);
    p[2] = (u8)(be >> 8);  p[3] = (u8)(be);
}

static void put_float(u8* p, float f)
{
    u32 h; memcpy(&h, &f, 4);
    put_word(p, h);
}

static void check(const char* test, const char* hlsl, const char* needle)
{
    if (strstr(hlsl, needle)) {
        printf("[PASS] %s -- found \"%s\"\n", test, needle);
        g_pass++;
    } else {
        printf("[FAIL] %s -- missing \"%s\"\nHLSL:\n%s\n", test, needle, hlsl);
        g_fail++;
    }
}

/* OPDEST/SRC bit helpers (mirror the decoder's macros). */
#define OPC(x)      ((u32)(x) << 24)
#define OUTMASK_ALL (0xFu << 9)
#define END         (1u << 0)
#define INSRC(x)    ((u32)(x) << 13)
#define TEXU(x)     ((u32)(x) << 17)
#define SWZ_IDENT   ((0u<<9)|(1u<<11)|(2u<<13)|(3u<<15))
#define T_TEMP      0u
#define T_INPUT     1u
#define T_CONST     2u
#define REG(i)      ((u32)(i) << 2)

/* exec_if / set_cond bit positions (SRC0=DWORD1): exec_if_lt@18,
 * exec_if_eq@19, exec_if_gr@20, cond_swizzle@21, cond_mod_reg_index@30,
 * cond_reg_index@31; set_cond = OPDEST@8. All three exec bits == the
 * unconditional default that real compiled programs always emit; none set
 * == never. */
#define EXEC_LT      (1u<<18)
#define EXEC_EQ      (1u<<19)
#define EXEC_GR      (1u<<20)
#define UNCOND       (EXEC_LT | EXEC_EQ | EXEC_GR)
#define COND_REG     (1u<<31)   /* cond_reg_index: exec_if READ source       */
#define COND_MOD_REG (1u<<30)   /* cond_mod_reg_index: set_cond WRITE target */
#define SET_COND     (1u<<8)

int main(void)
{
    char hlsl[8192];

    /* Test 1: MOV r0, COL0  (write all, END). */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | END); /* MOV, COL0 */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | UNCOND);              /* src0 = INPUT */
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        int n = rsx_fp_decompile(prog, sizeof(prog), RSX_FP_CTRL_AUTO, hlsl, sizeof(hlsl));
        printf("-- test1 MOV: %d instr\n", n);
        check("mov_dest",  hlsl, "r[0].xyzw =");
        check("mov_input", hlsl, "input.col");
        check("mov_return", hlsl, "return r[0];");
    }

    /* Test 2: MUL r1.xy, r0, r0  (partial write mask). */
    {
        u8 prog[16];
        u32 mask_xy = (1u<<9)|(1u<<10);
        put_word(prog + 0, OPC(0x02) | mask_xy | (1u<<1) | END); /* MUL, dest r1 */
        put_word(prog + 4, T_TEMP | REG(0) | SWZ_IDENT | UNCOND);
        put_word(prog + 8, T_TEMP | REG(0) | SWZ_IDENT);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), RSX_FP_CTRL_AUTO, hlsl, sizeof(hlsl));
        check("mul_mask", hlsl, "r[1].xy =");
        check("mul_expr", hlsl, "(r[0]).xyzw) * ((r[0]).xyzw)");
    }

    /* Test 3: MAD r0, r0, CONST, r0 with inline constant + saturate. */
    {
        u8 prog[32];
        put_word(prog + 0, OPC(0x04) | OUTMASK_ALL | (1u<<31) | END); /* MAD + SAT */
        put_word(prog + 4, T_TEMP  | REG(0) | SWZ_IDENT | UNCOND);
        put_word(prog + 8, T_CONST | SWZ_IDENT);                       /* src1 CONST */
        put_word(prog + 12, T_TEMP | REG(0) | SWZ_IDENT);
        put_float(prog + 16, 0.5f);  /* inline constant float4 */
        put_float(prog + 20, 0.25f);
        put_float(prog + 24, 0.0f);
        put_float(prog + 28, 1.0f);
        int n = rsx_fp_decompile(prog, sizeof(prog), RSX_FP_CTRL_AUTO, hlsl, sizeof(hlsl));
        printf("-- test3 MAD: %d instr\n", n);
        check("mad_sat",   hlsl, "saturate(");
        check("mad_const", hlsl, "float4(0.5,0.25,0,1)");
    }

    /* Test 4: TEX r0, TC0 from texture unit 3. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | TEXU(3) | END); /* TEX */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | UNCOND);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), RSX_FP_CTRL_AUTO, hlsl, sizeof(hlsl));
        check("tex_sample", hlsl, "rsx_tex[3].Sample(rsx_samp[3]");
    }

    /* Test 5: negate + abs on a source (MOV r0, -|r1|). */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | END);
        put_word(prog + 4, T_TEMP | REG(1) | SWZ_IDENT | (1u<<17) | (1u<<29) | UNCOND); /* neg+abs */
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), RSX_FP_CTRL_AUTO, hlsl, sizeof(hlsl));
        check("neg_abs", hlsl, "-(abs((r[1]).xyzw))");
    }

    /* Test 6: fixed-function alpha test is injected at the final output. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | END);
        put_word(prog + 4, T_INPUT | SWZ_IDENT | UNCOND);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), RSX_FP_CTRL_AUTO, hlsl, sizeof(hlsl));
        int patched = rsx_fp_apply_alpha_test(hlsl, sizeof(hlsl), 0x0204,
                                              rsx_fp_alpha_ref(100, 5));
        if (patched == 1) g_pass++; else g_fail++;
        check("alpha_output", hlsl, "float4 _rsx_out = r[0]");
        check("alpha_greater", hlsl, "_rsx_out.a > _rsx_alpha_ref");
        check("alpha_discard", hlsl, "discard;");
        if (fabsf(rsx_fp_alpha_ref(100, 5) - (100.0f / 255.0f)) < 0.000001f)
            g_pass++;
        else
            g_fail++;
    }

    /* Test 7: unconditional MOV (all three exec bits) must stay a plain
     * write -- i.e. predication support must NOT perturb the default path. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | END);
        put_word(prog + 4, T_TEMP | REG(1) | SWZ_IDENT | EXEC_LT | EXEC_EQ | EXEC_GR);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), RSX_FP_CTRL_AUTO, hlsl, sizeof(hlsl));
        check("uncond_plain", hlsl, "r[0].xyzw = _v.xyzw; }");
    }

    /* Test 8: predicated write, exec_if=GT reading cc0 -> per-component select. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | END);       /* MOV r0 */
        put_word(prog + 4, T_TEMP | REG(1) | SWZ_IDENT | EXEC_GR); /* exec_if_gr only */
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), RSX_FP_CTRL_AUTO, hlsl, sizeof(hlsl));
        check("pred_cc0",   hlsl, "(cc0).xxxx > (float4)0.0");
        check("pred_lerp",  hlsl, "lerp(r[0].xyzw, _v.xyzw, _p.xyzw)");
    }

    /* Test 9: exec_if=none (no bits) -> write suppressed. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | END);
        put_word(prog + 4, T_TEMP | REG(1) | SWZ_IDENT);         /* zero exec bits */
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), RSX_FP_CTRL_AUTO, hlsl, sizeof(hlsl));
        check("pred_none", hlsl, "exec_if=none: write suppressed");
    }

    /* Test 10: set_cond writes the CC register (cc1 via cond_mod_reg_index). */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | SET_COND | END); /* MOV r0, set CC */
        put_word(prog + 4, T_TEMP | REG(1) | SWZ_IDENT | EXEC_LT | EXEC_EQ | EXEC_GR | COND_MOD_REG);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), RSX_FP_CTRL_AUTO, hlsl, sizeof(hlsl));
        check("set_cond_write", hlsl, "cc1.xyzw = r[0].xyzw;");
    }

    /* Test 10: cube unit -> TextureCube declaration + 3-component sample. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | TEXU(6) | INSRC(4) | END); /* TEX unit 6, TC0 */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | UNCOND);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile_ex(prog, sizeof(prog), RSX_FP_CTRL_AUTO, (1u<<6), hlsl, sizeof(hlsl));
        check("cube_decl",   hlsl, "TextureCube rsx_tex6 : register(t6);");
        check("cube_sample", hlsl, "rsx_tex6.Sample(rsx_samp[6], ((input.tc0).xyzw).xyz)");
        check("cube_2d_unit", hlsl, "rsx_tex0 : register(t0);");   /* other units per-unit, still 2D */
    }

    /* Test 11: cube_mask 0 must keep the legacy 2D array form byte-for-byte. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | TEXU(6) | INSRC(4) | END);
        put_word(prog + 4, T_INPUT | SWZ_IDENT | UNCOND);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile_ex(prog, sizeof(prog), RSX_FP_CTRL_AUTO, 0u, hlsl, sizeof(hlsl));
        check("no_cube_array",  hlsl, "Texture2D    rsx_tex[16] : register(t0);");
        check("no_cube_sample", hlsl, "rsx_tex[6].Sample(rsx_samp[6], ((input.tc0).xyzw).xy)");
        if (strstr(hlsl, "TextureCube")) { printf("[FAIL] no_cube_leak -- TextureCube present at mask 0\n"); g_fail++; }
        else { printf("[PASS] no_cube_leak -- no TextureCube at mask 0\n"); g_pass++; }
    }

    printf("\n===========================================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
