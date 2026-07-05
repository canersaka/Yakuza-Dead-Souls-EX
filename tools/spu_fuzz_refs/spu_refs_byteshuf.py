#!/usr/bin/env python3
"""SPU fuzz reference plug-in -- family 'byteshuf' (byte-order / quadword SUBTLE).

Mnemonics: shufb, cbd, cbx, chd, chx, cwd, cwx, cdd, cdx.

All transcribed CLEAN-ROOM from SPU_ISA_v1.2_27Jan2007_pub.pdf (the RTL blocks,
not the prose), cross-checked against SPU_Assembly_Language_Spec_1.5.pdf
(encoding/immediate forms) and the ISA's own Appendix B mask tables (p265-266):

  * shufb              -- Shuffle Bytes,                      ISA p116 / Table 5-1
  * cbd / cbx          -- Generate Controls for Byte Insertion (d/x-form), p40/41
  * chd / chx          -- Generate Controls for Halfword Insertion,        p42/43
  * cwd / cwx          -- Generate Controls for Word Insertion,            p44/45
  * cdd / cdx          -- Generate Controls for Doubleword Insertion,      p46/47

BYTE ORDER: to_b()/from_b() give the true SPU big-endian byte view (byte 0 =
the quadword's most-significant byte = bits 24..31 of word 0). Every ref below
computes on that view so the ISA byte numbering (0..15, 0..31) is 1:1 with the
list index -- this is the whole point of the family and where the bug class lives.

--- Generate-controls RTL (identical shape for all eight) --------------------
The eight instructions build a shufb CONTROL (selector) quadword. Base value:
    RT <- 0x10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F     (byte t == 0x10+t)
i.e. selector byte t addresses concat byte 0x10+t == RB byte (0x10+t)-16 == the
same byte position of the shufb RB operand -> "pass RB through unchanged".

Then a 4-bit address t is computed and the sub-lane at position t is overwritten
with selectors 0x00..0x07 (which address concat bytes 0..7 == the LEFTMOST bytes
of the shufb RA operand -- its preferred scalar):

    cbd/cbx (byte):    t <- (RA0:3 + <I7|RB0:3>) & 0x0000000F ;  RTt      <- 0x03
    chd/chx (half):    t <- (RA0:3 + <I7|RB0:3>) & 0x0000000E ;  RTt::2   <- 0x0203
    cwd/cwx (word):    t <- (RA0:3 + <I7|RB0:3>) & 0x0000000C ;  RTt::4   <- 0x00010203
    cdd/cdx (dword):   t <- (RA0:3 + <I7|RB0:3>) & 0x00000008 ;  RTt::8   <- 0x0001020304050607

RA0:3 == word 0 (the preferred slot's full 32 bits). The mask (0xF/0xE/0xC/0x8)
IS the "force the low bit(s) to zero for alignment" the prose describes.

I7 SIGNEDNESS: the ISA writes RepLeftBit(I7,32) (sign-extend the 7-bit field),
but the assembler/disassembler treat cbd/chd/cwd/cdd's I7 as UNSIGNED u7 (0..127)
per SPU_Assembly_Language_Spec_1.5 Table 2-6, and the harness supplies i7 as
(imm & 0x7F). This is provably immaterial here: sign-extension changes the value
by exactly -128 when bit 6 is set, and 128 is a multiple of 16 (>= every mask
0x8/0xC/0xE/0xF), so (w0 + signext(i7)) & mask == (w0 + (i7 & 0x7F)) & mask. The
ref masks the field to the field width and does NOT re-sign-extend, matching the
u7 field the harness hands it -- identical result either way.
"""

from spu_refs_api import register, to_b, from_b, M32


# --- shufb  (Shuffle Bytes; ISA p116, Table 5-1) ----------------------------
# Rconcat = RA || RB (32 bytes, big-endian byte 0 first). For each of 16
# selector bytes b = RCj:  b0:1==0b10 -> 0x00 ; b0:2==0b110 -> 0xFF ;
# b0:2==0b111 -> 0x80 ; otherwise concat[b & 0x1F].
def ref_shufb(a, b, c):
    cat = to_b(a) + to_b(b)
    out = []
    for s in to_b(c):
        if (s & 0xC0) == 0x80:
            out.append(0x00)
        elif (s & 0xE0) == 0xC0:
            out.append(0xFF)
        elif (s & 0xE0) == 0xE0:
            out.append(0x80)
        else:
            out.append(cat[s & 0x1F])
    return from_b(out)

register("shufb", "rrr", ref_shufb, op4=0xB, subtle=True)


# --- generate-controls builders ---------------------------------------------
# Base selector: byte t = 0x10 + t (16 bytes, big-endian).
def _base_mask():
    return [0x10 + t for t in range(16)]


def _cb(t):                       # byte insertion: 1 byte 0x03 at position t
    m = _base_mask()
    m[t] = 0x03
    return from_b(m)


def _ch(t):                       # halfword insertion: 0x02,0x03 at t..t+1
    m = _base_mask()
    m[t] = 0x02
    m[t + 1] = 0x03
    return from_b(m)


def _cw(t):                       # word insertion: 0x00..0x03 at t..t+3
    m = _base_mask()
    for k in range(4):
        m[t + k] = k
    return from_b(m)


def _cd(t):                       # doubleword insertion: 0x00..0x07 at t..t+7
    m = _base_mask()
    for k in range(8):
        m[t + k] = k
    return from_b(m)


# I7 field arrives as an unsigned u7 (0..127); mask to the field width. The
# add + final mask makes any sign choice identical (see module docstring).
def _i7(v):
    return v & 0x7F


# ---- d-form: t = (word0(RA) + I7) & mask -----------------------------------
def ref_cbd(a, i7):  return _cb(((a[0] + _i7(i7)) & M32) & 0x0000000F)
def ref_chd(a, i7):  return _ch(((a[0] + _i7(i7)) & M32) & 0x0000000E)
def ref_cwd(a, i7):  return _cw(((a[0] + _i7(i7)) & M32) & 0x0000000C)
def ref_cdd(a, i7):  return _cd(((a[0] + _i7(i7)) & M32) & 0x00000008)

register("cbd", "ri7", ref_cbd, op11=0x1F4, subtle=True)
register("chd", "ri7", ref_chd, op11=0x1F5, subtle=True)
register("cwd", "ri7", ref_cwd, op11=0x1F6, subtle=True)
register("cdd", "ri7", ref_cdd, op11=0x1F7, subtle=True)


# ---- x-form: t = (word0(RA) + word0(RB)) & mask ----------------------------
def ref_cbx(a, b):  return _cb(((a[0] + b[0]) & M32) & 0x0000000F)
def ref_chx(a, b):  return _ch(((a[0] + b[0]) & M32) & 0x0000000E)
def ref_cwx(a, b):  return _cw(((a[0] + b[0]) & M32) & 0x0000000C)
def ref_cdx(a, b):  return _cd(((a[0] + b[0]) & M32) & 0x00000008)

register("cbx", "rr", ref_cbx, op11=0x1D4, subtle=True)
register("chx", "rr", ref_chx, op11=0x1D5, subtle=True)
register("cwx", "rr", ref_cwx, op11=0x1D6, subtle=True)
register("cdx", "rr", ref_cdx, op11=0x1D7, subtle=True)
