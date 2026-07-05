#!/usr/bin/env python3
"""SPU fuzz reference plug-in -- mpy family (integer multiplies).

Clean-room refs transcribed from the RTL pseudocode in
SPU_ISA_v1.2_27Jan2007_pub.pdf (multiply instructions, pp.72-82 of the PDF's
printed page numbering) cross-checked against the prose in
SPU_Assembly_Language_Spec_1.5.pdf (Table, pp.15-16). No lifter/helper/ctx is
referenced -- each ref computes the architectural 4-word result straight from
the ISA and returns [w0, w1, w2, w3] (word 0 = preferred slot / MSW).

SUBTLE family (byte-order / half-lane sensitive): every SPU word is split into
two 16-bit halves -- RAj:j+1 = the LEFTMOST (most-significant) 16 bits
("high"), RAj+2:j+3 = the RIGHTMOST (least-significant) 16 bits ("low"). The
recurring bug class is picking the wrong half or the wrong signedness. The ISA
uses `*` for a SIGNED 16x16 multiply and `|*|` for an UNSIGNED one; the product
is a full 32-bit value placed in the word slot (16x16 -> 32 always fits).

Half selectors, straight from each word:
    lo16s(w) = sign-extended rightmost 16 bits  (RA2:3, signed)
    lo16u(w) = zero-extended rightmost 16 bits   (RA2:3, unsigned)
    hi16s(w) = sign-extended leftmost 16 bits     (RA0:1, signed)
    hi16u(w) = zero-extended leftmost 16 bits      (RA0:1, unsigned)
"""

from spu_refs_api import register, sext, M32


def lo16s(w):
    return sext(w & 0xFFFF, 16)


def lo16u(w):
    return w & 0xFFFF


def hi16s(w):
    return sext((w >> 16) & 0xFFFF, 16)


def hi16u(w):
    return (w >> 16) & 0xFFFF


# --- mpy  (Multiply; ISA p72, op 01111000100 = 0x3C4) -----------------------
# RTL: RTj:j+3 <- RAj+2:j+3 * RBj+2:j+3  (signed rightmost-16 x rightmost-16),
# full 32-bit product into the word. Leftmost 16 bits of each operand ignored.
def ref_mpy(a, b):
    return [(lo16s(a[i]) * lo16s(b[i])) & M32 for i in range(4)]

register("mpy", "rr", ref_mpy, op11=0x3C4, subtle=True)


# --- mpyu  (Multiply Unsigned; ISA p73, op 01111001100 = 0x3CC) -------------
# RTL: RTj:j+3 <- RAj+2:j+3 |*| RBj+2:j+3  (unsigned rightmost-16 x rightmost-16).
def ref_mpyu(a, b):
    return [(lo16u(a[i]) * lo16u(b[i])) & M32 for i in range(4)]

register("mpyu", "rr", ref_mpyu, op11=0x3CC, subtle=True)


# --- mpyh  (Multiply High; ISA p77, op 01111000101 = 0x3C5) -----------------
# RTL: t <- RAj:j+1 * RBj+2:j+3 ; RTj:j+3 <- t[bytes 2:3] || 0x0000
#   i.e. take the LOW 16 bits of (RA_high x RB_low) and shift left by 16.
# Only the low 16 bits of the product survive, so the multiply's signedness is
# immaterial to the result -- signed is used to match the RTL's `*` literally.
def ref_mpyh(a, b):
    return [(((hi16s(a[i]) * lo16s(b[i])) & 0xFFFF) << 16) & M32
            for i in range(4)]

register("mpyh", "rr", ref_mpyh, op11=0x3C5, subtle=True)


# --- mpys  (Multiply and Shift Right; ISA p78, op 01111000111 = 0x3C7) ------
# RTL: t <- RAj+2:j+3 * RBj+2:j+3 (signed lo x lo); RTj:j+3 <- RepLeftBit(t[bytes 0:1], 32)
#   = take the HIGH 16 bits of the 32-bit product, sign-extend to 32 bits.
def ref_mpys(a, b):
    return [sext(((lo16s(a[i]) * lo16s(b[i])) >> 16) & 0xFFFF, 16) & M32
            for i in range(4)]

register("mpys", "rr", ref_mpys, op11=0x3C7, subtle=True)


# --- mpyhh  (Multiply High High; ISA p79, op 01111000110 = 0x3C6) -----------
# RTL: RTj:j+3 <- RAj:j+1 * RBj:j+1  (signed leftmost-16 x leftmost-16).
def ref_mpyhh(a, b):
    return [(hi16s(a[i]) * hi16s(b[i])) & M32 for i in range(4)]

register("mpyhh", "rr", ref_mpyhh, op11=0x3C6, subtle=True)


# --- mpyhhu  (Multiply High High Unsigned; ISA p81, op 01111001110 = 0x3CE) -
# RTL: RTj:j+3 <- RAj:j+1 |*| RBj:j+1  (unsigned leftmost-16 x leftmost-16).
def ref_mpyhhu(a, b):
    return [(hi16u(a[i]) * hi16u(b[i])) & M32 for i in range(4)]

register("mpyhhu", "rr", ref_mpyhhu, op11=0x3CE, subtle=True)


# --- mpyhha  (Multiply High High and Add; ISA p80, op 01101000110 = 0x346) --
# RTL: RTj:j+3 <- RAj:j+1 * RBj:j+1 + RTj:j+3  (signed hh product accumulated
#   into the existing RT word). RT is read as an input -> "rrt" form.
def ref_mpyhha(a, b, t):
    return [(hi16s(a[i]) * hi16s(b[i]) + t[i]) & M32 for i in range(4)]

register("mpyhha", "rrt", ref_mpyhha, op11=0x346, subtle=True)


# --- mpyhhau (Multiply High High Unsigned and Add; ISA p82, op 01101001110 = 0x34E)
# RTL: RTj:j+3 <- RAj:j+1 |*| RBj:j+1 + RTj:j+3  (unsigned hh product accumulated).
def ref_mpyhhau(a, b, t):
    return [(hi16u(a[i]) * hi16u(b[i]) + t[i]) & M32 for i in range(4)]

register("mpyhhau", "rrt", ref_mpyhhau, op11=0x34E, subtle=True)


# --- mpya  (Multiply and Add; ISA p76, RRR op 1100 = 0xC) -------------------
# RTL: t <- RAj+2:j+3 * RBj+2:j+3 (signed lo x lo); RTj:j+3 <- t + RCj:j+3.
#   RC is a genuine third source register -> "rrr" form ref(a, b, c).
def ref_mpya(a, b, c):
    return [(lo16s(a[i]) * lo16s(b[i]) + c[i]) & M32 for i in range(4)]

register("mpya", "rrr", ref_mpya, op4=0xC, subtle=True)


# --- mpyi  (Multiply Immediate; ISA p74, op 01110100 = 0x74) ----------------
# RTL: t <- RepLeftBit(I10,16) (I10 sign-extended to 16 bits, i.e. its signed
#   value); RTj:j+3 <- RAj+2:j+3 * t  (signed lo16(RA) x signed I10).
# `imm` arrives already sign-extended per the contract (range [-512,511]),
# which equals RepLeftBit(I10,16) as a signed integer.
def ref_mpyi(a, imm):
    return [(lo16s(w) * imm) & M32 for w in a]

register("mpyi", "ri10", ref_mpyi, op8=0x74, subtle=True)


# --- mpyui  (Multiply Unsigned Immediate; ISA p75, op 01110101 = 0x75) ------
# RTL: t <- RepLeftBit(I10,16); RTj:j+3 <- RAj+2:j+3 |*| t, BOTH treated as
#   UNSIGNED. So sign-extend I10 to a 16-bit pattern, then read that pattern as
#   an unsigned 16-bit value (0..65535), and multiply by unsigned lo16(RA).
#   `imm` arrives sign-extended, so (imm & 0xFFFF) reproduces the 16-bit pattern
#   before the unsigned reinterpretation (e.g. imm=-1 -> 0xFFFF -> 65535).
def ref_mpyui(a, imm):
    t = imm & 0xFFFF
    return [(lo16u(w) * t) & M32 for w in a]

register("mpyui", "ri10", ref_mpyui, op8=0x75, subtle=True)
