#!/usr/bin/env python3
"""SPU fuzz reference plug-in -- group 'elemshift'.

Per-element (word / halfword) shift and rotate instructions:

    shl   shlh   shli   shlhi        (shift left, zero-fill)
    rot   roth   roti   rothi        (rotate left, wrap-around)
    rotm  rothm  rotmi  rothmi       (rotate & mask  = LOGICAL right shift)
    rotma rotmah rotmai rotmahi      (rotate & mask algebraic = ARITHMETIC right shift)

CLEAN-ROOM: every ref below is transcribed from SPU_ISA_v1.2_27Jan2007_pub.pdf
(pages 118-121 shift-left, 127-130 rotate, 136-139 rotate&mask logical,
145-148 rotate&mask algebraic) -- the per-instruction "For each slot" prose +
the RTL count/mask expressions -- and from SPU_Assembly_Language_Spec_1.5.pdf
Table 2-2 (encodings) / Table 2-6 (immediate forms). No lifter, helper, ctx, or
RPCS3 code is referenced; this is the independent oracle the lifted C is checked
against.

SUBTLE (marked subtle=True): this family is the classic SPU byte-order / element
trap. All halfword variants operate on the EIGHT big-endian halfword lanes given
by to_h() (lane 0 = bits 0..15 of word 0 = the quadword's most-significant
halfword); all word variants on the FOUR words [w0,w1,w2,w3]. Each lane has its
OWN independent count taken from the CORRESPONDING lane of RB (register forms) or
the shared I7 immediate (immediate forms).

COUNT / MASK / WIDTH per the ISA RTL (this is where the bugs hide):

  op       lane  count expression                        result rule
  ------   ----  ---------------------------------       ------------------------
  shl      word  s = RB_word   & 0x3F  (mod 64)          s>31 -> 0, else RA<<s
  shlh     half  s = RB_half   & 0x1F  (mod 32)          s>15 -> 0, else RA<<s
  shli     word  s = sext(I7)  & 0x3F                     s>31 -> 0, else RA<<s
  shlhi    half  s = sext(I7)  & 0x1F                     s>15 -> 0, else RA<<s
  rot      word  s = RB_word   & 0x1F  (mod 32)          rotl32(RA, s)
  roth     half  s = RB_half   & 0x0F  (mod 16)          rotl16(RA, s)
  roti     word  s = sext(I7)  & 0x1F                     rotl32(RA, s)
  rothi    half  s = sext(I7)  & 0x0F                     rotl16(RA, s)
  rotm     word  s = (0-RB_word) & 0x3F (mod 64)         s<32 -> RA>>s(logical) else 0
  rothm    half  s = (0-RB_half) & 0x1F (mod 32)         s<16 -> RA>>s(logical) else 0
  rotmi    word  s = (0-sext(I7)) & 0x3F                  s<32 -> RA>>s(logical) else 0
  rothmi   half  s = (0-sext(I7)) & 0x1F                  s<16 -> RA>>s(logical) else 0
  rotma    word  s = (0-RB_word) & 0x3F (mod 64)         s<32 -> RA>>s(arith)   else sign-fill
  rotmah   half  s = (0-RB_half) & 0x1F (mod 32)         s<16 -> RA>>s(arith)   else sign-fill
  rotmai   word  s = (0-sext(I7)) & 0x3F                  s<32 -> RA>>s(arith)   else sign-fill
  rotmahi  half  s = (0-sext(I7)) & 0x1F                  s<16 -> RA>>s(arith)   else sign-fill

NOTE the two deliberate asymmetries the ISA specifies, both preserved below:
  * rotm/rotma modulo 64 (mask 0x3F) but rothm/rotmah modulo 32 (mask 0x1F) --
    a halfword rotate&mask masks the count with 0x1F (5 bits), NOT 0x0F.
  * the "otherwise" case: logical (rotm/rothm) yields ZERO; algebraic
    (rotma/rotmah) yields all-bits = the lane's original sign bit.

IMMEDIATE convention: the harness passes the RI7 field as a RAW UNSIGNED 7-bit
int in [0,127] (build_fuzz_cases does `i7 & 0x7F` then ref(a, i7)). The ISA RTL
uses RepLeftBit(I7, .) (sign-extend the 7-bit field) before the & mask, so each
ref reconstructs the signed field with sext(i7, 7) exactly as the RTL does. (For
these masks the sign-extension is value-equivalent to masking the raw field --
the sext delta is a multiple of 128, and 128 is a multiple of every mask 0xF..
0x3F -- but the ref performs the faithful RTL step regardless.)
"""

from spu_refs_api import register, to_h, from_h, sext, M32

M16 = 0xFFFF


# ---------------------------------------------------------------------------
# per-lane scalar kernels (width w = 32 for words, 16 for halfwords)
# ---------------------------------------------------------------------------

def _rotl(v, cnt, width):
    """Rotate the `width`-bit value v left by cnt bits (cnt already reduced to
    the field's modulo, so 0 <= cnt < width). RTL: bits out of the left end
    re-enter at the right (page 127/129)."""
    mask = (1 << width) - 1
    v &= mask
    cnt %= width
    if cnt == 0:
        return v
    return ((v << cnt) | (v >> (width - cnt))) & mask


def _shl_lane(v, s, width):
    """Shift left, zero fill; count s already masked to the field width's field
    (0..63 for word, 0..31 for half). ISA: count > (width-1) -> 0 (pages 118/120)."""
    if s > width - 1:
        return 0
    return (v << s) & ((1 << width) - 1)


def _lsr_lane(v, s, width):
    """Rotate-and-mask LOGICAL: s = the (0-count) two's-complement shift count
    already masked. s < width -> v >> s with zero fill; else 0 (pages 136/138)."""
    v &= (1 << width) - 1
    if s < width:
        return v >> s
    return 0


def _asr_lane(v, s, width):
    """Rotate-and-mask ALGEBRAIC: s < width -> arithmetic right shift (replicate
    the lane's bit 0 / sign at the left); else all bits = the lane sign
    (pages 145/147)."""
    mask = (1 << width) - 1
    v &= mask
    sign = (v >> (width - 1)) & 1
    signfill = mask if sign else 0
    if s < width:
        # arithmetic >> s: sign-extend v to a signed Python int, shift (Python
        # >> is floor/arithmetic), re-mask to width -> `s` sign copies enter left.
        sv = v - (1 << width) if sign else v
        return (sv >> s) & mask
    return signfill


# ---------------------------------------------------------------------------
# word (4-lane) refs
# ---------------------------------------------------------------------------

def ref_shl(a, b):
    return [_shl_lane(a[i], b[i] & 0x3F, 32) for i in range(4)]

def ref_shli(a, i7):
    s = sext(i7, 7) & 0x3F
    return [_shl_lane(w, s, 32) for w in a]

def ref_rot(a, b):
    return [_rotl(a[i], b[i] & 0x1F, 32) for i in range(4)]

def ref_roti(a, i7):
    s = sext(i7, 7) & 0x1F
    return [_rotl(w, s, 32) for w in a]

def ref_rotm(a, b):
    return [_lsr_lane(a[i], (0 - b[i]) & 0x3F, 32) for i in range(4)]

def ref_rotmi(a, i7):
    s = (0 - sext(i7, 7)) & 0x3F
    return [_lsr_lane(w, s, 32) for w in a]

def ref_rotma(a, b):
    return [_asr_lane(a[i], (0 - b[i]) & 0x3F, 32) for i in range(4)]

def ref_rotmai(a, i7):
    s = (0 - sext(i7, 7)) & 0x3F
    return [_asr_lane(w, s, 32) for w in a]


# ---------------------------------------------------------------------------
# halfword (8-lane) refs -- operate on to_h() big-endian halfword lanes
# ---------------------------------------------------------------------------

def ref_shlh(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([_shl_lane(ah[j], bh[j] & 0x1F, 16) for j in range(8)])

def ref_shlhi(a, i7):
    s = sext(i7, 7) & 0x1F
    return from_h([_shl_lane(h, s, 16) for h in to_h(a)])

def ref_roth(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([_rotl(ah[j], bh[j] & 0x0F, 16) for j in range(8)])

def ref_rothi(a, i7):
    s = sext(i7, 7) & 0x0F
    return from_h([_rotl(h, s, 16) for h in to_h(a)])

def ref_rothm(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([_lsr_lane(ah[j], (0 - bh[j]) & 0x1F, 16) for j in range(8)])

def ref_rothmi(a, i7):
    s = (0 - sext(i7, 7)) & 0x1F
    return from_h([_lsr_lane(h, s, 16) for h in to_h(a)])

def ref_rotmah(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([_asr_lane(ah[j], (0 - bh[j]) & 0x1F, 16) for j in range(8)])

def ref_rotmahi(a, i7):
    s = (0 - sext(i7, 7)) & 0x1F
    return from_h([_asr_lane(h, s, 16) for h in to_h(a)])


# ---------------------------------------------------------------------------
# registration (opcodes from spu_disasm.py SPU_RR + ri7_table, cross-checked
# against SPU_Assembly_Language_Spec_1.5.pdf Table 2-2 / 2-6)
# ---------------------------------------------------------------------------

# register (RR) forms -- op11
register("shl",    "rr", ref_shl,    op11=0x05B, subtle=True)
register("shlh",   "rr", ref_shlh,   op11=0x05F, subtle=True)
register("rot",    "rr", ref_rot,    op11=0x058, subtle=True)
register("roth",   "rr", ref_roth,   op11=0x05C, subtle=True)
register("rotm",   "rr", ref_rotm,   op11=0x059, subtle=True)
register("rothm",  "rr", ref_rothm,  op11=0x05D, subtle=True)
register("rotma",  "rr", ref_rotma,  op11=0x05A, subtle=True)
register("rotmah", "rr", ref_rotmah, op11=0x05E, subtle=True)

# immediate (RI7) forms -- op11 (the RI7 opcode is the full 11-bit field)
register("shli",    "ri7", ref_shli,    op11=0x07B, subtle=True,
         imm_pool=[0, 1, 31, 32, 33, 63, 64, 0x40, 0x7F])
register("shlhi",   "ri7", ref_shlhi,   op11=0x07F, subtle=True,
         imm_pool=[0, 1, 15, 16, 17, 31, 32, 0x40, 0x7F])
register("roti",    "ri7", ref_roti,    op11=0x078, subtle=True,
         imm_pool=[0, 1, 31, 32, 63, 0x40, 0x7F])
register("rothi",   "ri7", ref_rothi,   op11=0x07C, subtle=True,
         imm_pool=[0, 1, 15, 16, 17, 0x40, 0x7F])
register("rotmi",   "ri7", ref_rotmi,   op11=0x079, subtle=True,
         imm_pool=[0, 1, 31, 32, 33, 63, 64, 0x40, 0x7F])
register("rothmi",  "ri7", ref_rothmi,  op11=0x07D, subtle=True,
         imm_pool=[0, 1, 15, 16, 17, 31, 32, 0x40, 0x7F])
register("rotmai",  "ri7", ref_rotmai,  op11=0x07A, subtle=True,
         imm_pool=[0, 1, 31, 32, 33, 63, 64, 0x40, 0x7F])
register("rotmahi", "ri7", ref_rotmahi, op11=0x07E, subtle=True,
         imm_pool=[0, 1, 15, 16, 17, 31, 32, 0x40, 0x7F])
