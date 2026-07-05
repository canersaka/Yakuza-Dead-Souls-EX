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

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _MSC_VER
#include <intrin.h>
static inline int spu_clz32(uint32_t x) {
    unsigned long idx;
    if (!x) return 32;
    _BitScanReverse(&idx, x);
    return 31 - (int)idx;
}
#else
static inline int spu_clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }
#endif

/* SPU byte position -> our _u8 index.
 * Our u128 is HOST-NATIVE little-endian (_u32[i]=SPU word i as a value via
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
 * into the PREFERRED word (word 0) and ZEROS the other three slots (matches
 * RPCS3 v128::from32r, SPUInterpreter.cpp BRSL). Splatting it (the old bug)
 * corrupts any code that saves/reloads/operates on the full 128-bit link --
 * caught by the SPU trace-diff vs RPCS3 (gs_task lqd r0 @LS 0x3028). */
static inline u128 spu_link(uint32_t addr) {
    u128 r; r._u32[0]=addr; r._u32[1]=0; r._u32[2]=0; r._u32[3]=0; return r;
}
/* Preferred-slot scalar: value in word 0, remaining slots ZERO. CBEA's scalar
 * channel convention -- rchcnt returns the count this way (RPCS3 measured
 * r4={1,0,0,0} at gs_task LS 0x655C where the old splat gave {1,1,1,1}).
 * Same bug class as spu_link. */
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

/* ---- multiply (low halfword of each word × ... -> 32-bit, per SPU mpy) ----
 * Sub-lane indexing assumes a little-endian host (matches the recompiler's
 * target). The "low halfword of word i" in SPU BE semantics is _s16[2i] on
 * an LE host (NOT _s16[2i+1], which would be correct on a BE host). */
static inline u128 spu_mpy(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int32_t)b._s16[i*2]; return r; }
/* mpya: 16x16 signed multiply of low halves + add rc (per word, RRR form). */
static inline u128 spu_mpya(u128 a, u128 b, u128 c) { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int32_t)b._s16[i*2]+c._s32[i]; return r; }
/* sfx: extended subtract rb-ra-1+carry; carry-in = low bit of old rt (RT is 3rd src). */
static inline u128 spu_sfx(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=b._u32[i]+~a._u32[i]+(t._u32[i]&1u); return r; }
/* fi: floating interpolate. RB is disassembled per the Floating Reciprocal
 * Estimate encoding (SPU_ISA_v1.2_27Jan2007_pub.pdf, "Floating Reciprocal
 * Estimate" p.214, "Floating Reciprocal Absolute Square Root Estimate"
 * p.216 -- same base/step field layout in both): sign(1) | biased
 * exponent(8) | base fraction(13) | step fraction(10). RA bits 13:31 (its
 * low 19 bits) supply the interpolation fraction Y = (RA & 0x7FFFF) / 2^19.
 * Per ISA "Floating Interpolate" p.219:
 *   RT = (-1)^S * (1.BaseFraction - 0.000_StepFraction * Y) * 2^(exp-127)
 * sign|exp|BaseFraction (RB's top 22 bits) is already a valid IEEE-754 bit
 * pattern for the Base term, so `RB & 0xFFFFFC00` reinterpreted as float
 * gives Base directly; StepFraction has 3 implicit leading zero bits ahead
 * of its own 10 bits (the "0.000" prefix), i.e. Step = StepFrac*2^(exp-127-13),
 * same sign as Base.
 *
 * Verified bit-exact against RPCS3's ASMJIT recompiler
 * (rpcs3/rpcs3/Emu/Cell/SPUASMJITRecompiler.cpp, spu_recompiler::FI,
 * lines 3997-4044) by transcribing its integer bit-manipulation sequence
 * and comparing outputs across 500,000 random 32-bit RA/RB patterns (zero
 * mismatches > 1e-6 relative) plus 300,000 realistic reciprocal-shaped
 * patterns (zero mismatches).
 *
 * ORACLE TRAP: RPCS3's *interpreter* FI (SPUInterpreter.cpp:2679-2684) is
 * an unimplemented stub -- `spu.gpr[op.rt] = spu.gpr[op.rb]; return true;`
 * with a `// TODO` comment. Do NOT use it as an oracle; only the ASMJIT
 * recompiler (or LLVM recompiler, if checked) has real fi semantics.
 *
 * Old code (`y*(2.0f-a*b*y)`, a generic self-contained Newton-Raphson step)
 * never decoded RB's base/step bit-fields at all -- confirmed wrong, not
 * merely imprecise:
 *
 *   Example 1: x=5.331123525209338
 *     RA(bits)=0x40aa9890  RB(bits)=0x3e401460
 *     true 1/x            = 0.18757772076960696
 *     correct fi (ISA)    = 0.18710096180438995   (RPCS3 ASMJIT: same)
 *     old spu_fi output   = 0.3399700402100933    (~2x off)
 *
 *   Example 2: x=-76.91874321984837
 *     RA(bits)=0xc299d666  RB(bits)=0xbc550106
 *     true 1/x            = -0.013000732437109771
 *     correct fi (ISA)    = -0.012943098139658105 (RPCS3 ASMJIT: -0.01294309739023447)
 *     old spu_fi output   = -0.026170483918120862  (~2x off)
 *
 *   Example 3: x=10.859253470350062
 *     RA(bits)=0x412dbf81  RB(bits)=0x3dbc984c
 *     true 1/x            = 0.09208736150513334
 *     correct fi (ISA)    = 0.09167017677100375   (RPCS3 ASMJIT: 0.09167017042636871)
 *     old spu_fi output   = 0.17569464086128958    (~2x off)
 */
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
static inline u128 spu_rotqbyi(u128 a, int sh) { u128 r; sh&=0x0F; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }
/* Bit-level quadword shifts/rotates operate on the WHOLE 128-bit big-endian
 * value. Our _u64[0]/_u64[1] are word-SCRAMBLED (host-LE: _u64[0] = word0 |
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

/* ---- single-precision float (4 lanes) ---- */
static inline u128 spu_fa(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._f32[i]=a._f32[i]+b._f32[i]; return r; }
static inline u128 spu_fs(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._f32[i]=a._f32[i]-b._f32[i]; return r; }

/* Float<->int conversions with scale (RI8). HW/RPCS3 semantics (verified vs
 * SPUInterpreter.cpp precise CFLTS=ldexp(a,173-i8) + SPUASMJITRecompiler.cpp):
 *   cflts/cfltu: result = (int)(a * 2^(173-i8))   -- i8=173 is the identity
 *   csflt/cuflt: result = (float)int * 2^(i8-155) -- i8=155 is the identity
 * (Prior code applied a spurious -155 table offset -> off by 2^155; canonical
 * conversions collapsed to ~0. f computed in double so 2^173 doesn't overflow.) */
static inline u128 spu_cflts(u128 a, int i8){ u128 r; double f=exp2((double)(173-i8));
    for(int i=0;i<4;i++){ double v=(double)a._f32[i]*f; if(v>2147483647.0)v=2147483647.0; if(v<-2147483648.0)v=-2147483648.0; r._s32[i]=(int32_t)v; } return r; }
static inline u128 spu_cfltu(u128 a, int i8){ u128 r; double f=exp2((double)(173-i8));
    for(int i=0;i<4;i++){ double v=(double)a._f32[i]*f; if(v<0)v=0; if(v>4294967295.0)v=4294967295.0; r._u32[i]=(uint32_t)v; } return r; }
static inline u128 spu_csflt(u128 a, int i8){ u128 r; double f=exp2((double)(i8-155));
    for(int i=0;i<4;i++) r._f32[i]=(float)((double)a._s32[i]*f); return r; }
static inline u128 spu_cuflt(u128 a, int i8){ u128 r; double f=exp2((double)(i8-155));
    for(int i=0;i<4;i++) r._f32[i]=(float)((double)a._u32[i]*f); return r; }
static inline u128 spu_fm(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._f32[i]=a._f32[i]*b._f32[i]; return r; }
/* fma/fms/fnms: the SPU multiply is EXACT (single final rounding = true fused
 * multiply-add), ISA v1.2 p208/210/212 "the multiplication is exact and not
 * subject to limits on its range". Plain a*b+c under /fp:precise double-rounds
 * (rounds the product to f32 first) -> sparse 1-ULP errors; fmaf() gives the
 * single-rounded result RPCS3's precise interpreter (std::fma) also uses.
 * fms = a*b - c => fmaf(a,b,-c); fnms = c - a*b => fmaf(a,-b,c) (negate BEFORE
 * the FMA so the one rounding lands on the full expression). Note: on a build
 * without /arch:AVX2, MSVC lowers fmaf to a CRT software-FMA call (correctness
 * over the old 2-instruction product; RPCS3 pays the same cost). */
static inline u128 spu_fma(u128 a, u128 b, u128 c)  { u128 r; for(int i=0;i<4;i++) r._f32[i]=fmaf(a._f32[i], b._f32[i], c._f32[i]); return r; }
static inline u128 spu_fms(u128 a, u128 b, u128 c)  { u128 r; for(int i=0;i<4;i++) r._f32[i]=fmaf(a._f32[i], b._f32[i], -c._f32[i]); return r; }
static inline u128 spu_fnms(u128 a, u128 b, u128 c) { u128 r; for(int i=0;i<4;i++) r._f32[i]=fmaf(a._f32[i], -b._f32[i], c._f32[i]); return r; }
static inline u128 spu_fceq(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._f32[i]==b._f32[i])?0xFFFFFFFFu:0; return r; }
static inline u128 spu_fcgt(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._f32[i]>b._f32[i])?0xFFFFFFFFu:0; return r; }

/* ---- SPU double-precision (2 doubles/reg; dword i = words 2i(high),2i+1(low)).
 * Reassemble via _u32 -- our u128 is host-LE, so a naive _f64[i] would SWAP the
 * two words. Semantics from RPCS3 SPUInterpreter (DFASM / DFMA / FESD / FRDS). ---- */
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
/* double compares -> per-lane 64-bit mask (RPCS3 stubs these; sane impl here). */
static inline u128 spu_dfceq(u128 a, u128 b)  { u128 r; for(int i=0;i<2;i++){ uint64_t m=(spu__dget(a,i)==spu__dget(b,i))?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
static inline u128 spu_dfcmeq(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++){ double x=spu__dget(a,i),y=spu__dget(b,i); if(x<0)x=-x; if(y<0)y=-y; uint64_t m=(x==y)?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
/* fesd: extend single from the LEFT/first word of each dword pair (slots 0,2)
 * -> double (dwords 0,1). frds: round double -> single into slots 0,2, zero
 * slots 1,3. ISA v1.2 p224/225 "single-precision value in the left slot...
 * right word slot ignored" / "placed in the left word slot... zeros in the
 * right". (Was reading slots 1,3 -- the wrong lane; RPCS3 precise FESD/FRDS
 * confirm the left word = 2i.) */
static inline u128 spu_fesd(u128 a) { u128 r; memset(&r,0,sizeof r); for(int i=0;i<2;i++) spu__dset(&r,i,(double)a._f32[i*2]); return r; }
static inline u128 spu_frds(u128 a) { u128 r; memset(&r,0,sizeof r); for(int i=0;i<2;i++) r._f32[i*2]=(float)spu__dget(a,i); return r; }
/* mpyhhu: high-16 x high-16 of each word, unsigned, full 32-bit product. */
static inline u128 spu_mpyhhu(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[2*i+1]*(uint32_t)b._u16[2*i+1]; return r; }
/* cgx: extended carry-generate, carry-in = low bit of old rt (RPCS3 CGX). */
static inline u128 spu_cgx(u128 a, u128 b, u128 t){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)(t._u32[i]&1u)+a._u32[i]+b._u32[i])>>32); return r; }

/* ---- remaining SPU ISA ops (RPCS3 SPUInterpreter: EQV/ABSDB/AVGB/MPYHHA/
 * MPYHHAU/DFCGT/DFCMGT/XORBI/DFTSV). Completes the lifter's opcode coverage. ---- */
static inline u128 spu_eqv(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=~(a._u32[i]^b._u32[i]); return r; }
static inline u128 spu_xorbi(u128 a, int32_t imm){ u128 r; for(int i=0;i<16;i++) r._u8[i]=(uint8_t)(a._u8[i]^(uint8_t)imm); return r; }
static inline u128 spu_absdb(u128 a, u128 b)  { u128 r; for(int i=0;i<16;i++){ uint8_t x=a._u8[i],y=b._u8[i]; r._u8[i]=(uint8_t)(x>y?x-y:y-x); } return r; }
static inline u128 spu_avgb(u128 a, u128 b)   { u128 r; for(int i=0;i<16;i++) r._u8[i]=(uint8_t)(((uint32_t)a._u8[i]+(uint32_t)b._u8[i]+1u)>>1); return r; }
/* mpyhha/mpyhhau: high-16 x high-16, ACCUMULATE into rt (3-register). */
static inline u128 spu_mpyhha(u128 a, u128 b, u128 t)  { u128 r; for(int i=0;i<4;i++) r._s32[i]=t._s32[i]+(int32_t)a._s16[2*i+1]*(int32_t)b._s16[2*i+1]; return r; }
static inline u128 spu_mpyhhau(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=t._u32[i]+(uint32_t)a._u16[2*i+1]*(uint32_t)b._u16[2*i+1]; return r; }
/* double compare greater (RPCS3 stubs these; sane impl) -> per-lane mask. */
static inline u128 spu_dfcgt(u128 a, u128 b)  { u128 r; for(int i=0;i<2;i++){ uint64_t m=(spu__dget(a,i)>spu__dget(b,i))?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
static inline u128 spu_dfcmgt(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++){ double x=spu__dget(a,i),y=spu__dget(b,i); if(x<0)x=-x; if(y<0)y=-y; uint64_t m=(x>y)?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
/* dftsv: double test special value -- RPCS3 stubs (fatal); 0 = no special. */
static inline u128 spu_dftsv(u128 a, int32_t imm){ (void)a; (void)imm; u128 r; memset(&r,0,sizeof r); return r; }

/* ---- Phase 2: register-variable shifts/rotates ---- */
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
static inline u128 spu_rotqby(u128 a, u128 b){ int sh=b._u32[0]&0x0F; u128 r; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }
static inline u128 spu_shlqbybi(u128 a, u128 b){ int sh=(b._u32[0]>>3)&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_rotqbybi(u128 a, u128 b){ int sh=(b._u32[0]>>3)&0x0F; u128 r; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }

/* ---- Phase 2: rotmahi ---- */
static inline u128 spu_rotmahi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._s16[i]=(sh>15)?(a._s16[i]>>15):(a._s16[i]>>sh); return r; }

/* ---- Phase 2: byte/half immediate compares ---- */
static inline u128 spu_ceqbi(u128 a, int32_t imm)  { u128 r; uint8_t v=(uint8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]==v)?0xFFu:0; return r; }
static inline u128 spu_ceqhi(u128 a, int32_t imm)  { u128 r; int16_t v=(int16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]==v)?0xFFFFu:0; return r; }
static inline u128 spu_clgtbi(u128 a, int32_t imm) { u128 r; uint8_t v=(uint8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]>v)?0xFFu:0; return r; }
static inline u128 spu_clgthi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]>v)?0xFFFFu:0; return r; }
static inline u128 spu_cgthi(u128 a, int32_t imm)  { u128 r; int16_t v=(int16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]>v)?0xFFFFu:0; return r; }
static inline u128 spu_cgtbi(u128 a, int32_t imm)  { u128 r; int8_t v=(int8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._s8[i]>v)?0xFFu:0; return r; }

/* ---- Phase 2: misc one-offs ---- */
static inline u128 spu_fscrrd(u128 a) { (void)a; return spu_zero(); }
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
 * 4 bytes of word i. Computed on word VALUES so byte order is irrelevant. */
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
static inline u128 spu_fcmeq(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++){ float fa=fabsf(a._f32[i]),fb=fabsf(b._f32[i]); r._u32[i]=(fa==fb)?0xFFFFFFFFu:0; } return r; }
static inline u128 spu_fcmgt(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++){ float fa=fabsf(a._f32[i]),fb=fabsf(b._f32[i]); r._u32[i]=(fa>fb)?0xFFFFFFFFu:0; } return r; }
/* spu_frest / spu_frsqest: floating reciprocal estimate / floating reciprocal
 * absolute square root estimate. Both must emit the packed base+step encoding
 * that spu_fi (above) decodes -- NOT a plain full-precision 1/x or 1/sqrt(x)
 * float. This is the second half of the fi/frest/frsqest COUPLED fix: fi
 * alone was already wrong (fixed), but fi is a no-op-shaped bug generator
 * unless its *inputs* are also HW-shaped.
 *
 * ---- Sources ----
 * ISA: SPU_ISA_v1.2_27Jan2007_pub.pdf
 *   "Floating Reciprocal Estimate" p.214-215 (frest)
 *   "Floating Reciprocal Absolute Square Root Estimate" p.216-217 (frsqest)
 * Result format (both instructions, same layout fi already assumes):
 *   S(1) | BiasedExponent(8) | BaseFraction(13) | StepFraction(10)
 *   Base = S 1.BaseFraction * 2^(BiasedExponent-127)
 *   Step = 0.000 StepFraction * 2^(BiasedExponent-127)   (frest: sign of Base;
 *          frsqest: sign always 0 per ISA p.216 "The sign bit (S) will be zero")
 * ISA text documents the FIELD LAYOUT but does NOT print the lookup-table
 * CONSTANTS (they are silicon facts, not derivable from the prose) -- those
 * come from the oracle below.
 *
 * ORACLE = RPCS3's ASMJIT recompiler tables + logic, NOT the interpreter:
 *   rpcs3/rpcs3/Emu/Cell/SPUASMJITRecompiler.cpp
 *     FREST   lines 2753-2797 (fraction_lut + exponent_lut, both gathered by index)
 *     FRSQEST lines 2799-2836 (fraction_lut gathered by index; exponent computed
 *              inline: "(exponent==0)? 0xFF : 190 - (exponent + 1) / 2", integer div)
 *   Raw LUT constants: rpcs3/rpcs3/Emu/Cell/SPUThread.cpp lines 51-105
 *     spu_frest_fraction_lut[32], spu_frest_exponent_lut[256],
 *     spu_frsqest_fraction_lut[64], spu_frsqest_exponent_lut[256]
 *   Independent second source: rpcs3/rpcs3/Emu/Cell/SPULLVMRecompiler.cpp
 *     FREST lines 7389-7427, FRSQEST lines 7435-7461 -- a SEPARATE recompiler
 *     backend using the same fraction LUTs and an equivalent (algebraically
 *     verified, see below) inline exponent formula for frest:
 *       r_exponent = saturating(0x7E80 - a_exponent_u16lane), else 0x7F800000
 *     ASMJIT and LLVM formulas independently agree with the raw 256-entry
 *     tables (below) -- three-source cross-check.
 *
 * ORACLE TRAP (same class as fi's, confirmed here too): RPCS3's *interpreter*
 * FREST/FRSQEST (SPUInterpreter.cpp:2194-2238, spu_interpreter_precise::FREST/
 * FRSQEST) are ALSO not the packed encoding -- they call _mm_rcp_ps / std::sqrt
 * and store a plain full-precision reciprocal / rsqrt into rt. This is the
 * SAME approximation our current (wrong) spu_frest/spu_frsqest already use --
 * i.e. our bug independently reproduces RPCS3's interpreter-stub bug. Do NOT
 * use the interpreter as an oracle for these two instructions; only the
 * ASMJIT or LLVM recompiler paths implement real hardware semantics.
 *
 * ---- Closed-form verification of the exponent fields ----
 * The two 256-entry *exponent* LUTs (SPUThread.cpp lines 59-77, 87-105) were
 * checked by exhaustive Python cross-check (all 256 entries each) against:
 *   frest:   rexp = (exp==0) ? 0xFF : ( (253-exp)>0 ? (253-exp) : 0 )
 *   frsqest: rexp = (exp==0) ? 0xFF : (190 - (exp+1)/2)          [int div]
 * Result: frsqest formula matched the raw table on 256/256 entries with ZERO
 * mismatches. frest formula matched 254/256 directly (idx 0..253); the
 * remaining 2 (idx 254,255, i.e. input exponent so large that the reciprocal
 * exponent would go negative) resolve to 0 in the raw table, i.e. reciprocal
 * underflow-to-zero -- exactly the ISA's documented "Big: if |x|>=2^126, 1/x
 * underflows to zero" rule (p.215) -- and the clamped formula then matches
 * 256/256. Because these two fields are provably closed-form, this file does
 * NOT embed either 256-entry exponent table; it computes them directly,
 * which is simpler and removes 512 magic constants. The two *fraction* LUTs
 * (32 and 64 entries) have NO closed form (verified: not an arithmetic or
 * simple bit-derived sequence) -- they are genuine hardware lookup tables and
 * ARE reproduced verbatim below (hardware facts, not GPL'd code -- only the
 * numeric constants are taken; all surrounding C is original).
 *
 * ---- Pipeline validation (hand/script-verified, not fuzzed --
 * frest/frsqest ISA specs are error-bound-only, so single-instruction fuzzing
 * has no ground truth; the PIPELINE has one) ----
 * Reciprocal pipeline (ISA p.215): FREST y0,x / FI y1,x,y0 /
 *   FNMS t1,x,y1,ONE / FMA y2,t1,y1,y1
 * Rsqrt pipeline (ISA p.217): FRSQEST y0,x / AND ax,x,mask / FI y1,ax,y0 /
 *   FM t1,ax,y1 / FM t2,y1,HALF / FNMS t1,t1,y1,ONE / FMA y2,t1,t2,y1
 * Both pipelines were run in Python (float32-rounded at each step) using
 * THIS file's frest/frsqest bit-packing plus scratch/fix_fi_spu.c's fi
 * decode, across: two of fix_fi_spu.c's own worked examples, a third
 * representative value, 1.0 (identity), and 2^+-100 (extreme exponents
 * exercising the exponent-field arithmetic far from the biased-127 center):
 *
 *   x=5.331123525209338      y2=0.187577724   true 1/x=0.187577721   rel~2.0e-8
 *   x=-76.91874321984837     y2=-0.0130007323 true 1/x=-0.0130007324 rel~1.1e-8
 *   x=10.859253470350062     y2=0.0920873582  true 1/x=0.0920873615  rel~3.6e-8
 *   x=1.0                    y2=1.0           true 1/x=1.0           exact
 *   x=2^-100                 y2=2^100         true 1/x=2^100         exact
 *   x=2^100                  y2=2^-100        true 1/x=2^-100        exact
 *
 *   x=5.331123525209338  rsqrt y2=0.433102429 true=0.433102437 rel~1.8e-8
 *   x=76.91874321984837  rsqrt y2=0.114020757 true=0.114020754 rel~2.6e-8
 *   x=10.859253470350062 rsqrt y2=0.303458989 true=0.303458995 rel~2.0e-8
 *   x=1.0                rsqrt y2=1.0          true=1.0         exact
 *   x=2^-100 / 2^100      rsqrt exact to displayed precision
 *
 * All within ~1e-8 relative error (a handful of float32 ULPs) after the
 * documented one-Newton-Raphson-step refine, matching the ISA's claim of
 * "within 1 ulp of the IEEE single-precision reciprocal" (p.215) and
 * "sufficient to produce an IEEE single-precision reciprocal" for rsqrt
 * (p.217). Edge cases checked separately:
 *   - x=0.0 (and -0.0): input exponent field is 0 -> frest returns the ISA's
 *     documented special "maximum sfp" pattern sign|0x7FFFFF... i.e. bits
 *     sign|0xFF800000|0x7FFFFF = 0x7FFFFFFF (or 0xFFFFFFFF for -0.0) exactly
 *     matching ISA p.215 "1/0 is defined to give ... x`7FFF FFFF' (1.999*2^128)".
 *   - True denormals (smallest float32 subnormal, bits=0x1): biased exponent
 *     field is also 0, so they fall into the SAME special-case bucket as
 *     zero (matches HW: SPU treats a zero-exponent operand as the
 *     divide-by-zero-flagged case regardless of a nonzero mantissa).
 *   - Powers of 2 (tested above via 2^+-100): exercise the exponent-field
 *     arithmetic away from the fraction-LUT boundary; StepFraction happens to
 *     be at its max-magnitude-relative-to-exponent case for the fraction
 *     index that borders LUT rows 20-31/54-63 (identical adjacent entries in
 *     both fraction tables -- see NOTE below); no mismatch found.
 *
 * NOTE on the fraction LUTs' repeated-adjacent-pairs: spu_frest_fraction_lut
 * indices 20/21, 22/23, ..., 30/31 are IDENTICAL pairs, and
 * spu_frsqest_fraction_lut has many repeated adjacent pairs too. This is a
 * real hardware artifact (the table has fewer effective ulps of index
 * resolution at small fraction-index values) -- reproduced verbatim, not a
 * transcription bug (cross-checked against the LLVM recompiler's identical
 * constant arrays, SPULLVMRecompiler.cpp lines 1623-1624 referencing the same
 * spu_frest_fraction_lut / spu_frsqest_fraction_lut symbols).
 *
 * ---- Consistency with spu_fi (above) ----
 * spu_fi decodes RB as:
 *   exp        = (rb>>23) & 0xFF
 *   base_bits  = rb & 0xFFFFFC00        (sign(1)+exp(8)+basefrac(13), verbatim IEEE bits)
 *   step_frac  = rb & 0x3FF             (10 bits)
 *   step       = ldexp((float)step_frac, exp-127-13), sign-matched to base
 * This file's spu_frest/spu_frsqest PRODUCE exactly that layout: sign in bit
 * 31 (frest only; frsqest forces 0), exponent in bits 23-30, a 13-bit base
 * fraction in bits 10-22 (bits 5-17 of the LUT constant, which is already
 * pre-shifted so it can be OR'd straight into bits 0-22 alongside the 8-bit
 * exponent -- verified: every fraction_lut constant here is < 0x800000, i.e.
 * occupies only bits 0-22, so `exponent_field | fraction_field` cannot
 * collide), and the low 10 bits as StepFraction. CONFIRMED CONSISTENT: no
 * mismatch between what this file emits and what spu_fi consumes.
 *
 * Old code (bug, same class in both instructions): full-precision 1/x or
 * 1/sqrt(x) as a plain IEEE float, none of the base/step field structure --
 * confirmed wrong the same way old spu_fi was, and independently matches
 * RPCS3's interpreter-stub bug (see ORACLE TRAP above), which is presumably
 * how it entered this codebase (an early port likely used the interpreter as
 * its reference before a later ASMJIT-recompiler cross-check).
 */

/* ---- hardware lookup tables (RPCS3 SPUThread.cpp:51-57, 79-85) ----
 * Pre-shifted 23-bit BaseFraction|StepFraction fields, indexed by the top 5
 * (frest) or 6 (frsqest) bits of RA's IEEE fraction. No closed form exists
 * for these (verified not to follow an arithmetic sequence); they are
 * hardware facts reproduced verbatim, cited from RPCS3, not copied logic. */
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
 * consumed by spu_fi (above), NOT a plain 1/x float.
 * ISA "Floating Reciprocal Estimate" p.214-215; RPCS3 ASMJIT recompiler
 * SPUASMJITRecompiler.cpp:2753-2797 + LLVM recompiler
 * SPULLVMRecompiler.cpp:7389-7427 (independent cross-check). */
static inline u128 spu_frest(u128 a) {
    u128 r;
    for (int i = 0; i < 4; i++) {
        uint32_t bits = a._u32[i];
        uint32_t sign = bits & 0x80000000u;
        uint32_t exp  = (bits >> 23) & 0xFFu;
        uint32_t frac_idx = (bits >> 18) & 0x1Fu;
        uint32_t out;
        if (exp == 0u) {
            /* zero or denormal operand: ISA p.215 "1/0 is defined to give the
             * maximum SPU single-precision extended-range fp number", sign of
             * the (zero) input preserved. Same bucket for true denormals
             * (their biased exponent field is also 0). */
            out = sign | 0x7FFFFFFFu;
        } else {
            /* result biased exponent = 253 - input biased exponent (253 =
             * 2*127-1, i.e. reciprocal negates the unbiased exponent),
             * clamped to 0 on underflow (ISA p.215 "Big: if |x|>=2^126,
             * 1/x underflows to zero"). Verified bit-exact against RPCS3's
             * raw 256-entry spu_frest_exponent_lut for all 256 inputs. */
            uint32_t rexp = (exp >= 253u) ? 0u : (253u - exp);
            uint32_t fraction = spu_frest_fraction_lut[frac_idx];
            out = sign | (rexp << 23) | (fraction & 0x7FFFFFu);
        }
        memcpy(&r._f32[i], &out, sizeof out);
    }
    return r;
}

/* frsqest: floating reciprocal absolute square-root estimate. Produces the
 * packed base+step encoding consumed by spu_fi; sign bit is ALWAYS 0 per
 * ISA ("The sign bit (S) will be zero", p.216) -- frsqest(x) estimates
 * 1/sqrt(|x|), always positive.
 * ISA "Floating Reciprocal Absolute Square Root Estimate" p.216-217; RPCS3
 * ASMJIT recompiler SPUASMJITRecompiler.cpp:2799-2836 (exponent computed
 * inline: "(exponent==0)? 0xFF : 190-(exponent+1)/2", integer division) +
 * LLVM recompiler SPULLVMRecompiler.cpp:7435-7461 (independent formula,
 * algebraically identical). Verified bit-exact against RPCS3's raw 256-entry
 * spu_frsqest_exponent_lut for ALL 256 inputs (zero mismatches). */
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

/* ---- Phase 3: sign extension ----
 * LE host: low sub-lane = _u8[2i] / _s16[2i] / _s32[2i] (the byte/half/word
 * at the *lower* storage address). Same caveat as the mpy family. */
static inline u128 spu_xsbh(u128 a) { u128 r; for(int i=0;i<8;i++) r._s16[i] = (int8_t)a._u8[2*i]; return r; }
static inline u128 spu_xshw(u128 a) { u128 r; for(int i=0;i<4;i++) r._s32[i] = (int16_t)a._s16[2*i]; return r; }
/* xswd (ISA v1.2 p96): per doubleword, sign-extend the word in the RIGHT
 * slot -- the value stays in the right word, the sign fill goes in the left.
 * The old `_s64[i] = (int32_t)_s32[2*i]` was wrong on BOTH counts in our lane
 * convention (sourced the left word AND placed value/sign swapped); caught
 * RED by tools/test_spu_lift.py 2026-07-03 (61 live sites incl. the codec). */
static inline u128 spu_xswd(u128 a) {
    u128 r;
    for (int i = 0; i < 2; i++) {
        r._u32[2*i]   = (a._s32[2*i+1] < 0) ? 0xFFFFFFFFu : 0u;
        r._u32[2*i+1] = a._u32[2*i+1];
    }
    return r;
}

/* ---- Phase 3: OR across ---- */
static inline u128 spu_orx(u128 a) {
    u128 r = spu_zero();
    r._u32[0] = a._u32[0] | a._u32[1] | a._u32[2] | a._u32[3];
    return r;
}

/* ---- Phase 3: form-select mask from bits ---- */
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

/* ---- Phase 3: constant generators (insertion shuffle patterns) ---- */
/* Generate-controls (cbd/chd/cwd/cdd) — insertion selectors for the (now
 * SPU-byte-correct) shufb. Each builds a control in TRUE SPU byte order: the
 * base selects b's SPU byte t at result SPU byte t (selector 0x10+t), and the
 * insert positions select a's preferred scalar bytes -- SPU byte 3 for a byte
 * (selector 0x03), SPU bytes 2,3 for a halfword (0x02,0x03), SPU bytes 0..3 for
 * a word, 0..7 for a doubleword. Every store is mapped through SPU_W to land at
 * the right host index. Verified to compose correctly with the fixed shufb for
 * both cbd-generated and immediate/LS-constant controls. */
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

/* ---- Phase 3: rotate-and-mask family ---- */
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

/* ---- Phase 3: halfword/byte immediate logic ---- */
static inline u128 spu_andhi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]&v; return r; }
static inline u128 spu_andbi(u128 a, int32_t imm) { u128 r; uint8_t  v=(uint8_t)imm;  for(int i=0;i<16;i++) r._u8[i]=a._u8[i]&v;   return r; }
static inline u128 spu_orhi(u128 a, int32_t imm)  { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]|v; return r; }
static inline u128 spu_orbi(u128 a, int32_t imm)  { u128 r; uint8_t  v=(uint8_t)imm;  for(int i=0;i<16;i++) r._u8[i]=a._u8[i]|v;   return r; }
static inline u128 spu_xorhi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]^v; return r; }

/* ---- Phase 3: borrow-generate extended ---- */
static inline u128 spu_bgx(u128 a, u128 b, u128 t) {
    u128 r;
    for(int i=0;i<4;i++) {
        uint64_t s = (uint64_t)b._u32[i] + (uint64_t)(~a._u32[i]) + (uint64_t)(t._u32[i]&1);
        r._u32[i] = (uint32_t)(s >> 32);
    }
    return r;
}

/* ---- Phase 3: mfspr stub ---- */
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
