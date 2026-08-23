/* WB kernel differential test: baseline-ISA reference TU.
 * See reference.h. This TU must NOT be compiled with /arch:AVX2 -- it is
 * the production-shaped compilation of spu_helpers.h. */
#include "spu_helpers.h"
#include "reference.h"

/* Runtime globals the helper headers reference (same shape the s50 diff
 * harness provides). Config all-zero: xfloat SPU-accurate path active,
 * every diagnostic off. */
const yz_runtime_config g_yz_runtime_config = {0};
volatile long g_yz_a010_root_active = 0;
uint64_t ppu_timebase_now(void) { return 0; }
void yz_tagread_repair_fetch(struct spu_context* c, uint32_t lsa,
                             unsigned long long ea, uint32_t size)
{ (void)c; (void)lsa; (void)ea; (void)size; }
void yz_tagread_repair_read(struct spu_context* c, uint32_t lsa, uint32_t* v)
{ (void)c; (void)lsa; (void)v; }
void yz_a010_reltrace_gate(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                           uint32_t e, uint32_t f, uint32_t g, uint32_t h,
                           uint32_t i)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; }

#define REF1(n)  u128 ref_##n(u128 a) { return spu_##n(a); }
#define REF2(n)  u128 ref_##n(u128 a, u128 b) { return spu_##n(a, b); }
#define REF3(n)  u128 ref_##n(u128 a, u128 b, u128 c) { return spu_##n(a, b, c); }
#define REFI(n)  u128 ref_##n(u128 a, int32_t imm) { return spu_##n(a, imm); }

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
REFI(ceqi) REFI(cgti) REFI(clgti) REFI(ceqbi) REFI(ceqhi)
REFI(clgtbi) REFI(clgthi) REFI(cgthi) REFI(cgtbi)
REFI(mpyi) REFI(mpyui)
REFI(shli) REFI(shlhi) REFI(roti) REFI(rothi)
REFI(rotmi) REFI(rotmai) REFI(rotmhi) REFI(rothmi) REFI(rotmahi)
REFI(shlqbyi) REFI(rotqbyi) REFI(shlqbii) REFI(rotqbii)
REFI(rotqmbii) REFI(rotqmbyi)
REFI(cflts) REFI(cfltu) REFI(csflt) REFI(cuflt) REFI(dftsv)
REFI(cbd) REFI(chd) REFI(cwd) REFI(cdd)

/* iohl's helper takes uint16_t; keep the test-side int32 shape and truncate
 * exactly like the lifter's call site does. */
u128 ref_iohl(u128 a, int32_t imm) { return spu_iohl(a, (uint16_t)imm); }
