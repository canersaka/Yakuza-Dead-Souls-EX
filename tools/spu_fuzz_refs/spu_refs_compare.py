#!/usr/bin/env python3
"""SPU fuzz reference plug-in -- COMPARE family (clean-room from SPU ISA v1.2).

Group: compare
Mnemonics: ceq ceqh ceqb ceqi ceqhi ceqbi
           cgt cgth cgtb cgti cgthi cgtbi
           clgt clgth clgtb clgti clgthi clgtbi

Every ref is transcribed straight from the RTL blocks of
SPU_ISA_v1.2_27Jan2007_pub.pdf (pages 156-173, "Compare, Branch, and Halt
Instructions"), with encodings/immediate forms cross-checked against
SPU_Assembly_Language_Spec_1.5.pdf. No lifter/runtime code is referenced.

Result convention (all compares): a lane's result is all-ones (true) or
all-zeros (false), placed back in the SAME lane of RT. Byte lanes -> 0xFF/0x00,
halfword lanes -> 0xFFFF/0x0000, word lanes -> 0xFFFFFFFF/0x00000000.

SUBTLE (byte-order / preferred-slot): the sub-word lanes must be enumerated in
BIG-ENDIAN order (byte 0 = MSByte of word 0). to_b()/to_h() give exactly that
architectural order, so the RTL "for i = 0 to 15 [by k]" loops map 1:1 onto the
helper lists and from_b()/from_h() reassemble the words correctly.

Three comparison KINDS in this family:
  * ceq*  : EQUALITY   (bit pattern equal; sign irrelevant).
  * cgt*  : ALGEBRAIC greater-than (operands interpreted SIGNED, per-lane width).
  * clgt* : LOGICAL   greater-than (operands interpreted UNSIGNED, per-lane).

Immediate encoding (ISA, I10 field):
  * word   imm (ceqi/cgti/clgti)  : comparand = RepLeftBit(I10,32) = I10 sign-
                                     extended to 32 bits.
  * hword  imm (ceqhi/cgthi/clgthi): comparand = RepLeftBit(I10,16) = I10 sign-
                                     extended to 16 bits.
  * byte   imm (ceqbi/cgtbi/clgtbi): comparand = I10[2:9] = the RIGHTMOST 8 bits
                                     of the I10 field (NOT a 10->8 sign-extend;
                                     just the low byte of the field).
Per the harness contract, ri10 refs receive `imm` already sign-extended to a
signed Python int in [-512, 511] (the value the 10-bit field holds). So:
  - word  comparand = imm & 0xFFFFFFFF   (re-expand sign to 32b as unsigned bits)
  - hword comparand = imm & 0xFFFF       (low 16b == RepLeftBit(I10,16) bits)
  - byte  comparand = imm & 0xFF         (the field's rightmost 8 bits)
For ALGEBRAIC lane compares the comparand's lane bits are then re-interpreted
signed at the lane width (sext(...,8/16/32)); for LOGICAL/EQUALITY they are used
as raw unsigned lane bits.
"""

from spu_refs_api import register, to_b, from_b, to_h, from_h, sext, M32


def _true(bits):
    return (1 << bits) - 1


def _sel(cond, bits):
    return _true(bits) if cond else 0


# ===========================================================================
# EQUALITY compares  (ISA: RAi = RBi  ->  all-ones, sign irrelevant)
# ===========================================================================

# ceq  (Compare Equal Word; ISA p160)  -- for i=0..15 by 4
def ref_ceq(a, b):
    return [_sel(a[i] == b[i], 32) for i in range(4)]

register("ceq", "rr", ref_ceq, op11=0x3C0, subtle=True)


# ceqh (Compare Equal Halfword; ISA p158)  -- for i=0..15 by 2
def ref_ceqh(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([_sel(ah[j] == bh[j], 16) for j in range(8)])

register("ceqh", "rr", ref_ceqh, op11=0x3C8, subtle=True)


# ceqb (Compare Equal Byte; ISA p156)  -- for i=0..15
def ref_ceqb(a, b):
    ab, bb = to_b(a), to_b(b)
    return from_b([_sel(ab[j] == bb[j], 8) for j in range(16)])

register("ceqb", "rr", ref_ceqb, op11=0x3D0, subtle=True)


# ceqi (Compare Equal Word Immediate; ISA p161)  -- cmp vs RepLeftBit(I10,32)
def ref_ceqi(a, imm):
    c = imm & M32
    return [_sel((w & M32) == c, 32) for w in a]

register("ceqi", "ri10", ref_ceqi, op8=0x7C, imm_pool=[5, -1, 0, 511, -512],
         subtle=True)


# ceqhi (Compare Equal Halfword Immediate; ISA p159) -- vs RepLeftBit(I10,16)
def ref_ceqhi(a, imm):
    c = imm & 0xFFFF
    return from_h([_sel(h == c, 16) for h in to_h(a)])

register("ceqhi", "ri10", ref_ceqhi, op8=0x7D, imm_pool=[0x100, -1, 0, 511, -512],
         subtle=True)


# ceqbi (Compare Equal Byte Immediate; ISA p157) -- vs I10[2:9] (rightmost 8b)
def ref_ceqbi(a, imm):
    c = imm & 0xFF
    return from_b([_sel(x == c, 8) for x in to_b(a)])

register("ceqbi", "ri10", ref_ceqbi, op8=0x7E, imm_pool=[0x7F, -1, 0, 0x80, 511],
         subtle=True)


# ===========================================================================
# ALGEBRAIC (SIGNED) greater-than compares  (ISA: RAi > RBi, signed)
# ===========================================================================

# cgt  (Compare Greater Than Word; ISA p166)  -- signed 32b, i=0..15 by 4
def ref_cgt(a, b):
    return [_sel(sext(a[i], 32) > sext(b[i], 32), 32) for i in range(4)]

register("cgt", "rr", ref_cgt, op11=0x240, subtle=True)


# cgth (Compare Greater Than Halfword; ISA p164)  -- signed 16b, i=0..15 by 2
def ref_cgth(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([_sel(sext(ah[j], 16) > sext(bh[j], 16), 16) for j in range(8)])

register("cgth", "rr", ref_cgth, op11=0x248, subtle=True)


# cgtb (Compare Greater Than Byte; ISA p162)  -- signed 8b, i=0..15
def ref_cgtb(a, b):
    ab, bb = to_b(a), to_b(b)
    return from_b([_sel(sext(ab[j], 8) > sext(bb[j], 8), 8) for j in range(16)])

register("cgtb", "rr", ref_cgtb, op11=0x250, subtle=True)


# cgti (Compare Greater Than Word Immediate; ISA p167) -- signed vs sext(I10,32)
def ref_cgti(a, imm):
    # RepLeftBit(I10,32) reinterpreted signed == imm itself (already signed).
    c = sext(imm & M32, 32)
    return [_sel(sext(w, 32) > c, 32) for w in a]

register("cgti", "ri10", ref_cgti, op8=0x4C, imm_pool=[5, -1, 0, 511, -512],
         subtle=True)


# cgthi (Compare Greater Than Halfword Immediate; ISA p165) -- signed vs sext16
def ref_cgthi(a, imm):
    c = sext(imm & 0xFFFF, 16)
    return from_h([_sel(sext(h, 16) > c, 16) for h in to_h(a)])

register("cgthi", "ri10", ref_cgthi, op8=0x4D, imm_pool=[0x100, -1, 0, 511, -512],
         subtle=True)


# cgtbi (Compare Greater Than Byte Immediate; ISA p163) -- signed vs sext8(I10[2:9])
def ref_cgtbi(a, imm):
    c = sext(imm & 0xFF, 8)
    return from_b([_sel(sext(x, 8) > c, 8) for x in to_b(a)])

register("cgtbi", "ri10", ref_cgtbi, op8=0x4E, imm_pool=[0x7F, -1, 0, 0x80, 511],
         subtle=True)


# ===========================================================================
# LOGICAL (UNSIGNED) greater-than compares  (ISA: RAi >u RBi, unsigned)
# ===========================================================================

# clgt (Compare Logical Greater Than Word; ISA p172)  -- unsigned 32b
def ref_clgt(a, b):
    return [_sel((a[i] & M32) > (b[i] & M32), 32) for i in range(4)]

register("clgt", "rr", ref_clgt, op11=0x2C0, subtle=True)


# clgth (Compare Logical Greater Than Halfword; ISA p170)  -- unsigned 16b
def ref_clgth(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([_sel(ah[j] > bh[j], 16) for j in range(8)])

register("clgth", "rr", ref_clgth, op11=0x2C8, subtle=True)


# clgtb (Compare Logical Greater Than Byte; ISA p168)  -- unsigned 8b
def ref_clgtb(a, b):
    ab, bb = to_b(a), to_b(b)
    return from_b([_sel(ab[j] > bb[j], 8) for j in range(16)])

register("clgtb", "rr", ref_clgtb, op11=0x2D0, subtle=True)


# clgti (Compare Logical Greater Than Word Immediate; ISA p173) -- unsigned vs
#   RepLeftBit(I10,32) taken as unsigned bits (so imm=-1 -> 0xFFFFFFFF).
def ref_clgti(a, imm):
    c = imm & M32
    return [_sel((w & M32) > c, 32) for w in a]

register("clgti", "ri10", ref_clgti, op8=0x5C, imm_pool=[5, -1, 0, 511, -512],
         subtle=True)


# clgthi (Compare Logical Greater Than Halfword Immediate; ISA p171) -- unsigned
#   vs RepLeftBit(I10,16) taken as unsigned 16b bits.
def ref_clgthi(a, imm):
    c = imm & 0xFFFF
    return from_h([_sel(h > c, 16) for h in to_h(a)])

register("clgthi", "ri10", ref_clgthi, op8=0x5D, imm_pool=[0x100, -1, 0, 511, -512],
         subtle=True)


# clgtbi (Compare Logical Greater Than Byte Immediate; ISA p169) -- unsigned vs
#   I10[2:9] (rightmost 8b) as unsigned byte.
def ref_clgtbi(a, imm):
    c = imm & 0xFF
    return from_b([_sel(x > c, 8) for x in to_b(a)])

register("clgtbi", "ri10", ref_clgtbi, op8=0x5E, imm_pool=[0x7F, -1, 0, 0x80, 511],
         subtle=True)
