#!/usr/bin/env python3
"""SPU fuzz reference plug-in -- QUADSHIFT family (clean-room).

Group: quadshift. Mnemonics (15):
  bit shifts/rotates of the whole 128-bit quadword:
    shlqbi  shlqbii   -- shift left by bits
    rotqbi  rotqbii   -- rotate left by bits
    rotqmbi rotqmbii  -- rotate-and-mask by bits (== logical shift right by bits)
  byte shifts/rotates of the whole 128-bit quadword:
    shlqby  shlqbyi  shlqbybi   -- shift left by bytes
    rotqby  rotqbyi  rotqbybi   -- rotate left by bytes
    rotqmby rotqmbyi rotqmbybi  -- rotate-and-mask by bytes (== logical shift right by bytes)

CLEAN-ROOM SOURCE: every ref below is transcribed straight from the RTL blocks
in SPU_ISA_v1.2_27Jan2007_pub.pdf (the "s <- ...; for b = ...; RT <- r" pseudo-
code), NOT from the lifter, the runtime helpers, or the harness's own static
refs. Encoding/immediate forms cross-checked against
SPU_Assembly_Language_Spec_1.5.pdf. The refs run the ISA loop literally rather
than taking a shortcut, so this oracle is genuinely independent of whatever the
lifted C does.

BYTE/BIT ORDER (contract): a register value is [w0,w1,w2,w3], word 0 = preferred
slot = the MOST-significant 32 bits of the architectural quadword.
  * to_b(v)  -> 16 ints, BIG-ENDIAN, byte 0 = the quadword's MSByte (bits 24..31
               of word 0). from_b() is its inverse. This is exactly the ISA's
               byte index b (RA_b, byte 0 = leftmost).
  * to_q(v)  -> the 128-bit architectural quadword as one Python int, word 0 in
               bits 96..127 (MSB-first). from_q() unpacks. This is exactly the
               ISA's bit index b (RA_b, bit 0 = leftmost = MSB of the quadword).
So "for b = 0 to 15: RT_b <- RA_{b+s}" over BYTES maps 1:1 onto to_b/from_b, and
"for b = 0 to 127" over BITS maps 1:1 onto a big-endian bit list from to_q.

IMMEDIATE DELIVERY (contract + harness _fuzz): the ri7 forms receive i7 as the
raw 7-bit value 0..127 (the harness draws it and masks &0x7F). The ISA extracts
a sub-field of that I7 (e.g. "I7 & 0x07", "I7 & 0x1F", "I7_14:17"); each ref
does that extraction itself, per the RTL.

REGISTER SHIFT-COUNT SOURCE: the rr forms take the count from a named bit-field
of RB's PREFERRED slot (word 0), per the ISA "bits X:Y of the preferred slot":
  RB29:31 = b[0] & 0x07     (low 3 bits)
  RB27:31 = b[0] & 0x1F     (low 5 bits)
  RB28:31 = b[0] & 0x0F     (low 4 bits)
  RB24:28 = (b[0] >> 3) & 0x1F   (bits 24..28, MSB=bit0 numbering => >>3 & 0x1F)
"""

from spu_refs_api import register, to_b, from_b, to_q, from_q, M32, Q_MASK

# --- big-endian bit view of the whole quadword (bit 0 = MSB) ----------------
# The ISA's bit index b: RA_0 is the most-significant bit of the 128-bit value.
def to_bits(v):
    q = to_q(v)
    return [(q >> (127 - b)) & 1 for b in range(128)]   # bit 0 first (MSB)

def from_bits(bits):
    q = 0
    for b in range(128):
        q = (q << 1) | (bits[b] & 1)
    return from_q(q & Q_MASK)


# ===========================================================================
# BIT shifts/rotates of the whole quadword (ISA p122, p123, p134, p135,
# p143, p144). RTL loop is over b = 0..127; output bit b sources input bit
# b+s (SHL/ROT) or b-s (ROTQM). Bit 0 = MSByte-MSBit = to_bits index 0.
# ===========================================================================

# shlqbi  (Shift Left Quadword by Bits; ISA p122)
#   s <- RB29:31; for b=0..127: if b+s<128: r_b <- RA_{b+s} else r_b <- 0
def ref_shlqbi(a, b):
    s = b[0] & 0x07
    ra = to_bits(a)
    r = [ra[i + s] if i + s < 128 else 0 for i in range(128)]
    return from_bits(r)

# shlqbii (Shift Left Quadword by Bits Immediate; ISA p123)  s <- I7 & 0x07
def ref_shlqbii(a, i7):
    s = i7 & 0x07
    ra = to_bits(a)
    r = [ra[i + s] if i + s < 128 else 0 for i in range(128)]
    return from_bits(r)

# rotqbi  (Rotate Quadword by Bits; ISA p134)
#   s <- RB29:31; r_b <- RA_{(b+s) mod 128}
def ref_rotqbi(a, b):
    s = b[0] & 0x07
    ra = to_bits(a)
    r = [ra[(i + s) % 128] for i in range(128)]
    return from_bits(r)

# rotqbii (Rotate Quadword by Bits Immediate; ISA p135)
#   prose: "bits 15 to 17 of the I7 field" == low 3 bits == I7 & 0x07
def ref_rotqbii(a, i7):
    s = i7 & 0x07
    ra = to_bits(a)
    r = [ra[(i + s) % 128] for i in range(128)]
    return from_bits(r)

# rotqmbi (Rotate and Mask Quadword by Bits; ISA p143) == logical shift right
#   s <- (0 - RB29:31) & 0x07; for b=0..127: if b>=s: r_b <- RA_{b-s} else 0
def ref_rotqmbi(a, b):
    s = (0 - (b[0] & 0x07)) & 0x07
    ra = to_bits(a)
    r = [ra[i - s] if i >= s else 0 for i in range(128)]
    return from_bits(r)

# rotqmbii (Rotate and Mask Quadword by Bits Immediate; ISA p144)
#   s <- (0 - I7) & 0x07
def ref_rotqmbii(a, i7):
    s = (0 - i7) & 0x07
    ra = to_bits(a)
    r = [ra[i - s] if i >= s else 0 for i in range(128)]
    return from_bits(r)


# ===========================================================================
# BYTE shifts/rotates of the whole quadword (ISA p124, p125, p126, p131,
# p132, p133, p140, p141, p142). RTL loop is over b = 0..15; byte index b,
# byte 0 = MSByte = to_b index 0.
# ===========================================================================

# shlqby  (Shift Left Quadword by Bytes; ISA p124)
#   s <- RB27:31 (0..31); for b=0..15: if b+s<16: r_b <- RA_{b+s} else 0
def ref_shlqby(a, b):
    s = b[0] & 0x1F
    ra = to_b(a)
    r = [ra[i + s] if i + s < 16 else 0 for i in range(16)]
    return from_b(r)

# shlqbyi (Shift Left Quadword by Bytes Immediate; ISA p125)  s <- I7 & 0x1F
def ref_shlqbyi(a, i7):
    s = i7 & 0x1F
    ra = to_b(a)
    r = [ra[i + s] if i + s < 16 else 0 for i in range(16)]
    return from_b(r)

# shlqbybi (Shift Left Quadword by Bytes from Bit Shift Count; ISA p126)
#   s <- RB24:28 (bits 24..28 of preferred slot) = (b[0] >> 3) & 0x1F
def ref_shlqbybi(a, b):
    s = (b[0] >> 3) & 0x1F
    ra = to_b(a)
    r = [ra[i + s] if i + s < 16 else 0 for i in range(16)]
    return from_b(r)

# rotqby  (Rotate Quadword by Bytes; ISA p131)
#   s <- RB28:31 (rightmost 4 bits); r_b <- RA_{(b+s) mod 16}
def ref_rotqby(a, b):
    s = b[0] & 0x0F
    ra = to_b(a)
    r = [ra[(i + s) % 16] for i in range(16)]
    return from_b(r)

# rotqbyi (Rotate Quadword by Bytes Immediate; ISA p132)
#   s <- I7_14:17 == the low 4 bits of the 7-bit field == I7 & 0x0F
def ref_rotqbyi(a, i7):
    s = i7 & 0x0F
    ra = to_b(a)
    r = [ra[(i + s) % 16] for i in range(16)]
    return from_b(r)

# rotqbybi (Rotate Quadword by Bytes from Bit Shift Count; ISA p133)
#   s <- RB24:28 (bits 24..28 of preferred slot) = (b[0] >> 3) & 0x1F.
#   Rotate is mod 16, so the top bit of the 5-bit field is naturally absorbed.
def ref_rotqbybi(a, b):
    s = (b[0] >> 3) & 0x1F
    ra = to_b(a)
    r = [ra[(i + s) % 16] for i in range(16)]
    return from_b(r)

# rotqmby  (Rotate and Mask Quadword by Bytes; ISA p140) == logical shift right
#   s <- (0 - RB27:31) & 0x1F; for b=0..15: if b>=s: r_b <- RA_{b-s} else 0
def ref_rotqmby(a, b):
    s = (0 - (b[0] & 0x1F)) & 0x1F
    ra = to_b(a)
    r = [ra[i - s] if i >= s else 0 for i in range(16)]
    return from_b(r)

# rotqmbyi (Rotate and Mask Quadword by Bytes Immediate; ISA p141)
#   s <- (0 - I7) & 0x1F
def ref_rotqmbyi(a, i7):
    s = (0 - i7) & 0x1F
    ra = to_b(a)
    r = [ra[i - s] if i >= s else 0 for i in range(16)]
    return from_b(r)

# rotqmbybi (Rotate and Mask Quadword Bytes from Bit Shift Count; ISA p142)
#   s <- (0 - RB24:28) & 0x1F; RB24:28 = (b[0] >> 3) & 0x1F
def ref_rotqmbybi(a, b):
    s = (0 - ((b[0] >> 3) & 0x1F)) & 0x1F
    ra = to_b(a)
    r = [ra[i - s] if i >= s else 0 for i in range(16)]
    return from_b(r)


# ===========================================================================
# Registration. Opcodes from spu_disasm.py (RR/RI7 op11 tables):
#   shlqbi 0x1DB  rotqbi 0x1D8  rotqmbi 0x1D9
#   shlqby 0x1DF  rotqby 0x1DC  rotqmby 0x1DD
#   shlqbybi 0x1CF rotqbybi 0x1CC rotqmbybi 0x1CD
#   shlqbii 0x1FB rotqbii 0x1F8 rotqmbii 0x1F9
#   shlqbyi 0x1FF rotqbyi 0x1FC rotqmbyi 0x1FD
# All rr forms: ref(a, b), count from RB word 0. All ri7 forms: ref(a, i7).
# subtle=True: this family is exactly the byte-order/quadword bug zone.
# ===========================================================================

# --- register-count (RR) forms ---
register("shlqbi",    "rr",  ref_shlqbi,    op11=0x1DB, subtle=True)
register("rotqbi",    "rr",  ref_rotqbi,    op11=0x1D8, subtle=True)
register("rotqmbi",   "rr",  ref_rotqmbi,   op11=0x1D9, subtle=True)
register("shlqby",    "rr",  ref_shlqby,    op11=0x1DF, subtle=True)
register("rotqby",    "rr",  ref_rotqby,    op11=0x1DC, subtle=True)
register("rotqmby",   "rr",  ref_rotqmby,   op11=0x1DD, subtle=True)
register("shlqbybi",  "rr",  ref_shlqbybi,  op11=0x1CF, subtle=True)
register("rotqbybi",  "rr",  ref_rotqbybi,  op11=0x1CC, subtle=True)
register("rotqmbybi", "rr",  ref_rotqmbybi, op11=0x1CD, subtle=True)

# --- immediate (RI7) forms ---
register("shlqbii",   "ri7", ref_shlqbii,   op11=0x1FB, subtle=True)
register("rotqbii",   "ri7", ref_rotqbii,   op11=0x1F8, subtle=True)
register("rotqmbii",  "ri7", ref_rotqmbii,  op11=0x1F9, subtle=True)
register("shlqbyi",   "ri7", ref_shlqbyi,   op11=0x1FF, subtle=True)
register("rotqbyi",   "ri7", ref_rotqbyi,   op11=0x1FC, subtle=True)
register("rotqmbyi",  "ri7", ref_rotqmbyi,  op11=0x1FD, subtle=True)
