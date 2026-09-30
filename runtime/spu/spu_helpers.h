/*
 * ps3recomp - SPU instruction semantics helpers
 *
 * Pure-C, header-only implementation of the per-instruction semantics used
 * by lifter-generated code. Extracted from spu_lifter.py so the helpers
 * have one source of truth and can be unit-tested directly (see
 * runtime/spu/tests/test_spu_helpers.c).
 *
 * Each helper is `static inline u128 spu_<mnemonic>(...)`. Naming, lane
 * widths and big-endian conventions match runtime/spu/spu_context.h.
 */

#ifndef SPU_HELPERS_H
#define SPU_HELPERS_H

#include "spu_context.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#if !defined(_MSC_VER) && defined(__SSSE3__)
#include <tmmintrin.h>
#elif !defined(_MSC_VER) && defined(__SSE2__)
#include <emmintrin.h>
#endif

#ifndef YZ_SPU_SIMD_ABSDB
#define YZ_SPU_SIMD_ABSDB 0
#endif

#ifndef YZ_SPU_SIMD_XFLOAT
#define YZ_SPU_SIMD_XFLOAT 0
#endif
#if YZ_SPU_SIMD_XFLOAT
#include "spu_xfloat_simd.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _MSC_VER
#include <intrin.h>
#include <tmmintrin.h>
static inline int spu_clz32(uint32_t x) {
    unsigned long idx;
    if (!x) return 32;
    _BitScanReverse(&idx, x);
    return 31 - (int)idx;
}
#else
static inline int spu_clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }
#endif

/* Architectural SPU barrier: the lifted `sync`/`dsync`/`syncc` emission.
 * It orders all earlier LS and channel accesses before later ones, and LS must
 * be consistent when observed by another entity (this runtime's host-thread
 * producer -> SPU consumer handoff). A no-op is neither a compiler nor a CPU
 * barrier: MSVC at /O2 may reorder plain LS accesses across an empty
 * statement. This is a full fence: the locked exchange forces total order and
 * is opaque to the optimizer. */
#ifdef _MSC_VER
static inline void spu_arch_fence(void) { long spu_fence_tmp = 0; _InterlockedExchange(&spu_fence_tmp, 1); }
#else
static inline void spu_arch_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#endif

/* SPU byte position -> our _u8 index.
 * Our u128 is host-native little-endian (_u32[i]=SPU word i as a value via
 * spu_ls_read128's big-endian load), so within each 4-byte word the _u8[]
 * bytes are reversed vs SPU big-endian byte order. Any op defined in terms of
 * SPU *byte positions* (quadword byte rotates/shifts, gen-controls, shuffle
 * insertion) must map SPU byte P to our index W(P). Word-aligned operations
 * are unaffected (W is identity modulo the within-word reversal). */
#define SPU_W(P) (((P) & ~3) | (3 - ((P) & 3)))

/* ---- constructors ---- */
static inline u128 spu_splat_u32(uint32_t v) {
    u128 r; r._u32[0]=v; r._u32[1]=v; r._u32[2]=v; r._u32[3]=v; return r;
}
/* Link register value for brsl/bisl/bisled: the SPU writes the return address
 * into the preferred word (word 0) and zeroes the other three slots. Splatting
 * it would corrupt any code that saves, reloads or operates on the full
 * 128-bit link. */
static inline u128 spu_link(uint32_t addr) {
    u128 r; r._u32[0]=addr; r._u32[1]=0; r._u32[2]=0; r._u32[3]=0; return r;
}
/* Preferred-slot scalar: value in word 0, remaining slots zero. This is the
 * CBEA scalar channel convention; rchcnt returns the count this way (for
 * example {1,0,0,0}, not a splat). Same layout as spu_link. */
static inline u128 spu_pref_u32(uint32_t v) {
    u128 r; r._u32[0]=v; r._u32[1]=0; r._u32[2]=0; r._u32[3]=0; return r;
}
static inline u128 spu_splat_u16(uint16_t v) {
    u128 r; for (int i=0;i<8;i++) r._u16[i]=v; return r;
}
static inline u128 spu_splat_u8(uint8_t v) {
    u128 r; for (int i=0;i<16;i++) r._u8[i]=v; return r;
}
static inline u128 spu_zero(void) { u128 r; memset(&r,0,sizeof r); return r; }

/* ---- integer arithmetic (SIMD) ---- */
static inline u128 spu_a(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+b._u32[i]; return r; }
static inline u128 spu_sf(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=b._u32[i]-a._u32[i]; return r; }
static inline u128 spu_ah(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]+b._u16[i]; return r; }
static inline u128 spu_sfh(u128 a, u128 b){ u128 r; for(int i=0;i<8;i++) r._u16[i]=b._u16[i]-a._u16[i]; return r; }
static inline u128 spu_ai(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+(uint32_t)imm; return r; }
static inline u128 spu_ahi(u128 a, int32_t imm){ u128 r; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]+(uint16_t)imm; return r; }
static inline u128 spu_sfi(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)imm-a._u32[i]; return r; }
static inline u128 spu_sfhi(u128 a, int32_t imm){ u128 r; for(int i=0;i<8;i++) r._u16[i]=(uint16_t)imm-a._u16[i]; return r; }

/* ---- multiply (low halfword of each word x ... -> 32-bit, per SPU mpy) ----
 * Sub-lane indexing assumes a little-endian host (matches the recompiler's
 * target). The "low halfword of word i" in SPU BE semantics is _s16[2i] on
 * an LE host (not _s16[2i+1], which would be correct on a BE host). */
static inline u128 spu_mpy(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int32_t)b._s16[i*2]; return r; }
/* mpya: 16x16 signed multiply of low halves + add rc (per word, RRR form). */
static inline u128 spu_mpya(u128 a, u128 b, u128 c) { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int32_t)b._s16[i*2]+c._s32[i]; return r; }
/* sfx: extended subtract rb-ra-1+carry; carry-in = low bit of old rt (RT is 3rd src). */
static inline u128 spu_sfx(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=b._u32[i]+~a._u32[i]+(t._u32[i]&1u); return r; }
/* fi: floating interpolate. RB is decoded per the Floating Reciprocal
 * Estimate encoding (the reciprocal absolute square root estimate uses the
 * same base/step field layout): sign(1) | biased exponent(8) | base
 * fraction(13) | step fraction(10). RA bits 13:31 (its low 19 bits) supply
 * the interpolation fraction Y = (RA & 0x7FFFF) / 2^19.
 *   RT = (-1)^S * (1.BaseFraction - 0.000_StepFraction * Y) * 2^(exp-127)
 * sign|exp|BaseFraction (RB's top 22 bits) is already a valid IEEE-754 bit
 * pattern for the Base term, so `RB & 0xFFFFFC00` reinterpreted as float
 * gives Base directly; StepFraction has 3 implicit leading zero bits ahead
 * of its own 10 bits (the "0.000" prefix), i.e. Step = StepFrac*2^(exp-127-13),
 * same sign as Base. */
static inline u128 spu_fi(u128 a, u128 b) {
    u128 r;
    for (int i = 0; i < 4; i++) {
        uint32_t rb = b._u32[i];
        uint32_t ra = a._u32[i];
        uint32_t exp = (rb >> 23) & 0xFFu;
        uint32_t base_bits = rb & 0xFFFFFC00u;
        float base; memcpy(&base, &base_bits, sizeof base);
        uint32_t step_frac = rb & 0x3FFu;
        float step = ldexpf((float)step_frac, (int)exp - 127 - 13);
        if (base < 0.0f) step = -step;
        float y = ldexpf((float)(ra & 0x7FFFFu), -19);
        r._f32[i] = base - step * y;
    }
    return r;
}
static inline u128 spu_mpyu(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[i*2]*(uint32_t)b._u16[i*2]; return r; }
static inline u128 spu_mpyi(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int16_t)imm; return r; }

/* ---- bitwise logic (whole 128 bits) ---- */
static inline u128 spu_and(u128 a, u128 b) { u128 r; r._u64[0]=a._u64[0]&b._u64[0]; r._u64[1]=a._u64[1]&b._u64[1]; return r; }
static inline u128 spu_or(u128 a, u128 b)  { u128 r; r._u64[0]=a._u64[0]|b._u64[0]; r._u64[1]=a._u64[1]|b._u64[1]; return r; }
static inline u128 spu_xor(u128 a, u128 b) { u128 r; r._u64[0]=a._u64[0]^b._u64[0]; r._u64[1]=a._u64[1]^b._u64[1]; return r; }
static inline u128 spu_nand(u128 a, u128 b){ u128 r; r._u64[0]=~(a._u64[0]&b._u64[0]); r._u64[1]=~(a._u64[1]&b._u64[1]); return r; }
static inline u128 spu_nor(u128 a, u128 b) { u128 r; r._u64[0]=~(a._u64[0]|b._u64[0]); r._u64[1]=~(a._u64[1]|b._u64[1]); return r; }
static inline u128 spu_andc(u128 a, u128 b){ u128 r; r._u64[0]=a._u64[0]&~b._u64[0]; r._u64[1]=a._u64[1]&~b._u64[1]; return r; }
static inline u128 spu_orc(u128 a, u128 b) { u128 r; r._u64[0]=a._u64[0]|~b._u64[0]; r._u64[1]=a._u64[1]|~b._u64[1]; return r; }
static inline u128 spu_andi(u128 a, int32_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]&(uint32_t)imm; return r; }
static inline u128 spu_ori(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]|(uint32_t)imm; return r; }
static inline u128 spu_xori(u128 a, int32_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]^(uint32_t)imm; return r; }

/* ---- count leading zeros / population count per byte ---- */
static inline u128 spu_clz(u128 a)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)spu_clz32(a._u32[i]); return r; }
static inline u128 spu_cntb(u128 a) { u128 r; for(int i=0;i<16;i++){ uint8_t v=a._u8[i],c=0; while(v){c+=v&1;v>>=1;} r._u8[i]=c; } return r; }

/* ---- compares: all-ones / all-zeros per lane ---- */
static inline u128 spu_ceq(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._u32[i]==b._u32[i])?0xFFFFFFFFu:0; return r; }
static inline u128 spu_ceqh(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]==b._u16[i])?0xFFFFu:0; return r; }
static inline u128 spu_ceqb(u128 a, u128 b) { u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]==b._u8[i])?0xFFu:0; return r; }
static inline u128 spu_cgt(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._s32[i]>b._s32[i])?0xFFFFFFFFu:0; return r; }
static inline u128 spu_cgth(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]>b._s16[i])?0xFFFFu:0; return r; }
static inline u128 spu_cgtb(u128 a, u128 b) { u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._s8[i]>b._s8[i])?0xFFu:0; return r; }
static inline u128 spu_clgt(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._u32[i]>b._u32[i])?0xFFFFFFFFu:0; return r; }
static inline u128 spu_clgth(u128 a, u128 b){ u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]>b._u16[i])?0xFFFFu:0; return r; }
static inline u128 spu_clgtb(u128 a, u128 b){ u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]>b._u8[i])?0xFFu:0; return r; }
static inline u128 spu_ceqi(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._s32[i]==imm)?0xFFFFFFFFu:0; return r; }
static inline u128 spu_cgti(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._s32[i]>imm)?0xFFFFFFFFu:0; return r; }
static inline u128 spu_clgti(u128 a, int32_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._u32[i]>(uint32_t)imm)?0xFFFFFFFFu:0; return r; }

/* ---- select / shuffle ---- */
static inline u128 spu_selb(u128 a, u128 b, u128 c) {
    u128 r;
    r._u64[0]=(a._u64[0]&~c._u64[0])|(b._u64[0]&c._u64[0]);
    r._u64[1]=(a._u64[1]&~c._u64[1])|(b._u64[1]&c._u64[1]);
    return r;
}
/* shufb special selectors per Cell BE ISA:
 *   sel & 0xE0 == 0xE0 -> 0x80
 *   sel & 0xC0 == 0xC0 -> 0xFF
 *   sel & 0xC0 == 0x80 -> 0x00
 *   otherwise           -> concat{a,b}[sel & 0x1F] */
static inline u128 spu_shufb(u128 a, u128 b, u128 c) {
#if (!defined(YZ_SPU_SIMD_SHUFB) || YZ_SPU_SIMD_SHUFB) && \
    (defined(_MSC_VER) || defined(__SSSE3__))
    /*
     * PSHUFB performs the same lane-local byte lookup once the runtime's
     * within-word SPU byte mapping is folded into the selector.  SPU_W(p) is
     * p ^ 3, so toggling the selector's low two bits maps an architectural
     * byte index to the corresponding host byte index.  Bit 4 still selects
     * the second source vector; bits 5 and 6 are ignored by the ISA.
     *
     * SPU's special selectors need one extra mask pass:
     *   80..BF -> 00, C0..DF -> FF, E0..FF -> 80.
     * This path is kept branch-free because shufb is very hot.
     */
    const __m128i va = _mm_loadu_si128((const __m128i*)(const void*)&a);
    const __m128i vb = _mm_loadu_si128((const __m128i*)(const void*)&b);
    const __m128i vc = _mm_loadu_si128((const __m128i*)(const void*)&c);
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
    const __m128i value = _mm_or_si128(
        _mm_andnot_si128(special, selected), special_value);
    u128 r;
    _mm_storeu_si128((__m128i*)(void*)&r, value);
    return r;
#else
    /* shufb is defined on SPU byte positions. Our u128 is host-native LE, so SPU
     * byte P lives at _u8[SPU_W(P)]; map every access (the concat source, the
     * control, and the result) through SPU_W so a control supplied as an immediate
     * (ila/il) or an LS-loaded constant -- already in true SPU byte order -- is
     * interpreted correctly. The cbd/chd/cwd/cdd generators below produce true
     * SPU-byte-order selectors to match. */
    uint8_t cat[32];
    for (int j=0;j<16;j++) cat[j]    = a._u8[SPU_W(j)];   /* concat SPU byte j  = a SPU byte j */
    for (int j=0;j<16;j++) cat[16+j] = b._u8[SPU_W(j)];   /* concat SPU byte 16+j = b SPU byte j */
    u128 r;
    for (int t=0;t<16;t++) {                              /* result SPU byte t */
        uint8_t s = c._u8[SPU_W(t)];                      /* control SPU byte t */
        uint8_t v;
        if      ((s & 0xE0)==0xE0) v=0x80;
        else if ((s & 0xC0)==0xC0) v=0xFF;
        else if ((s & 0xC0)==0x80) v=0x00;
        else                       v=cat[s & 0x1F];       /* concat SPU byte (s & 0x1F) */
        r._u8[SPU_W(t)] = v;
    }
    return r;
#endif
}

/* ---- shift / rotate immediate (word lanes) ---- */
static inline u128 spu_shli(u128 a, int sh)  { u128 r; sh&=0x3F; for(int i=0;i<4;i++) r._u32[i]=(sh>31)?0:(a._u32[i]<<sh); return r; }
static inline u128 spu_shlhi(u128 a, int sh) { u128 r; sh&=0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]<<sh); return r; }
static inline u128 spu_roti(u128 a, int sh)  { u128 r; sh&=31; for(int i=0;i<4;i++) r._u32[i]= sh ? ((a._u32[i]<<sh)|(a._u32[i]>>(32-sh))) : a._u32[i]; return r; }
static inline u128 spu_rothi(u128 a, int sh) { u128 r; sh&=15; for(int i=0;i<8;i++) r._u16[i]=(uint16_t)((a._u16[i]<<sh)|(a._u16[i]>>(16-sh))); return r; }
static inline u128 spu_rotmi(u128 a, int i7)  { u128 r; int sh=(0-i7)&0x3F; for(int i=0;i<4;i++) r._u32[i]=(sh>31)?0:(a._u32[i]>>sh); return r; }
static inline u128 spu_rotmai(u128 a, int i7) { u128 r; int sh=(0-i7)&0x3F; for(int i=0;i<4;i++) r._s32[i]=(sh>31)?(a._s32[i]>>31):(a._s32[i]>>sh); return r; }
static inline u128 spu_rotmhi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); return r; }
static inline u128 spu_shlqbyi(u128 a, int sh) { u128 r=spu_zero(); sh&=0x1F; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
/* Host byte i is architectural byte i^3. The mask maps the rotated
 * architectural source back to host order; every lane is in [0,15]. */
static inline u128 spu_rotqby_bytes(u128 value, unsigned shift)
{
#if (!defined(YZ_SPU_SIMD_ROTQBY) || YZ_SPU_SIMD_ROTQBY) && \
    (defined(_MSC_VER) || defined(__SSSE3__))
    static const unsigned char masks[16][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {7, 0, 1, 2, 11, 4, 5, 6, 15, 8, 9, 10, 3, 12, 13, 14},
    {6, 7, 0, 1, 10, 11, 4, 5, 14, 15, 8, 9, 2, 3, 12, 13},
    {5, 6, 7, 0, 9, 10, 11, 4, 13, 14, 15, 8, 1, 2, 3, 12},
    {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3},
    {11, 4, 5, 6, 15, 8, 9, 10, 3, 12, 13, 14, 7, 0, 1, 2},
    {10, 11, 4, 5, 14, 15, 8, 9, 2, 3, 12, 13, 6, 7, 0, 1},
    {9, 10, 11, 4, 13, 14, 15, 8, 1, 2, 3, 12, 5, 6, 7, 0},
    {8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7},
    {15, 8, 9, 10, 3, 12, 13, 14, 7, 0, 1, 2, 11, 4, 5, 6},
    {14, 15, 8, 9, 2, 3, 12, 13, 6, 7, 0, 1, 10, 11, 4, 5},
    {13, 14, 15, 8, 1, 2, 3, 12, 5, 6, 7, 0, 9, 10, 11, 4},
    {12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
    {3, 12, 13, 14, 7, 0, 1, 2, 11, 4, 5, 6, 15, 8, 9, 10},
    {2, 3, 12, 13, 6, 7, 0, 1, 10, 11, 4, 5, 14, 15, 8, 9},
    {1, 2, 3, 12, 5, 6, 7, 0, 9, 10, 11, 4, 13, 14, 15, 8}
    };
    const __m128i input = _mm_loadu_si128((const __m128i*)(const void*)&value);
    const __m128i mask = _mm_loadu_si128((const __m128i*)(const void*)masks[shift&15u]);
    u128 result;
    _mm_storeu_si128((__m128i*)(void*)&result, _mm_shuffle_epi8(input,mask));
    return result;
#else
    u128 result;
    shift &= 15u;
    for (unsigned i=0; i<16; ++i)
        result._u8[SPU_W(i)] = value._u8[SPU_W((i+shift)&15u)];
    return result;
#endif
}

static inline u128 spu_rotqbyi(u128 a, int sh) { return spu_rotqby_bytes(a,(unsigned)sh); }
/* Bit-level quadword shifts/rotates operate on the whole 128-bit big-endian
 * value. Our _u64[0]/_u64[1] are word-scrambled (host-LE: _u64[0] = word0 |
 * word1<<32, but SPU's MS half is word0<<32 | word1), so native shifts on them
 * corrupt any bits crossing a 32-bit boundary. Assemble the logical hi/lo halves
 * with _u32[0] as most-significant, shift, then disassemble. */
static inline u128 spu_shlqbii(u128 a, int sh) { sh&=7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqbii(u128 a, int sh) { sh&=7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh)|(hi>>(64-sh));
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }

/* ---- single-precision float (4 lanes) ----
 *
 * SPU extended-range ("xfloat") semantics for fa/fs/fm/fma/fms/fnms.
 *
 * SPU single precision differs from IEEE 754 single precision:
 *   - Register range is extended: an 8-bit exponent field of 0xFF is a
 *     normal value here (magnitude up to Smax), never Inf/NaN. Smin bit
 *     pattern = 0x00800000, Smax bit pattern = 0x7FFFFFFF.
 *   - No NaN or Inf is ever produced. A result whose magnitude would exceed
 *     Smax saturates to Smax with the operation's sign.
 *   - Denormals are not supported: an operation that would generate a denorm
 *     under IEEE rules generates a positive zero, and a denorm operand is
 *     treated as a zero. The ISA does not state a sign for the
 *     single-precision operand flush, so every flush here, operand or
 *     result, is unsigned +0.0.
 *   - The only rounding mode is truncation toward zero.
 *   - The multiply in fma/fms/fnms is exact and unbounded in range before
 *     the add/subtract; only the final (post add/subtract) result is checked
 *     against Smax/Smin.
 *
 * Implementation: decode each SPU-shaped f32 bit pattern (never through the
 * host `float` type: an extended-range SPU value's bits, read as IEEE
 * binary32, are an Inf/NaN pattern, e.g. Smax=0x7FFFFFFF is a QNaN bit
 * pattern, and a real float/double add/compare would apply the host FPU's
 * Inf/NaN rules) into a double (flushing an exponent-field-0 operand to +0.0
 * first), do the requested arithmetic in double, then encode the double
 * result back to an SPU-shaped f32 bit pattern: truncate the mantissa to 23
 * bits, flush a sub-Smin magnitude to +0.0, saturate a magnitude beyond Smax
 * to Smax with the result's sign.
 *
 * Accuracy: fm is bit-exact. For fa/fs/fma/fms/fnms the rounding of the double
 * arithmetic follows the calling thread's MXCSR rounding mode before the final
 * truncate. Threads that ran spu_xfloat_simd_thread_enter() on a machine with
 * the packed path available use round-toward-zero; other threads keep the host
 * default (round to nearest) and can differ from hardware by 1 ULP. Near the
 * Smin flush boundary the fma family can misclassify flush versus normal.
 */

#define SPU_XF_SMAX_MAG   0x7FFFFFFFu   /* Smax magnitude bits (sign OR'd in by the caller) */

/* SPU-shaped f32 bits -> double. Flushes an exponent-field-0 operand
 * (true zero or IEEE-denormal-shaped) to unsigned +0.0; any other exponent
 * (including 0xFF, "extended") decodes as a genuine 1.fraction * 2^(exp-127)
 * magnitude, never routed through the host `float` type. */
static inline double spu_xf_decode(uint32_t bits)
{
    uint32_t exp  = (bits >> 23) & 0xFFu;
    if (exp == 0) return 0.0;
    /* Every nonzero SPU exponent is a normal finite binary64 value. Move
     * the exact sign/fraction bits and rebase the exponent; exponent 255
     * remains an extended finite SPU value rather than an IEEE float NaN.
     * This is the same exact value as ldexp(1+fraction/2^23, exp-127),
     * without a floating-point conversion or math-library call. */
    const uint64_t encoded = ((uint64_t)(bits & 0x80000000u) << 32) |
        ((uint64_t)(exp + (1023u - 127u)) << 52) |
        ((uint64_t)(bits & 0x7FFFFFu) << 29);
    double value;
    memcpy(&value, &encoded, sizeof(value));
    return value;
}

/* double -> SPU-shaped f32 bits. Zero (either sign) and any magnitude below
 * Smin flush to unsigned +0.0. A magnitude beyond Smax saturates to Smax with
 * the value's sign. Otherwise truncates (never rounds) the mantissa to 23
 * fraction bits. */
static inline uint32_t spu_xf_encode(double val)
{
    if (val == 0.0) return 0u;
    int sign = (val < 0.0) ? 1 : 0;
    double mag = sign ? -val : val;
    int fexp;
    double mant = frexp(mag, &fexp);      /* mag = mant * 2^fexp, mant in [0.5,1) */
    mant *= 2.0; fexp -= 1;               /* rebase: mant in [1,2), mag = mant * 2^fexp */
    int biased = fexp + 127;
    if (biased > 255)
        return ((uint32_t)sign << 31) | SPU_XF_SMAX_MAG;   /* overflow: saturate to Smax */
    if (biased <= 0)
        return 0u;                                          /* underflow: flush to +0.0 */
    uint32_t frac = (uint32_t)((mant - 1.0) * 8388608.0);   /* truncate toward zero */
    if (frac > 0x7FFFFFu) frac = 0x7FFFFFu;   /* defensive; truncation of mant<2.0 can't reach this */
    return ((uint32_t)sign << 31) | ((uint32_t)biased << 23) | frac;
}

static inline u128 spu_xf_scalar_fa(u128 a, u128 b)
{
    u128 r;
    for (int i = 0; i < 4; i++)
        r._u32[i] = spu_xf_encode(spu_xf_decode(a._u32[i]) + spu_xf_decode(b._u32[i]));
    return r;
}
static inline u128 spu_fa(u128 a, u128 b)
{
    u128 r;
#if YZ_SPU_SIMD_XFLOAT
    if (g_spu_xfloat_simd_enabled && spu_xfloat_simd_fa(&r, &a, &b)) return r;
#endif
    return spu_xf_scalar_fa(a, b);
}
static inline u128 spu_xf_scalar_fs(u128 a, u128 b)
{
    u128 r;
    for (int i = 0; i < 4; i++)
        r._u32[i] = spu_xf_encode(spu_xf_decode(a._u32[i]) - spu_xf_decode(b._u32[i]));
    return r;
}
static inline u128 spu_fs(u128 a, u128 b)
{
    u128 r;
#if YZ_SPU_SIMD_XFLOAT
    if (g_spu_xfloat_simd_enabled && spu_xfloat_simd_fs(&r, &a, &b)) return r;
#endif
    return spu_xf_scalar_fs(a, b);
}

/* Float<->int conversions with scale (RI8):
 *   cflts/cfltu: result = (int)(a * 2^(173-i8))   -- i8=173 is the identity
 *   csflt/cuflt: result = (float)int * 2^(i8-155) -- i8=155 is the identity
 * The scale factor is computed in double so 2^173 doesn't overflow. */
/* The operand is decoded through spu_xf_decode(), not (double)a._f32[i].
 * The SPU has no NaN/Inf; every f32 bit pattern, including ones that look
 * like a host QNaN/Inf (exponent field 0xFF), is a genuine extended-range
 * finite value per the xfloat model documented above spu_xf_decode/encode.
 * Going through the native `float` member would route those bit patterns
 * through the host FPU's NaN/Inf comparison and cast rules: both saturation
 * compares below would be false for a NaN operand and the final
 * (int32_t)v / (uint32_t)v cast would be UB on a NaN double. Decoding via
 * spu_xf_decode always yields a finite double, so the saturate-then-truncate
 * logic is well-defined for every input bit pattern, with deterministic
 * saturation at the int32/uint32 boundary. */
static inline u128 spu_cflts(u128 a, int i8){ u128 r; double f=exp2((double)(173-i8));
    for(int i=0;i<4;i++){ double v=spu_xf_decode(a._u32[i])*f; if(v>2147483647.0)v=2147483647.0; if(v<-2147483648.0)v=-2147483648.0; r._s32[i]=(int32_t)v; } return r; }
static inline u128 spu_cfltu(u128 a, int i8){ u128 r; double f=exp2((double)(173-i8));
    for(int i=0;i<4;i++){ double v=spu_xf_decode(a._u32[i])*f; if(v<0)v=0; if(v>4294967295.0)v=4294967295.0; r._u32[i]=(uint32_t)v; } return r; }
static inline u128 spu_csflt(u128 a, int i8){ u128 r; double f=exp2((double)(i8-155));
    for(int i=0;i<4;i++) r._f32[i]=(float)((double)a._s32[i]*f); return r; }
static inline u128 spu_cuflt(u128 a, int i8){ u128 r; double f=exp2((double)(i8-155));
    for(int i=0;i<4;i++) r._f32[i]=(float)((double)a._u32[i]*f); return r; }
static inline u128 spu_xf_scalar_fm(u128 a, u128 b)
{
    u128 r;
    for (int i = 0; i < 4; i++)
        r._u32[i] = spu_xf_encode(spu_xf_decode(a._u32[i]) * spu_xf_decode(b._u32[i]));
    return r;
}
static inline u128 spu_fm(u128 a, u128 b)
{
    u128 r;
#if YZ_SPU_SIMD_XFLOAT
    if (g_spu_xfloat_simd_enabled && spu_xfloat_simd_fm(&r, &a, &b)) return r;
#endif
    return spu_xf_scalar_fm(a, b);
}
/* fma/fms/fnms: the SPU multiply is exact (single final rounding = true fused
 * multiply-add); see the xfloat block comment above spu_fa. The product and
 * accumulate are computed entirely in double (exact, per the xfloat block
 * comment) and encoded once at the end: the same "exact multiply, one final
 * rounding" property, but in the SPU's own extended-range/no-NaN/no-Inf
 * format rather than IEEE. */
static inline u128 spu_xf_scalar_fma(u128 a, u128 b, u128 c)
{
    u128 r;
    for (int i = 0; i < 4; i++)
        r._u32[i] = spu_xf_encode(spu_xf_decode(a._u32[i]) * spu_xf_decode(b._u32[i]) + spu_xf_decode(c._u32[i]));
    return r;
}
static inline u128 spu_fma(u128 a, u128 b, u128 c)
{
    u128 r;
#if YZ_SPU_SIMD_XFLOAT
    if (g_spu_xfloat_simd_enabled && spu_xfloat_simd_fma(&r, &a, &b, &c)) return r;
#endif
    return spu_xf_scalar_fma(a, b, c);
}
static inline u128 spu_xf_scalar_fms(u128 a, u128 b, u128 c)
{
    u128 r;
    for (int i = 0; i < 4; i++)
        r._u32[i] = spu_xf_encode(spu_xf_decode(a._u32[i]) * spu_xf_decode(b._u32[i]) - spu_xf_decode(c._u32[i]));
    return r;
}
static inline u128 spu_fms(u128 a, u128 b, u128 c)
{
    u128 r;
#if YZ_SPU_SIMD_XFLOAT
    if (g_spu_xfloat_simd_enabled && spu_xfloat_simd_fms(&r, &a, &b, &c)) return r;
#endif
    return spu_xf_scalar_fms(a, b, c);
}
static inline u128 spu_xf_scalar_fnms(u128 a, u128 b, u128 c)
{
    u128 r;
    for (int i = 0; i < 4; i++)
        r._u32[i] = spu_xf_encode(spu_xf_decode(c._u32[i]) - spu_xf_decode(a._u32[i]) * spu_xf_decode(b._u32[i]));
    return r;
}
static inline u128 spu_fnms(u128 a, u128 b, u128 c)
{
    u128 r;
#if YZ_SPU_SIMD_XFLOAT
    if (g_spu_xfloat_simd_enabled && spu_xfloat_simd_fnms(&r, &a, &b, &c)) return r;
#endif
    return spu_xf_scalar_fnms(a, b, c);
}
/* Float compare family (see the xfloat block comment above spu_fa). Any
 * operand with |bits| < Smin (0x00800000: zero and every denormal-shaped
 * pattern) compares as zero, and extended-range magnitudes (exponent field
 * 0xFF, an IEEE Inf/NaN bit shape here) compare by their true SPU magnitude,
 * not through the host float type (where 0x7FFFFFFF is a QNaN pattern and a
 * host `>`/`==` against it is false unconditionally). spu_xf_decode covers
 * both requirements at once: it flushes an exponent-field-0 operand to
 * unsigned +0.0 and decodes exp==0xFF as a genuine ~Smax-magnitude double.
 * The default path decodes both operands and compares the doubles. Decode is
 * a lossless bit-pattern-to-double widening and there is no rounding step,
 * so the result is exact. */
static inline u128 spu_fceq(u128 a, u128 b) {
    u128 r;
    for (int i = 0; i < 4; i++)
        r._u32[i] = (spu_xf_decode(a._u32[i]) == spu_xf_decode(b._u32[i])) ? 0xFFFFFFFFu : 0;
    return r;
}
static inline u128 spu_fcgt(u128 a, u128 b) {
    u128 r;
    for (int i = 0; i < 4; i++)
        r._u32[i] = (spu_xf_decode(a._u32[i]) > spu_xf_decode(b._u32[i])) ? 0xFFFFFFFFu : 0;
    return r;
}

/* ---- SPU double-precision (2 doubles/reg; dword i = words 2i(high),2i+1(low)).
 * Reassemble via _u32 -- our u128 is host-LE, so a naive _f64[i] would SWAP the
 * two words. ---- */
static inline double spu__dget(u128 r, int i) {
    uint64_t u = ((uint64_t)r._u32[i*2] << 32) | r._u32[i*2+1];
    double d; memcpy(&d, &u, sizeof d); return d;
}
static inline void spu__dset(u128* r, int i, double d) {
    uint64_t u; memcpy(&u, &d, sizeof u);
    r->_u32[i*2] = (uint32_t)(u >> 32); r->_u32[i*2+1] = (uint32_t)u;
}
static inline u128 spu_dfa(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(a,i)+spu__dget(b,i)); return r; }
static inline u128 spu_dfs(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(a,i)-spu__dget(b,i)); return r; }
static inline u128 spu_dfm(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(a,i)*spu__dget(b,i)); return r; }
/* double FMA family: c = rt (accumulator, 3-register). */
static inline u128 spu_dfma(u128 a, u128 b, u128 t)  { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(a,i)*spu__dget(b,i)+spu__dget(t,i)); return r; }
static inline u128 spu_dfms(u128 a, u128 b, u128 t)  { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(a,i)*spu__dget(b,i)-spu__dget(t,i)); return r; }
static inline u128 spu_dfnms(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(t,i)-spu__dget(a,i)*spu__dget(b,i)); return r; }
static inline u128 spu_dfnma(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, -(spu__dget(a,i)*spu__dget(b,i)+spu__dget(t,i))); return r; }
/* double compares -> per-lane 64-bit mask. */
static inline u128 spu_dfceq(u128 a, u128 b)  { u128 r; for(int i=0;i<2;i++){ uint64_t m=(spu__dget(a,i)==spu__dget(b,i))?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
static inline u128 spu_dfcmeq(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++){ double x=spu__dget(a,i),y=spu__dget(b,i); if(x<0)x=-x; if(y<0)y=-y; uint64_t m=(x==y)?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
/* fesd: extend single from the LEFT/first word of each dword pair (slots 0,2)
 * -> double (dwords 0,1). frds: round double -> single into slots 0,2, zero
 * slots 1,3. The single lives in the left word slot of each pair (word 2i)
 * and the right word slot is ignored on input and zeroed on output. */
static inline u128 spu_fesd(u128 a) { u128 r; memset(&r,0,sizeof r); for(int i=0;i<2;i++) spu__dset(&r,i,(double)a._f32[i*2]); return r; }
static inline u128 spu_frds(u128 a) { u128 r; memset(&r,0,sizeof r); for(int i=0;i<2;i++) r._f32[i*2]=(float)spu__dget(a,i); return r; }
/* mpyhhu: high-16 x high-16 of each word, unsigned, full 32-bit product. */
static inline u128 spu_mpyhhu(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[2*i+1]*(uint32_t)b._u16[2*i+1]; return r; }
/* cgx: extended carry-generate, carry-in = low bit of old rt. */
static inline u128 spu_cgx(u128 a, u128 b, u128 t){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)(t._u32[i]&1u)+a._u32[i]+b._u32[i])>>32); return r; }

/* ---- remaining SPU ISA ops (eqv/absdb/avgb/mpyhha/mpyhhau/dfcgt/dfcmgt/
 * xorbi/dftsv) ---- */
static inline u128 spu_eqv(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=~(a._u32[i]^b._u32[i]); return r; }
static inline u128 spu_xorbi(u128 a, int32_t imm){ u128 r; for(int i=0;i<16;i++) r._u8[i]=(uint8_t)(a._u8[i]^(uint8_t)imm); return r; }
static inline u128 spu_absdb(u128 a, u128 b)  {
#if YZ_SPU_SIMD_ABSDB && (defined(_MSC_VER) || defined(__SSE2__))
    /* ABSDB is max(a-b, 0) | max(b-a, 0) independently in every byte. */
    const __m128i va = _mm_loadu_si128((const __m128i*)(const void*)&a);
    const __m128i vb = _mm_loadu_si128((const __m128i*)(const void*)&b);
    const __m128i value = _mm_or_si128(_mm_subs_epu8(va, vb),
                                       _mm_subs_epu8(vb, va));
    u128 r;
    _mm_storeu_si128((__m128i*)(void*)&r, value);
    return r;
#else
    u128 r;
    for(int i=0;i<16;i++){
        uint8_t x=a._u8[i],y=b._u8[i];
        r._u8[i]=(uint8_t)(x>y?x-y:y-x);
    }
    return r;
#endif
}
static inline u128 spu_avgb(u128 a, u128 b)   { u128 r; for(int i=0;i<16;i++) r._u8[i]=(uint8_t)(((uint32_t)a._u8[i]+(uint32_t)b._u8[i]+1u)>>1); return r; }
/* mpyhha/mpyhhau: high-16 x high-16, accumulate into rt (3-register). */
static inline u128 spu_mpyhha(u128 a, u128 b, u128 t)  { u128 r; for(int i=0;i<4;i++) r._s32[i]=t._s32[i]+(int32_t)a._s16[2*i+1]*(int32_t)b._s16[2*i+1]; return r; }
static inline u128 spu_mpyhhau(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=t._u32[i]+(uint32_t)a._u16[2*i+1]*(uint32_t)b._u16[2*i+1]; return r; }
/* double compare greater -> per-lane mask. */
static inline u128 spu_dfcgt(u128 a, u128 b)  { u128 r; for(int i=0;i<2;i++){ uint64_t m=(spu__dget(a,i)>spu__dget(b,i))?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
static inline u128 spu_dfcmgt(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++){ double x=spu__dget(a,i),y=spu__dget(b,i); if(x<0)x=-x; if(y<0)y=-y; uint64_t m=(x>y)?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
/* dftsv: double test special value; not modeled, 0 = no special. */
static inline u128 spu_dftsv(u128 a, int32_t imm){ (void)a; (void)imm; u128 r; memset(&r,0,sizeof r); return r; }

/* ---- register-variable shifts/rotates ---- */
static inline u128 spu_shl(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++){ uint32_t sh=b._u32[i]&0x3F; r._u32[i]=(sh>31)?0:(a._u32[i]<<sh); } return r; }
static inline u128 spu_shlh(u128 a, u128 b)  { u128 r; for(int i=0;i<8;i++){ uint32_t sh=b._u16[i]&0x1F; r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]<<sh); } return r; }
static inline u128 spu_rot(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++){ uint32_t sh=b._u32[i]&31; r._u32[i]= sh ? ((a._u32[i]<<sh)|(a._u32[i]>>(32-sh))) : a._u32[i]; } return r; }
static inline u128 spu_roth(u128 a, u128 b)  { u128 r; for(int i=0;i<8;i++){ uint32_t sh=b._u16[i]&15; r._u16[i]= sh ? (uint16_t)((a._u16[i]<<sh)|(a._u16[i]>>(16-sh))) : a._u16[i]; } return r; }
static inline u128 spu_shlqbi(u128 a, u128 b){ int sh=b._u32[0]&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqbi(u128 a, u128 b){ int sh=b._u32[0]&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh)|(hi>>(64-sh));
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_shlqby(u128 a, u128 b){ int sh=b._u32[0]&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_rotqby(u128 a, u128 b){ return spu_rotqby_bytes(a,b._u32[0]&0x0F); }
static inline u128 spu_shlqbybi(u128 a, u128 b){ int sh=(b._u32[0]>>3)&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_rotqbybi(u128 a, u128 b){ return spu_rotqby_bytes(a,(b._u32[0]>>3)&0x0F); }

/* ---- rotmahi ---- */
static inline u128 spu_rotmahi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._s16[i]=(sh>15)?(a._s16[i]>>15):(a._s16[i]>>sh); return r; }

/* ---- byte/half immediate compares ---- */
static inline u128 spu_ceqbi(u128 a, int32_t imm)  { u128 r; uint8_t v=(uint8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]==v)?0xFFu:0; return r; }
static inline u128 spu_ceqhi(u128 a, int32_t imm)  { u128 r; int16_t v=(int16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]==v)?0xFFFFu:0; return r; }
static inline u128 spu_clgtbi(u128 a, int32_t imm) { u128 r; uint8_t v=(uint8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]>v)?0xFFu:0; return r; }
static inline u128 spu_clgthi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]>v)?0xFFFFu:0; return r; }
static inline u128 spu_cgthi(u128 a, int32_t imm)  { u128 r; int16_t v=(int16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]>v)?0xFFFFu:0; return r; }
static inline u128 spu_cgtbi(u128 a, int32_t imm)  { u128 r; int8_t v=(int8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._s8[i]>v)?0xFFu:0; return r; }

/* ---- misc one-offs ---- */
static inline u128 spu_fscrrd(const spu_context* ctx) { return ctx->fpscr; }
static inline void spu_fscrwr(spu_context* ctx, u128 value) { ctx->fpscr = value; }
static inline u128 spu_gb(u128 a) {
    uint32_t v = ((a._u32[0]&1)<<3)|((a._u32[1]&1)<<2)|((a._u32[2]&1)<<1)|(a._u32[3]&1);
    u128 r = spu_zero(); r._u32[0]=v; return r;
}
static inline u128 spu_gbh(u128 a) {
    /* gather LSB of each SPU halfword H into bit (7-H). SPU halfword H maps to
     * our _u16[H^1] (the within-word halfword swap of the value layout). */
    uint32_t v=0; for(int i=0;i<8;i++) v |= ((uint32_t)(a._u16[i^1]&1) << (7-i));
    u128 r = spu_zero(); r._u32[0]=v; return r;
}
/* gather LSB of each SPU byte i into bit (15-i); exact inverse of spu_fsmb.
 * SPU byte i lives at our _u8[SPU_W(i)] (byte-reversed within each word). */
static inline u128 spu_gbb(u128 a) {
    uint32_t v=0; for(int i=0;i<16;i++) v |= ((uint32_t)(a._u8[SPU_W(i)]&1) << (15-i));
    u128 r = spu_zero(); r._u32[0]=v; return r;
}
static inline u128 spu_cg(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)a._u32[i]+(uint64_t)b._u32[i])>>32); return r; }
/* sumb: sum the 4 bytes of each word. Per CBEA, RT halfword 2i (the HIGH half of
 * word i) = sum of RB's 4 bytes of word i; halfword 2i+1 (LOW half) = sum of RA's
 * 4 bytes of word i. Computed on word values so byte order is irrelevant. */
static inline u128 spu_sumb(u128 a, u128 b) {
    u128 r;
    for(int i=0;i<4;i++) {
        uint32_t wa=a._u32[i], wb=b._u32[i];
        uint32_t sa=((wa>>24)&0xFF)+((wa>>16)&0xFF)+((wa>>8)&0xFF)+(wa&0xFF);
        uint32_t sb=((wb>>24)&0xFF)+((wb>>16)&0xFF)+((wb>>8)&0xFF)+(wb&0xFF);
        r._u32[i]=(sb<<16)|sa;
    }
    return r;
}
/* Borrow generate: carry-out of (b + ~a + 1) == (b >= a unsigned ? 1 : 0).
 * The subtract-side sibling of cg; pairs with sf/sfx for extended subtraction. */
static inline u128 spu_bg(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)b._u32[i]+(uint64_t)(~a._u32[i])+1u)>>32); return r; }
static inline u128 spu_addx(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+b._u32[i]+(t._u32[i]&1); return r; }
/* LE host: high half of word i = _s16[2i+1], low half = _s16[2i]. */
static inline u128 spu_mpyh(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._s32[i]=((int32_t)a._s16[2*i+1] * (int32_t)b._s16[2*i]) << 16; return r; }
static inline u128 spu_mpyhh(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[2*i+1] * (int32_t)b._s16[2*i+1]; return r; }
static inline u128 spu_mpys(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++){ int32_t p=(int32_t)a._s16[2*i]*(int32_t)b._s16[2*i]; r._s32[i]=(int16_t)(p>>16); } return r; }
static inline u128 spu_mpyui(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[2*i]*(uint32_t)(uint16_t)imm; return r; }
/* Compare family: same spu_xf_decode
 * reuse as fceq/fcgt above (see that comment) -- the magnitude compare here is
 * `fabs` of the decoded double rather than of the raw bit pattern, which
 * is equivalent (decode's sign bit only flips the final sign, so
 * fabs(decode(bits)) == decode(bits & 0x7FFFFFFF)) and avoids a second
 * decode helper. */
static inline u128 spu_fcmeq(u128 a, u128 b){
    u128 r;
    for(int i=0;i<4;i++){ double fa=fabs(spu_xf_decode(a._u32[i])), fb=fabs(spu_xf_decode(b._u32[i])); r._u32[i]=(fa==fb)?0xFFFFFFFFu:0; }
    return r;
}
static inline u128 spu_fcmgt(u128 a, u128 b){
    u128 r;
    for(int i=0;i<4;i++){ double fa=fabs(spu_xf_decode(a._u32[i])), fb=fabs(spu_xf_decode(b._u32[i])); r._u32[i]=(fa>fb)?0xFFFFFFFFu:0; }
    return r;
}
/* spu_frest / spu_frsqest: floating reciprocal estimate / floating reciprocal
 * absolute square root estimate. Both emit the packed base+step encoding that
 * spu_fi (above) decodes, not a plain full-precision 1/x or 1/sqrt(x) float;
 * the estimate is only meaningful as input to fi.
 *
 * Result format (both instructions, same layout fi assumes):
 *   S(1) | BiasedExponent(8) | BaseFraction(13) | StepFraction(10)
 *   Base = S 1.BaseFraction * 2^(BiasedExponent-127)
 *   Step = 0.000 StepFraction * 2^(BiasedExponent-127)   (frest: sign of Base;
 *          frsqest: sign always 0)
 *
 * The exponent fields are closed-form and computed directly:
 *   frest:   rexp = (exp==0) ? 0xFF : ( (253-exp)>0 ? (253-exp) : 0 )
 *   frsqest: rexp = (exp==0) ? 0xFF : (190 - (exp+1)/2)          [int div]
 * For frest, an input exponent of 254 or 255 would make the reciprocal
 * exponent negative; the result underflows to zero (|x| >= 2^126, 1/x
 * underflows). The two fraction tables (32 and 64 entries) have no closed
 * form; they are hardware lookup tables reproduced verbatim below. Repeated
 * adjacent pairs in them (frest indices 20/21 ... 30/31, and many pairs in
 * frsqest) are part of the hardware tables, not transcription errors.
 *
 * Layout consistency with spu_fi: spu_fi decodes RB as
 *   exp        = (rb>>23) & 0xFF
 *   base_bits  = rb & 0xFFFFFC00        (sign(1)+exp(8)+basefrac(13), verbatim IEEE bits)
 *   step_frac  = rb & 0x3FF             (10 bits)
 *   step       = ldexp((float)step_frac, exp-127-13), sign-matched to base
 * spu_frest/spu_frsqest produce exactly that layout: sign in bit 31 (frest
 * only; frsqest forces 0), exponent in bits 23-30, a 13-bit base fraction in
 * bits 10-22, and the low 10 bits as StepFraction. Each fraction table
 * constant is already pre-shifted and below 0x800000 (bits 0-22 only), so
 * `exponent_field | fraction_field` cannot collide.
 *
 * The estimates feed the usual Newton-Raphson refinements:
 *   reciprocal: frest y0,x / fi y1,x,y0 / fnms t1,x,y1,one / fma y2,t1,y1,y1
 *   rsqrt: frsqest y0,x / and ax,x,mask / fi y1,ax,y0 / fm t1,ax,y1 /
 *          fm t2,y1,half / fnms t1,t1,y1,one / fma y2,t1,t2,y1
 * which land within about 1e-8 relative error of the true result.
 */

/* ---- hardware lookup tables ----
 * Pre-shifted 23-bit BaseFraction|StepFraction fields, indexed by the top 5
 * (frest) or 6 (frsqest) bits of RA's IEEE fraction. */
static const uint32_t spu_frest_fraction_lut[32] = {
    0x7FFBE0, 0x7F87A6, 0x70EF72, 0x708B40, 0x638B12, 0x633AEA, 0x5792C4, 0x574AA0,
    0x4CCA7E, 0x4C9262, 0x430A44, 0x42D62A, 0x3A2E12, 0x39FDFA, 0x3215E4, 0x31F1D2,
    0x2AA9BE, 0x2A85AC, 0x23D59A, 0x23BD8E, 0x1D8576, 0x1D8576, 0x17AD5A, 0x17AD5A,
    0x124543, 0x124543, 0x0D392D, 0x0D392D, 0x08851A, 0x08851A, 0x041D07, 0x041D07
};

static const uint32_t spu_frsqest_fraction_lut[64] = {
    0x350160, 0x34E954, 0x2F993D, 0x2F993D, 0x2AA523, 0x2AA523, 0x26190D, 0x26190D,
    0x21E4F9, 0x21E4F9, 0x1E00E9, 0x1E00E9, 0x1A5CD9, 0x1A5CD9, 0x16F8CB, 0x16F8CB,
    0x13CCC0, 0x13CCC0, 0x10CCB3, 0x10CCB3, 0x0E00AA, 0x0E00AA, 0x0B58A1, 0x0B58A1,
    0x08D498, 0x08D498, 0x067491, 0x067491, 0x043089, 0x043089, 0x020C83, 0x020C83,
    0x7FFDF4, 0x7FD1DE, 0x7859C8, 0x783DBA, 0x71559C, 0x71559C, 0x6AE57C, 0x6AE57C,
    0x64F561, 0x64F561, 0x5F7149, 0x5F7149, 0x5A4D33, 0x5A4D33, 0x55811F, 0x55811F,
    0x51050F, 0x51050F, 0x4CC8FE, 0x4CC8FE, 0x48D0F0, 0x48D0F0, 0x4510E4, 0x4510E4,
    0x4180D7, 0x4180D7, 0x3E24CC, 0x3E24CC, 0x3AF4C3, 0x3AF4C3, 0x37E8BA, 0x37E8BA
};

/* frest: floating reciprocal estimate. Produces the packed base+step encoding
 * consumed by spu_fi (above), not a plain 1/x float. */
static inline u128 spu_frest(u128 a) {
    u128 r;
    for (int i = 0; i < 4; i++) {
        uint32_t bits = a._u32[i];
        uint32_t sign = bits & 0x80000000u;
        uint32_t exp  = (bits >> 23) & 0xFFu;
        uint32_t frac_idx = (bits >> 18) & 0x1Fu;
        uint32_t out;
        if (exp == 0u) {
            /* zero or denormal operand: 1/0 is defined to give the maximum
             * SPU single-precision extended-range fp number (0x7FFFFFFF), sign
             * of the (zero) input preserved. Same bucket for true denormals
             * (their biased exponent field is also 0). */
            out = sign | 0x7FFFFFFFu;
        } else {
            /* result biased exponent = 253 - input biased exponent (253 =
             * 2*127-1, i.e. reciprocal negates the unbiased exponent),
             * clamped to 0 on underflow (|x| >= 2^126, 1/x underflows to
             * zero). */
            uint32_t rexp = (exp >= 253u) ? 0u : (253u - exp);
            uint32_t fraction = spu_frest_fraction_lut[frac_idx];
            out = sign | (rexp << 23) | (fraction & 0x7FFFFFu);
        }
        memcpy(&r._f32[i], &out, sizeof out);
    }
    return r;
}

/* frsqest: floating reciprocal absolute square-root estimate. Produces the
 * packed base+step encoding consumed by spu_fi; the sign bit is always 0
 * because frsqest(x) estimates 1/sqrt(|x|), always positive. The exponent is
 * computed inline as (exponent==0) ? 0xFF : 190-(exponent+1)/2 (integer
 * division). */
static inline u128 spu_frsqest(u128 a) {
    u128 r;
    for (int i = 0; i < 4; i++) {
        uint32_t bits = a._u32[i];
        uint32_t exp  = (bits >> 23) & 0xFFu;
        uint32_t frac_idx = (bits >> 18) & 0x3Fu;
        uint32_t rexp = (exp == 0u) ? 0xFFu : (190u - (exp + 1u) / 2u);
        uint32_t fraction = spu_frsqest_fraction_lut[frac_idx];
        uint32_t out = (rexp << 23) | (fraction & 0x7FFFFFu);
        memcpy(&r._f32[i], &out, sizeof out);
    }
    return r;
}

/* ---- sign extension ----
 * LE host: low sub-lane = _u8[2i] / _s16[2i] / _s32[2i] (the byte/half/word
 * at the *lower* storage address). Same caveat as the mpy family. */
static inline u128 spu_xsbh(u128 a) { u128 r; for(int i=0;i<8;i++) r._s16[i] = (int8_t)a._u8[2*i]; return r; }
static inline u128 spu_xshw(u128 a) { u128 r; for(int i=0;i<4;i++) r._s32[i] = (int16_t)a._s16[2*i]; return r; }
/* xswd: per doubleword, sign-extend the word in the right slot; the value
 * stays in the right word, the sign fill goes in the left.
 * Lane caveat: the sub-lane rule above applies to sub-lanes within one
 * host-endian value (bytes in a halfword, halfwords in a word). Words are not
 * sub-lanes: u128 stores _u32[i] = SPU word i in SPU order, so the right word
 * of doubleword i is _u32[2*i+1] (the higher index), and the left/sign-fill
 * word is _u32[2*i]. Test vectors built with host-endian u64 packing scramble
 * word lanes for asymmetric values and would suggest the swapped form. */
static inline u128 spu_xswd(u128 a) {
    u128 r;
    for (int i = 0; i < 2; i++) {
        r._u32[2*i]   = (a._s32[2*i+1] < 0) ? 0xFFFFFFFFu : 0u;
        r._u32[2*i+1] = a._u32[2*i+1];
    }
    return r;
}

/* ---- OR across ---- */
static inline u128 spu_orx(u128 a) {
    u128 r = spu_zero();
    r._u32[0] = a._u32[0] | a._u32[1] | a._u32[2] | a._u32[3];
    return r;
}

/* ---- form-select mask from bits ---- */
static inline u128 spu_fsm(u128 a) {
    u128 r; uint32_t v = a._u32[0] & 0xF;
    for(int i=0;i<4;i++) r._u32[i] = ((v>>(3-i))&1) ? 0xFFFFFFFFu : 0;
    return r;
}
/* fsmh: per-halfword mask from 8 bits. SPU halfword H <- bit (7-H); store at
 * our _u16[H^1] (within-word halfword swap of the value layout). Per-halfword
 * both bytes are identical so byte order within a halfword is irrelevant, but
 * the halfword ORDER within each word must be swapped. */
static inline u128 spu_fsmh(u128 a) {
    u128 r; uint32_t v = a._u32[0] & 0xFF;
    for(int i=0;i<8;i++) r._u16[i^1] = ((v>>(7-i))&1) ? 0xFFFFu : 0;
    return r;
}
/* fsmb/fsmbi: per-byte mask from 16 bits. SPU byte position P <- bit (15-P);
 * store at our _u8[SPU_W(P)] (byte-reversed within each word). A bit set at SPU
 * byte P must land at our _u8[SPU_W(P)] so that downstream word arithmetic (e.g.
 * andbi(fsmbi(0x101),-128) building a +0x80 EA offset in the low byte of words
 * 1,3) places the value in the correct byte lane. Raw _u8[P] would put 0x80 in
 * the high byte (bit 31) instead -> wrong DMA EA. */
static inline u128 spu_fsmb(u128 a) {
    u128 r; uint32_t v = a._u32[0] & 0xFFFF;
    for(int i=0;i<16;i++) r._u8[SPU_W(i)] = ((v>>(15-i))&1) ? 0xFFu : 0;
    return r;
}
static inline u128 spu_fsmbi(int32_t imm) {
    u128 r; uint32_t v = imm & 0xFFFF;
    for(int i=0;i<16;i++) r._u8[SPU_W(i)] = ((v>>(15-i))&1) ? 0xFFu : 0;
    return r;
}

/* ---- constant generators (insertion shuffle patterns) ---- */
/* Generate-controls (cbd/chd/cwd/cdd): insertion selectors for shufb. Each
 * builds a control in true SPU byte order: the
 * base selects b's SPU byte t at result SPU byte t (selector 0x10+t), and the
 * insert positions select a's preferred scalar bytes -- SPU byte 3 for a byte
 * (selector 0x03), SPU bytes 2,3 for a halfword (0x02,0x03), SPU bytes 0..3 for
 * a word, 0..7 for a doubleword. Every store is mapped through SPU_W to land at
 * the right host index. They compose correctly with shufb for both
 * cbd-generated and immediate/LS-constant controls. */
static inline u128 spu_cbd_pos(int pos) {
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    r._u8[SPU_W(pos & 0xF)] = 0x03;                 /* a's preferred byte = SPU byte 3 */
    return r;
}
static inline u128 spu_chd_pos(int pos) {
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~1;                       /* SPU halfword byte positions p, p+1 */
    r._u8[SPU_W(p)]   = 0x02;                        /* a SPU byte 2 (hi byte of preferred hw) */
    r._u8[SPU_W(p+1)] = 0x03;                        /* a SPU byte 3 (lo byte) */
    return r;
}
static inline u128 spu_cwd_pos(int pos) {
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~3;
    for(int k=0;k<4;k++) r._u8[SPU_W(p+k)] = (uint8_t)k;   /* a SPU bytes 0..3 (preferred word) */
    return r;
}
static inline u128 spu_cdd_pos(int pos) {
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~7;
    for(int k=0;k<8;k++) r._u8[SPU_W(p+k)] = (uint8_t)k;   /* a SPU bytes 0..7 (preferred dword) */
    return r;
}
static inline u128 spu_cbd(u128 a, int i7){ return spu_cbd_pos((int)a._u32[0]+i7); }
static inline u128 spu_chd(u128 a, int i7){ return spu_chd_pos((int)a._u32[0]+i7); }
static inline u128 spu_cwd(u128 a, int i7){ return spu_cwd_pos((int)a._u32[0]+i7); }
static inline u128 spu_cdd(u128 a, int i7){ return spu_cdd_pos((int)a._u32[0]+i7); }
static inline u128 spu_cbx(u128 a, u128 b){ return spu_cbd_pos((int)(a._u32[0]+b._u32[0])); }
static inline u128 spu_chx(u128 a, u128 b){ return spu_chd_pos((int)(a._u32[0]+b._u32[0])); }
static inline u128 spu_cwx(u128 a, u128 b){ return spu_cwd_pos((int)(a._u32[0]+b._u32[0])); }
static inline u128 spu_cdx(u128 a, u128 b){ return spu_cdd_pos((int)(a._u32[0]+b._u32[0])); }

/* ---- rotate-and-mask family ---- */
static inline u128 spu_rotm(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++){ uint32_t sh=(0-b._u32[i])&0x3F; r._u32[i]=(sh>31)?0:(a._u32[i]>>sh); } return r; }
static inline u128 spu_rotma(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++){ uint32_t sh=(0-b._u32[i])&0x3F; r._s32[i]=(sh>31)?(a._s32[i]>>31):(a._s32[i]>>sh); } return r; }
static inline u128 spu_rothm(u128 a, u128 b)  { u128 r; for(int i=0;i<8;i++){ uint32_t sh=(0-b._u16[i])&0x1F; r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); } return r; }
static inline u128 spu_rothma(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++){ uint32_t sh=(0-b._u16[i])&0x1F; r._s16[i]=(sh>15)?(a._s16[i]>>15):(a._s16[i]>>sh); } return r; }
static inline u128 spu_rothmi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); return r; }
static inline u128 spu_rotqmbi(u128 a, u128 b)   { int sh=(0-(int)b._u32[0])&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nlo=(lo>>sh)|(hi<<(64-sh)), nhi=(hi>>sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqmby(u128 a, u128 b)   { int sh=(0-(int)b._u32[0])&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_rotqmbybi(u128 a, u128 b) { int sh=(0-((int)b._u32[0]>>3))&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_rotqmbii(u128 a, int i7)  { int sh=(0-i7)&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nlo=(lo>>sh)|(hi<<(64-sh)), nhi=(hi>>sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqmbyi(u128 a, int i7)  { int sh=(0-i7)&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }

/* ---- halfword/byte immediate logic ---- */
static inline u128 spu_andhi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]&v; return r; }
static inline u128 spu_andbi(u128 a, int32_t imm) { u128 r; uint8_t  v=(uint8_t)imm;  for(int i=0;i<16;i++) r._u8[i]=a._u8[i]&v;   return r; }
static inline u128 spu_orhi(u128 a, int32_t imm)  { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]|v; return r; }
static inline u128 spu_orbi(u128 a, int32_t imm)  { u128 r; uint8_t  v=(uint8_t)imm;  for(int i=0;i<16;i++) r._u8[i]=a._u8[i]|v;   return r; }
static inline u128 spu_xorhi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]^v; return r; }

/* ---- borrow-generate extended ---- */
static inline u128 spu_bgx(u128 a, u128 b, u128 t) {
    u128 r;
    for(int i=0;i<4;i++) {
        uint64_t s = (uint64_t)b._u32[i] + (uint64_t)(~a._u32[i]) + (uint64_t)(t._u32[i]&1);
        r._u32[i] = (uint32_t)(s >> 32);
    }
    return r;
}

/* ---- mfspr stub ---- */
static inline u128 spu_mfspr(u128 a) { (void)a; return spu_zero(); }

/* ---- immediate loaders ---- */
static inline u128 spu_il(int32_t imm)   { return spu_splat_u32((uint32_t)imm); }
static inline u128 spu_ila(uint32_t i18) { return spu_splat_u32(i18 & 0x3FFFF); }
static inline u128 spu_ilh(uint16_t imm) { return spu_splat_u16(imm); }
static inline u128 spu_ilhu(uint16_t imm){ return spu_splat_u32((uint32_t)imm << 16); }
static inline u128 spu_iohl(u128 a, uint16_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]|(uint32_t)imm; return r; }

#ifdef __cplusplus
}
#endif

#endif /* SPU_HELPERS_H */
