/*
 * ps3recomp - whole-block SPU backend SIMD kernels (WB lane)
 *
 * One `static inline` kernel per SPU operation, taking and returning __m128i
 * with the SAME lane convention as u128: 32-bit lane i == SPU word i (lane 0
 * is the preferred slot), bytes within each word host-native little-endian
 * (SPU byte P of the quadword lives at host byte SPU_W(P) = P ^ 3).
 *
 * The correctness reference is runtime/spu/spu_helpers.h. Hot operations are
 * lowered natively to SSE2/SSSE3/SSE4.1/AVX2; complex or rare operations wrap
 * their spu_helpers twin through a u128 round trip (bit-exact by
 * construction). Every kernel -- native or wrapped -- is differentially
 * tested against its helper twin by tests/spu_wb_kernels.
 *
 * This header is ONLY included by WB-generated translation units (compiled
 * with /arch:AVX2) and by the kernel test harness. The WB registration path
 * gates on spu_wb_runtime_ok() so a host without AVX2/OS support falls back
 * to the FAST region twins (see docs/SPU_WB_BACKEND.md).
 */

#ifndef SPU_WB_SIMD_H
#define SPU_WB_SIMD_H

#include "spu_helpers.h"
#include <immintrin.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- runtime gate (AVX2 + OSXSAVE + YMM state enabled) -------------------
 * Same check as spu_xfloat_simd.c's init; duplicated per-TU as a benignly
 * racy idempotent probe (cpuid is pure, every racer computes the same
 * value). */
static inline int spu_wb_runtime_ok(void)
{
    static volatile int s_ok = -1;
    if (s_ok < 0) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        int cpu[4] = {0, 0, 0, 0};
        int ok = 0;
        __cpuidex(cpu, 1, 0);
        if ((cpu[2] & (1 << 27)) != 0 && (cpu[2] & (1 << 28)) != 0 &&
            (_xgetbv(0) & 0x6) == 0x6) {
            __cpuidex(cpu, 7, 0);
            if ((cpu[1] & (1 << 5)) != 0)
                ok = 1;
        }
        s_ok = ok;
#elif defined(__GNUC__) || defined(__clang__)
        s_ok = __builtin_cpu_supports("avx2") ? 1 : 0;
#else
        s_ok = 0;
#endif
    }
    return s_ok;
}

/* ---- u128 <-> __m128i (for wrapped kernels and channel/LS boundaries) ---- */
static inline u128 wb_to_u128(__m128i v)
{
    u128 r;
    _mm_storeu_si128((__m128i*)(void*)&r, v);
    return r;
}
static inline __m128i wb_from_u128(u128 v)
{
    return _mm_loadu_si128((const __m128i*)(const void*)&v);
}

/* gpr slots are 16-byte aligned (SPU_ALIGN16 array of 16-byte elements). */
#define WB_GPR_LOAD(ctx, n)     _mm_load_si128((const __m128i*)(const void*)&(ctx)->gpr[(n)])
#define WB_GPR_STORE(ctx, n, v) _mm_store_si128((__m128i*)(void*)&(ctx)->gpr[(n)], (v))

/* Preferred-slot scalar extraction (word 0 = lane 0). */
static inline uint32_t wb_pref(__m128i v) { return (uint32_t)_mm_cvtsi128_si32(v); }
/* Scalar -> preferred slot, other slots zero (spu_link / spu_pref_u32). */
static inline __m128i wb_pref_set(uint32_t v) { return _mm_cvtsi32_si128((int)v); }

/* LS access through the canonical helpers (keeps the tagread-repair hook and
 * env-gated witnesses; WB TUs define YZ_SPU_SIMD_LS128=1 so the swap itself
 * is the validated PSHUFB path). Force-inlined: MSVC's heuristics outline
 * them at hundreds of call sites (MEASURED by the asm audit), and a call per
 * LS access defeats the point. */
#if defined(_MSC_VER)
#define SPU_WB_FORCEINLINE __forceinline
#else
#define SPU_WB_FORCEINLINE __attribute__((always_inline)) inline
#endif
static SPU_WB_FORCEINLINE __m128i wbk_ls_read(const spu_context* ctx, uint32_t lsa)
{
    return wb_from_u128(spu_ls_read128(ctx, lsa));
}
static SPU_WB_FORCEINLINE void wbk_ls_write(spu_context* ctx, uint32_t lsa, __m128i v)
{
    spu_ls_write128(ctx, lsa, wb_to_u128(v));
}

/* ---- shared building blocks --------------------------------------------- */
static inline __m128i wb_ones(void)  { return _mm_set1_epi32(-1); }
static inline __m128i wb_not(__m128i a) { return _mm_xor_si128(a, wb_ones()); }
/* unsigned per-lane compares via sign-bit flip */
static inline __m128i wb_cmpgtu32(__m128i a, __m128i b)
{
    const __m128i f = _mm_set1_epi32((int)0x80000000u);
    return _mm_cmpgt_epi32(_mm_xor_si128(a, f), _mm_xor_si128(b, f));
}
static inline __m128i wb_cmpgtu16(__m128i a, __m128i b)
{
    const __m128i f = _mm_set1_epi16((short)0x8000u);
    return _mm_cmpgt_epi16(_mm_xor_si128(a, f), _mm_xor_si128(b, f));
}
static inline __m128i wb_cmpgtu8(__m128i a, __m128i b)
{
    const __m128i f = _mm_set1_epi8((char)0x80u);
    return _mm_cmpgt_epi8(_mm_xor_si128(a, f), _mm_xor_si128(b, f));
}
/* sign-extended / zero-extended low halfword of each word */
static inline __m128i wb_lo16sx(__m128i a) { return _mm_srai_epi32(_mm_slli_epi32(a, 16), 16); }
static inline __m128i wb_lo16zx(__m128i a) { return _mm_and_si128(a, _mm_set1_epi32(0xFFFF)); }
/* host byte index j -> SPU byte position (j ^ 3), as a vector iota */
static inline __m128i wb_spuidx(void)
{
    return _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
}

/* =========================================================================
 * Integer arithmetic
 * =======================================================================*/
static inline __m128i wbk_a(__m128i a, __m128i b)   { return _mm_add_epi32(a, b); }
static inline __m128i wbk_sf(__m128i a, __m128i b)  { return _mm_sub_epi32(b, a); }
static inline __m128i wbk_ah(__m128i a, __m128i b)  { return _mm_add_epi16(a, b); }
static inline __m128i wbk_sfh(__m128i a, __m128i b) { return _mm_sub_epi16(b, a); }
static inline __m128i wbk_ai(__m128i a, int32_t imm)  { return _mm_add_epi32(a, _mm_set1_epi32(imm)); }
static inline __m128i wbk_ahi(__m128i a, int32_t imm) { return _mm_add_epi16(a, _mm_set1_epi16((short)imm)); }
static inline __m128i wbk_sfi(__m128i a, int32_t imm) { return _mm_sub_epi32(_mm_set1_epi32(imm), a); }
static inline __m128i wbk_sfhi(__m128i a, int32_t imm){ return _mm_sub_epi16(_mm_set1_epi16((short)imm), a); }
/* addx: a + b + (t & 1) per word */
static inline __m128i wbk_addx(__m128i a, __m128i b, __m128i t)
{
    return _mm_add_epi32(_mm_add_epi32(a, b), _mm_and_si128(t, _mm_set1_epi32(1)));
}
/* sfx: b + ~a + (t & 1) per word */
static inline __m128i wbk_sfx(__m128i a, __m128i b, __m128i t)
{
    return _mm_add_epi32(_mm_add_epi32(b, wb_not(a)), _mm_and_si128(t, _mm_set1_epi32(1)));
}
/* cg: carry-out of unsigned a + b */
static inline __m128i wbk_cg(__m128i a, __m128i b)
{
    const __m128i sum = _mm_add_epi32(a, b);
    return _mm_and_si128(wb_cmpgtu32(a, sum), _mm_set1_epi32(1));
}
/* cgx: carry-out of unsigned a + b + (t & 1) */
static inline __m128i wbk_cgx(__m128i a, __m128i b, __m128i t)
{
    const __m128i cin  = _mm_and_si128(t, _mm_set1_epi32(1));
    const __m128i s1   = _mm_add_epi32(a, b);
    const __m128i c1   = wb_cmpgtu32(a, s1);
    /* s1 + 1 overflows iff s1 == ~0 (only possible when cin lane is 1) */
    const __m128i c2   = _mm_and_si128(_mm_cmpeq_epi32(s1, wb_ones()),
                                       _mm_cmpeq_epi32(cin, _mm_set1_epi32(1)));
    return _mm_and_si128(_mm_or_si128(c1, c2), _mm_set1_epi32(1));
}
/* bg: (b + ~a + 1) >> 32  ==  b >= a (unsigned) */
static inline __m128i wbk_bg(__m128i a, __m128i b)
{
    return _mm_andnot_si128(wb_cmpgtu32(a, b), _mm_set1_epi32(1));
}
/* bgx: carry-out of b + ~a + (t & 1) */
static inline __m128i wbk_bgx(__m128i a, __m128i b, __m128i t)
{
    const __m128i cin = _mm_and_si128(t, _mm_set1_epi32(1));
    const __m128i na  = wb_not(a);
    const __m128i s1  = _mm_add_epi32(b, na);
    const __m128i c1  = wb_cmpgtu32(b, s1);   /* b + ~a overflowed */
    const __m128i c2  = _mm_and_si128(_mm_cmpeq_epi32(s1, wb_ones()),
                                      _mm_cmpeq_epi32(cin, _mm_set1_epi32(1)));
    return _mm_and_si128(_mm_or_si128(c1, c2), _mm_set1_epi32(1));
}

/* =========================================================================
 * Multiplies (low/high halfword forms; products wrap exactly like the
 * helpers' int32 arithmetic)
 * =======================================================================*/
static inline __m128i wbk_mpy(__m128i a, __m128i b)   { return _mm_mullo_epi32(wb_lo16sx(a), wb_lo16sx(b)); }
static inline __m128i wbk_mpyu(__m128i a, __m128i b)  { return _mm_mullo_epi32(wb_lo16zx(a), wb_lo16zx(b)); }
static inline __m128i wbk_mpyi(__m128i a, int32_t imm) { return _mm_mullo_epi32(wb_lo16sx(a), _mm_set1_epi32((int32_t)(int16_t)imm)); }
static inline __m128i wbk_mpyui(__m128i a, int32_t imm){ return _mm_mullo_epi32(wb_lo16zx(a), _mm_set1_epi32((uint16_t)imm)); }
static inline __m128i wbk_mpyh(__m128i a, __m128i b)
{
    return _mm_slli_epi32(_mm_mullo_epi32(_mm_srai_epi32(a, 16), wb_lo16sx(b)), 16);
}
static inline __m128i wbk_mpyhh(__m128i a, __m128i b)  { return _mm_mullo_epi32(_mm_srai_epi32(a, 16), _mm_srai_epi32(b, 16)); }
static inline __m128i wbk_mpyhhu(__m128i a, __m128i b) { return _mm_mullo_epi32(_mm_srli_epi32(a, 16), _mm_srli_epi32(b, 16)); }
static inline __m128i wbk_mpyhha(__m128i a, __m128i b, __m128i t)
{
    return _mm_add_epi32(t, wbk_mpyhh(a, b));
}
static inline __m128i wbk_mpyhhau(__m128i a, __m128i b, __m128i t)
{
    return _mm_add_epi32(t, wbk_mpyhhu(a, b));
}
static inline __m128i wbk_mpys(__m128i a, __m128i b)
{
    return _mm_srai_epi32(wbk_mpy(a, b), 16);
}
static inline __m128i wbk_mpya(__m128i a, __m128i b, __m128i c)
{
    return _mm_add_epi32(wbk_mpy(a, b), c);
}

/* =========================================================================
 * Bitwise logic
 * =======================================================================*/
static inline __m128i wbk_and(__m128i a, __m128i b)  { return _mm_and_si128(a, b); }
static inline __m128i wbk_or(__m128i a, __m128i b)   { return _mm_or_si128(a, b); }
static inline __m128i wbk_xor(__m128i a, __m128i b)  { return _mm_xor_si128(a, b); }
static inline __m128i wbk_nand(__m128i a, __m128i b) { return wb_not(_mm_and_si128(a, b)); }
static inline __m128i wbk_nor(__m128i a, __m128i b)  { return wb_not(_mm_or_si128(a, b)); }
static inline __m128i wbk_andc(__m128i a, __m128i b) { return _mm_andnot_si128(b, a); }
static inline __m128i wbk_orc(__m128i a, __m128i b)  { return _mm_or_si128(a, wb_not(b)); }
static inline __m128i wbk_eqv(__m128i a, __m128i b)  { return wb_not(_mm_xor_si128(a, b)); }
static inline __m128i wbk_andi(__m128i a, int32_t imm) { return _mm_and_si128(a, _mm_set1_epi32(imm)); }
static inline __m128i wbk_ori(__m128i a, int32_t imm)  { return _mm_or_si128(a, _mm_set1_epi32(imm)); }
static inline __m128i wbk_xori(__m128i a, int32_t imm) { return _mm_xor_si128(a, _mm_set1_epi32(imm)); }
static inline __m128i wbk_andhi(__m128i a, int32_t imm){ return _mm_and_si128(a, _mm_set1_epi16((short)imm)); }
static inline __m128i wbk_orhi(__m128i a, int32_t imm) { return _mm_or_si128(a, _mm_set1_epi16((short)imm)); }
static inline __m128i wbk_xorhi(__m128i a, int32_t imm){ return _mm_xor_si128(a, _mm_set1_epi16((short)imm)); }
static inline __m128i wbk_andbi(__m128i a, int32_t imm){ return _mm_and_si128(a, _mm_set1_epi8((char)imm)); }
static inline __m128i wbk_orbi(__m128i a, int32_t imm) { return _mm_or_si128(a, _mm_set1_epi8((char)imm)); }
static inline __m128i wbk_xorbi(__m128i a, int32_t imm){ return _mm_xor_si128(a, _mm_set1_epi8((char)imm)); }
static inline __m128i wbk_iohl(__m128i a, int32_t imm) { return _mm_or_si128(a, _mm_set1_epi32((uint16_t)imm)); }

/* =========================================================================
 * Compares (all-ones / all-zeros per lane)
 * =======================================================================*/
static inline __m128i wbk_ceq(__m128i a, __m128i b)   { return _mm_cmpeq_epi32(a, b); }
static inline __m128i wbk_ceqh(__m128i a, __m128i b)  { return _mm_cmpeq_epi16(a, b); }
static inline __m128i wbk_ceqb(__m128i a, __m128i b)  { return _mm_cmpeq_epi8(a, b); }
static inline __m128i wbk_cgt(__m128i a, __m128i b)   { return _mm_cmpgt_epi32(a, b); }
static inline __m128i wbk_cgth(__m128i a, __m128i b)  { return _mm_cmpgt_epi16(a, b); }
static inline __m128i wbk_cgtb(__m128i a, __m128i b)  { return _mm_cmpgt_epi8(a, b); }
static inline __m128i wbk_clgt(__m128i a, __m128i b)  { return wb_cmpgtu32(a, b); }
static inline __m128i wbk_clgth(__m128i a, __m128i b) { return wb_cmpgtu16(a, b); }
static inline __m128i wbk_clgtb(__m128i a, __m128i b) { return wb_cmpgtu8(a, b); }
static inline __m128i wbk_ceqi(__m128i a, int32_t imm)  { return _mm_cmpeq_epi32(a, _mm_set1_epi32(imm)); }
static inline __m128i wbk_cgti(__m128i a, int32_t imm)  { return _mm_cmpgt_epi32(a, _mm_set1_epi32(imm)); }
static inline __m128i wbk_clgti(__m128i a, int32_t imm) { return wb_cmpgtu32(a, _mm_set1_epi32(imm)); }
static inline __m128i wbk_ceqhi(__m128i a, int32_t imm) { return _mm_cmpeq_epi16(a, _mm_set1_epi16((short)imm)); }
static inline __m128i wbk_cgthi(__m128i a, int32_t imm) { return _mm_cmpgt_epi16(a, _mm_set1_epi16((short)imm)); }
static inline __m128i wbk_clgthi(__m128i a, int32_t imm){ return wb_cmpgtu16(a, _mm_set1_epi16((short)imm)); }
static inline __m128i wbk_ceqbi(__m128i a, int32_t imm) { return _mm_cmpeq_epi8(a, _mm_set1_epi8((char)imm)); }
static inline __m128i wbk_cgtbi(__m128i a, int32_t imm) { return _mm_cmpgt_epi8(a, _mm_set1_epi8((char)imm)); }
static inline __m128i wbk_clgtbi(__m128i a, int32_t imm){ return wb_cmpgtu8(a, _mm_set1_epi8((char)imm)); }

/* =========================================================================
 * Select / shuffle
 * =======================================================================*/
static inline __m128i wbk_selb(__m128i a, __m128i b, __m128i c)
{
    return _mm_or_si128(_mm_andnot_si128(c, a), _mm_and_si128(c, b));
}
/* shufb: the proven SSSE3 sequence from spu_helpers.h, on __m128i directly. */
static inline __m128i wbk_shufb(__m128i va, __m128i vb, __m128i vc)
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i mapped = _mm_xor_si128(vc, _mm_set1_epi8(3));
    const __m128i a_bytes = _mm_shuffle_epi8(va, mapped);
    const __m128i b_bytes = _mm_shuffle_epi8(vb, mapped);
    const __m128i select_a = _mm_cmpeq_epi8(
        _mm_and_si128(vc, _mm_set1_epi8(0x10)), zero);
    const __m128i selected = _mm_or_si128(
        _mm_and_si128(a_bytes, select_a),
        _mm_andnot_si128(select_a, b_bytes));
    const __m128i special = _mm_cmpgt_epi8(zero, vc);
    const __m128i c0 = _mm_cmpeq_epi8(
        _mm_and_si128(vc, _mm_set1_epi8((char)0xC0)),
        _mm_set1_epi8((char)0xC0));
    const __m128i e0 = _mm_cmpeq_epi8(
        _mm_and_si128(vc, _mm_set1_epi8((char)0xE0)),
        _mm_set1_epi8((char)0xE0));
    const __m128i special_value = _mm_or_si128(
        _mm_andnot_si128(e0, c0),
        _mm_and_si128(e0, _mm_set1_epi8((char)0x80)));
    return _mm_or_si128(_mm_andnot_si128(special, selected), special_value);
}

/* =========================================================================
 * Shifts / rotates -- word and halfword lanes
 * (immediate forms take the raw i7 field where the helper does; the same
 *  masking/clamping as spu_helpers is applied here)
 * =======================================================================*/
static inline __m128i wbk_shli(__m128i a, int sh)
{
    sh &= 0x3F;
    return (sh > 31) ? _mm_setzero_si128() : _mm_slli_epi32(a, sh);
}
static inline __m128i wbk_shlhi(__m128i a, int sh)
{
    sh &= 0x1F;
    return (sh > 15) ? _mm_setzero_si128() : _mm_slli_epi16(a, sh);
}
static inline __m128i wbk_roti(__m128i a, int sh)
{
    sh &= 31;
    return sh ? _mm_or_si128(_mm_slli_epi32(a, sh), _mm_srli_epi32(a, 32 - sh)) : a;
}
static inline __m128i wbk_rothi(__m128i a, int sh)
{
    sh &= 15;
    return sh ? _mm_or_si128(_mm_slli_epi16(a, sh), _mm_srli_epi16(a, 16 - sh)) : a;
}
static inline __m128i wbk_rotmi(__m128i a, int i7)
{
    const int sh = (0 - i7) & 0x3F;
    return (sh > 31) ? _mm_setzero_si128() : _mm_srli_epi32(a, sh);
}
static inline __m128i wbk_rotmai(__m128i a, int i7)
{
    const int sh = (0 - i7) & 0x3F;
    return _mm_srai_epi32(a, sh > 31 ? 31 : sh);
}
static inline __m128i wbk_rotmhi(__m128i a, int i7)   /* a.k.a. rothmi text form */
{
    const int sh = (0 - i7) & 0x1F;
    return (sh > 15) ? _mm_setzero_si128() : _mm_srli_epi16(a, sh);
}
static inline __m128i wbk_rothmi(__m128i a, int i7) { return wbk_rotmhi(a, i7); }
static inline __m128i wbk_rotmahi(__m128i a, int i7)
{
    const int sh = (0 - i7) & 0x1F;
    return _mm_srai_epi16(a, sh > 15 ? 15 : sh);
}
/* per-lane variable word shifts (AVX2) */
static inline __m128i wbk_shl(__m128i a, __m128i b)
{
    return _mm_sllv_epi32(a, _mm_and_si128(b, _mm_set1_epi32(0x3F)));
}
static inline __m128i wbk_rotm(__m128i a, __m128i b)
{
    const __m128i sh = _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), b),
                                     _mm_set1_epi32(0x3F));
    return _mm_srlv_epi32(a, sh);
}
static inline __m128i wbk_rotma(__m128i a, __m128i b)
{
    const __m128i sh = _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), b),
                                     _mm_set1_epi32(0x3F));
    return _mm_srav_epi32(a, sh);
}
static inline __m128i wbk_rot(__m128i a, __m128i b)
{
    const __m128i sh = _mm_and_si128(b, _mm_set1_epi32(31));
    return _mm_or_si128(_mm_sllv_epi32(a, sh),
                        _mm_srlv_epi32(a, _mm_sub_epi32(_mm_set1_epi32(32), sh)));
}

/* =========================================================================
 * Quadword shifts / rotates
 *
 * Bit forms (<= 7 bits) act on the architectural 128-bit big-endian value:
 * per 32-bit lane, the carry-in comes from the neighboring lane (lane i+1 is
 * less significant). Byte forms are pure byte permutations in SPU byte
 * order, done with a computed PSHUFB selector: host selector byte j =
 * SPU_W(SPU_W(j) +/- count), out-of-range marked 0x80 (PSHUFB zeroing).
 * =======================================================================*/
static inline __m128i wbk_shlqbii(__m128i a, int sh)
{
    sh &= 7;
    if (!sh) return a;
    const __m128i nxt = _mm_srli_si128(a, 4);   /* lane i := lane i+1, lane3 := 0 */
    return _mm_or_si128(_mm_slli_epi32(a, sh), _mm_srli_epi32(nxt, 32 - sh));
}
static inline __m128i wbk_rotqbii(__m128i a, int sh)
{
    sh &= 7;
    if (!sh) return a;
    const __m128i nxt = _mm_alignr_epi8(a, a, 4);  /* lane i := lane (i+1) & 3 */
    return _mm_or_si128(_mm_slli_epi32(a, sh), _mm_srli_epi32(nxt, 32 - sh));
}
static inline __m128i wbk_rotqmbii(__m128i a, int i7)
{
    const int sh = (0 - i7) & 7;
    if (!sh) return a;
    const __m128i prv = _mm_slli_si128(a, 4);   /* lane i := lane i-1, lane0 := 0 */
    return _mm_or_si128(_mm_srli_epi32(a, sh), _mm_slli_epi32(prv, 32 - sh));
}
static inline __m128i wbk_shlqbi(__m128i a, __m128i b)  { return wbk_shlqbii(a, (int)(wb_pref(b) & 7)); }
static inline __m128i wbk_rotqbi(__m128i a, __m128i b)  { return wbk_rotqbii(a, (int)(wb_pref(b) & 7)); }
static inline __m128i wbk_rotqmbi(__m128i a, __m128i b)
{
    const int sh = (0 - (int)wb_pref(b)) & 7;
    if (!sh) return a;
    const __m128i prv = _mm_slli_si128(a, 4);
    return _mm_or_si128(_mm_srli_epi32(a, sh), _mm_slli_epi32(prv, 32 - sh));
}

/* byte-shift selector builders (see block comment above) */
static inline __m128i wb_byteshift_left_sel(int sh)   /* result SPU byte i := SPU byte i+sh, else 0 */
{
    const __m128i spuidx = wb_spuidx();
    const __m128i t = _mm_add_epi8(spuidx, _mm_set1_epi8((char)sh));
    const __m128i oob = _mm_cmpgt_epi8(t, _mm_set1_epi8(15));
    return _mm_or_si128(_mm_xor_si128(t, _mm_set1_epi8(3)),
                        _mm_and_si128(oob, _mm_set1_epi8((char)0x80)));
}
static inline __m128i wb_byterot_sel(int sh)          /* result SPU byte i := SPU byte (i+sh) & 15 */
{
    const __m128i spuidx = wb_spuidx();
    const __m128i t = _mm_and_si128(_mm_add_epi8(spuidx, _mm_set1_epi8((char)sh)),
                                    _mm_set1_epi8(15));
    return _mm_xor_si128(t, _mm_set1_epi8(3));
}
static inline __m128i wb_byteshift_right_sel(int sh)  /* result SPU byte i := SPU byte i-sh, else 0 */
{
    const __m128i spuidx = wb_spuidx();
    const __m128i t = _mm_sub_epi8(spuidx, _mm_set1_epi8((char)sh));
    const __m128i oob = _mm_cmpgt_epi8(_mm_setzero_si128(), t);
    return _mm_or_si128(_mm_xor_si128(t, _mm_set1_epi8(3)),
                        _mm_and_si128(oob, _mm_set1_epi8((char)0x80)));
}
static inline __m128i wbk_shlqbyi(__m128i a, int sh)
{
    sh &= 0x1F;
    if (sh >= 16) return _mm_setzero_si128();
    return _mm_shuffle_epi8(a, wb_byteshift_left_sel(sh));
}
static inline __m128i wbk_rotqbyi(__m128i a, int sh)
{
    return _mm_shuffle_epi8(a, wb_byterot_sel(sh & 0x0F));
}
static inline __m128i wbk_rotqmbyi(__m128i a, int i7)
{
    const int sh = (0 - i7) & 0x1F;
    if (sh >= 16) return _mm_setzero_si128();
    return _mm_shuffle_epi8(a, wb_byteshift_right_sel(sh));
}
static inline __m128i wbk_shlqby(__m128i a, __m128i b)   { return wbk_shlqbyi(a, (int)(wb_pref(b) & 0x1F)); }
static inline __m128i wbk_rotqby(__m128i a, __m128i b)   { return wbk_rotqbyi(a, (int)(wb_pref(b) & 0x0F)); }
static inline __m128i wbk_shlqbybi(__m128i a, __m128i b) { return wbk_shlqbyi(a, (int)((wb_pref(b) >> 3) & 0x1F)); }
static inline __m128i wbk_rotqbybi(__m128i a, __m128i b) { return wbk_rotqbyi(a, (int)((wb_pref(b) >> 3) & 0x0F)); }
static inline __m128i wbk_rotqmby(__m128i a, __m128i b)
{
    const int sh = (0 - (int)wb_pref(b)) & 0x1F;
    if (sh >= 16) return _mm_setzero_si128();
    return _mm_shuffle_epi8(a, wb_byteshift_right_sel(sh));
}
static inline __m128i wbk_rotqmbybi(__m128i a, __m128i b)
{
    const int sh = (0 - ((int)wb_pref(b) >> 3)) & 0x1F;
    if (sh >= 16) return _mm_setzero_si128();
    return _mm_shuffle_epi8(a, wb_byteshift_right_sel(sh));
}

/* =========================================================================
 * Extends, gathers, masks, byte ops
 * =======================================================================*/
static inline __m128i wbk_xsbh(__m128i a) { return _mm_srai_epi16(_mm_slli_epi16(a, 8), 8); }
static inline __m128i wbk_xshw(__m128i a) { return _mm_srai_epi32(_mm_slli_epi32(a, 16), 16); }
static inline __m128i wbk_xswd(__m128i a)
{
    const __m128i odd = _mm_shuffle_epi32(a, _MM_SHUFFLE(3, 3, 1, 1));
    return _mm_blend_epi16(_mm_srai_epi32(odd, 31), odd, 0xCC);
}
static inline __m128i wbk_orx(__m128i a)
{
    __m128i t = _mm_or_si128(a, _mm_shuffle_epi32(a, _MM_SHUFFLE(1, 0, 3, 2)));
    t = _mm_or_si128(t, _mm_shuffle_epi32(t, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm_and_si128(t, _mm_setr_epi32(-1, 0, 0, 0));
}
static inline __m128i wbk_cntb(__m128i a)
{
    const __m128i lut = _mm_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
    const __m128i lo  = _mm_and_si128(a, _mm_set1_epi8(0x0F));
    const __m128i hi  = _mm_and_si128(_mm_srli_epi16(a, 4), _mm_set1_epi8(0x0F));
    return _mm_add_epi8(_mm_shuffle_epi8(lut, lo), _mm_shuffle_epi8(lut, hi));
}
static inline __m128i wbk_absdb(__m128i a, __m128i b)
{
    return _mm_or_si128(_mm_subs_epu8(a, b), _mm_subs_epu8(b, a));
}
static inline __m128i wbk_avgb(__m128i a, __m128i b) { return _mm_avg_epu8(a, b); }
static inline __m128i wbk_sumb(__m128i a, __m128i b)
{
    const __m128i ones8  = _mm_set1_epi8(1);
    const __m128i ones16 = _mm_set1_epi16(1);
    const __m128i sa = _mm_madd_epi16(_mm_maddubs_epi16(a, ones8), ones16);
    const __m128i sb = _mm_madd_epi16(_mm_maddubs_epi16(b, ones8), ones16);
    return _mm_or_si128(_mm_slli_epi32(sb, 16), sa);
}
/* gb: LSB of each word gathered into bits 3..0 of the preferred word */
static inline __m128i wbk_gb(__m128i a)
{
    const int m = _mm_movemask_ps(_mm_castsi128_ps(_mm_slli_epi32(a, 31)));
    const uint32_t v = (uint32_t)(((m & 1) << 3) | ((m & 2) << 1) |
                                  ((m & 4) >> 1) | ((m & 8) >> 3));
    return wb_pref_set(v);
}
/* fsm / fsmh / fsmb: expand preferred-slot bits to lane masks */
static inline __m128i wbk_fsm(__m128i a)
{
    const __m128i bits = _mm_setr_epi32(8, 4, 2, 1);
    const __m128i v = _mm_and_si128(_mm_shuffle_epi32(a, 0), bits);
    return _mm_cmpeq_epi32(v, bits);
}
static inline __m128i wbk_fsmh(__m128i a)
{
    /* host _u16 lane l holds SPU halfword H = l^1, which takes bit (7-H).
     * Only bits 0-7 of the preferred word matter, so broadcast byte 0 to
     * every byte (a 32-bit lane broadcast would put the word's HIGH half in
     * the odd 16-bit lanes); every mask below is < 0x100, so the duplicated
     * high byte ANDs away. */
    const __m128i bits = _mm_setr_epi16(0x40, (short)0x80, 0x10, 0x20, 4, 8, 1, 2);
    const __m128i v = _mm_and_si128(_mm_shuffle_epi8(a, _mm_setzero_si128()), bits);
    return _mm_cmpeq_epi16(v, bits);
}
static inline __m128i wbk_fsmb(__m128i a)
{
    /* host byte j holds SPU byte P = j^3, which takes bit (15-P); byte 0/1 of
     * the broadcast value hold bits 0-7 / 8-15. */
    const __m128i bytesel = _mm_setr_epi8(1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0);
    const __m128i bitmask = _mm_setr_epi8(
        0x10, 0x20, 0x40, (char)0x80, 0x01, 0x02, 0x04, 0x08,
        0x10, 0x20, 0x40, (char)0x80, 0x01, 0x02, 0x04, 0x08);
    const __m128i bcast = _mm_shuffle_epi8(a, bytesel);
    const __m128i v = _mm_and_si128(bcast, bitmask);
    return _mm_cmpeq_epi8(v, bitmask);
}

/* =========================================================================
 * Generate-controls (insertion selectors for shufb), scalar position input
 * =======================================================================*/
static inline __m128i wb_gen_base(void)
{
    /* host byte j = 0x10 + SPU_W(j): selector "b's SPU byte t at result t" */
    return _mm_setr_epi8(0x13, 0x12, 0x11, 0x10, 0x17, 0x16, 0x15, 0x14,
                         0x1B, 0x1A, 0x19, 0x18, 0x1F, 0x1E, 0x1D, 0x1C);
}
static inline __m128i wbk_cbd_pos(uint32_t pos)
{
    const __m128i spuidx = wb_spuidx();
    const __m128i mask = _mm_cmpeq_epi8(spuidx, _mm_set1_epi8((char)(pos & 0xF)));
    return _mm_blendv_epi8(wb_gen_base(), _mm_set1_epi8(0x03), mask);
}
static inline __m128i wbk_chd_pos(uint32_t pos)
{
    const __m128i spuidx = wb_spuidx();
    const __m128i mask = _mm_cmpeq_epi8(
        _mm_and_si128(spuidx, _mm_set1_epi8((char)0xFE)),
        _mm_set1_epi8((char)(pos & 0xE)));
    const __m128i ins = _mm_or_si128(_mm_set1_epi8(0x02),
                                     _mm_and_si128(spuidx, _mm_set1_epi8(1)));
    return _mm_blendv_epi8(wb_gen_base(), ins, mask);
}
static inline __m128i wbk_cwd_pos(uint32_t pos)
{
    const __m128i spuidx = wb_spuidx();
    const __m128i mask = _mm_cmpeq_epi8(
        _mm_and_si128(spuidx, _mm_set1_epi8((char)0xFC)),
        _mm_set1_epi8((char)(pos & 0xC)));
    return _mm_blendv_epi8(wb_gen_base(),
                           _mm_and_si128(spuidx, _mm_set1_epi8(3)), mask);
}
static inline __m128i wbk_cdd_pos(uint32_t pos)
{
    const __m128i spuidx = wb_spuidx();
    const __m128i mask = _mm_cmpeq_epi8(
        _mm_and_si128(spuidx, _mm_set1_epi8((char)0xF8)),
        _mm_set1_epi8((char)(pos & 0x8)));
    return _mm_blendv_epi8(wb_gen_base(),
                           _mm_and_si128(spuidx, _mm_set1_epi8(7)), mask);
}

/* =========================================================================
 * Wrapped kernels: exact spu_helpers semantics through a u128 round trip.
 * Rare/complex operations and the whole FP stack (which must keep the
 * yz_xf_ieee gate, the xfloat scalar reference, and the optional SIMD
 * adapter behavior exactly as in production).
 * =======================================================================*/
#define WBK_WRAP1(name, helper) \
    static inline __m128i name(__m128i a) \
    { return wb_from_u128(helper(wb_to_u128(a))); }
#define WBK_WRAP2(name, helper) \
    static inline __m128i name(__m128i a, __m128i b) \
    { return wb_from_u128(helper(wb_to_u128(a), wb_to_u128(b))); }
#define WBK_WRAP3(name, helper) \
    static inline __m128i name(__m128i a, __m128i b, __m128i c) \
    { return wb_from_u128(helper(wb_to_u128(a), wb_to_u128(b), wb_to_u128(c))); }
#define WBK_WRAP1I(name, helper) \
    static inline __m128i name(__m128i a, int32_t imm) \
    { return wb_from_u128(helper(wb_to_u128(a), imm)); }

WBK_WRAP1(wbk_clz, spu_clz)
WBK_WRAP1(wbk_gbh, spu_gbh)
WBK_WRAP1(wbk_gbb, spu_gbb)
WBK_WRAP2(wbk_shlh, spu_shlh)
WBK_WRAP2(wbk_roth, spu_roth)
WBK_WRAP2(wbk_rothm, spu_rothm)
WBK_WRAP2(wbk_rothma, spu_rothma)
/* float family: full production behavior stack */
WBK_WRAP2(wbk_fa, spu_fa)
WBK_WRAP2(wbk_fs, spu_fs)
WBK_WRAP2(wbk_fm, spu_fm)
WBK_WRAP2(wbk_fi, spu_fi)
WBK_WRAP3(wbk_fma, spu_fma)
WBK_WRAP3(wbk_fms, spu_fms)
WBK_WRAP3(wbk_fnms, spu_fnms)
WBK_WRAP2(wbk_fceq, spu_fceq)
WBK_WRAP2(wbk_fcgt, spu_fcgt)
WBK_WRAP2(wbk_fcmeq, spu_fcmeq)
WBK_WRAP2(wbk_fcmgt, spu_fcmgt)
WBK_WRAP1(wbk_frest, spu_frest)
WBK_WRAP1(wbk_frsqest, spu_frsqest)
WBK_WRAP1(wbk_fesd, spu_fesd)
WBK_WRAP1(wbk_frds, spu_frds)
WBK_WRAP1I(wbk_cflts, spu_cflts)
WBK_WRAP1I(wbk_cfltu, spu_cfltu)
WBK_WRAP1I(wbk_csflt, spu_csflt)
WBK_WRAP1I(wbk_cuflt, spu_cuflt)
WBK_WRAP1I(wbk_dftsv, spu_dftsv)
/* double family */
WBK_WRAP2(wbk_dfa, spu_dfa)
WBK_WRAP2(wbk_dfs, spu_dfs)
WBK_WRAP2(wbk_dfm, spu_dfm)
WBK_WRAP3(wbk_dfma, spu_dfma)
WBK_WRAP3(wbk_dfms, spu_dfms)
WBK_WRAP3(wbk_dfnms, spu_dfnms)
WBK_WRAP3(wbk_dfnma, spu_dfnma)
WBK_WRAP2(wbk_dfceq, spu_dfceq)
WBK_WRAP2(wbk_dfcmeq, spu_dfcmeq)
WBK_WRAP2(wbk_dfcgt, spu_dfcgt)
WBK_WRAP2(wbk_dfcmgt, spu_dfcmgt)
WBK_WRAP1(wbk_mfspr, spu_mfspr)

#undef WBK_WRAP1
#undef WBK_WRAP2
#undef WBK_WRAP3
#undef WBK_WRAP1I

#ifdef __cplusplus
}
#endif

#endif /* SPU_WB_SIMD_H */
