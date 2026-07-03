#!/usr/bin/env python3
"""SPU lifter conformance suite -- the SPU analogue of test_ppu_lift.py.

Tests the lifter's ACTUAL EMISSIONS end-to-end: every case encodes a real SPU
instruction word, round-trips it through spu_disasm.spu_decode (so an encoding
mistake here reports as ENCODING, never as a false lifter failure), lays all
cases out as one raw code blob (one case per 16-byte slot, one lifted function
per case via explicit --functions-style bounds), lifts the blob with the real
SPULifter, and compiles the emitted C together with a generated driver that
seeds spu_context registers with SPU-word-order inputs and compares all four
result words against expectations computed here in Python straight from the
SPU ISA v1.2 RTL (page cites per family below and in scratch/specaudit_spu.md).

Register model contract being tested: ctx->gpr[r]._u32[i] == SPU word i's
VALUE (word 0 = preferred slot); byte-position ops must land values per the
SPU_W() mapping in runtime/spu/spu_helpers.h. Expectations are computed on the
true big-endian architectural quadword, so any lane/byte-order slip in a
helper or in the lifter's operand wiring shows up as a word mismatch.

Coverage (by this project's measured bug-history priority):
  extend       xsbh/xshw/xswd            (ISA p94-96; xswd = KNOWN-RED F2)
  qshift-*     shlqbi(i)/rotqbi(i)/rotqmbi(i), shlqby(i)/rotqby(i)/rotqmby(i),
               shlqbybi/rotqbybi/rotqmbybi   (p122-144; 4-bit vs 5-bit counts)
  elem-shift   shl(h)(i)/rot(h)(i)/rotm(a)(i)/rothm(i)/rotmah(i)
               (p117-148; register-form rotmah = KNOWN-RED F1, lifter TODO)
  shuffle      shufb/selb (RRR operand order; p115-116)
  genctl       cbd/chd/cwd/cdd/cbx/chx/cwx/cdx (p108-114)
  fsm-gb       fsm(h)(b)/fsmbi, gb(h)(b), orx  (p85-93)
  imm-load     il/ilh/ilhu/ila/iohl/fsmbi (negative-immediate class -- the il
               double-sign-extension regression lock)
  imm-alu      ai/ahi/sfi/sfhi, and/or/xor x word/half/byte immediates
               (orbi/xorhi/xorbi = KNOWN-RED F14 decode gaps), mpyi/mpyui
  carry        cg/bg/cgx/bgx/addx/sfx (rt-as-carry-in emission order)
  int-basic    a/ah/sf/sfh, clz/cntb/sumb/avgb/absdb
  mpy          mpy/mpyu/mpyh/mpys/mpyhh/mpyhhu/mpyhha/mpyhhau/mpya
  compare      ceq/cgt/clgt x word/half/byte, register + immediate forms

Deliberately excluded (runtime state, not lifter data semantics -- belongs in
runtime/spu/tests/ C-level tests per the specaudit design):
  channels/rdch/wrch/rchcnt, MFC DMA + tag model, the decrementer (F9/F21),
  the event facility (F10), stop/branch/link control flow (F18/F19), FP
  (fa/fm/fma..., FPSCR unmodelled, no measured bug class yet).

Red-then-green expectation as of 2026-07-03 (proof the suite bites):
  xswd            FAILS (F2: helper sources words 0/2 + swaps value/sign)
  rotmah (reg)    FAILS (F1: mnemonic unmapped in the lifter -> silent no-op)
  orbi/xorhi/xorbi FAIL (F14: RI10 decode entries missing -> .word -> no-op)
Everything else green = the finding. Do NOT fix the lifter from here; report
per the verify-then-implement protocol.

Usage:
    py -3 tools\\test_spu_lift.py             # generate + lift + compile + run
    py -3 tools\\test_spu_lift.py --emit      # stop after writing C sources
    py -3 tools\\test_spu_lift.py --verbose   # also print each PASS line
Artifacts go to scratch\\sputest\\ (blob, lifted C, driver, exe, log).
Exits nonzero on any failure. Gate rule: green (minus documented known-reds)
required before any relift that touches spu_lifter.py / spu_disasm.py.
"""

import argparse
import os
import re
import struct
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import spu_disasm                     # noqa: E402
from spu_lifter import SPULifter      # noqa: E402

OUTDIR = os.path.join(ROOT, "scratch", "sputest")

M32 = 0xFFFFFFFF
Q_MASK = (1 << 128) - 1
NOP_WORD = 0x201 << 21               # RR op11 0x201 = nop (slot padding)

RT, RA, RB, RC = 3, 4, 5, 6          # register roles (dest, srcA, srcB, ctl)
CANARY = [0xA5A5A5A5] * 4            # seeded into RT so a no-op lift FAILS

# ---------------------------------------------------------------------------
# Instruction encoders (verified against spu_disasm decode + RPCS3 SPUOpcodes.h
# field defs: rt/rc = bits 0-6, ra = 7-13, rb/i7 = 14-20, rt4 (RRR dest) = 21-27).
# ---------------------------------------------------------------------------

def enc_rr(op11, rt, ra, rb=0):
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt

def enc_ri7(op11, rt, ra, i7):
    return (op11 << 21) | ((i7 & 0x7F) << 14) | (ra << 7) | rt

def enc_ri10(op8, rt, ra, i10):
    return (op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt

def enc_ri16(op9, rt, i16):
    return (op9 << 23) | ((i16 & 0xFFFF) << 7) | rt

def enc_ri18(op7, rt, i18):
    return (op7 << 25) | ((i18 & 0x3FFFF) << 7) | rt

def enc_rrr(op4, rt, ra, rb, rc):
    # SPU ISA RRR: op(4) | RT(7, dest) | RB(7) | RA(7) | RC(7), MSB->LSB.
    return (op4 << 28) | (rt << 21) | (rb << 14) | (ra << 7) | rc

# ---------------------------------------------------------------------------
# Reference lane utilities: registers are lists of 4 u32 SPU words
# (word 0 = preferred). Halfword/byte views are in true SPU (big-endian)
# position order, so RTL transcription from the ISA is 1:1.
# ---------------------------------------------------------------------------

def sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v

def to_h(w):
    h = []
    for wd in w:
        h += [(wd >> 16) & 0xFFFF, wd & 0xFFFF]
    return h

def from_h(h):
    return [((h[2 * i] << 16) | (h[2 * i + 1] & 0xFFFF)) & M32 for i in range(4)]

def to_b(w):
    return list(struct.pack(">4I", *[x & M32 for x in w]))

def from_b(bs):
    return list(struct.unpack(">4I", bytes(bs)))

def to_q(w):
    q = 0
    for x in w:
        q = (q << 32) | (x & M32)
    return q

def from_q(q):
    return [(q >> s) & M32 for s in (96, 64, 32, 0)]

def hw_vec(*h):
    assert len(h) == 8
    return from_h([x & 0xFFFF for x in h])

def rotl32(v, n):
    v &= M32
    n &= 31
    return ((v << n) | (v >> (32 - n))) & M32 if n else v

def rotl16(v, n):
    v &= 0xFFFF
    n &= 15
    return ((v << n) | (v >> (16 - n))) & 0xFFFF if n else v

def lo16s(w):  return sext(w & 0xFFFF, 16)
def hi16s(w):  return sext((w >> 16) & 0xFFFF, 16)

# ---------------------------------------------------------------------------
# SPU ISA v1.2 reference semantics (independent of the lifter/helpers).
# All binary refs take full 4-word vectors; quadword-form counts come from
# the preferred word of RB per the ISA.
# ---------------------------------------------------------------------------

# -- extend (ISA p94-96: sign of the RIGHT sub-lane propagates left) --
def ref_xsbh(a):
    return from_h([sext(x & 0xFF, 8) & 0xFFFF for x in to_h(a)])

def ref_xshw(a):
    return [sext(w & 0xFFFF, 16) & M32 for w in a]

def ref_xswd(a):
    # p96 RTL: RT0:7 <- RepLeftBit(RA4:7,64); RT8:15 <- RepLeftBit(RA12:15,64)
    # i.e. source = words 1/3; value stays in the RIGHT word, sign fills LEFT.
    r = []
    for d in (0, 1):
        v = a[2 * d + 1] & M32
        r += [M32 if (v >> 31) else 0, v]
    return r

# -- quadword shifts/rotates (ISA p122-144) --
def bytes_shl(a, n):
    b = to_b(a)
    return from_b([b[i + n] if i + n < 16 else 0 for i in range(16)])

def bytes_rot(a, n):
    b = to_b(a)
    return from_b([b[(i + n) & 15] for i in range(16)])

def bytes_shr(a, n):
    b = to_b(a)
    return from_b([b[i - n] if i - n >= 0 else 0 for i in range(16)])

def bits_shl(a, n):  return from_q((to_q(a) << n) & Q_MASK)
def bits_rot(a, n):  q = to_q(a); return from_q(((q << n) | (q >> (128 - n))) & Q_MASK) if n else list(a)
def bits_shr(a, n):  return from_q(to_q(a) >> n)

def ref_shlqbi(a, b):   return bits_shl(a, b[0] & 7)              # RB29:31
def ref_rotqbi(a, b):   return bits_rot(a, b[0] & 7)
def ref_rotqmbi(a, b):  return bits_shr(a, (0 - b[0]) & 7)
def ref_shlqbii(a, i7): return bits_shl(a, i7 & 7)
def ref_rotqbii(a, i7): return bits_rot(a, i7 & 7)
def ref_rotqmbii(a, i7): return bits_shr(a, (0 - i7) & 7)

def _shlqby_n(a, n):    return [0] * 4 if n > 15 else bytes_shl(a, n)
def _rotqmby_n(a, n):   return [0] * 4 if n > 15 else bytes_shr(a, n)

def ref_shlqby(a, b):    return _shlqby_n(a, b[0] & 0x1F)          # RB27:31
def ref_rotqby(a, b):    return bytes_rot(a, b[0] & 0x0F)          # RB28:31
def ref_rotqmby(a, b):   return _rotqmby_n(a, (0 - b[0]) & 0x1F)
def ref_shlqbyi(a, i7):  return _shlqby_n(a, i7 & 0x1F)
def ref_rotqbyi(a, i7):  return bytes_rot(a, i7 & 0x0F)
def ref_rotqmbyi(a, i7): return _rotqmby_n(a, (0 - i7) & 0x1F)
def ref_shlqbybi(a, b):  return _shlqby_n(a, (b[0] >> 3) & 0x1F)   # RB24:28 (5-bit)
def ref_rotqbybi(a, b):  return bytes_rot(a, (b[0] >> 3) & 0x0F)   # RB25:28 (4-bit)
def ref_rotqmbybi(a, b): return _rotqmby_n(a, (0 - ((b[0] >> 3) & 0x1F)) & 0x1F)

# -- element shifts/rotates (ISA p117-121, 136-148) --
def ref_rot(a, b):   return [rotl32(a[i], b[i] & 0x1F) for i in range(4)]
def ref_shl(a, b):
    return [0 if (b[i] & 0x3F) > 31 else (a[i] << (b[i] & 0x3F)) & M32 for i in range(4)]
def ref_roth(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([rotl16(ah[j], bh[j] & 0xF) for j in range(8)])
def ref_shlh(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([0 if (bh[j] & 0x1F) > 15 else (ah[j] << (bh[j] & 0x1F)) & 0xFFFF
                   for j in range(8)])

def _rotm_w(w, c):   sh = (0 - c) & 0x3F; return 0 if sh > 31 else (w & M32) >> sh
def _rotma_w(w, c):  sh = (0 - c) & 0x3F; return (sext(w, 32) >> min(sh, 31)) & M32
def _rothm_h(h, c):  sh = (0 - c) & 0x1F; return 0 if sh > 15 else (h & 0xFFFF) >> sh
def _rotmah_h(h, c): sh = (0 - c) & 0x1F; return (sext(h, 16) >> min(sh, 15)) & 0xFFFF

def ref_rotm(a, b):    return [_rotm_w(a[i], b[i]) for i in range(4)]
def ref_rotma(a, b):   return [_rotma_w(a[i], b[i]) for i in range(4)]
def ref_rothm(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([_rothm_h(ah[j], bh[j]) for j in range(8)])
def ref_rotmah(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([_rotmah_h(ah[j], bh[j]) for j in range(8)])

def ref_roti(a, i7):    return [rotl32(w, i7 & 0x1F) for w in a]
def ref_shli(a, i7):    return [0 if (i7 & 0x3F) > 31 else (w << (i7 & 0x3F)) & M32 for w in a]
def ref_rothi(a, i7):   return from_h([rotl16(h, i7 & 0xF) for h in to_h(a)])
def ref_shlhi(a, i7):
    return from_h([0 if (i7 & 0x1F) > 15 else (h << (i7 & 0x1F)) & 0xFFFF for h in to_h(a)])
def ref_rotmi(a, i7):   return [_rotm_w(w, i7) for w in a]
def ref_rotmai(a, i7):  return [_rotma_w(w, i7) for w in a]
def ref_rothmi(a, i7):  return from_h([_rothm_h(h, i7) for h in to_h(a)])
def ref_rotmahi(a, i7): return from_h([_rotmah_h(h, i7) for h in to_h(a)])

# -- shuffle/select (ISA p115-116) --
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

def ref_selb(a, b, c):
    return [((a[i] & ~c[i]) | (b[i] & c[i])) & M32 for i in range(4)]

# -- generate controls for insertion (ISA p108-114) --
def _gen_ctl(kind, t):
    base = [0x10 + i for i in range(16)]
    t &= 0xF
    if kind == "b":
        base[t] = 0x03
    elif kind == "h":
        p = t & ~1
        base[p], base[p + 1] = 0x02, 0x03
    elif kind == "w":
        p = t & ~3
        base[p:p + 4] = [0, 1, 2, 3]
    else:
        p = t & ~7
        base[p:p + 8] = list(range(8))
    return from_b(base)

def ref_cbd(a, i7): return _gen_ctl("b", a[0] + i7)
def ref_chd(a, i7): return _gen_ctl("h", a[0] + i7)
def ref_cwd(a, i7): return _gen_ctl("w", a[0] + i7)
def ref_cdd(a, i7): return _gen_ctl("d", a[0] + i7)
def ref_cbx(a, b):  return _gen_ctl("b", a[0] + b[0])
def ref_chx(a, b):  return _gen_ctl("h", a[0] + b[0])
def ref_cwx(a, b):  return _gen_ctl("w", a[0] + b[0])
def ref_cdx(a, b):  return _gen_ctl("d", a[0] + b[0])

# -- form select mask / gather / or-across (ISA p85-93) --
def ref_fsm(a):
    v = a[0] & 0xF
    return [M32 if (v >> (3 - i)) & 1 else 0 for i in range(4)]
def ref_fsmh(a):
    v = a[0] & 0xFF
    return from_h([0xFFFF if (v >> (7 - j)) & 1 else 0 for j in range(8)])
def _fsmb_bits(v):
    return from_b([0xFF if (v >> (15 - p)) & 1 else 0 for p in range(16)])
def ref_fsmb(a):     return _fsmb_bits(a[0] & 0xFFFF)
def ref_fsmbi(i16):  return _fsmb_bits(i16 & 0xFFFF)
def ref_gb(a):
    v = 0
    for i in range(4):
        v |= (a[i] & 1) << (3 - i)
    return [v, 0, 0, 0]
def ref_gbh(a):
    h = to_h(a)
    v = 0
    for j in range(8):
        v |= (h[j] & 1) << (7 - j)
    return [v, 0, 0, 0]
def ref_gbb(a):
    b = to_b(a)
    v = 0
    for p in range(16):
        v |= (b[p] & 1) << (15 - p)
    return [v, 0, 0, 0]
def ref_orx(a):
    return [(a[0] | a[1] | a[2] | a[3]) & M32, 0, 0, 0]

# -- immediate loads (ISA p31-36) --
def ref_il(i16):   return [sext(i16, 16) & M32] * 4
def ref_ilh(i16):  v = i16 & 0xFFFF; return [((v << 16) | v) & M32] * 4
def ref_ilhu(i16): return [((i16 & 0xFFFF) << 16) & M32] * 4
def ref_ila(i18):  return [i18 & 0x3FFFF] * 4
def ref_iohl(t, i16): return [(t[i] | (i16 & 0xFFFF)) & M32 for i in range(4)]

# -- add/sub + immediates (ISA p37-52) --
def ref_a(a, b):   return [(a[i] + b[i]) & M32 for i in range(4)]
def ref_sf(a, b):  return [(b[i] - a[i]) & M32 for i in range(4)]
def ref_ah(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([(ah[j] + bh[j]) & 0xFFFF for j in range(8)])
def ref_sfh(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([(bh[j] - ah[j]) & 0xFFFF for j in range(8)])
def ref_ai(a, imm):   return [(w + imm) & M32 for w in a]
def ref_sfi(a, imm):  return [(imm - w) & M32 for w in a]
def ref_ahi(a, imm):  return from_h([(h + imm) & 0xFFFF for h in to_h(a)])
def ref_sfhi(a, imm): return from_h([(imm - h) & 0xFFFF for h in to_h(a)])

def ref_andi(a, imm):  return [w & (imm & M32) for w in a]
def ref_ori(a, imm):   return [(w | (imm & M32)) & M32 for w in a]
def ref_xori(a, imm):  return [(w ^ (imm & M32)) & M32 for w in a]
def ref_andhi(a, imm): return from_h([h & (imm & 0xFFFF) for h in to_h(a)])
def ref_orhi(a, imm):  return from_h([h | (imm & 0xFFFF) for h in to_h(a)])
def ref_xorhi(a, imm): return from_h([h ^ (imm & 0xFFFF) for h in to_h(a)])
def ref_andbi(a, imm): return from_b([x & (imm & 0xFF) for x in to_b(a)])
def ref_orbi(a, imm):  return from_b([x | (imm & 0xFF) for x in to_b(a)])
def ref_xorbi(a, imm): return from_b([x ^ (imm & 0xFF) for x in to_b(a)])

# -- carry/borrow (ISA p41-48: CG/CGX/BG/BGX/ADDX/SFX RTL) --
def ref_cg(a, b):     return [((a[i] + b[i]) >> 32) & 1 for i in range(4)]
def ref_cgx(a, b, t): return [((a[i] + b[i] + (t[i] & 1)) >> 32) & 1 for i in range(4)]
def ref_bg(a, b):     return [1 if b[i] >= a[i] else 0 for i in range(4)]
def ref_bgx(a, b, t):
    return [((b[i] + ((~a[i]) & M32) + (t[i] & 1)) >> 32) & 1 for i in range(4)]
def ref_addx(a, b, t):
    return [(a[i] + b[i] + (t[i] & 1)) & M32 for i in range(4)]
def ref_sfx(a, b, t):
    return [(b[i] + ((~a[i]) & M32) + (t[i] & 1)) & M32 for i in range(4)]

# -- multiplies (ISA p53-66) --
def ref_mpy(a, b):    return [(lo16s(a[i]) * lo16s(b[i])) & M32 for i in range(4)]
def ref_mpyu(a, b):   return [((a[i] & 0xFFFF) * (b[i] & 0xFFFF)) & M32 for i in range(4)]
def ref_mpyi(a, imm): return [(lo16s(w) * imm) & M32 for w in a]
def ref_mpyui(a, imm): return [((w & 0xFFFF) * (imm & 0xFFFF)) & M32 for w in a]
def ref_mpyh(a, b):
    return [((hi16s(a[i]) * lo16s(b[i])) << 16) & M32 for i in range(4)]
def ref_mpys(a, b):
    return [sext(((lo16s(a[i]) * lo16s(b[i])) >> 16) & 0xFFFF, 16) & M32 for i in range(4)]
def ref_mpyhh(a, b):  return [(hi16s(a[i]) * hi16s(b[i])) & M32 for i in range(4)]
def ref_mpyhhu(a, b):
    return [(((a[i] >> 16) & 0xFFFF) * ((b[i] >> 16) & 0xFFFF)) & M32 for i in range(4)]
def ref_mpyhha(a, b, t):
    return [(t[i] + hi16s(a[i]) * hi16s(b[i])) & M32 for i in range(4)]
def ref_mpyhhau(a, b, t):
    return [(t[i] + ((a[i] >> 16) & 0xFFFF) * ((b[i] >> 16) & 0xFFFF)) & M32
            for i in range(4)]
def ref_mpya(a, b, c):
    return [(lo16s(a[i]) * lo16s(b[i]) + c[i]) & M32 for i in range(4)]

# -- misc integer (ISA p67-73) --
def ref_clz(a):  return [32 if w == 0 else 32 - w.bit_length() for w in a]
def ref_cntb(a): return from_b([bin(x).count("1") for x in to_b(a)])
def ref_avgb(a, b):
    return from_b([(x + y + 1) >> 1 for x, y in zip(to_b(a), to_b(b))])
def ref_absdb(a, b):
    return from_b([abs(x - y) for x, y in zip(to_b(a), to_b(b))])
def ref_sumb(a, b):
    r = []
    for i in range(4):
        sa = sum(to_b([a[i]] * 4)[:4])   # 4 bytes of word i of A
        sb = sum(to_b([b[i]] * 4)[:4])
        r.append(((sb << 16) | sa) & M32)
    return r

# -- compares (ISA p74-84) --
def _m(c, bits):  return (1 << bits) - 1 if c else 0
def ref_ceq(a, b):   return [_m(a[i] == b[i], 32) for i in range(4)]
def ref_cgt(a, b):   return [_m(sext(a[i], 32) > sext(b[i], 32), 32) for i in range(4)]
def ref_clgt(a, b):  return [_m(a[i] > b[i], 32) for i in range(4)]
def ref_ceqh(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([_m(ah[j] == bh[j], 16) for j in range(8)])
def ref_cgth(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([_m(sext(ah[j], 16) > sext(bh[j], 16), 16) for j in range(8)])
def ref_clgth(a, b):
    ah, bh = to_h(a), to_h(b)
    return from_h([_m(ah[j] > bh[j], 16) for j in range(8)])
def ref_ceqb(a, b):
    return from_b([_m(x == y, 8) for x, y in zip(to_b(a), to_b(b))])
def ref_cgtb(a, b):
    return from_b([_m(sext(x, 8) > sext(y, 8), 8) for x, y in zip(to_b(a), to_b(b))])
def ref_clgtb(a, b):
    return from_b([_m(x > y, 8) for x, y in zip(to_b(a), to_b(b))])
def ref_ceqi(a, imm):   return [_m(w == (imm & M32), 32) for w in a]
def ref_cgti(a, imm):   return [_m(sext(w, 32) > imm, 32) for w in a]
def ref_clgti(a, imm):  return [_m(w > (imm & M32), 32) for w in a]
def ref_ceqhi(a, imm):  return from_h([_m(h == (imm & 0xFFFF), 16) for h in to_h(a)])
def ref_cgthi(a, imm):  return from_h([_m(sext(h, 16) > imm, 16) for h in to_h(a)])
def ref_clgthi(a, imm): return from_h([_m(h > (imm & 0xFFFF), 16) for h in to_h(a)])
def ref_ceqbi(a, imm):  return from_b([_m(x == (imm & 0xFF), 8) for x in to_b(a)])
def ref_cgtbi(a, imm):
    return from_b([_m(sext(x, 8) > sext(imm & 0xFF, 8), 8) for x in to_b(a)])
def ref_clgtbi(a, imm): return from_b([_m(x > (imm & 0xFF), 8) for x in to_b(a)])

# ---------------------------------------------------------------------------
# Input vectors (named; distinct per-lane values, sign edges in every width)
# ---------------------------------------------------------------------------

V = {
    "VA":  [0x00010203, 0x84858687, 0xFFFFFFFF, 0x00000000],
    "VB":  [0x12345678, 0x9ABCDEF0, 0x7FFFFFFF, 0x80000000],
    "VC":  [0xDEADBEEF, 0x00FF00FF, 0x55AA55AA, 0xCAFEBABE],
    "VD":  [0x80017FFF, 0x7FFF8001, 0x00808080, 0xFF7F00FF],
    "VE":  [0x00112233, 0x44556677, 0x8899AABB, 0xCCDDEEFF],
    # word-lane count vectors (element shifts: per-lane counts)
    "CW1": [0, 1, 31, 32],
    "CW2": [33, 63, 64, 0xFFFFFFFF],
    # halfword-lane count vectors (both fill regimes + two's-complement wrap)
    "CH1": hw_vec(0, 1, 15, 16, 17, 31, 32, 0xFFFF),
    "CH2": hw_vec(2, 5, 14, 15, 3, 8, 0xFFF1, 64),
    # quadword counts (preferred word only; junk in other lanes to catch
    # a helper reading the wrong slot)
    "QC0": [0, 0xDEAD0001, 3, 0x80000005],
    "QC1": [1, 0xDEAD0001, 3, 0x80000005],
    "QC3": [3, 0xDEAD0001, 3, 0x80000005],
    "QC7": [7, 0xDEAD0001, 3, 0x80000005],
    "QCM7": [0xFFFFFFF9, 0xDEAD0001, 3, 0x80000005],
    "QB0":  [0, 0xDEAD0001, 3, 0x80000005],
    "QB1":  [1, 0xDEAD0001, 3, 0x80000005],
    "QB7":  [7, 0xDEAD0001, 3, 0x80000005],
    "QB15": [15, 0xDEAD0001, 3, 0x80000005],
    "QB16": [16, 0xDEAD0001, 3, 0x80000005],
    "QB31": [31, 0xDEAD0001, 3, 0x80000005],
    "QBM1":  [0xFFFFFFFF, 0xDEAD0001, 3, 0x80000005],
    "QBM15": [0xFFFFFFF1, 0xDEAD0001, 3, 0x80000005],
    "QBM16": [0xFFFFFFF0, 0xDEAD0001, 3, 0x80000005],
    # bybi counts: byte count = bits 24:28 of the raw value (>>3)
    "BB0":  [0x00, 0xDEAD0001, 3, 0x80000005],
    "BB8":  [0x08, 0xDEAD0001, 3, 0x80000005],
    "BB78": [0x78, 0xDEAD0001, 3, 0x80000005],
    "BB80": [0x80, 0xDEAD0001, 3, 0x80000005],
    "BB88": [0x88, 0xDEAD0001, 3, 0x80000005],   # 4-bit vs 5-bit discriminator
    "BBFF": [0xFF, 0xDEAD0001, 3, 0x80000005],
    "BBF8": [0xFFFFFFF8, 0xDEAD0001, 3, 0x80000005],
    # carry-edge vectors
    "CGA": [0xFFFFFFFF, 0x00000001, 0x80000000, 0x7FFFFFFF],
    "CGB": [0x00000001, 0xFFFFFFFF, 0x80000000, 0x00000001],
    "T0":  [0, 0, 0, 0],
    "T1":  [1, 0xFFFFFFFF, 2, 1],                # lsbs 1,1,0,1
    "BGA": [5, 5, 6, 0x00000000],
    "BGB": [6, 5, 5, 0xFFFFFFFF],
    # multiply sign-edge vectors (per-halfword)
    "MPA": [0x80017FFF, 0xFFFF0001, 0x7FFF8000, 0x00020003],
    "MPB": [0x7FFF8001, 0x00010002, 0x80008000, 0xFFFFFFFE],
    # compare vectors
    "CMA": [0x80000000, 5, 5, 0xFFFFFFFF],
    "CMB": [0x00000001, 5, 6, 0x00000000],
    "HHA": [0x80017FFF, 0x00FF0100, 0xFFFF0000, 0x7F80807F],
    "HHB": [0x7FFF8001, 0x00FF00FF, 0x00000000, 0x807F7F80],
    # preferred-word-only inputs (genctl / fsm; junk elsewhere)
    "P0":  [0x0, 0xDEAD0001, 0xDEAD0002, 0xDEAD0003],
    "P5":  [0x5, 0xDEAD0001, 0xDEAD0002, 0xDEAD0003],
    "PE":  [0xE, 0xDEAD0001, 0xDEAD0002, 0xDEAD0003],
    "PBIG": [0xFFFFFFFF, 0xDEAD0001, 0xDEAD0002, 0xDEAD0003],
    "PF":  [0xF, 0xDEAD0001, 0xDEAD0002, 0xDEAD0003],
    "PA5": [0xA5, 0xDEAD0001, 0xDEAD0002, 0xDEAD0003],
    "P8001": [0x8001, 0xDEAD0001, 0xDEAD0002, 0xDEAD0003],
    "P5A5A": [0x5A5A, 0xDEAD0001, 0xDEAD0002, 0xDEAD0003],
    "P135": [0x135, 0xDEAD0001, 0xDEAD0002, 0xDEAD0003],
    # gather vectors (LSB patterns)
    "GBA": [0x1, 0x0, 0xFFFFFFFF, 0x2],
    "GBH": hw_vec(1, 0, 0, 1, 1, 1, 0, 2),
    "GBB": from_b([1, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0, 2, 3, 4, 1]),
    # shufb controls
    "SID": [0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F],   # identity(a)
    "SRV": [0x1F1E1D1C, 0x1B1A1918, 0x17161514, 0x13121110],   # reverse(b)
    "SMX": [0x03130010, 0x80C0E09F, 0x0B1B0717, 0xFF5F4A35],   # specials + cross
    # selb masks
    "SMA": [0xFFFF0000, 0x00FF00FF, 0x00000000, 0xFFFFFFFF],
}

FAMILIES = []      # insertion-ordered family names
CASES = []


def add(family, name, word, exp_mn, exp_ops, in_regs, expect_rt, note=None):
    if family not in FAMILIES:
        FAMILIES.append(family)
    insn = spu_disasm.spu_decode(word, 0)
    enc_err = None
    if insn.mnemonic != exp_mn:
        enc_err = (f"decoded as '{insn.mnemonic} {insn.operands}' "
                   f"(wanted {exp_mn})")
    elif exp_ops is not None:
        got_ops = [o.strip() for o in insn.operands.split(",")]
        if got_ops[:len(exp_ops)] != exp_ops:
            enc_err = (f"operands '{insn.operands}' != expected "
                       f"{','.join(exp_ops)}")
    expect_rt = [w & M32 for w in expect_rt]
    canary = CANARY if expect_rt != CANARY else [0x3C3C3C3C] * 4
    CASES.append(dict(family=family, name=name, word=word, mn=exp_mn,
                      in_regs={r: [w & M32 for w in v] for r, v in in_regs.items()},
                      expect=expect_rt, canary=canary, note=note,
                      enc_err=enc_err, todo=False, addr=None))


def rr2(fam, mn, op11, ref, vecnames, note=None):
    for vn in vecnames:
        a = V[vn]
        add(fam, f"{mn} {vn}", enc_rr(op11, RT, RA), mn,
            [f"$r{RT}", f"$r{RA}"], {RA: a}, ref(a), note)


def rr3(fam, mn, op11, ref, pairs, note=None):
    for an, bn in pairs:
        a, b = V[an], V[bn]
        add(fam, f"{mn} {an},{bn}", enc_rr(op11, RT, RA, RB), mn,
            [f"$r{RT}", f"$r{RA}", f"$r{RB}"], {RA: a, RB: b}, ref(a, b), note)


def rr3t(fam, mn, op11, ref, triples, note=None):
    for an, bn, tn in triples:
        a, b, t = V[an], V[bn], V[tn]
        add(fam, f"{mn} {an},{bn},t={tn}", enc_rr(op11, RT, RA, RB), mn,
            [f"$r{RT}", f"$r{RA}", f"$r{RB}"], {RA: a, RB: b, RT: t},
            ref(a, b, t), note)


def ri7(fam, mn, op11, ref, vecname, i7s, note=None):
    a = V[vecname]
    for i7 in i7s:
        i7 &= 0x7F
        add(fam, f"{mn} {vecname},i7={i7}", enc_ri7(op11, RT, RA, i7), mn,
            [f"$r{RT}", f"$r{RA}", str(i7)], {RA: a}, ref(a, i7), note)


def ri10(fam, mn, op8, ref, vecnames, imms, note=None, gap=False):
    for vn in vecnames:
        a = V[vn]
        for imm in imms:                     # imm is the signed value -512..511
            word = enc_ri10(op8, RT, RA, imm)
            exp_mn = ".word" if gap else mn
            exp_ops = None if gap else [f"$r{RT}", f"$r{RA}", str(imm)]
            add(fam, f"{mn} {vn},imm={imm}", word, exp_mn, exp_ops,
                {RA: a}, ref(a, imm), note)


def rrr3(fam, mn, op4, ref, triples, note=None):
    for an, bn, cn in triples:
        a, b, c = V[an], V[bn], V[cn]
        add(fam, f"{mn} {an},{bn},{cn}", enc_rrr(op4, RT, RA, RB, RC), mn,
            [f"$r{RT}", f"$r{RA}", f"$r{RB}", f"$r{RC}"],
            {RA: a, RB: b, RC: c}, ref(a, b, c), note)


def build_cases():
    # ---- extend --------------------------------------------------------
    rr2("extend", "xsbh", 0x2B6, ref_xsbh, ["VA", "VB", "VD"])
    rr2("extend", "xshw", 0x2AE, ref_xshw, ["VA", "VB", "VD"])
    rr2("extend", "xswd", 0x2A6, ref_xswd, ["VA", "VB", "VD"],
        note="KNOWN-RED F2 (ISA p96): helper sources words 0/2 and swaps "
             "value/sign word placement")

    # ---- quadword shifts: bit forms -------------------------------------
    for mn, op11, ref in (("shlqbi", 0x1DB, ref_shlqbi),
                          ("rotqbi", 0x1D8, ref_rotqbi),
                          ("rotqmbi", 0x1D9, ref_rotqmbi)):
        rr3("qshift-bit", mn, op11, ref,
            [("VE", c) for c in ("QC0", "QC1", "QC3", "QC7", "QCM7")])
    for mn, op11, ref in (("shlqbii", 0x1FB, ref_shlqbii),
                          ("rotqbii", 0x1F8, ref_rotqbii),
                          ("rotqmbii", 0x1F9, ref_rotqmbii)):
        ri7("qshift-bit", mn, op11, ref, "VE", [0, 1, 4, 7, 0x79])

    # ---- quadword shifts: byte forms ------------------------------------
    rr3("qshift-byte", "shlqby", 0x1DF, ref_shlqby,
        [("VE", c) for c in ("QB0", "QB1", "QB7", "QB15", "QB16", "QB31")])
    rr3("qshift-byte", "rotqby", 0x1DC, ref_rotqby,
        [("VE", c) for c in ("QB0", "QB1", "QB7", "QB15", "QB16", "QB31")])
    rr3("qshift-byte", "rotqmby", 0x1DD, ref_rotqmby,
        [("VE", c) for c in ("QB0", "QBM1", "QBM15", "QBM16", "QB1", "QB16")])
    ri7("qshift-byte", "shlqbyi", 0x1FF, ref_shlqbyi, "VE", [0, 1, 15, 16, 31])
    ri7("qshift-byte", "rotqbyi", 0x1FC, ref_rotqbyi, "VE", [0, 1, 15, 16, 31])
    ri7("qshift-byte", "rotqmbyi", 0x1FD, ref_rotqmbyi, "VE",
        [0, 0x7F, 0x71, 0x70, 1, 16])

    # ---- quadword shifts: bybi forms (4-bit vs 5-bit field distinction) --
    rr3("qshift-bybi", "shlqbybi", 0x1CF, ref_shlqbybi,
        [("VE", c) for c in ("BB0", "BB8", "BB78", "BB80", "BB88", "BBFF")])
    rr3("qshift-bybi", "rotqbybi", 0x1CC, ref_rotqbybi,
        [("VE", c) for c in ("BB0", "BB8", "BB78", "BB80", "BB88", "BBFF")])
    rr3("qshift-bybi", "rotqmbybi", 0x1CD, ref_rotqmbybi,
        [("VE", c) for c in ("BB0", "BBF8", "BB88", "BB80", "BB8")])

    # ---- element shifts/rotates: register forms --------------------------
    for mn, op11, ref, counts in (
            ("rot", 0x058, ref_rot, ("CW1", "CW2")),
            ("shl", 0x05B, ref_shl, ("CW1", "CW2")),
            ("rotm", 0x059, ref_rotm, ("CW1", "CW2")),
            ("rotma", 0x05A, ref_rotma, ("CW1", "CW2")),
            ("roth", 0x05C, ref_roth, ("CH1", "CH2")),
            ("shlh", 0x05F, ref_shlh, ("CH1", "CH2")),
            ("rothm", 0x05D, ref_rothm, ("CH1", "CH2")),
    ):
        rr3("elem-shift", mn, op11, ref,
            [(d, c) for d in ("VD", "VB") for c in counts])
    rr3("elem-shift", "rotmah", 0x05E, ref_rotmah,
        [(d, c) for d in ("VD", "VB") for c in ("CH1", "CH2")],
        note="KNOWN-RED F1: mnemonic 'rotmah' unmapped in spu_lifter rr_bin "
             "(only 'rothma' exists) -> lifted as TODO no-op")

    # ---- element shifts/rotates: immediate forms -------------------------
    ri7("elem-shift", "roti", 0x078, ref_roti, "VD", [0, 1, 31, 0x7F])
    ri7("elem-shift", "shli", 0x07B, ref_shli, "VD", [0, 1, 31, 32, 63, 0x7F])
    ri7("elem-shift", "rothi", 0x07C, ref_rothi, "VD", [0, 1, 15, 0x7F])
    ri7("elem-shift", "shlhi", 0x07F, ref_shlhi, "VD", [0, 1, 15, 16, 0x7F])
    ri7("elem-shift", "rotmi", 0x079, ref_rotmi, "VD",
        [0, 0x7F, 0x71, 0x61, 0x60, 4])
    ri7("elem-shift", "rotmai", 0x07A, ref_rotmai, "VD",
        [0, 0x7F, 0x71, 0x61, 0x60, 4])
    ri7("elem-shift", "rothmi", 0x07D, ref_rothmi, "VD",
        [0, 0x7F, 0x79, 0x71, 0x70, 8])
    ri7("elem-shift", "rotmahi", 0x07E, ref_rotmahi, "VD",
        [0, 0x7F, 0x79, 0x71, 0x70, 8])

    # ---- shuffle / select -------------------------------------------------
    rrr3("shuffle", "shufb", 0xB, ref_shufb,
         [("VE", "VC", "SID"), ("VE", "VC", "SRV"), ("VE", "VC", "SMX")])
    rrr3("shuffle", "selb", 0x8, ref_selb,
         [("VA", "VB", "SMA"), ("VC", "VE", "VD")])

    # ---- generate controls for insertion ---------------------------------
    for mn, op11, ref in (("cbd", 0x1F4, ref_cbd), ("chd", 0x1F5, ref_chd),
                          ("cwd", 0x1F6, ref_cwd), ("cdd", 0x1F7, ref_cdd)):
        ri7("genctl", mn, op11, ref, "P0", [3])
        ri7("genctl", mn, op11, ref, "P5", [0x7D])   # negative i7 wrap
        ri7("genctl", mn, op11, ref, "PE", [5])
    for mn, op11, ref in (("cbx", 0x1D4, ref_cbx), ("chx", 0x1D5, ref_chx),
                          ("cwx", 0x1D6, ref_cwx), ("cdx", 0x1D7, ref_cdx)):
        rr3("genctl", mn, op11, ref, [("P5", "PE"), ("PBIG", "P5")])

    # ---- form-select-mask / gather / or-across ---------------------------
    rr2("fsm-gb", "fsm", 0x1B4, ref_fsm, ["P0", "PF", "PA5", "P135"])
    rr2("fsm-gb", "fsmh", 0x1B5, ref_fsmh, ["PF", "PA5", "P135"])
    rr2("fsm-gb", "fsmb", 0x1B6, ref_fsmb, ["P8001", "P5A5A", "PBIG"])
    for i16 in (0xFFFF, 0x8001, 0x0101):
        add("fsm-gb", f"fsmbi 0x{i16:X}", enc_ri16(0x065, RT, i16), "fsmbi",
            [f"$r{RT}", f"0x{i16:X}"], {}, ref_fsmbi(i16))
    rr2("fsm-gb", "gb", 0x1B0, ref_gb, ["GBA", "VD"])
    rr2("fsm-gb", "gbh", 0x1B1, ref_gbh, ["GBH", "VD"])
    rr2("fsm-gb", "gbb", 0x1B2, ref_gbb, ["GBB", "VD"])
    rr2("fsm-gb", "orx", 0x1F0, ref_orx, ["VA", "VC"])

    # ---- immediate loads --------------------------------------------------
    for i16 in (0x1234, 0xFFFC, 0x8000, 0x7FFF):    # incl. il -4 (the il bug)
        s = sext(i16, 16)
        add("imm-load", f"il {s}", enc_ri16(0x081, RT, i16), "il",
            [f"$r{RT}", str(s)], {}, ref_il(i16))
    for i16 in (0xA55A, 0x8001):
        add("imm-load", f"ilh 0x{i16:X}", enc_ri16(0x083, RT, i16), "ilh",
            [f"$r{RT}", f"0x{i16:X}"], {}, ref_ilh(i16))
        add("imm-load", f"ilhu 0x{i16:X}", enc_ri16(0x082, RT, i16), "ilhu",
            [f"$r{RT}", f"0x{i16:X}"], {}, ref_ilhu(i16))
        add("imm-load", f"iohl 0x{i16:X}", enc_ri16(0x0C1, RT, i16), "iohl",
            [f"$r{RT}", f"0x{i16:X}"], {RT: V["VC"]}, ref_iohl(V["VC"], i16))
    for i18 in (0x3FFFF, 0x12345, 0x00000):
        add("imm-load", f"ila 0x{i18:X}", enc_ri18(0x21, RT, i18), "ila",
            [f"$r{RT}", f"0x{i18:X}"], {}, ref_ila(i18))

    # ---- ALU immediates ----------------------------------------------------
    ri10("imm-alu", "ai", 0x1C, ref_ai, ["VA"], [1, -1, -512, 511])
    ri10("imm-alu", "ahi", 0x1D, ref_ahi, ["VD"], [1, -1, -512, 511])
    ri10("imm-alu", "sfi", 0x0C, ref_sfi, ["VA"], [-1, 0, 511])
    ri10("imm-alu", "sfhi", 0x0D, ref_sfhi, ["VD"], [1, -512])
    ri10("imm-alu", "andi", 0x14, ref_andi, ["VC"], [-1, 0xF3, -16])
    ri10("imm-alu", "andhi", 0x15, ref_andhi, ["VC"], [-4, 0xFF])
    ri10("imm-alu", "andbi", 0x16, ref_andbi, ["VC"], [-1, 0x55, -128])
    ri10("imm-alu", "ori", 0x04, ref_ori, ["VA"], [0x1F0, -256])
    ri10("imm-alu", "orhi", 0x05, ref_orhi, ["VA"], [0x101, -2])
    ri10("imm-alu", "orbi", 0x06, ref_orbi, ["VA"], [0x33, -1],
         note="was F14 KNOWN-RED (RI10_TABLE gap); fixed 2026-07-03")
    ri10("imm-alu", "xori", 0x44, ref_xori, ["VC"], [-1, 0x155])
    ri10("imm-alu", "xorhi", 0x45, ref_xorhi, ["VC"], [-4, 0x0F0],
         note="was F14 KNOWN-RED (RI10_TABLE gap); fixed 2026-07-03")
    ri10("imm-alu", "xorbi", 0x46, ref_xorbi, ["VC"], [0x7F, -1],
         note="was F14 KNOWN-RED (RI10_TABLE gap); fixed 2026-07-03")
    ri10("imm-alu", "mpyi", 0x74, ref_mpyi, ["MPA"], [3, -3, 511, -512])
    ri10("imm-alu", "mpyui", 0x75, ref_mpyui, ["MPA"], [-1, 5])

    # ---- carry / borrow ----------------------------------------------------
    rr3("carry", "cg", 0x0C2, ref_cg, [("CGA", "CGB"), ("VA", "VB")])
    rr3("carry", "bg", 0x042, ref_bg, [("BGA", "BGB"), ("VA", "VB")])
    rr3t("carry", "cgx", 0x342, ref_cgx,
         [("CGA", "CGB", "T0"), ("CGA", "CGB", "T1"), ("VA", "VB", "T1")])
    rr3t("carry", "bgx", 0x343, ref_bgx,
         [("BGA", "BGB", "T0"), ("BGA", "BGB", "T1"), ("VA", "VB", "T1")])
    rr3t("carry", "addx", 0x340, ref_addx,
         [("CGA", "CGB", "T0"), ("CGA", "CGB", "T1"), ("VA", "VB", "T1")])
    rr3t("carry", "sfx", 0x341, ref_sfx,
         [("BGA", "BGB", "T0"), ("BGA", "BGB", "T1"), ("VA", "VB", "T1")])

    # ---- basic integer -----------------------------------------------------
    rr3("int-basic", "a", 0x0C0, ref_a, [("VA", "VB"), ("VC", "VD")])
    rr3("int-basic", "ah", 0x0C8, ref_ah, [("VA", "VB"), ("VC", "VD")])
    rr3("int-basic", "sf", 0x040, ref_sf, [("VA", "VB"), ("VC", "VD")])
    rr3("int-basic", "sfh", 0x048, ref_sfh, [("VA", "VB"), ("VC", "VD")])
    rr2("int-basic", "clz", 0x2A5, ref_clz, ["VA", "VB", "GBA"])
    rr2("int-basic", "cntb", 0x2B4, ref_cntb, ["VA", "VC"])
    rr3("int-basic", "sumb", 0x253, ref_sumb, [("VA", "VB"), ("VC", "VD")])
    rr3("int-basic", "avgb", 0x0D3, ref_avgb, [("VA", "VB"), ("VC", "VD")])
    rr3("int-basic", "absdb", 0x053, ref_absdb, [("VA", "VB"), ("VC", "VD")])

    # ---- multiplies --------------------------------------------------------
    for mn, op11, ref in (("mpy", 0x3C4, ref_mpy), ("mpyu", 0x3CC, ref_mpyu),
                          ("mpyh", 0x3C5, ref_mpyh), ("mpys", 0x3C7, ref_mpys),
                          ("mpyhh", 0x3C6, ref_mpyhh),
                          ("mpyhhu", 0x3CE, ref_mpyhhu)):
        rr3("mpy", mn, op11, ref, [("MPA", "MPB"), ("VA", "VB")])
    rr3t("mpy", "mpyhha", 0x346, ref_mpyhha,
         [("MPA", "MPB", "VC"), ("VA", "VB", "T1")])
    rr3t("mpy", "mpyhhau", 0x34E, ref_mpyhhau,
         [("MPA", "MPB", "VC"), ("VA", "VB", "T1")])
    rrr3("mpy", "mpya", 0xC, ref_mpya, [("MPA", "MPB", "VC"), ("VA", "VB", "VE")])

    # ---- compares ----------------------------------------------------------
    for mn, op11, ref in (("ceq", 0x3C0, ref_ceq), ("cgt", 0x240, ref_cgt),
                          ("clgt", 0x2C0, ref_clgt)):
        rr3("compare", mn, op11, ref, [("CMA", "CMB"), ("CMB", "CMA")])
    for mn, op11, ref in (("ceqh", 0x3C8, ref_ceqh), ("cgth", 0x248, ref_cgth),
                          ("clgth", 0x2C8, ref_clgth)):
        rr3("compare", mn, op11, ref, [("HHA", "HHB"), ("HHB", "HHA")])
    for mn, op11, ref in (("ceqb", 0x3D0, ref_ceqb), ("cgtb", 0x250, ref_cgtb),
                          ("clgtb", 0x2D0, ref_clgtb)):
        rr3("compare", mn, op11, ref, [("HHA", "HHB"), ("HHB", "HHA")])
    ri10("compare", "ceqi", 0x7C, ref_ceqi, ["CMA"], [5, -1, 0])
    ri10("compare", "cgti", 0x4C, ref_cgti, ["CMA"], [5, -1, 0])
    ri10("compare", "clgti", 0x5C, ref_clgti, ["CMA"], [5, -1, 0])
    ri10("compare", "ceqhi", 0x7D, ref_ceqhi, ["HHA"], [0x100, -1])
    ri10("compare", "cgthi", 0x4D, ref_cgthi, ["HHA"], [0x100, -1])
    ri10("compare", "clgthi", 0x5D, ref_clgthi, ["HHA"], [0x100, -1])
    ri10("compare", "ceqbi", 0x7E, ref_ceqbi, ["HHA"], [0x7F, -1])
    ri10("compare", "cgtbi", 0x4E, ref_cgtbi, ["HHA"], [0x7F, -1])
    ri10("compare", "clgtbi", 0x5E, ref_clgtbi, ["HHA"], [0x7F, -1])


# ---------------------------------------------------------------------------
# Emission: blob -> real SPULifter -> generated C driver
# ---------------------------------------------------------------------------

def lift_and_emit(kept):
    blob = bytearray()
    for idx, c in enumerate(kept):
        c["addr"] = idx * 16
        blob += struct.pack(">I", c["word"])
        blob += struct.pack(">I", NOP_WORD) * 3          # 16-byte slot padding
    with open(os.path.join(OUTDIR, "spu_conf_blob.bin"), "wb") as f:
        f.write(bytes(blob))

    insns = spu_disasm.disassemble_spu(bytes(blob), 0)
    lifter = SPULifter(trace=False)
    lifter.header_name = "spu_recomp.h"
    lifter.register_name = "spu_conf_register"
    lifter.func_prefix = "spu_case_"
    lifter.func_starts = {c["addr"] for c in kept}
    for c in kept:
        fn = lifter.lift_function(insns, c["addr"], c["addr"] + 4)
        c["todo"] = any("TODO" in ln for ln in fn.body_lines)
    with open(os.path.join(OUTDIR, "spu_recomp.h"), "w", newline="\n") as f:
        f.write(lifter.emit_header())
    with open(os.path.join(OUTDIR, "spu_recomp.c"), "w", newline="\n") as f:
        f.write(lifter.emit_source())
    return lifter


def emit_driver(kept):
    out = ['/* Auto-generated by tools/test_spu_lift.py -- conformance driver. */',
           '#define _CRT_SECURE_NO_WARNINGS',
           '#include "spu_recomp.h"',
           '#include "spu_helpers.h"',
           '#include <stdio.h>',
           '',
           '/* Runtime stubs: the cases are pure register->register data ops,',
           ' * so channel/dispatch/trampoline machinery is inert. */',
           'SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = 0;',
           'SPU_THREAD_LOCAL spu_context* g_spu_cur_ctx = 0;',
           'int g_spu_prof_on = 0;',
           'unsigned long g_spu_wrun_log = 0;',
           'void spu_prof_hop(void* fn) { (void)fn; }',
           'void spu_task_launch_check(spu_context* ctx, void* fn) { (void)ctx; (void)fn; }',
           'void spu_halt(spu_context* ctx, int status) { ctx->status = (uint32_t)status; }',
           'void spu_indirect_branch(spu_context* ctx) { (void)ctx; }',
           'void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }',
           'u128 spu_rdch(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return spu_zero(); }',
           'uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }',
           'void spu_wrch(spu_context* ctx, uint32_t ch, u128 v) { (void)ctx; (void)ch; (void)v; }',
           'void spu_trace_init(const char* p) { (void)p; }',
           'void spu_trace_pc(spu_context* c, uint32_t pc) { (void)c; (void)pc; }',
           'void spu_trace_rt(spu_context* c, uint32_t r) { (void)c; (void)r; }',
           '',
           'static spu_context g_ctx;',
           'static int g_pass = 0, g_fail = 0;',
           '',
           'static void set4(u128* r, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {',
           '    r->_u32[0] = w0; r->_u32[1] = w1; r->_u32[2] = w2; r->_u32[3] = w3;',
           '}',
           'static void check4(const char* tag, int reg, const u128* got,',
           '                   uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {',
           '    if (got->_u32[0] != w0 || got->_u32[1] != w1 ||',
           '        got->_u32[2] != w2 || got->_u32[3] != w3) {',
           '        printf("FAIL %s: r%d got=%08X.%08X.%08X.%08X want=%08X.%08X.%08X.%08X\\n",',
           '               tag, reg, got->_u32[0], got->_u32[1], got->_u32[2], got->_u32[3],',
           '               w0, w1, w2, w3);',
           '        g_fail++;',
           '    } else g_pass++;',
           '}',
           '',
           'int main(void) {',
           '    spu_context* ctx = &g_ctx;']
    for idx, c in enumerate(kept):
        tag = f'[{c["family"]}] {c["name"]}'
        out.append(f'    {{ /* case {idx} @0x{c["addr"]:X}: {tag} '
                   f'| word 0x{c["word"]:08X} */')
        out.append('      memset(ctx, 0, sizeof(*ctx));')
        if RT not in c["in_regs"]:
            cw = c["canary"]
            out.append(f'      set4(&ctx->gpr[{RT}], 0x{cw[0]:08X}u, '
                       f'0x{cw[1]:08X}u, 0x{cw[2]:08X}u, 0x{cw[3]:08X}u);')
        for r, v in sorted(c["in_regs"].items()):
            out.append(f'      set4(&ctx->gpr[{r}], 0x{v[0]:08X}u, '
                       f'0x{v[1]:08X}u, 0x{v[2]:08X}u, 0x{v[3]:08X}u);')
        e = c["expect"]
        out.append(f'      spu_case_{c["addr"]:08X}(ctx);')
        out.append(f'      check4("{tag}", {RT}, &ctx->gpr[{RT}], 0x{e[0]:08X}u, '
                   f'0x{e[1]:08X}u, 0x{e[2]:08X}u, 0x{e[3]:08X}u);')
        out.append('    }')
    out.append('')
    out.append('    printf("[spu-conformance] %d passed, %d FAILED (of %d cases)\\n",')
    out.append('           g_pass, g_fail, g_pass + g_fail);')
    out.append('    return g_fail ? 1 : 0;')
    out.append('}')
    with open(os.path.join(OUTDIR, "spu_conf_driver.c"), "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")


def run_suite():
    vcvars = (r"C:\Program Files\Microsoft Visual Studio\18\Community"
              r"\VC\Auxiliary\Build\vcvars64.bat")
    bat = os.path.join(OUTDIR, "spu_conformance_run.bat")
    log = os.path.join(OUTDIR, "spu_conformance.log")
    exe = os.path.join(OUTDIR, "spu_conformance.exe")
    with open(bat, "w") as f:
        f.write("@echo off\n")
        f.write(f'call "{vcvars}" >nul 2>nul\n')
        f.write(f'cd /d "{ROOT}"\n')
        f.write(f'cl /nologo /O1 /W3 /std:c17 /I {OUTDIR} /I runtime\\spu '
                f'/Fo{OUTDIR}\\ /Fe:{exe} '
                f'{OUTDIR}\\spu_conf_driver.c {OUTDIR}\\spu_recomp.c '
                f'> "{log}" 2>&1\n')
        f.write("if errorlevel 1 exit /b 2\n")
        f.write(f'"{exe}" >> "{log}" 2>&1\n')
    r = subprocess.run(["cmd", "/c", bat], cwd=ROOT)
    text = open(log).read() if os.path.exists(log) else ""
    return r.returncode, text


def summarize(kept, skipped, log_text, verbose):
    fail_re = re.compile(r"^FAIL \[([^\]]+)\] (.+?): r\d+ (got=\S+ want=\S+)")
    fails = {}                                   # (family, name) -> detail
    for ln in log_text.splitlines():
        m = fail_re.match(ln)
        if m:
            fails[(m.group(1), m.group(2))] = m.group(3)

    print()
    shown = 0
    for c in kept:
        key = (c["family"], c["name"])
        if key in fails:
            todo = " [LIFTED-AS-NO-OP]" if c["todo"] else ""
            note = f'  <-- {c["note"]}' if c["note"] else ""
            print(f'FAIL [{c["family"]}] {c["name"]}: {fails[key]}{todo}{note}')
            shown += 1
        elif verbose:
            print(f'PASS [{c["family"]}] {c["name"]}')
    if shown != len(fails):
        print(f"(WARNING: {len(fails) - shown} FAIL lines did not match a case)")

    print()
    print(f'{"family":<14}{"cases":>6}{"pass":>6}{"fail":>6}  notes')
    tot_pass = tot_fail = 0
    for fam in FAMILIES:
        fam_cases = [c for c in kept if c["family"] == fam]
        n = len(fam_cases)
        nf = sum(1 for c in fam_cases if (fam, c["name"]) in fails)
        tot_pass += n - nf
        tot_fail += nf
        notes = sorted({c["note"].split(":")[0] for c in fam_cases
                        if c["note"] and (fam, c["name"]) in fails})
        mns = sorted({c["mn"] for c in fam_cases if (fam, c["name"]) in fails})
        extra = f'{",".join(mns)}' if mns else ""
        if notes:
            extra += f'  ({"; ".join(notes)})'
        print(f'{fam:<14}{n:>6}{n - nf:>6}{nf:>6}  {extra}')
    print(f'{"TOTAL":<14}{len(kept):>6}{tot_pass:>6}{tot_fail:>6}'
          f'  ({skipped} skipped at encoding)')
    for ln in log_text.splitlines():
        if "[spu-conformance]" in ln or "error C" in ln or "error LNK" in ln:
            print(ln)
    return tot_fail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", action="store_true",
                    help="only generate the blob + lifted C + driver")
    ap.add_argument("--verbose", action="store_true",
                    help="print PASS lines too")
    args = ap.parse_args()

    os.makedirs(OUTDIR, exist_ok=True)
    build_cases()

    kept, skipped = [], 0
    for c in CASES:
        if c["enc_err"]:
            print(f'ENCODING [{c["family"]}] {c["name"]}: {c["enc_err"]} '
                  f'-- case skipped')
            skipped += 1
        else:
            kept.append(c)

    lifter = lift_and_emit(kept)
    emit_driver(kept)
    n_todo = sum(1 for c in kept if c["todo"])
    print(f"generated {len(kept)} cases ({skipped} encoding-skipped, "
          f"{n_todo} lifted as TODO no-op) -> {OUTDIR}")
    if lifter.unsupported:
        print(f"  lifter TODO mnemonics: "
              f"{', '.join(sorted(lifter.unsupported))}")
    if args.emit:
        return

    rc, log_text = run_suite()
    if rc == 2:
        print("COMPILE FAILED -- see", os.path.join(OUTDIR, "spu_conformance.log"))
        for ln in log_text.splitlines():
            if "error" in ln.lower():
                print(ln)
        sys.exit(2)
    n_fail = summarize(kept, skipped, log_text, args.verbose)
    sys.exit(1 if (n_fail or skipped) else 0)


if __name__ == "__main__":
    main()
