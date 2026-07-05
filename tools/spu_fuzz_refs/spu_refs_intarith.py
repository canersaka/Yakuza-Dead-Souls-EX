#!/usr/bin/env python3
"""SPU fuzz reference plug-in -- INTEGER ARITHMETIC family.

Group 'intarith': a, ah, addx, sf, sfh, sfx, ai, ahi, sfi, sfhi.

Clean-room refs transcribed from the RTL blocks of SPU_ISA_v1.2_27Jan2007_pub
(pages cited per mnemonic below) and the encoding fields of
SPU_Assembly_Language_Spec_1.5. NO lifter/helper/ctx reference -- these are the
independent oracle the lifted C is checked against.

Register model (per the REF-FUNCTION CONTRACT): a value is a 4-uint32 list
[w0,w1,w2,w3], word 0 = preferred slot = the most-significant 32 bits of the
128-bit architectural quadword. Halfword ops go through to_h/from_h so the eight
16-bit slots stay in true big-endian architectural order (slot 0 = MS halfword
of word 0). Immediates arrive ALREADY sign-extended to the Python int the ISA
field holds (I10 in [-512,511]); each ref masks/re-expands per RepLeftBit RTL.

Semantics summary (all "overflows and carries are not detected" -- pure modular):
  a     p60  RTk = RAk + RBk                      (4 words, mod 2^32)
  ah    p58  RTh = RAh + RBh                       (8 halfwords, mod 2^16)
  addx  p66  RTk = RAk + RBk + RT[k*32+31]         (carry-in = LSB of RT word k)
  sf    p64  RTk = RBk + (~RAk) + 1  (= RBk-RAk)
  sfh   p62  RTh = RBh + (~RAh) + 1
  sfx   p69  RTk = RBk + (~RAk) + RT[k*32+31]      (borrow-in = LSB of RT word k)
  ai    p61  RTk = RAk + RepLeftBit(I10,32)
  ahi   p59  RTh = RAh + RepLeftBit(I10,16)
  sfi   p65  RTk = RepLeftBit(I10,32) + (~RAk) + 1
  sfhi  p63  RTh = RepLeftBit(I10,16) + (~RAh) + 1
"""

from spu_refs_api import register, to_b, from_b, to_h, from_h, sext, M32

H16 = 0xFFFF


# --- a  (Add Word; ISA p60) --------------------------------------------------
# RTL: RTk = RAk + RBk for each of four 32-bit word slots; carry discarded.
def ref_a(a, b):
    return [(a[i] + b[i]) & M32 for i in range(4)]

register("a", "rr", ref_a, op11=0x0C0)


# --- ah  (Add Halfword; ISA p58) ---------------------------------------------
# RTL: eight halfword slots, RTh = RAh + RBh mod 2^16. Lane order via to_h/from_h.
def ref_ah(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([(ah[j] + bh[j]) & H16 for j in range(8)])

register("ah", "rr", ref_ah, op11=0x0C8, subtle=True)


# --- addx  (Add Extended; ISA p66) -------------------------------------------
# RTL: RTk = RAk + RBk + RT(k*32+31). RT(k*32+31) is bit 31 of quadword-word k,
# i.e. the LSB of RT word k (t[k] & 1). Bits 0..30 of each RT word are reserved
# (should be zero) -- the fuzzer may seed junk there; the RTL only reads bit 31.
def ref_addx(a, b, t):
    return [(a[i] + b[i] + (t[i] & 1)) & M32 for i in range(4)]

register("addx", "rrt", ref_addx, op11=0x340)


# --- sf  (Subtract from Word; ISA p64) ---------------------------------------
# RTL: RTk = RBk + (~RAk) + 1  == (RBk - RAk) mod 2^32.
def ref_sf(a, b):
    return [(b[i] + ((~a[i]) & M32) + 1) & M32 for i in range(4)]

register("sf", "rr", ref_sf, op11=0x040)


# --- sfh  (Subtract from Halfword; ISA p62) ----------------------------------
# RTL: RTh = RBh + (~RAh) + 1 (eight halfwords). Lane order via to_h/from_h.
def ref_sfh(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([(bh[j] + ((~ah[j]) & H16) + 1) & H16 for j in range(8)])

register("sfh", "rr", ref_sfh, op11=0x048, subtle=True)


# --- sfx  (Subtract from Extended; ISA p69) ----------------------------------
# RTL: RTk = RBk + (~RAk) + RT(k*32+31). Borrow-in = LSB of RT word k.
def ref_sfx(a, b, t):
    return [(b[i] + ((~a[i]) & M32) + (t[i] & 1)) & M32 for i in range(4)]

register("sfx", "rrt", ref_sfx, op11=0x341)


# --- ai  (Add Word Immediate; ISA p61) ---------------------------------------
# RTL: t = RepLeftBit(I10,32); RTk = RAk + t. imm arrives sign-extended in
# [-512,511]; (imm & M32) reproduces RepLeftBit(I10,32) exactly.
def ref_ai(a, imm):
    t = sext(imm, 10) & M32
    return [(a[i] + t) & M32 for i in range(4)]

register("ai", "ri10", ref_ai, op8=0x1C)


# --- ahi  (Add Halfword Immediate; ISA p59) ----------------------------------
# RTL: s = RepLeftBit(I10,16); RTh = RAh + s (eight halfwords).
def ref_ahi(a, imm):
    s = sext(imm, 10) & H16
    return from_h([(h + s) & H16 for h in to_h(a)])

register("ahi", "ri10", ref_ahi, op8=0x1D, subtle=True)


# --- sfi  (Subtract from Word Immediate; ISA p65) ----------------------------
# RTL: t = RepLeftBit(I10,32); RTk = t + (~RAk) + 1  == (t - RAk) mod 2^32.
def ref_sfi(a, imm):
    t = sext(imm, 10) & M32
    return [(t + ((~a[i]) & M32) + 1) & M32 for i in range(4)]

register("sfi", "ri10", ref_sfi, op8=0x0C)


# --- sfhi  (Subtract from Halfword Immediate; ISA p63) -----------------------
# RTL: t = RepLeftBit(I10,16); RTh = t + (~RAh) + 1 (eight halfwords).
def ref_sfhi(a, imm):
    t = sext(imm, 10) & H16
    return from_h([(t + ((~h) & H16) + 1) & H16 for h in to_h(a)])

register("sfhi", "ri10", ref_sfhi, op8=0x0D, subtle=True)
