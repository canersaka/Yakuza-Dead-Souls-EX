/* WB kernel differential test: reference wrappers.
 *
 * Every tested spu_helpers.h operation is exported through an extern
 * function compiled in a BASELINE-ISA translation unit (reference.c), while
 * the WB kernels under test compile with /arch:AVX2 in main.c. This models
 * production exactly (FAST twins are baseline, WB twins are AVX2) and makes
 * the comparison sensitive to any AVX2-codegen behavior change in the
 * wrapped scalar paths (e.g. FP contraction), not only to kernel bugs. */
#ifndef SPU_WB_KERNEL_REFERENCE_H
#define SPU_WB_KERNEL_REFERENCE_H

#include "../../include/ps3emu/ps3types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REF1(n)  u128 ref_##n(u128 a);
#define REF2(n)  u128 ref_##n(u128 a, u128 b);
#define REF3(n)  u128 ref_##n(u128 a, u128 b, u128 c);
#define REFI(n)  u128 ref_##n(u128 a, int32_t imm);

REF1(clz) REF1(cntb) REF1(gb) REF1(gbh) REF1(gbb) REF1(orx)
REF1(xsbh) REF1(xshw) REF1(xswd) REF1(fsm) REF1(fsmh) REF1(fsmb)
REF1(frest) REF1(frsqest) REF1(fesd) REF1(frds) REF1(mfspr)

REF2(a) REF2(sf) REF2(ah) REF2(sfh)
REF2(and) REF2(or) REF2(xor) REF2(nand) REF2(nor) REF2(andc) REF2(orc) REF2(eqv)
REF2(ceq) REF2(ceqh) REF2(ceqb) REF2(cgt) REF2(cgth) REF2(cgtb)
REF2(clgt) REF2(clgth) REF2(clgtb)
REF2(mpy) REF2(mpyu) REF2(mpyh) REF2(mpyhh) REF2(mpyhhu) REF2(mpys)
REF2(fi) REF2(fa) REF2(fs) REF2(fm)
REF2(fceq) REF2(fcgt) REF2(fcmeq) REF2(fcmgt)
REF2(dfa) REF2(dfs) REF2(dfm) REF2(dfceq) REF2(dfcmeq) REF2(dfcgt) REF2(dfcmgt)
REF2(absdb) REF2(avgb) REF2(cg) REF2(bg) REF2(sumb)
REF2(shl) REF2(rot) REF2(rotm) REF2(rotma)
REF2(shlh) REF2(roth) REF2(rothm) REF2(rothma)
REF2(shlqbi) REF2(rotqbi) REF2(shlqby) REF2(rotqby)
REF2(shlqbybi) REF2(rotqbybi) REF2(rotqmbi) REF2(rotqmby) REF2(rotqmbybi)
REF2(cbx) REF2(chx) REF2(cwx) REF2(cdx)

REF3(selb) REF3(shufb) REF3(mpya) REF3(fma) REF3(fms) REF3(fnms)
REF3(addx) REF3(sfx) REF3(cgx) REF3(bgx)
REF3(dfma) REF3(dfms) REF3(dfnms) REF3(dfnma)
REF3(mpyhha) REF3(mpyhhau)

REFI(ai) REFI(ahi) REFI(sfi) REFI(sfhi)
REFI(andi) REFI(ori) REFI(xori)
REFI(andhi) REFI(andbi) REFI(orhi) REFI(orbi) REFI(xorhi) REFI(xorbi)
REFI(iohl)
REFI(ceqi) REFI(cgti) REFI(clgti) REFI(ceqbi) REFI(ceqhi)
REFI(clgtbi) REFI(clgthi) REFI(cgthi) REFI(cgtbi)
REFI(mpyi) REFI(mpyui)
REFI(shli) REFI(shlhi) REFI(roti) REFI(rothi)
REFI(rotmi) REFI(rotmai) REFI(rotmhi) REFI(rothmi) REFI(rotmahi)
REFI(shlqbyi) REFI(rotqbyi) REFI(shlqbii) REFI(rotqbii)
REFI(rotqmbii) REFI(rotqmbyi)
REFI(cflts) REFI(cfltu) REFI(csflt) REFI(cuflt) REFI(dftsv)
REFI(cbd) REFI(chd) REFI(cwd) REFI(cdd)

#undef REF1
#undef REF2
#undef REF3
#undef REFI

#ifdef __cplusplus
}
#endif

#endif
