#!/usr/bin/env python3
"""PPU lifter conformance suite.

Tests the lifter's ACTUAL EMISSIONS (not a parallel implementation): each case
encodes a real instruction word, round-trips it through ppu_disasm.decode
(verifying the encoding), asks PPULifter for the C statement it would emit for
that instruction, and wraps all statements in a generated C driver that runs
them against edge-case register inputs and compares results with expectations
computed here in Python straight from the PowerISA definitions.

Why this exists: two multi-session silent-miscompile hunts (spu_disasm il
double sign-extension; cntlzw(0) = raw __builtin_clz UB that dropped every
SPURS-queue wakeup signal) were single-instruction semantics bugs that a suite
like this catches in milliseconds. Sweep the class, don't spot-fix.

Usage:
    py -3 tools\\test_ppu_lift.py            # generate + compile + run
    py -3 tools\\test_ppu_lift.py --emit     # just write the C file
The compile step needs cl on PATH (the script self-launches vcvars64 like
scratch\\dobuild2.bat when cl is absent).

Scope (tranche 1): integer ALU/rotate/shift/carry/compare classes -- the
proven silent-miscompile territory. Memory ops, FP, and VMX are follow-on
tranches (VMX semantics already have manual-verified handling; loads/stores
need a vm stub harness).
"""

import argparse
import math
import os
import random
import struct
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)

# Seeded-bug self-test hook (T7 acceptance #1/#2): point the suite at a TEMP
# COPY of ppu_disasm.py/ppu_lifter.py (e.g. a mutated decoder/lifter dropped
# in the session scratchpad) without ever forking this file. Unset (the
# default) imports from tools/ exactly as before -- zero behavior change.
_OVERRIDE_DIR = os.environ.get("YZ_TEST_TOOLS_DIR")
sys.path.insert(0, TOOLS)
if _OVERRIDE_DIR:
    sys.path.insert(0, _OVERRIDE_DIR)   # must win over TOOLS -- inserted LAST

import ppu_disasm                    # noqa: E402
from ppu_lifter import PPULifter, LiftedFunction   # noqa: E402

if _OVERRIDE_DIR:
    print(f"[test_ppu_lift] YZ_TEST_TOOLS_DIR override active: "
          f"ppu_disasm={ppu_disasm.__file__} ppu_lifter loaded from override dir "
          f"if present there", file=sys.stderr)

MASK64 = (1 << 64) - 1
MASK32 = (1 << 32) - 1

# ---------------------------------------------------------------------------
# Instruction encoders (big-endian words). Every encoded word is round-tripped
# through ppu_disasm.decode and the mnemonic checked, so an encoding mistake
# here is reported as ENCODING, never as a false lifter failure.
# ---------------------------------------------------------------------------

def xo_form(xo, rt, ra, rb, oe=0, rc=0):
    return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (oe << 10) | (xo << 1) | rc

def x_logic(xo, rs, ra, rb, rc=0):   # and/or/... rs sits in the rt slot
    return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc

def d_form(op, rt, ra, imm):
    return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF)

def m_form(op, rs, ra, sh, mb, me, rc=0):
    return (op << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc

def md_form(xo3, rs, ra, sh, mbe, rc=0):   # rldicl/rldicr/rldic/rldimi
    return ((30 << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11)
            | ((mbe & 31) << 6) | ((mbe >> 5) << 5) | (xo3 << 2) | ((sh >> 5) << 1) | rc)

def xs_sradi(rs, ra, sh, rc=0):
    return ((31 << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11)
            | (413 << 2) | ((sh >> 5) << 1) | rc)

def cmp_form(xo, bf, l, ra, rb):
    return (31 << 26) | (bf << 23) | (l << 21) | (ra << 16) | (rb << 11) | (xo << 1)

def cmpi_form(op, bf, l, ra, imm):
    return (op << 26) | (bf << 23) | (l << 21) | (ra << 16) | (imm & 0xFFFF)

# FP tranche encoders (Book I ch. 4; op63 = double/X-forms, op59 = single A-forms)
def fp_x(opcd, frd, fra, frb, xo, rc=0):        # fctiw/fctiwz/fctid/fctidz/frsp
    return (opcd << 26) | (frd << 21) | (fra << 16) | (frb << 11) | (xo << 1) | rc

def fp_a(opcd, frd, fra, frb, frc, xo, rc=0):   # fadds/fsubs/fmuls/fdivs/fmadds/fsqrts
    return (opcd << 26) | (frd << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (xo << 1) | rc

def fcmpu_form(bf, fra, frb):
    return (63 << 26) | (bf << 23) | (fra << 16) | (frb << 11)

# ---------------------------------------------------------------------------
# PowerISA reference semantics (independent of the lifter). 64-bit registers;
# CA per the 64-bit operation (PowerISA v2.03, the manual in the repo root).
# ---------------------------------------------------------------------------

def s64(v): v &= MASK64; return v - (1 << 64) if v >> 63 else v
def s32(v): v &= MASK32; return v - (1 << 32) if v >> 31 else v
def sxw(v): return s32(v) & MASK64          # sign-extend low word to 64

def ref_add(a, b):        return (a + b) & MASK64, None
def ref_addc(a, b):       s = a + b; return s & MASK64, s >> 64
def ref_adde(a, b, ca):   s = a + b + ca; return s & MASK64, s >> 64
def ov64(a, b, result):
    """Signed 64-bit overflow of a 64-bit sum, per RPCS3 ADDE/SUBFE's
    ppu_ov_set: operands share a sign bit AND the result's sign bit differs
    (rpcs3/Emu/Cell/PPUInterpreter.cpp ADDE/SUBFE -- semantics only)."""
    sa, sb, sr = (a >> 63) & 1, (b >> 63) & 1, (result >> 63) & 1
    return 1 if (sa == sb and sa != sr) else 0
def ref_addeo(a, b, ca):
    v, cout = ref_adde(a, b, ca)
    return v, cout, ov64(a, b, v)
def ref_subfeo(a, b, ca):
    na = (~a) & MASK64
    v, cout = ref_subfe(a, b, ca)
    return v, cout, ov64(na, b, v)
def ref_addme(a, ca):     s = a + MASK64 + ca; return s & MASK64, s >> 64
def ref_addze(a, ca):     s = a + ca; return s & MASK64, s >> 64
def ref_subf(a, b):       return (b - a) & MASK64, None
def ref_subfc(a, b):      s = ((~a) & MASK64) + b + 1; return s & MASK64, s >> 64
def ref_subfe(a, b, ca):  s = ((~a) & MASK64) + b + ca; return s & MASK64, s >> 64
def ref_subfme(a, ca):    s = ((~a) & MASK64) + MASK64 + ca; return s & MASK64, s >> 64
def ref_subfze(a, ca):    s = ((~a) & MASK64) + ca; return s & MASK64, s >> 64
def ref_neg(a):           return (-a) & MASK64, None
def ref_cntlzw(a):        w = a & MASK32; return (32 if w == 0 else 31 - w.bit_length() + 1), None
def ref_cntlzd(a):        return (64 if a == 0 else 64 - a.bit_length()), None
def ref_extsb(a):         v = a & 0xFF; return (v - 0x100 if v >> 7 else v) & MASK64, None
def ref_extsh(a):         v = a & 0xFFFF; return (v - 0x10000 if v >> 15 else v) & MASK64, None
def ref_extsw(a):         return sxw(a), None
def ref_mullw(a, b):      return (s32(a) * s32(b)) & MASK64, None
def ref_mulhw(a, b):      return ((s32(a) * s32(b)) >> 32) & MASK32, None   # low32 defined; high undefined -> mask32 compare
def ref_mulhwu(a, b):     return (((a & MASK32) * (b & MASK32)) >> 32) & MASK32, None
def ref_mulld(a, b):      return (s64(a) * s64(b)) & MASK64, None
def ref_mulhd(a, b):      return ((s64(a) * s64(b)) >> 64) & MASK64, None
def ref_mulhdu(a, b):     return ((a * b) >> 64) & MASK64, None
def ref_mulli(a, imm):    return (s64(a) * imm) & MASK64, None

def rotl32(v, n): v &= MASK32; n &= 31; return ((v << n) | (v >> (32 - n))) & MASK32 if n else v
def rotl64(v, n): v &= MASK64; n &= 63; return ((v << n) | (v >> (64 - n))) & MASK64 if n else v

def mask32(mb, me):
    if mb <= me:
        return (MASK32 >> mb) & (MASK32 << (31 - me)) & MASK32
    return ((MASK32 >> mb) | (MASK32 << (31 - me))) & MASK32

def mask64(mb, me):
    if mb <= me:
        m = MASK64 if mb == 0 else (MASK64 >> mb)
        return m & ((MASK64 << (63 - me)) & MASK64)
    return ((MASK64 >> mb) | ((MASK64 << (63 - me)) & MASK64)) & MASK64

def ref_rlwinm(a, sh, mb, me):
    # PowerISA full 64-bit semantics: r = ROTL64(x||x, sh), m = MASK(mb+32, me+32).
    # For mb<=me the result is the zero-extended masked word; for mb>me the mask
    # WRAPS through the high 32 bits, which legitimately receive the rotated
    # duplicate. (The old 32-bit reference + MASK32 compares hid a lifter
    # sign-extension bug at 95k call sites — never mask a word-op compare.)
    r = rotl32(a, sh)
    return ((r << 32) | r) & mask64(mb + 32, me + 32), None
def ref_rlwimi(ra, rs, sh, mb, me):
    # Insert under the same 64-bit wrapped mask; RA bits outside m are PRESERVED
    # across the full 64 bits (high half survives a non-wrap mask untouched).
    r = rotl32(rs, sh)
    m = mask64(mb + 32, me + 32)
    return ((((r << 32) | r) & m) | (ra & MASK64 & ~m)) & MASK64, None
def ref_rldicl(a, sh, mb): return rotl64(a, sh) & mask64(mb, 63), None
def ref_rldicr(a, sh, me): return rotl64(a, sh) & mask64(0, me), None
def ref_rldimi(ra, rs, sh, mb):
    m = mask64(mb, 63 - sh)
    return ((rotl64(rs, sh) & m) | (ra & ~m)) & MASK64, None

def ref_slw(a, b):
    n = b & 0x3F
    return 0 if n & 0x20 else ((a & MASK32) << n) & MASK32, None
def ref_srw(a, b):
    n = b & 0x3F
    return 0 if n & 0x20 else (a & MASK32) >> n, None
def ref_sld(a, b):
    n = b & 0x7F
    return 0 if n & 0x40 else (a << n) & MASK64, None
def ref_srd(a, b):
    n = b & 0x7F
    return 0 if n & 0x40 else a >> n, None

def ref_srawi(a, sh):
    rs = s32(a)
    if sh == 0:
        return sxw(a), 0
    out = (a & MASK32) & ((1 << sh) - 1)
    return (rs >> sh) & MASK64, 1 if (rs < 0 and out) else 0
def ref_sraw(a, b):
    n = b & 0x3F
    rs = s32(a)
    if n == 0:
        return sxw(a), 0
    if n >= 32:
        return (rs >> 31) & MASK64, 1 if (rs < 0) else 0
    out = (a & MASK32) & ((1 << n) - 1)
    return (rs >> n) & MASK64, 1 if (rs < 0 and out) else 0
def ref_sradi(a, sh):
    rs = s64(a)
    if sh == 0:
        return a & MASK64, 0
    out = a & ((1 << sh) - 1)
    return (rs >> sh) & MASK64, 1 if (rs < 0 and out) else 0

def ref_logic(op, a, b):
    f = {"and": a & b, "or": a | b, "xor": a ^ b, "nand": ~(a & b),
         "nor": ~(a | b), "andc": a & ~b, "orc": a | ~b, "eqv": ~(a ^ b)}[op]
    return f & MASK64, None

# CR-field logical ops, generalized over arbitrary bt/ba/bb bit indices (host
# CR bit 0 = MSB) -- the fuzz tranche for the crnand/crnor bug class (T7
# mandatory). PowerISA v2.03 Book I ch. 2.5.2 / Sec 3.3.11.
def ref_cr_op(op, cr, bt, ba, bb):
    a = (cr >> (31 - ba)) & 1
    b = (cr >> (31 - bb)) & 1
    r = {"crand": a & b, "cror": a | b, "crxor": a ^ b,
         "crnand": 1 - (a & b), "crnor": 1 - (a | b), "creqv": 1 - (a ^ b),
         "crandc": a & (1 - b), "crorc": a | (1 - b)}[op]
    return (cr & ~(1 << (31 - bt)) | (r << (31 - bt))) & MASK32, r

def ref_addi(a, imm):  return (a + imm) & MASK64, None
def ref_addic(a, imm): s = a + (imm & MASK64); return s & MASK64, s >> 64
def ref_subfic(a, imm):
    s = ((~a) & MASK64) + (imm & MASK64) + 1
    return s & MASK64, s >> 64

def ref_divw(a, b):
    d, n = s32(a), s32(b)
    if n == 0 or (d == -(1 << 31) and n == -1):
        return None, None                        # UNDEFINED: assert no-crash only
    return (int(d / n) if (d < 0) != (n < 0) else d // n) & MASK32, None
def ref_divwu(a, b):
    d, n = a & MASK32, b & MASK32
    if n == 0:
        return None, None
    return (d // n) & MASK32, None
def ref_divd(a, b):
    d, n = s64(a), s64(b)
    if n == 0 or (d == -(1 << 63) and n == -1):
        return None, None
    q = abs(d) // abs(n)
    return (q if (d < 0) == (n < 0) else -q) & MASK64, None
def ref_divdu(a, b):
    if b == 0:
        return None, None
    return (a // b) & MASK64, None

def cr_nibble_signed(a, b):
    if s64(a) < s64(b): return 8
    if s64(a) > s64(b): return 4
    return 2
def cr_nibble_signed32(a, b):
    if s32(a) < s32(b): return 8
    if s32(a) > s32(b): return 4
    return 2
def cr_nibble_unsigned(a, b, bits):
    m = (1 << bits) - 1
    if (a & m) < (b & m): return 8
    if (a & m) > (b & m): return 4
    return 2

# ---------------------------------------------------------------------------
# Test-case construction
# ---------------------------------------------------------------------------

E64 = [0, 1, 2, MASK64, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000,
       0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x100000000,
       0x123456789ABCDEF0, 0xFEDCBA9876543210]
rng = random.Random(0x59414B5A)   # deterministic
E64 += [rng.getrandbits(64) for _ in range(4)]

CASES = []   # dicts: name, word, in_regs {idx:val}, in_ca, expects [(reg, val, mask)], exp_ca, exp_cr(nibble,pos), may_trap
             # FP tranche additions: in_fprs {idx: 64-bit pattern}, exp_fprs [(idx, pattern, mask)]

# ---------------------------------------------------------------------------
# VMX (AltiVec) endianness canary tranche (audit S2-1 / finding 4).
#
# ctx->vr holds RAW big-endian bytes (lvx/stvx do a plain memcpy). Register-only
# tests can't catch lane byte-reversal because the bug is symmetric under a
# consistent-but-wrong convention; a MEMORY round-trip does: we write a known
# 16-byte big-endian vector into the vm stub, lvx it, run the op, stvx the
# result back, and compare the emitted bytes against a Python reference that
# treats memory as big-endian throughout. If a handler read/wrote a lane
# little-endian, the output bytes differ. Each canary is a *sequence* of real
# lifted instructions (loads -> op -> store), so it validates the actual
# lvx/op/stvx emissions end-to-end.
#
# vm layout in the driver's 64 KB stub: input vectors at 0x100, 0x110, 0x120,
# result at 0x200. GPRs: r10=0x100(vA), r11=0x110(vB), r12=0x120(vC),
# r13=0x200(result). All quadword-aligned.

VCASES = []  # dicts: name, prog [insn words], vin {addr: bytes16}, vout {addr: bytes16}, exp_cr(nibble,pos)|None

def vx_form(xo, vd, va, vb):
    return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | xo

def vxr_form(xo10, vd, va, vb, rc=0):    # VXR compares: Rc at bit 10 (of low 11)
    return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | (rc << 10) | xo10

def va_form(xo6, vd, va, vb, vc):        # VA-form: vmaddfp/vnmsubfp/vmsum*/vperm/vsel/vsldoi
    return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | (vc << 6) | xo6

def lvx_word(vd, ra, rb):   return xo_form(103, vd, ra, rb)   # op31 xo 103
def stvx_word(vs, ra, rb):  return xo_form(231, vs, ra, rb)   # op31 xo 231

# big-endian byte pack/unpack helpers for reference vectors
def be_bytes_w(words):      # 4 x 32-bit BE -> 16 bytes
    return b"".join(struct.pack(">I", w & MASK32) for w in words)
def be_bytes_h(halfs):      # 8 x 16-bit BE -> 16 bytes
    return b"".join(struct.pack(">H", h & 0xFFFF) for h in halfs)
def be_bytes_f(flts):       # 4 x float BE -> 16 bytes
    return b"".join(struct.pack(">f", f) for f in flts)
def words_of(b):  return list(struct.unpack(">4I", b))
def halfs_of(b):  return list(struct.unpack(">8H", b))
def bytes_of(b):  return list(b)
def floats_of(b): return list(struct.unpack(">4f", b))

A_ADDR, B_ADDR, C_ADDR, R_ADDR = 0x100, 0x110, 0x120, 0x200

def vcase(name, prog, vin, vout, exp_cr=None):
    VCASES.append(dict(name=name, prog=prog, vin=vin, vout=vout, exp_cr=exp_cr))

def _bin_vcase(name, xo, ain, bin_, result_bytes, form="vx", exp_cr=None, rc=0):
    """One vA op vB -> vD canary: lvx v0,r10; lvx v1,r11; op v2,v0,v1;
    stvx v2,r13. ain/bin_ are 16-byte big-endian input vectors."""
    if form == "vx":
        opw = vx_form(xo, 2, 0, 1)
    else:  # vxr (compares, optional Rc)
        opw = vxr_form(xo, 2, 0, 1, rc)
    prog = [lvx_word(0, 0, 10), lvx_word(1, 0, 11), opw, stvx_word(2, 0, 13)]
    vcase(name, prog, {A_ADDR: ain, B_ADDR: bin_},
          {R_ADDR: result_bytes}, exp_cr=exp_cr)

def case(name, word, in_regs, expects, in_ca=None, exp_ca=None, exp_cr=None, may_trap=False,
         in_fprs=None, exp_fprs=None, in_cr=0, exp_ov=None, exp_so=None):
    CASES.append(dict(name=name, word=word, in_regs=in_regs, expects=expects,
                      in_ca=in_ca, exp_ca=exp_ca, exp_cr=exp_cr, may_trap=may_trap,
                      in_fprs=in_fprs or {}, exp_fprs=exp_fprs or [], in_cr=in_cr,
                      exp_ov=exp_ov, exp_so=exp_so))

def dbits(x):
    """64-bit pattern of a Python float as an IEEE double."""
    return struct.unpack("<Q", struct.pack("<d", x))[0]

def fbits_rounded(x):
    """64-bit double pattern of x after rounding to single precision."""
    return dbits(struct.unpack("<f", struct.pack("<f", x))[0])

def pairs(n=6):
    ps = []
    for i in range(n):
        ps.append((E64[i * 3 % len(E64)], E64[(i * 5 + 2) % len(E64)]))
    ps += [(0, 0), (MASK64, 1), (0x7FFFFFFFFFFFFFFF, 1), (0x8000000000000000, MASK64),
           (0xFFFFFFFF, 1), (0x80000000, 0x80000000)]
    return ps

def build_cases():
    R = 3, 4, 5   # rt, ra, rb

    # --- XO-form arithmetic + carry family -------------------------------
    xo_ops = [
        ("add",    266, lambda a, b, ca: ref_add(a, b)),
        ("subf",    40, lambda a, b, ca: ref_subf(a, b)),
        ("addc",    10, lambda a, b, ca: ref_addc(a, b)),
        ("subfc",    8, lambda a, b, ca: ref_subfc(a, b)),
        ("adde",   138, lambda a, b, ca: ref_adde(a, b, ca)),
        ("subfe",  136, lambda a, b, ca: ref_subfe(a, b, ca)),
        ("mullw",  235, lambda a, b, ca: ref_mullw(a, b)),
        ("mulld",  233, lambda a, b, ca: ref_mulld(a, b)),
        ("mulhw",   75, lambda a, b, ca: ref_mulhw(a, b)),
        ("mulhwu",  11, lambda a, b, ca: ref_mulhwu(a, b)),
        ("mulhd",   73, lambda a, b, ca: ref_mulhd(a, b)),
        ("mulhdu",   9, lambda a, b, ca: ref_mulhdu(a, b)),
    ]
    for name, xo, ref in xo_ops:
        uses_ca = name in ("adde", "subfe")
        sets_ca = name in ("addc", "subfc", "adde", "subfe")
        for a, b in pairs():
            for ca in ((0, 1) if uses_ca else (None,)):
                v, cout = ref(a, b, ca or 0)
                mask = MASK32 if name in ("mulhw", "mulhwu") else MASK64
                case(f"{name} a={a:#x} b={b:#x} ca={ca}",
                     xo_form(xo, R[0], R[1], R[2]),
                     {R[1]: a, R[2]: b}, [(R[0], v, mask)],
                     in_ca=ca, exp_ca=(cout if sets_ca else None))

    # --- addeo/subfeo: XER[OV]/XER[SO] on the OE-form of adde/subfe -------
    # (PowerISA_V2.03_Final_Public.pdf p.60/p.36; cross-checked against
    # RPCS3's ADDE/SUBFE ppu_ov_set -- semantics only, no code copied.)
    ov_ops = [("addeo", 138, ref_addeo), ("subfeo", 136, ref_subfeo)]
    # (a, b, ca) tuples span pos+pos and neg+neg 64-bit overflow, a small
    # no-overflow sum, and a ca-dependent tip-over case. Each pair's expected
    # OV/SO is computed HERE by ref_addeo/ref_subfeo (an independent Python
    # implementation of the same two's-complement overflow rule), not
    # hand-asserted per pair, so the set only needs to span the OV=1/OV=0
    # space -- it doesn't need every pair to land a specific op's OV bit.
    ov_pairs = [
        (0x7FFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0),
        (0x8000000000000000, 0x8000000000000000, 0),
        (0x7FFFFFFFFFFFFFFF, 0x0000000000000001, 0),
        (1, 2, 0),
        (0x7FFFFFFFFFFFFFFF, 0, 1),
        (0x8000000000000000, 0x7FFFFFFFFFFFFFFF, 1),
    ]
    for name, xo, ref in ov_ops:
        for a, b, ca in ov_pairs:
            v, cout, ov = ref(a, b, ca)
            case(f"{name} a={a:#x} b={b:#x} ca={ca}",
                 xo_form(xo, R[0], R[1], R[2], oe=1),
                 {R[1]: a, R[2]: b}, [(R[0], v, MASK64)],
                 in_ca=ca, exp_ca=cout, exp_ov=ov, exp_so=ov)

    # --- mulhwo/mulhwuo: reserved-OE-bit encodings of mulhw/mulhwu ---------
    # PowerISA has no architected OE form for the high-word multiplies (only
    # mullw/mulld get one). ppu_disasm.py's shared OE-suffix logic can still
    # emit "mulhwo"/"mulhwuo" for XO=75/11 with OE=1 set (a reserved encoding,
    # decoded the same way as the mullw/mulld family it shares a table with).
    # The lifter must treat it exactly like the plain form: same result, and
    # critically NO XER write (OV/SO must stay whatever they were before).
    for name, xo, ref in [("mulhwo", 75, ref_mulhw), ("mulhwuo", 11, ref_mulhwu)]:
        for a, b in pairs():
            v, _ = ref(a, b)
            case(f"{name} a={a:#x} b={b:#x}",
                 xo_form(xo, R[0], R[1], R[2], oe=1),
                 {R[1]: a, R[2]: b}, [(R[0], v, MASK32)],
                 exp_ov=0, exp_so=0)   # OE bit ignored: OV/SO must NOT be set

    for name, xo, ref in [("addme", 234, ref_addme), ("addze", 202, ref_addze),
                          ("subfme", 232, ref_subfme), ("subfze", 200, ref_subfze)]:
        for a in E64[:8]:
            for ca in (0, 1):
                v, cout = ref(a, ca)
                case(f"{name} a={a:#x} ca={ca}",
                     xo_form(xo, R[0], R[1], 0),
                     {R[1]: a}, [(R[0], v, MASK64)], in_ca=ca, exp_ca=cout)

    for a in (0, 1, MASK64, 0x8000000000000000):
        v, _ = ref_neg(a)
        case(f"neg a={a:#x}", xo_form(104, R[0], R[1], 0), {R[1]: a}, [(R[0], v, MASK64)])

    # divides: defined-result vectors + UNDEFINED vectors as no-crash probes
    for name, xo, ref, m in [("divw", 491, ref_divw, MASK32), ("divwu", 459, ref_divwu, MASK32),
                             ("divd", 489, ref_divd, MASK64), ("divdu", 457, ref_divdu, MASK64)]:
        for a, b in [(100, 7), (MASK64, 3), (0x80000000, 0xFFFFFFFF),
                     (0x7FFFFFFF, 2), (5, MASK64)]:
            v, _ = ref(a, b)
            if v is not None:
                case(f"{name} a={a:#x} b={b:#x}", xo_form(xo, R[0], R[1], R[2]),
                     {R[1]: a, R[2]: b}, [(R[0], v, m)])
        for a, b in [(1, 0), (0x8000000000000000, MASK64), (0x80000000, 0xFFFFFFFF00000000 | MASK32)]:
            case(f"{name} UNDEF a={a:#x} b={b:#x}", xo_form(xo, R[0], R[1], R[2]),
                 {R[1]: a, R[2]: b}, [], may_trap=True)

    # --- X-form logicals / extends / counts ------------------------------
    for name, xo in [("and", 28), ("or", 444), ("xor", 316), ("nand", 476),
                     ("nor", 124), ("andc", 60), ("orc", 412), ("eqv", 284)]:
        for a, b in pairs(3):
            v, _ = ref_logic(name, a, b)
            case(f"{name} a={a:#x} b={b:#x}", x_logic(xo, R[0], R[1], R[2]),
                 {R[0]: a, R[2]: b}, [(R[1], v, MASK64)])

    for name, xo, ref in [("extsb", 954, ref_extsb), ("extsh", 922, ref_extsh),
                          ("extsw", 986, ref_extsw),
                          ("cntlzw", 26, ref_cntlzw), ("cntlzd", 58, ref_cntlzd)]:
        for a in [0, 1, 0x80, 0x7F, 0xFF, 0x8000, 0xFFFF, 0x80000000, MASK32,
                  0x8000000000000000, MASK64, 0x40000000, 0x0000000100000000]:
            v, _ = ref(a)
            case(f"{name} a={a:#x}", x_logic(xo, R[0], R[1], 0),
                 {R[0]: a}, [(R[1], v, MASK64)])

    # --- shifts -----------------------------------------------------------
    for name, xo, ref in [("slw", 24, ref_slw), ("srw", 536, ref_srw),
                          ("sld", 27, ref_sld), ("srd", 539, ref_srd)]:
        for a in (0x1, 0x80000000, MASK32, 0x8000000000000000, MASK64):
            for n in (0, 1, 31, 32, 33, 63, 64):
                v, _ = ref(a, n)
                case(f"{name} a={a:#x} n={n}", x_logic(xo, R[0], R[1], R[2]),
                     {R[0]: a, R[2]: n}, [(R[1], v, MASK64)])

    for a in (0x1, 0x80000000, 0xFFFFFFFF, 0x80000001, 0x7FFFFFFF, 0xFFFFFFFF80000000):
        for sh in (0, 1, 4, 31):
            v, ca = ref_srawi(a, sh)
            case(f"srawi a={a:#x} sh={sh}",
                 (31 << 26) | (R[0] << 21) | (R[1] << 16) | (sh << 11) | (824 << 1),
                 {R[0]: a}, [(R[1], v, MASK64)], exp_ca=ca)
        for n in (0, 1, 31, 32, 40):
            v, ca = ref_sraw(a, n)
            case(f"sraw a={a:#x} n={n}", x_logic(792, R[0], R[1], R[2]),
                 {R[0]: a, R[2]: n}, [(R[1], v, MASK64)], exp_ca=ca)
    for a in (0x1, 0x8000000000000000, MASK64, 0x8000000000000001):
        for sh in (0, 1, 32, 63):
            v, ca = ref_sradi(a, sh)
            case(f"sradi a={a:#x} sh={sh}", xs_sradi(R[0], R[1], sh),
                 {R[0]: a}, [(R[1], v, MASK64)], exp_ca=ca)

    # --- rotates ----------------------------------------------------------
    for a in (0x12345678, 0x80000001, MASK32, 0xFEDCBA9876543210):
        for sh, mb, me in [(0, 0, 31), (1, 0, 31), (13, 5, 20), (16, 16, 31),
                           (4, 28, 3), (31, 31, 0), (0, 31, 31)]:   # incl. mb>me wraps
            v, _ = ref_rlwinm(a, sh, mb, me)
            case(f"rlwinm a={a:#x} sh={sh} mb={mb} me={me}",
                 m_form(21, R[0], R[1], sh, mb, me),
                 {R[0]: a}, [(R[1], v, MASK64)])
            r0 = 0xAAAAAAAABBBBBBBB
            v2, _ = ref_rlwimi(r0, a, sh, mb, me)
            case(f"rlwimi a={a:#x} sh={sh} mb={mb} me={me}",
                 m_form(20, R[0], R[1], sh, mb, me),
                 {R[0]: a, R[1]: r0}, [(R[1], v2, MASK64)])
    for a in (0x123456789ABCDEF0, 0x8000000000000001, MASK64):
        for sh, mbe in [(0, 0), (0, 63), (1, 0), (17, 5), (32, 31), (63, 1), (40, 63)]:
            v, _ = ref_rldicl(a, sh, mbe)
            case(f"rldicl a={a:#x} sh={sh} mb={mbe}", md_form(0, R[0], R[1], sh, mbe),
                 {R[0]: a}, [(R[1], v, MASK64)])
            v, _ = ref_rldicr(a, sh, mbe)
            case(f"rldicr a={a:#x} sh={sh} me={mbe}", md_form(1, R[0], R[1], sh, mbe),
                 {R[0]: a}, [(R[1], v, MASK64)])
        for sh, mb in [(8, 4), (0, 0), (32, 16)]:
            if mb <= 63 - sh:
                r0 = 0xCCCCCCCCDDDDDDDD
                v, _ = ref_rldimi(r0, a, sh, mb)
                case(f"rldimi a={a:#x} sh={sh} mb={mb}", md_form(3, R[0], R[1], sh, mb),
                     {R[0]: a, R[1]: r0}, [(R[1], v, MASK64)])

    # --- D-form immediates (negative-immediate class) ---------------------
    for a in (0, 5, MASK64, 0x7FFFFFFFFFFFFFFF):
        for imm in (0, 1, -1, -4, 0x7FFF, -0x8000):
            v, _ = ref_addi(a, imm)
            case(f"addi a={a:#x} imm={imm}", d_form(14, R[0], R[1], imm),
                 {R[1]: a}, [(R[0], v, MASK64)])
            v, _ = ref_addi(a, imm << 16)
            case(f"addis a={a:#x} imm={imm}", d_form(15, R[0], R[1], imm),
                 {R[1]: a}, [(R[0], v, MASK64)])
            v, ca = ref_addic(a, imm if imm >= 0 else (imm & MASK64))
            case(f"addic a={a:#x} imm={imm}", d_form(12, R[0], R[1], imm),
                 {R[1]: a}, [(R[0], v, MASK64)], exp_ca=ca)
            v, ca = ref_subfic(a, imm if imm >= 0 else (imm & MASK64))
            case(f"subfic a={a:#x} imm={imm}", d_form(8, R[0], R[1], imm),
                 {R[1]: a}, [(R[0], v, MASK64)], exp_ca=ca)
            v, _ = ref_mulli(a, imm)
            case(f"mulli a={a:#x} imm={imm}", d_form(7, R[0], R[1], imm),
                 {R[1]: a}, [(R[0], v, MASK64)])

    # --- compares (CR field placement; cr0 and cr7) -----------------------
    for a, b in [(0, 0), (1, 2), (2, 1), (MASK64, 1), (0x80000000, 0x7FFFFFFF),
                 (0xFFFFFFFF80000000, 0x7FFFFFFF)]:
        for bf in (0, 7):
            shift = 28 - bf * 4
            case(f"cmpd bf={bf} a={a:#x} b={b:#x}", cmp_form(0, bf, 1, R[1], R[2]),
                 {R[1]: a, R[2]: b}, [], exp_cr=(cr_nibble_signed(a, b), shift))
            case(f"cmpw bf={bf} a={a:#x} b={b:#x}", cmp_form(0, bf, 0, R[1], R[2]),
                 {R[1]: a, R[2]: b}, [], exp_cr=(cr_nibble_signed32(a, b), shift))
            case(f"cmpld bf={bf} a={a:#x} b={b:#x}", cmp_form(32, bf, 1, R[1], R[2]),
                 {R[1]: a, R[2]: b}, [], exp_cr=(cr_nibble_unsigned(a, b, 64), shift))
            case(f"cmplw bf={bf} a={a:#x} b={b:#x}", cmp_form(32, bf, 0, R[1], R[2]),
                 {R[1]: a, R[2]: b}, [], exp_cr=(cr_nibble_unsigned(a, b, 32), shift))
        case(f"cmpdi a={a:#x}", cmpi_form(11, 0, 1, R[1], 1),
             {R[1]: a}, [], exp_cr=(cr_nibble_signed(a, 1), 28))
        case(f"cmpwi a={a:#x}", cmpi_form(11, 0, 0, R[1], 1),
             {R[1]: a}, [], exp_cr=(cr_nibble_signed32(a, 1), 28))

    # --- Rc=1 CR0 recording ------------------------------------------------
    for a, b in [(1, 1), (0, 0), (MASK64, 1), (0x8000000000000000, 0)]:
        v, _ = ref_add(a, b)
        nib = 8 if s64(v) < 0 else (4 if s64(v) > 0 else 2)
        case(f"add. a={a:#x} b={b:#x}", xo_form(266, R[0], R[1], R[2], rc=1),
             {R[1]: a, R[2]: b}, [(R[0], v, MASK64)], exp_cr=(nib, 28))

    # --- FP tranche (2026-07-03: audit S2-2/S2-3/S2-4 landed) --------------
    F = 1, 2, 3   # frd, fra, frb
    NAN = dbits(float("nan"))

    # fcmpu: Book I 4.6.8 -- NaN operand => FU (nibble bit 0). check_cr masks
    # SO out, so the FU cases assert LT/GT/EQ all CLEAR (the old bug set EQ).
    for name, a, b, nib in [
        ("lt", 1.0, 2.0, 8), ("gt", 2.0, 1.0, 4), ("eq", 1.5, 1.5, 2),
        ("nan-a", None, 1.0, 1), ("nan-b", 1.0, None, 1), ("nan-both", None, None, 1),
    ]:
        pa = NAN if a is None else dbits(a)
        pb = NAN if b is None else dbits(b)
        case(f"fcmpu {name}", fcmpu_form(0, F[1], F[2]), {},
             [], exp_cr=(nib, 28), in_fprs={F[1]: pa, F[2]: pb})

    # fctiwz/fctiw: saturation + rounding (S2-3). fctiw uses nearest-even.
    LOW32 = 0x00000000FFFFFFFF
    for name, v, zexp, nexp in [
        ("2.5", 2.5, 2, 2),                 # nearest-even: 2.5 -> 2
        ("1.5", 1.5, 1, 2),                 # nearest-even: 1.5 -> 2
        ("-2.5", -2.5, -2 & 0xFFFFFFFF, -2 & 0xFFFFFFFF),
        ("3e9", 3e9, 0x7FFFFFFF, 0x7FFFFFFF),      # positive SATURATES (old: sign-flip)
        ("-3e9", -3e9, 0x80000000, 0x80000000),
        ("nan", None, 0x80000000, 0x80000000),
    ]:
        pv = NAN if v is None else dbits(v)
        case(f"fctiwz {name}", fp_x(63, F[0], 0, F[2], 15), {},
             [], in_fprs={F[2]: pv}, exp_fprs=[(F[0], zexp, LOW32)])
        case(f"fctiw {name}", fp_x(63, F[0], 0, F[2], 14), {},
             [], in_fprs={F[2]: pv}, exp_fprs=[(F[0], nexp, LOW32)])

    for name, v, zexp in [
        ("2.5", 2.5, 2), ("1e19", 1e19, 0x7FFFFFFFFFFFFFFF),
        ("-1e19", -1e19, 0x8000000000000000), ("nan", None, 0x8000000000000000),
    ]:
        pv = NAN if v is None else dbits(v)
        case(f"fctidz {name}", fp_x(63, F[0], 0, F[2], 815), {},
             [], in_fprs={F[2]: pv}, exp_fprs=[(F[0], zexp, MASK64)])

    # Single-precision rounding (S2-4): the double intermediate must round to
    # single. Inputs chosen so double != float(double).
    sp_cases = [
        ("fadds", 21, 1.0, 2.0 ** -30, 1.0 + 2.0 ** -30),
        ("fsubs", 20, 1.0, -(2.0 ** -30), 1.0 + 2.0 ** -30),
        ("fmuls", 25, 1.0 + 2.0 ** -12, 1.0 + 2.0 ** -12, (1.0 + 2.0 ** -12) ** 2),
        ("fdivs", 18, 1.0, 3.0, 1.0 / 3.0),
    ]
    for name, xo, a, b, dres in sp_cases:
        if name == "fmuls":   # A-form: frc holds the multiplier
            word = fp_a(59, F[0], F[1], 0, F[2], xo)
        else:
            word = fp_a(59, F[0], F[1], F[2], 0, xo)
        case(f"{name} rounds-to-single", word, {},
             [], in_fprs={F[1]: dbits(a), F[2]: dbits(b)},
             exp_fprs=[(F[0], fbits_rounded(dres), MASK64)])

    # fmadds: (a*c)+b rounded to single
    a, c, b = 1.0 + 2.0 ** -12, 1.0 + 2.0 ** -12, 2.0 ** -30
    case("fmadds rounds-to-single", fp_a(59, 4, F[1], F[2], 5, 29), {},
         [], in_fprs={F[1]: dbits(a), F[2]: dbits(b), 5: dbits(c)},
         exp_fprs=[(4, fbits_rounded(a * c + b), MASK64)])

    # --- s24 lifter fix batch (2026-07-09): XER mfspr/mtspr, FP NaN
    # canonicalization, fma()-based fused multiply-add, frsp NaN handling.
    # ---------------------------------------------------------------------

    # mfspr rD,XER / mtspr XER,rS. ctx->xer is the flags register every
    # carry/overflow op already writes (CA=bit29, OV=bit30, SO=bit31); these
    # prove the explicit SPR read/write path -- previously an unhandled TODO
    # no-op that silently returned/discarded 0 -- round-trips it instead.
    XER_SPR = 1

    def xfx_form(xo, rt, spr):
        # XFX-form SPR field is bit-swapped (low 5 bits of the encoded field
        # hold the SPR's high 5 bits and vice versa) -- see ppu_disasm.py's
        # mfspr/mtspr decode (spr = ((raw&0x1F)<<5) | ((raw>>5)&0x1F)).
        spr_raw = ((spr & 0x1F) << 5) | ((spr >> 5) & 0x1F)
        return (31 << 26) | (rt << 21) | (spr_raw << 11) | (xo << 1)

    # "XER read after a carry-setting op": in_ca seeds ctx->xer with CA=1
    # exactly as a prior adde/subfe/addc/etc. would have left it (the same
    # carry-seeding idiom the xo_ops/ov_ops cases above use), so this
    # exercises mfspr reading back a real prior op's flag state, zero-
    # extended into the full 64-bit GPR.
    case("mfspr XER after CA-setting op", xfx_form(339, R[0], XER_SPR),
         {}, [(R[0], 1 << 29, MASK64)], in_ca=1)

    # mtspr XER,rS: CA/OV/SO all set via the GPR's low 32 bits; garbage in
    # the untouched high 32 bits must not leak into the 32-bit ctx->xer.
    case("mtspr XER sets CA/OV/SO", xfx_form(467, R[1], XER_SPR),
         {R[1]: 0xDEADBEEFE0000000}, [], exp_ca=1, exp_ov=1, exp_so=1)

    # FP NaN canonicalization: host x64 SSE2 arithmetic produces a QNaN with
    # the sign bit SET (0xFFF8_..) for these two classic "generated NaN"
    # cases -- MEASURED via a scratch MSVC probe on this toolchain: both
    # inf+(-inf) and 0*inf emit exactly 0xFFF8000000000000. PowerISA Book I
    # 4.3.3 (PowerISA_V2.03_Final_Public.pdf p.117, manual p.95) mandates
    # sign 0 for a QNaN generated as an operation's own result
    # (0x7FF8_0000_0000_0000). Double-precision forms (opcode 63) exercise
    # ppu_fp_canon_nan on the exact op family the divergence came from.
    PINF = dbits(float("inf"))
    NINF = dbits(float("-inf"))
    PZERO = dbits(0.0)
    QNAN_CANON = 0x7FF8000000000000
    case("fadd inf+(-inf) canon NaN", fp_a(63, F[0], F[1], F[2], 0, 21),
         {}, [], in_fprs={F[1]: PINF, F[2]: NINF},
         exp_fprs=[(F[0], QNAN_CANON, MASK64)])
    case("fmul 0*inf canon NaN", fp_a(63, F[0], F[1], 0, F[2], 25),
         {}, [], in_fprs={F[1]: PZERO, F[2]: PINF},
         exp_fprs=[(F[0], QNAN_CANON, MASK64)])

    # fma() fused single-rounding: a case where a plain `a*c + b` in double
    # rounds TWICE (once implicitly for the product, once for the add) and
    # differs from the true fused result by 1 ULP -- the classic double-
    # rounding construction. The expected value is computed two independent
    # ways and cross-checked here at suite-build time: Python's math.fma
    # (3.13+, itself independent of this repo) AND a one-off MSVC `fma()`
    # probe compiled with this project's exact toolchain (scratch, not
    # committed) -- both agreed: naive=0x3CA0000000000000,
    # fused=0x3C90000000000000.
    _fma_a = struct.unpack("<d", struct.pack("<Q", 0x3FF0000002000000))[0]
    _fma_c = struct.unpack("<d", struct.pack("<Q", 0x3FEFFFFFFC000000))[0]
    _fma_b = struct.unpack("<d", struct.pack("<Q", 0xBFEFFFFFFFFFFFFF))[0]
    _fma_naive = _fma_a * _fma_c + _fma_b
    _fma_fused = math.fma(_fma_a, _fma_c, _fma_b)
    assert dbits(_fma_naive) == 0x3CA0000000000000, "fma test fixture drifted (naive)"
    assert dbits(_fma_fused) == 0x3C90000000000000, "fma test fixture drifted (fused)"
    case("fmadd fused differs from naive double-round",
         fp_a(63, 4, F[1], F[2], 5, 29), {}, [],
         in_fprs={F[1]: dbits(_fma_a), F[2]: dbits(_fma_b), 5: dbits(_fma_c)},
         exp_fprs=[(4, dbits(_fma_fused), MASK64)])

    # frsp NaN passthrough + quieting: sign/payload preserved, only the
    # quiet bit (mantissa MSB, bit 51) forced on. PowerISA Book I
    # 4.6.6.2/4.3.3 (p.117/141, manual p.95/119): "SNaNs that are converted
    # to QNaNs ... retain the sign bit of the SNaN"; an already-quiet QNaN
    # passes through unchanged since the bit is already 1.
    SNAN_NEG = 0xFFF4000000000000          # sign=1, quiet bit clear, payload 0x4...
    SNAN_NEG_QUIETED = 0xFFFC000000000000  # same, quiet bit forced on
    QNAN_NEG = 0xFFFC000000000000          # already quiet -- unchanged
    case("frsp SNaN quiets, keeps sign+payload", fp_x(63, F[0], 0, F[2], 12),
         {}, [], in_fprs={F[2]: SNAN_NEG}, exp_fprs=[(F[0], SNAN_NEG_QUIETED, MASK64)])
    case("frsp QNaN passthrough unchanged", fp_x(63, F[0], 0, F[2], 12),
         {}, [], in_fprs={F[2]: QNAN_NEG}, exp_fprs=[(F[0], QNAN_NEG, MASK64)])
    # Regression: a normal (non-NaN) value must still round to single --
    # the fast path in ppu_frsp is unaffected by the NaN branch added above.
    case("frsp normal value rounds to single", fp_x(63, F[0], 0, F[2], 12),
         {}, [], in_fprs={F[2]: dbits(1.0 + 2.0 ** -30)},
         exp_fprs=[(F[0], fbits_rounded(1.0 + 2.0 ** -30), MASK64)])

    # Store-with-update RA writeback (S2-7): stwux/sthux/stbux (needs vm stub)
    for name, xo in [("stwux", 183), ("sthux", 439), ("stbux", 247)]:
        case(f"{name} RA update", x_logic(xo, R[0], R[1], R[2]),
             {R[0]: 0x11223344AABBCCDD, R[1]: 0x100, R[2]: 0x24},
             [(R[1], 0x124, MASK64)])

build_cases()

def _sat_s32(x):  return 0x7FFFFFFF if x > 0x7FFFFFFF else (-0x80000000 if x < -0x80000000 else x)

def build_vcases():
    # Distinct big-endian input vectors with asymmetric bytes so a lane
    # byte-reversal changes the numeric result. Word views deliberately have
    # every byte distinct within a lane.
    AW = [0x01020304, 0x05060708, 0x090A0B0C, 0x0D0E0F10]
    BW = [0x11121314, 0x15161718, 0x191A1B1C, 0x1D1E1F20]
    A16 = be_bytes_w(AW); B16 = be_bytes_w(BW)
    aw, bw = words_of(A16), words_of(B16)
    ah, bh = halfs_of(A16), halfs_of(B16)

    # ---- integer add/sub, word (vadduwm / vsubuwm) ----
    _bin_vcase("vadduwm canary", 128, A16, B16,
               be_bytes_w([(aw[i] + bw[i]) & MASK32 for i in range(4)]))
    _bin_vcase("vsubuwm canary", 1152, A16, B16,
               be_bytes_w([(aw[i] - bw[i]) & MASK32 for i in range(4)]))
    # ---- integer add/sub, halfword (vadduhm / vsubuhm) ----
    _bin_vcase("vadduhm canary", 64, A16, B16,
               be_bytes_h([(ah[i] + bh[i]) & 0xFFFF for i in range(8)]))
    _bin_vcase("vsubuhm canary", 1088, A16, B16,
               be_bytes_h([(ah[i] - bh[i]) & 0xFFFF for i in range(8)]))
    # ---- min/max word/half (byte-reversal would pick the wrong element) ----
    _bin_vcase("vmaxuw canary", 130, A16, B16,
               be_bytes_w([max(aw[i], bw[i]) for i in range(4)]))
    _bin_vcase("vminuh canary", 578, A16, B16,
               be_bytes_h([min(ah[i], bh[i]) for i in range(8)]))
    def s32(x): return x - (1 << 32) if x >> 31 else x
    _bin_vcase("vmaxsw canary", 386, A16, B16,
               be_bytes_w([(max(s32(aw[i]), s32(bw[i]))) & MASK32 for i in range(4)]))

    # ---- FP add/sub/mul-add/min/max ----
    FA = [1.5, -2.25, 100.0, -0.5]; FB = [0.5, 4.0, -50.0, 8.0]
    FAb = be_bytes_f(FA); FBb = be_bytes_f(FB)
    fa, fb = floats_of(FAb), floats_of(FBb)
    _bin_vcase("vaddfp canary", 10, FAb, FBb,
               be_bytes_f([fa[i] + fb[i] for i in range(4)]))
    _bin_vcase("vsubfp canary", 74, FAb, FBb,
               be_bytes_f([fa[i] - fb[i] for i in range(4)]))
    _bin_vcase("vmaxfp canary", 1034, FAb, FBb,
               be_bytes_f([max(fa[i], fb[i]) for i in range(4)]))
    _bin_vcase("vminfp canary", 1098, FAb, FBb,
               be_bytes_f([min(fa[i], fb[i]) for i in range(4)]))
    # vmaddfp: VA-form vD=vA*vC+vB (operand order vD,vA,vC,vB). Three loads.
    FC = [2.0, 0.5, 1.0, -1.0]; FCb = be_bytes_f(FC); fc = floats_of(FCb)
    prog = [lvx_word(0, 0, 10), lvx_word(1, 0, 11), lvx_word(3, 0, 12),
            va_form(46, 2, 0, 1, 3), stvx_word(2, 0, 13)]  # vmaddfp v2,v0,v3(=vC r12),v1(=vB r11)
    # careful: encoding is vmaddfp vD,vA,vC,vB => fields vA=v0, vC(frc)=?, vB=?
    # va_form(xo, vd, va, vb, vc): bits vA, vB, vC(bit6). vmaddfp semantics use
    # vA*vC+vB. Put vA=v0(r10), vC=v3(r12), vB=v1(r11).
    prog = [lvx_word(0, 0, 10), lvx_word(1, 0, 11), lvx_word(3, 0, 12),
            va_form(46, 2, 0, 1, 3), stvx_word(2, 0, 13)]
    res = be_bytes_f([struct.unpack(">f", struct.pack(">f", fa[i] * fc[i] + fb[i]))[0] for i in range(4)])
    vcase("vmaddfp canary", prog, {A_ADDR: FAb, B_ADDR: FBb, C_ADDR: FCb},
          {R_ADDR: res})

    # ---- FP compares + CR6 dot forms (finding 4) ----
    # vcmpeqfp.: choose vectors with a mix so CR6 = mixed (nibble 0).
    EQa = be_bytes_f([1.0, 2.0, 3.0, 4.0]); EQb = be_bytes_f([1.0, 9.0, 3.0, 9.0])
    ea, eb = floats_of(EQa), floats_of(EQb)
    _bin_vcase("vcmpeqfp. mixed", 198, EQa, EQb,
               be_bytes_w([0xFFFFFFFF if ea[i] == eb[i] else 0 for i in range(4)]),
               form="vxr", rc=1, exp_cr=(0, 4))   # mixed -> nibble 0 at CR6 (bits 4-7)
    ALLa = be_bytes_f([1.0, 2.0, 3.0, 4.0])
    _bin_vcase("vcmpeqfp. all-true", 198, ALLa, ALLa,
               be_bytes_w([0xFFFFFFFF] * 4), form="vxr", rc=1, exp_cr=(8, 4))
    NEa = be_bytes_f([1.0, 2.0, 3.0, 4.0]); NEb = be_bytes_f([5.0, 6.0, 7.0, 8.0])
    _bin_vcase("vcmpeqfp. all-false", 198, NEa, NEb,
               be_bytes_w([0] * 4), form="vxr", rc=1, exp_cr=(2, 4))
    # vcmpgefp. all-true (a>=b for all)
    GEa = be_bytes_f([5.0, 6.0, 7.0, 8.0]); GEb = be_bytes_f([1.0, 6.0, 3.0, 4.0])
    ga, gb = floats_of(GEa), floats_of(GEb)
    _bin_vcase("vcmpgefp. all-true", 454, GEa, GEb,
               be_bytes_w([0xFFFFFFFF if ga[i] >= gb[i] else 0 for i in range(4)]),
               form="vxr", rc=1, exp_cr=(8, 4))
    # vcmpbfp.: all in-bounds (|a|<=|b|) => CR6 nibble 2; result lanes 0.
    Ba = be_bytes_f([1.0, -2.0, 3.0, -4.0]); Bb = be_bytes_f([2.0, 3.0, 4.0, 5.0])
    _bin_vcase("vcmpbfp. all-in-bounds", 966, Ba, Bb,
               be_bytes_w([0, 0, 0, 0]), form="vxr", rc=1, exp_cr=(2, 4))
    # vcmpbfp. one out-of-bounds (a>b) => bit0 set that lane; CR6 nibble 0.
    Bc = be_bytes_f([9.0, -2.0, 3.0, -4.0])
    _bin_vcase("vcmpbfp. one-oob", 966, Bc, Bb,
               be_bytes_w([0x80000000, 0, 0, 0]), form="vxr", rc=1, exp_cr=(0, 4))

    # ---- integer compares + CR6 dot ----
    _bin_vcase("vcmpequw. all-false", 134, A16, B16,
               be_bytes_w([0xFFFFFFFF if aw[i] == bw[i] else 0 for i in range(4)]),
               form="vxr", rc=1, exp_cr=(2, 4))
    _bin_vcase("vcmpequw. all-true", 134, A16, A16,
               be_bytes_w([0xFFFFFFFF] * 4), form="vxr", rc=1, exp_cr=(8, 4))
    # vcmpgtsw. signed: pick so half true -> mixed nibble 0; validates the
    # big-endian lane read (a byte-reversal would flip the comparison).
    GTa = be_bytes_w([5, 0xFFFFFFFF, 100, 1])   # {5, -1, 100, 1}
    GTb = be_bytes_w([2, 3, 100, 0xFFFFFFF0])   # {2, 3, 100, -16}
    gta = [s32(x) for x in words_of(GTa)]; gtb = [s32(x) for x in words_of(GTb)]
    _bin_vcase("vcmpgtsw. mixed", 902, GTa, GTb,
               be_bytes_w([0xFFFFFFFF if gta[i] > gtb[i] else 0 for i in range(4)]),
               form="vxr", rc=1, exp_cr=(0, 4))
    # vcmpgtsh. signed halfword (validates half-lane BE read)
    def s16(x): return x - (1 << 16) if x >> 15 else x
    HTa = be_bytes_h([1, 0xFFFF, 100, 5, 0x7FFF, 0x8000, 0, 3])
    HTb = be_bytes_h([2, 0xFFFE, 100, 4, 0, 0, 0, 3])
    hta = [s16(x) for x in halfs_of(HTa)]; htb = [s16(x) for x in halfs_of(HTb)]
    _bin_vcase("vcmpgtsh. mixed", 838, HTa, HTb,
               be_bytes_h([0xFFFF if hta[i] > htb[i] else 0 for i in range(8)]),
               form="vxr", rc=1, exp_cr=(0, 4))
    # vcmpgt[su]b byte lanes (regression guard: the s8 VMX batch left
    # union-member access on the now-void* locals in the w==1 branch --
    # caught only at relift compile because no byte-form case existed)
    def s8v(x): return x - (1 << 8) if x >> 7 else x
    BTa = bytes([1, 0xFF, 100, 5, 0x7F, 0x80, 0, 3, 9, 0xFE, 2, 2, 0x81, 0x40, 0xC0, 0])
    BTb = bytes([2, 0xFE, 100, 4, 0, 0, 0, 3, 8, 0xFF, 3, 1, 0x80, 0x41, 0xBF, 0])
    _bin_vcase("vcmpgtsb mixed", 774, BTa, BTb,
               bytes([0xFF if s8v(BTa[i]) > s8v(BTb[i]) else 0 for i in range(16)]),
               form="vxr", rc=0)
    _bin_vcase("vcmpgtub. mixed", 518, BTa, BTb,
               bytes([0xFF if BTa[i] > BTb[i] else 0 for i in range(16)]),
               form="vxr", rc=1, exp_cr=(0, 4))

    # ---- splat-immediate (word/halfword lanes) ----
    # vspltisw v2, -8 : all four words = 0xFFFFFFF8. vD,SIMM (SIMM in vA field).
    def vspltis_word(xo, vd, simm5):
        return (4 << 26) | (vd << 21) | ((simm5 & 0x1F) << 16) | (0 << 11) | xo
    prog = [vspltis_word(908, 2, -8 & 0x1F), stvx_word(2, 0, 13)]
    vcase("vspltisw canary", prog, {}, {R_ADDR: be_bytes_w([0xFFFFFFF8] * 4)})
    prog = [vspltis_word(844, 2, -8 & 0x1F), stvx_word(2, 0, 13)]  # vspltish
    vcase("vspltish canary", prog, {}, {R_ADDR: be_bytes_h([0xFFF8] * 8)})

    # ---- even/odd halfword multiply ----
    MA = be_bytes_h([2, 3, 4, 5, 6, 7, 8, 9]); MB = be_bytes_h([10, 11, 12, 13, 14, 15, 16, 17])
    ma, mb2 = halfs_of(MA), halfs_of(MB)
    # disasm XOs: vmulouh=72 (odd), vmuleuh=584 (even), vmulosh=328 (odd signed)
    _bin_vcase("vmulouh canary", 72, MA, MB,
               be_bytes_w([ma[2*i+1] * mb2[2*i+1] for i in range(4)]))
    _bin_vcase("vmuleuh canary", 584, MA, MB,
               be_bytes_w([ma[2*i] * mb2[2*i] for i in range(4)]))
    _bin_vcase("vmulosh canary", 328, MA, MB,
               be_bytes_w([(s16(ma[2*i+1]) * s16(mb2[2*i+1])) & MASK32 for i in range(4)]))

    # ---- int<->float convert ----
    IV = be_bytes_w([1, 0xFFFFFFFF, 256, 0x7FFFFFFF])   # signed {1,-1,256,maxint}
    iv = [s32(x) for x in words_of(IV)]
    # vcfsx v2, v0, 0 : UIMM in vB field. vD, vB, UIMM. Encode UIMM(=0) in vA slot.
    def vcfx_word(xo, vd, uimm, vb):
        return (4 << 26) | (vd << 21) | ((uimm & 0x1F) << 16) | (vb << 11) | xo
    prog = [lvx_word(0, 0, 10), vcfx_word(842, 2, 0, 0), stvx_word(2, 0, 13)]  # vcfsx
    vcase("vcfsx canary", prog, {A_ADDR: IV},
          {R_ADDR: be_bytes_f([float(iv[i]) for i in range(4)])})
    # vctsxs v2, v0, 0 (float->int saturate). Feed values incl. overflow + NaN.
    FV = be_bytes_f([2.7, -3.9, 1e30, float("nan")])
    prog = [lvx_word(0, 0, 10), vcfx_word(970, 2, 0, 0), stvx_word(2, 0, 13)]  # vctsxs
    def sat_cvt(v):
        if v != v: return 0
        if v >= 2147483647.0: return 0x7FFFFFFF
        if v <= -2147483648.0: return 0x80000000
        return int(v) & MASK32
    vfv = floats_of(FV)
    vcase("vctsxs canary", prog, {A_ADDR: FV},
          {R_ADDR: be_bytes_w([sat_cvt(vfv[i]) for i in range(4)])})

    # ---- pack / unpack ----
    # vpkshus: signed halfword -> unsigned byte saturate. a lanes -> bytes 0-7.
    PA = be_bytes_h([0, 300, 0xFFFF, 100, 200, 0x8000, 255, 256])   # signed
    PB = be_bytes_h([1, 2, 3, 4, 5, 6, 7, 8])
    pa = [s16(x) for x in halfs_of(PA)]; pb = [s16(x) for x in halfs_of(PB)]
    def clampu8(v): return 255 if v > 255 else (0 if v < 0 else v)
    res = bytes([clampu8(pa[i]) for i in range(8)] + [clampu8(pb[i]) for i in range(8)])
    _bin_vcase("vpkshus canary", 270, PA, PB, res)
    # vupkhsh: sign-extend high 4 halfwords -> 4 words.
    UA = be_bytes_h([0xFFFF, 0x7FFF, 0x8000, 1, 5, 6, 7, 8])
    ua = halfs_of(UA)
    prog = [lvx_word(0, 0, 10), vx_form(590, 2, 0, 0), stvx_word(2, 0, 13)]  # vupkhsh vD,vB (xo 590)
    vcase("vupkhsh canary", prog, {A_ADDR: UA},
          {R_ADDR: be_bytes_w([s16(ua[i]) & MASK32 for i in range(4)])})
    # vupklsh: sign-extend LOW 4 halfwords (storage idx 4-7) -> 4 words.
    # Reuse UA/ua (mixed pos/neg lanes: 0x7FFF, 0x8000, 1, 5, 6, 7, 8 at low
    # idx 4-7 = {5,6,7,8}, all positive -- add a case with a negative lane
    # in the low half too so the sign-extension direction is unambiguous.
    prog = [lvx_word(0, 0, 10), vx_form(718, 2, 0, 0), stvx_word(2, 0, 13)]  # vupklsh vD,vB (xo 718)
    vcase("vupklsh canary (positive low lanes)", prog, {A_ADDR: UA},
          {R_ADDR: be_bytes_w([s16(ua[4 + i]) & MASK32 for i in range(4)])})
    UA2 = be_bytes_h([1, 2, 3, 4, 0xFFFF, 0x7FFF, 0x8000, 42])
    ua2 = halfs_of(UA2)
    prog = [lvx_word(0, 0, 10), vx_form(718, 2, 0, 0), stvx_word(2, 0, 13)]
    vcase("vupklsh canary (mixed sign low lanes)", prog, {A_ADDR: UA2},
          {R_ADDR: be_bytes_w([s16(ua2[4 + i]) & MASK32 for i in range(4)])})

    # vupkhsb: sign-extend HIGH 8 signed bytes (storage idx 0-7) -> 8 halfwords.
    # Mixed positive/negative/boundary byte lanes across the whole 16-byte
    # vector so both halves (high used, low ignored) are distinguishable.
    UB = bytes([0x7F, 0x80, 0xFF, 1, 5, 6, 100, 200,      # high 8 (used)
                9, 10, 11, 12, 13, 14, 15, 16])            # low 8 (ignored)
    prog = [lvx_word(0, 0, 10), vx_form(526, 2, 0, 0), stvx_word(2, 0, 13)]  # vupkhsb vD,vB (xo 526)
    vcase("vupkhsb canary", prog, {A_ADDR: UB},
          {R_ADDR: be_bytes_h([s8v(UB[i]) & 0xFFFF for i in range(8)])})

    # vupklsb: sign-extend LOW 8 signed bytes (storage idx 8-15) -> 8 halfwords.
    UB2 = bytes([1, 2, 3, 4, 5, 6, 7, 8,                       # high 8 (ignored)
                 0x7F, 0x80, 0xFF, 100, 200, 0, 0x81, 0x7E])   # low 8 (used, mixed sign)
    prog = [lvx_word(0, 0, 10), vx_form(654, 2, 0, 0), stvx_word(2, 0, 13)]  # vupklsb vD,vB (xo 654)
    vcase("vupklsb canary", prog, {A_ADDR: UB2},
          {R_ADDR: be_bytes_h([s8v(UB2[8 + i]) & 0xFFFF for i in range(8)])})

    # ---- word shifts ----
    SA = be_bytes_w([0x00000001, 0x80000000, 0x0000000F, 0xF0000000])
    SB = be_bytes_w([4, 1, 8, 4])
    sa, sb = words_of(SA), words_of(SB)
    _bin_vcase("vslw canary", 388, SA, SB,
               be_bytes_w([(sa[i] << (sb[i] & 31)) & MASK32 for i in range(4)]))
    _bin_vcase("vsraw canary", 900, SA, SB,
               be_bytes_w([(s32(sa[i]) >> (sb[i] & 31)) & MASK32 for i in range(4)]))

    # ---- byte-order-agnostic sanity: vperm/vsldoi/vmrghw must be UNCHANGED by
    # the endianness work (pure byte permutations). ----
    # vsldoi v2,v0,v1,4  (shift the 32-byte concat left by 4 => bytes 4..19)
    def vsldoi_word(vd, va, vb, shb):
        return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | ((shb & 0xF) << 6) | 44
    concat = A16 + B16
    prog = [lvx_word(0, 0, 10), lvx_word(1, 0, 11), vsldoi_word(2, 0, 1, 4), stvx_word(2, 0, 13)]
    vcase("vsldoi byte-sanity", prog, {A_ADDR: A16, B_ADDR: B16},
          {R_ADDR: concat[4:20]})
    # vmrghw: high words interleaved {a0,b0,a1,b1}
    _bin_vcase("vmrghw byte-sanity", 140, A16, B16,
               be_bytes_w([aw[0], bw[0], aw[1], bw[1]]))

build_cases()
build_vcases()

# ---------------------------------------------------------------------------
# VMX vector-op SEMANTIC tranche (2026-07-04): the suite tested VMX endianness
# canaries but never the high-frequency vector ops' semantics. These are the
# untested, byte-order-sensitive, historically-buggy class (the VMX opcode
# scramble; the lane-endianness batch) and they sit on the render critical
# path. Each case round-trips through g_vm_stub (lvx inputs -> op -> stvx) and
# byte-checks the result against reference semantics. XOs verified vs RPCS3
# PPUOpcodes.h (VADDFP 0xa, VPERM 0x2b, VSLDOI 0x2c, VMADDFP 0x2e, VSUBFP 0x4a,
# VSPLTW 0x28c, VCFSX 0x34a, VSPLTISW 0x38c).
# ---------------------------------------------------------------------------
def _tri_vcase(name, xo6, a, b, c, result_bytes):   # vD=v3, vA=v0, vB=v1, vC=v2
    opw = va_form(xo6, 3, 0, 1, 2)
    prog = [lvx_word(0,0,10), lvx_word(1,0,11), lvx_word(2,0,12), opw, stvx_word(3,0,13)]
    vcase(name, prog, {A_ADDR:a, B_ADDR:b, C_ADDR:c}, {R_ADDR:result_bytes})

def _s5(v): return v - 32 if (v & 0x10) else v      # sign-extend 5-bit immediate

_VA = be_bytes_w([0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F])
_VB = be_bytes_w([0x10111213, 0x14151617, 0x18191A1B, 0x1C1D1E1F])

# vperm: index bytes span the 32-byte (vA:vB) source, incl. cross-boundary + wrap
_PC = bytes([0,15,16,31, 5,20,1,30, 0x23&0x1F,0x38&0x1F,10,25, 3,18,7,22])
_SRC = _VA + _VB
_tri_vcase("vperm bytes", 0x2b, _VA, _VB, _PC,
           bytes(_SRC[_PC[i] & 0x1F] for i in range(16)))

# vsldoi SHB octets: high 16 bytes of (vA:vB) << SHB*8
for _shb in (0, 5, 15):
    _op = va_form(0x2c, 3, 0, 1, _shb)
    vcase(f"vsldoi shb={_shb}",
          [lvx_word(0,0,10), lvx_word(1,0,11), _op, stvx_word(3,0,13)],
          {A_ADDR:_VA, B_ADDR:_VB}, {R_ADDR:(_VA+_VB)[_shb:_shb+16]})

# vspltw: splat word UIMM of vB across all four words
for _u in (0, 2, 3):
    _w = words_of(_VB)[_u]
    vcase(f"vspltw w={_u}",
          [lvx_word(1,0,11), vx_form(0x28c, 3, _u, 1), stvx_word(3,0,13)],
          {B_ADDR:_VB}, {R_ADDR:be_bytes_w([_w]*4)})

# vspltisw: splat sign-extended 5-bit immediate as 32-bit
for _s in (1, 15, 16, 31):    # 16 and 31 are negative after sign-extend
    _v = _s5(_s) & MASK32
    vcase(f"vspltisw simm={_s}",
          [vx_form(0x38c, 3, _s, 0), stvx_word(3,0,13)],
          {}, {R_ADDR:be_bytes_w([_v]*4)})

# vaddfp / vsubfp: per-lane single-precision (values exact in single -> no rounding noise)
_FA = be_bytes_f([1.5, -2.0, 3.25, 100.0]); _FB = be_bytes_f([0.5, 2.0, 0.75, -4.0])
_bin_vcase("vaddfp", 0xa, _FA, _FB, be_bytes_f([2.0, 0.0, 4.0, 96.0]))
_bin_vcase("vsubfp", 0x4a, _FA, _FB, be_bytes_f([1.0, -4.0, 2.5, 104.0]))

# vmaddfp: vD = vA*vC + vB  (guards the operand order -- a classic FMA bug)
_FC = be_bytes_f([2.0, -1.0, 4.0, 0.5])
_tri_vcase("vmaddfp A*C+B", 0x2e, _FA, _FB, _FC, be_bytes_f([3.5, 4.0, 13.75, 46.0]))

# vcfsx: float(signed word) / 2^UIMM
_IV = be_bytes_w([8, (-16) & MASK32, 100, (-4) & MASK32])
for _u in (0, 2):
    vcase(f"vcfsx u={_u}",
          [lvx_word(1,0,11), vx_form(0x34a, 3, _u, 1), stvx_word(3,0,13)],
          {B_ADDR:_IV}, {R_ADDR:be_bytes_f([float(s32(w))/(1<<_u) for w in words_of(_IV)])})

# ---------------------------------------------------------------------------
# lmw / stmw (opcodes 46/47, PowerISA_V2.03_Final_Public.pdf p.72, Book I
# section 3.3.5). Both were previously unimplemented (lifter fell through to
# the `/* TODO: lmw ... */` catch-all). Round-trip through the SAME vm stub
# used by lwz/stw: for lmw, memory -> GPRs r28..r31 -> stw each back out for a
# byte-exact check; for stmw, lwz each GPR in -> stmw all four out. r28..r31
# keep clear of the r10-r13 base-pointer GPRs the vcase harness reserves.
# ---------------------------------------------------------------------------
_MW_DATA = be_bytes_w([0x11223344, 0xAABBCCDD, 0x00000001, 0xFFFFFFFF])

vcase("lmw r28,0(r10) then stw back",
      [d_form(46, 28, 10, 0),          # lmw r28, 0(r10)
       d_form(36, 28, 13, 0),          # stw r28, 0(r13)
       d_form(36, 29, 13, 4),          # stw r29, 4(r13)
       d_form(36, 30, 13, 8),          # stw r30, 8(r13)
       d_form(36, 31, 13, 12)],        # stw r31, 12(r13)
      {A_ADDR: _MW_DATA}, {R_ADDR: _MW_DATA})

vcase("stmw r28,0(r13) after lwz",
      [d_form(32, 28, 10, 0),          # lwz r28, 0(r10)
       d_form(32, 29, 10, 4),          # lwz r29, 4(r10)
       d_form(32, 30, 10, 8),          # lwz r30, 8(r10)
       d_form(32, 31, 10, 12),         # lwz r31, 12(r10)
       d_form(47, 28, 13, 0)],         # stmw r28, 0(r13)
      {A_ADDR: _MW_DATA}, {R_ADDR: _MW_DATA})

# ---------------------------------------------------------------------------
# CR-logical ops (opcode 19). REGRESSION for the crnor/crnand disasm swap
# (2026-07-04): ppu_disasm.py had opcodes 33/225 mapped to crnand/crnor
# (should be crnor/crnand), and there was ZERO coverage, so 987 mis-lifted
# sites (inverted NAND<->NOR logic) went undetected through 11 audits. These
# cases set CR bit 4 = 1 and CR bit 8 = 0 (differing inputs, so AND/OR/NAND/NOR
# all give distinct results) and land the result bit in the CR0 nibble.
# ---------------------------------------------------------------------------
def _cr_word(xo, bt, ba, bb):
    return (19 << 26) | (bt << 21) | (ba << 16) | (bb << 11) | (xo << 1)

_CR_IN = 1 << 27   # host bit 27 => CR bit 4 = 1 (ba); CR bit 8 = 0 (bb)
for _nm, _xo, _res in [("crand", 257, 0), ("cror", 449, 1), ("crxor", 193, 1),
                       ("crnand", 225, 1), ("crnor", 33, 0), ("creqv", 289, 0),
                       ("crandc", 129, 1), ("crorc", 417, 1)]:
    # bt=0 -> host bit 31 -> top bit of the CR0 nibble (shift 28).
    case(f"{_nm} bt0 a=1 b=0", _cr_word(_xo, 0, 4, 8),
         {}, [], in_cr=_CR_IN, exp_cr=(0x8 if _res else 0x0, 28))

# Hard disassembler-table guard: the cases above only SKIP (mnemonic mismatch)
# on a swap, so assert the table directly -- a re-swap of crnor/crnand (or any
# CR-logical opcode) is a clean, loud failure, not a silent skip.
for _nm, _xo in [("crnor", 33), ("crnand", 225), ("crand", 257), ("cror", 449),
                 ("crxor", 193), ("creqv", 289), ("crandc", 129), ("crorc", 417)]:
    _got = ppu_disasm.decode(_cr_word(_xo, 0, 4, 8), 0).mnemonic
    assert _got == _nm, (f"DISASM TABLE BUG (ppu_disasm.py cr_ops): CR-logical opcode "
                         f"{_xo} decodes as {_got!r}, must be {_nm!r} (PowerISA / RPCS3 "
                         f"PPUOpcodes.h). The crnor/crnand swap cost sessions 5-9.")

# ---------------------------------------------------------------------------
# T7: semantic fuzzer mode. Generates N deterministic cases per covered family
# on top of the SAME encoders/refs/case() infra above -- not a new harness.
# Cases land in FUZZ_CASES (module CASES is untouched, so the default
# no-flag run stays byte-identical). Failure output reuses check_reg/
# check_ca/check_cr, which already print the encoded case name; the fuzz
# case names embed the ENCODED WORD hex + decoded mnemonic/operands so a
# FAIL line alone is enough to file a report without rerunning (spec: T7).
# ---------------------------------------------------------------------------

FUZZ_CASES = []

# Edge-biased operand pool (T7 spec): zeros/all-ones/signed-boundary/word-
# split values, shared by every family below.
_EDGE_POOL = [
    0, 1, -1 & MASK64, 2,
    0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
    0x7FFFFFFFFFFFFFFF, 0x8000000000000000, MASK64,
    0x7FFF, 0x8000, 0xFFFF,                       # halfword sign boundary
    0x7F, 0x80, 0xFF,                             # byte sign boundary
    0x00000000FFFFFFFF,                           # low word set, high clear
    0xFFFFFFFF00000000,                           # high word set, low clear
    0x100000000,
]

def _fuzz_operand(rng, bits=64):
    """One edge-biased-or-uniform operand, masked to `bits`."""
    if rng.random() < 0.5:
        v = rng.choice(_EDGE_POOL)
    else:
        v = rng.getrandbits(64)
    return v & (MASK64 if bits == 64 else MASK32)

def _fuzz_case(name, word, in_regs, expects, in_ca=None, exp_ca=None, exp_cr=None,
               may_trap=False, in_cr=0, insn=None, exp_crbit=None):
    """Like case() but appends to FUZZ_CASES and stamps the name with the
    encoded word hex + decoded mnemonic/operands (T7: failure output must
    include these without rerunning). exp_crbit=(bit_index, want_bit) is a
    single-CR-bit assertion for arbitrary bt (the static suite's check_cr
    only asserts a fixed 4-bit nibble at bt=0/28, which doesn't generalize
    to fuzzed bt)."""
    if insn is None:
        insn = ppu_disasm.decode(word, 0)
    # emit_c's shared encoding-check compares name.split()[0].rstrip(".") against
    # the decoded mnemonic (also rstripped of "."): lead the tag with the ACTUAL
    # decoded mnemonic (not the family label) so oe=1 (.../ "o" suffix) and rc=1
    # ("." suffix) variants -- which the static suite never generates -- don't
    # false-positive as an "ENCODING mismatch" and get silently skipped.
    tagged = f"{insn.mnemonic} [{name}] word=0x{word:08X} operands={insn.operands}"
    FUZZ_CASES.append(dict(name=tagged, word=word, in_regs=in_regs, expects=expects,
                           in_ca=in_ca, exp_ca=exp_ca, exp_cr=exp_cr, may_trap=may_trap,
                           in_fprs={}, exp_fprs=[], in_cr=in_cr, exp_crbit=exp_crbit))

def build_fuzz_cases(n, seed):
    """Populate FUZZ_CASES. n = generated cases per covered family."""
    rng = random.Random(seed)
    R = 3, 4, 5   # rt, ra, rb -- same fixed slots the static suite uses

    # --- XO-form arithmetic + carry/overflow family (ref_* already exist) --
    xo_ops = [
        ("add", 266, lambda a, b, ca: ref_add(a, b)),
        ("subf", 40, lambda a, b, ca: ref_subf(a, b)),
        ("addc", 10, lambda a, b, ca: ref_addc(a, b)),
        ("subfc", 8, lambda a, b, ca: ref_subfc(a, b)),
        ("adde", 138, lambda a, b, ca: ref_adde(a, b, ca)),
        ("subfe", 136, lambda a, b, ca: ref_subfe(a, b, ca)),
        ("mullw", 235, lambda a, b, ca: ref_mullw(a, b)),
        ("mulld", 233, lambda a, b, ca: ref_mulld(a, b)),
        ("mulhw", 75, lambda a, b, ca: ref_mulhw(a, b)),
        ("mulhwu", 11, lambda a, b, ca: ref_mulhwu(a, b)),
        ("mulhd", 73, lambda a, b, ca: ref_mulhd(a, b)),
        ("mulhdu", 9, lambda a, b, ca: ref_mulhdu(a, b)),
    ]
    for name, xo, ref in xo_ops:
        uses_ca = name in ("adde", "subfe")
        sets_ca = name in ("addc", "subfc", "adde", "subfe")
        is_32bit_result = name in ("mulhw", "mulhwu")   # placed in RT SIGN-EXTENDED
        for i in range(n):
            a, b = _fuzz_operand(rng), _fuzz_operand(rng)
            ca = rng.randint(0, 1) if uses_ca else None
            oe = rng.randint(0, 1)
            rc = rng.randint(0, 1)
            v, cout = ref(a, b, ca or 0)
            mask = MASK32 if is_32bit_result else MASK64
            word = xo_form(xo, R[0], R[1], R[2], oe=oe, rc=0)  # rc handled via CR0 check below
            exp_cr = None
            if rc:
                # CR0 (Rc=1) reflects the full 64-bit RT as this lifter emits it:
                # mulhw/mulhwu sign-extend their 32-bit result into RT (ppu_lifter.py
                # "mulhw"/"mulhwu" handlers), so the nibble must be derived from the
                # SIGN-EXTENDED value, not a raw 32-bit-masked compare (a fuzz-harness
                # bug caught by dogfooding this exact sweep -- s32(v) sign-extends,
                # everything else in this XO family is already a real 64-bit result).
                cr_v = sxw(v) if is_32bit_result else v
                nib = 8 if s64(cr_v) < 0 else (4 if s64(cr_v) > 0 else 2)
                word = xo_form(xo, R[0], R[1], R[2], oe=oe, rc=1)
                exp_cr = (nib, 28)
            _fuzz_case(f"{name} a={a:#x} b={b:#x} ca={ca} oe={oe} rc={rc}", word,
                       {R[1]: a, R[2]: b}, [(R[0], v, mask)],
                       in_ca=ca, exp_ca=(cout if sets_ca else None), exp_cr=exp_cr)

    for name, xo, ref in [("addme", 234, ref_addme), ("addze", 202, ref_addze),
                          ("subfme", 232, ref_subfme), ("subfze", 200, ref_subfze)]:
        for i in range(n):
            a = _fuzz_operand(rng)
            ca = rng.randint(0, 1)
            v, cout = ref(a, ca)
            _fuzz_case(f"{name} a={a:#x} ca={ca}", xo_form(xo, R[0], R[1], 0),
                       {R[1]: a}, [(R[0], v, MASK64)], in_ca=ca, exp_ca=cout)

    for i in range(n):
        a = _fuzz_operand(rng)
        v, _ = ref_neg(a)
        _fuzz_case(f"neg a={a:#x}", xo_form(104, R[0], R[1], 0), {R[1]: a}, [(R[0], v, MASK64)])

    # --- X-form logicals / extends / counts --------------------------------
    for name, xo in [("and", 28), ("or", 444), ("xor", 316), ("nand", 476),
                     ("nor", 124), ("andc", 60), ("orc", 412), ("eqv", 284)]:
        for i in range(n):
            a, b = _fuzz_operand(rng), _fuzz_operand(rng)
            v, _ = ref_logic(name, a, b)
            _fuzz_case(f"{name} a={a:#x} b={b:#x}", x_logic(xo, R[0], R[1], R[2]),
                       {R[0]: a, R[2]: b}, [(R[1], v, MASK64)])

    # sign-extension forms (the il double-sign-extension class analog)
    for name, xo, ref in [("extsb", 954, ref_extsb), ("extsh", 922, ref_extsh),
                          ("extsw", 986, ref_extsw),
                          ("cntlzw", 26, ref_cntlzw), ("cntlzd", 58, ref_cntlzd)]:
        for i in range(n):
            a = _fuzz_operand(rng)
            v, _ = ref(a)
            _fuzz_case(f"{name} a={a:#x}", x_logic(xo, R[0], R[1], 0),
                       {R[0]: a}, [(R[1], v, MASK64)])

    # D-form immediates (the il class analog: signed 16-bit imm, single
    # sign-extension only -- a double sign-extension bug shows up here).
    for i in range(n):
        a = _fuzz_operand(rng)
        imm = rng.randint(-0x8000, 0x7FFF)
        v, _ = ref_addi(a, imm)
        _fuzz_case(f"addi a={a:#x} imm={imm}", d_form(14, R[0], R[1], imm),
                   {R[1]: a}, [(R[0], v, MASK64)])
        v, _ = ref_addi(a, imm << 16)
        _fuzz_case(f"addis a={a:#x} imm={imm}", d_form(15, R[0], R[1], imm),
                   {R[1]: a}, [(R[0], v, MASK64)])
        v, ca = ref_addic(a, imm if imm >= 0 else (imm & MASK64))
        _fuzz_case(f"addic a={a:#x} imm={imm}", d_form(12, R[0], R[1], imm),
                   {R[1]: a}, [(R[0], v, MASK64)], exp_ca=ca)
        v, ca = ref_subfic(a, imm if imm >= 0 else (imm & MASK64))
        _fuzz_case(f"subfic a={a:#x} imm={imm}", d_form(8, R[0], R[1], imm),
                   {R[1]: a}, [(R[0], v, MASK64)], exp_ca=ca)
        v, _ = ref_mulli(a, imm)
        _fuzz_case(f"mulli a={a:#x} imm={imm}", d_form(7, R[0], R[1], imm),
                   {R[1]: a}, [(R[0], v, MASK64)])

    # --- 32/64-bit rotates -----------------------------------------------
    for i in range(n):
        a = _fuzz_operand(rng, 32)
        sh, mb, me = rng.randint(0, 31), rng.randint(0, 31), rng.randint(0, 31)
        v, _ = ref_rlwinm(a, sh, mb, me)
        _fuzz_case(f"rlwinm a={a:#x} sh={sh} mb={mb} me={me}",
                   m_form(21, R[0], R[1], sh, mb, me), {R[0]: a}, [(R[1], v, MASK64)])
        r0 = _fuzz_operand(rng, 64)
        v2, _ = ref_rlwimi(r0, a, sh, mb, me)
        _fuzz_case(f"rlwimi a={a:#x} sh={sh} mb={mb} me={me}",
                   m_form(20, R[0], R[1], sh, mb, me),
                   {R[0]: a, R[1]: r0}, [(R[1], v2, MASK64)])

    for i in range(n):
        a = _fuzz_operand(rng, 64)
        sh = rng.randint(0, 63)
        mbe = rng.randint(0, 63)
        v, _ = ref_rldicl(a, sh, mbe)
        _fuzz_case(f"rldicl a={a:#x} sh={sh} mb={mbe}", md_form(0, R[0], R[1], sh, mbe),
                   {R[0]: a}, [(R[1], v, MASK64)])
        v, _ = ref_rldicr(a, sh, mbe)
        _fuzz_case(f"rldicr a={a:#x} sh={sh} me={mbe}", md_form(1, R[0], R[1], sh, mbe),
                   {R[0]: a}, [(R[1], v, MASK64)])
        sh2 = rng.randint(0, 63)
        mb = rng.randint(0, 63 - sh2)
        r0 = _fuzz_operand(rng, 64)
        v3, _ = ref_rldimi(r0, a, sh2, mb)
        _fuzz_case(f"rldimi a={a:#x} sh={sh2} mb={mb}", md_form(3, R[0], R[1], sh2, mb),
                   {R[0]: a, R[1]: r0}, [(R[1], v3, MASK64)])

    # --- 32/64-bit shifts ----------------------------------------------------
    for name, xo, ref in [("slw", 24, ref_slw), ("srw", 536, ref_srw),
                          ("sld", 27, ref_sld), ("srd", 539, ref_srd)]:
        bits = 32 if name in ("slw", "srw") else 64
        for i in range(n):
            a = _fuzz_operand(rng, bits)
            nmax = 63 if bits == 32 else 127
            nshift = rng.choice([0, 1, bits - 1, bits, bits + 1, nmax]) if rng.random() < 0.5 \
                else rng.randint(0, nmax)
            v, _ = ref(a, nshift)
            _fuzz_case(f"{name} a={a:#x} n={nshift}", x_logic(xo, R[0], R[1], R[2]),
                       {R[0]: a, R[2]: nshift}, [(R[1], v, MASK64)])

    for i in range(n):
        a = _fuzz_operand(rng, 32)
        sh = rng.randint(0, 31)
        v, ca = ref_srawi(a, sh)
        _fuzz_case(f"srawi a={a:#x} sh={sh}",
                   (31 << 26) | (R[0] << 21) | (R[1] << 16) | (sh << 11) | (824 << 1),
                   {R[0]: a}, [(R[1], v, MASK64)], exp_ca=ca)
        nshift = rng.choice([0, 1, 31, 32, 40, 63])
        v, ca = ref_sraw(a, nshift)
        _fuzz_case(f"sraw a={a:#x} n={nshift}", x_logic(792, R[0], R[1], R[2]),
                   {R[0]: a, R[2]: nshift}, [(R[1], v, MASK64)], exp_ca=ca)
    for i in range(n):
        a = _fuzz_operand(rng, 64)
        sh = rng.randint(0, 63)
        v, ca = ref_sradi(a, sh)
        _fuzz_case(f"sradi a={a:#x} sh={sh}", xs_sradi(R[0], R[1], sh),
                   {R[0]: a}, [(R[1], v, MASK64)], exp_ca=ca)

    # --- compares (bf/l randomized; cr0..cr7) --------------------------------
    for i in range(n):
        a, b = _fuzz_operand(rng), _fuzz_operand(rng)
        bf = rng.randint(0, 7)
        shift = 28 - bf * 4
        _fuzz_case(f"cmpd bf={bf} a={a:#x} b={b:#x}", cmp_form(0, bf, 1, R[1], R[2]),
                   {R[1]: a, R[2]: b}, [], exp_cr=(cr_nibble_signed(a, b), shift))
        _fuzz_case(f"cmpw bf={bf} a={a:#x} b={b:#x}", cmp_form(0, bf, 0, R[1], R[2]),
                   {R[1]: a, R[2]: b}, [], exp_cr=(cr_nibble_signed32(a, b), shift))
        _fuzz_case(f"cmpld bf={bf} a={a:#x} b={b:#x}", cmp_form(32, bf, 1, R[1], R[2]),
                   {R[1]: a, R[2]: b}, [], exp_cr=(cr_nibble_unsigned(a, b, 64), shift))
        _fuzz_case(f"cmplw bf={bf} a={a:#x} b={b:#x}", cmp_form(32, bf, 0, R[1], R[2]),
                   {R[1]: a, R[2]: b}, [], exp_cr=(cr_nibble_unsigned(a, b, 32), shift))

    # --- CR-field logic: crand/crnand/cror/crnor/crxor/creqv/crandc/crorc ---
    # (the crnand class -- MANDATORY per T7). Randomize bt/ba/bb bit indices
    # AND the input CR pattern so every operand combination (0,0)/(0,1)/(1,0)/
    # (1,1) gets covered, not just the one fixed pattern the static suite uses.
    _CR_XO = {"crand": 257, "cror": 449, "crxor": 193, "crnand": 225,
              "crnor": 33, "creqv": 289, "crandc": 129, "crorc": 417}
    for name, xo in _CR_XO.items():
        for i in range(n):
            bt = rng.randint(0, 31)
            ba = rng.randint(0, 31)
            bb = rng.randint(0, 31)
            while bb == ba:
                bb = rng.randint(0, 31)
            cr_in = rng.getrandbits(32)
            # force the (ba,bb) bit pair to cover all four combinations across
            # the sweep instead of whatever random bits happened to land there
            want_a, want_b = (i // 1) % 2, (i // 2) % 2
            cr_in = (cr_in & ~(1 << (31 - ba)) & ~(1 << (31 - bb)) & MASK32)
            cr_in |= (want_a << (31 - ba)) | (want_b << (31 - bb))
            _new_cr, rbit = ref_cr_op(name, cr_in, bt, ba, bb)
            _fuzz_case(f"{name} bt={bt} ba={ba} bb={bb} a={want_a} b={want_b} cr_in=0x{cr_in:08X}",
                       _cr_word(xo, bt, ba, bb), {}, [],
                       in_cr=cr_in, exp_crbit=(bt, rbit))

    return FUZZ_CASES

def emit_c(path, cases=None, vcases=None):
    """Generate the C conformance driver. cases/vcases default to the static
    suite's CASES/VCASES (default no-flag run, byte-identical to before this
    parameter existed); --fuzz passes FUZZ_CASES/[] instead."""
    if cases is None:
        cases = CASES
    if vcases is None:
        vcases = VCASES
    lifter = PPULifter()
    dummy = LiftedFunction(name="conf", start_addr=0, end_addr=0x10000)
    pre = lifter._preamble_lines()
    # replace the generated-header include with the real (small) context header
    pre[0] = pre[0].replace(f'#include "{lifter.header_name}"',
                            '#include "ppu_context.h"\n#include <stdint.h>')
    out = ["/* Auto-generated by tools/test_ppu_lift.py -- conformance driver. */"]
    out.append("#define _CRT_SECURE_NO_WARNINGS")
    out.extend(pre)
    out.append("""
static int g_fail = 0, g_pass = 0, g_skip = 0;
static ppu_context g_ctx;

static void check_reg(const char* name, int reg, uint64_t got, uint64_t want, uint64_t mask) {
    if ((got & mask) != (want & mask)) {
        printf("FAIL %s: r%d = 0x%016llX want 0x%016llX (mask 0x%016llX)\\n",
               name, reg, (unsigned long long)got, (unsigned long long)want,
               (unsigned long long)mask);
        g_fail++;
    } else g_pass++;
}
static void check_ca(const char* name, uint32_t xer, int want) {
    int got = (xer >> 29) & 1;
    if (got != want) { printf("FAIL %s: XER[CA] = %d want %d\\n", name, got, want); g_fail++; }
    else g_pass++;
}
static void check_ov(const char* name, uint32_t xer, int want) {
    int got = (xer >> 30) & 1;
    if (got != want) { printf("FAIL %s: XER[OV] = %d want %d\\n", name, got, want); g_fail++; }
    else g_pass++;
}
static void check_so(const char* name, uint32_t xer, int want) {
    int got = (xer >> 31) & 1;
    if (got != want) { printf("FAIL %s: XER[SO] = %d want %d\\n", name, got, want); g_fail++; }
    else g_pass++;
}
static void check_cr(const char* name, uint32_t cr, int nib, int shift) {
    int got = (cr >> shift) & 0xF;
    /* only LT/GT/EQ (top 3 bits of the nibble); SO passthrough not asserted */
    if ((got & 0xE) != (nib & 0xE)) {
        printf("FAIL %s: CR nibble@%d = %X want %X\\n", name, shift, got, nib); g_fail++;
    } else g_pass++;
}
/* Single-CR-bit check (T7 fuzz: CR-logical ops with arbitrary bt can't use
 * check_cr's fixed nibble/shift convention). bit = host CR bit index (0=MSB). */
static void check_crbit(const char* name, uint32_t cr, int bit, int want) {
    int got = (cr >> (31 - bit)) & 1;
    if (got != want) {
        printf("FAIL %s: CR bit %d = %d want %d (full CR=0x%08X)\\n",
               name, bit, got, want, cr);
        g_fail++;
    } else g_pass++;
}
static void check_fpr(const char* name, int reg, double got_d, uint64_t want, uint64_t mask) {
    uint64_t got; memcpy(&got, &got_d, 8);
    if ((got & mask) != (want & mask)) {
        printf("FAIL %s: f%d = 0x%016llX want 0x%016llX (mask 0x%016llX)\\n",
               name, reg, (unsigned long long)got, (unsigned long long)want,
               (unsigned long long)mask);
        g_fail++;
    } else g_pass++;
}
/* VMX canary: compare 16 result bytes (big-endian memory) against reference. */
static void check_bytes(const char* name, const uint8_t* got, const uint8_t* want) {
    for (int i = 0; i < 16; i++) {
        if (got[i] != want[i]) {
            printf("FAIL %s: result bytes differ at [%d]: got", name, i);
            for (int j = 0; j < 16; j++) printf(" %02X", got[j]);
            printf(" want");
            for (int j = 0; j < 16; j++) printf(" %02X", want[j]);
            printf("\\n");
            g_fail++;
            return;
        }
    }
    g_pass++;
}
/* vm stubs over a 64 KB scratch page (memory-op cases mask EAs into it).
 * The preamble declares these extern "C"; only the emissions actually used
 * by cases need to resolve, but the whole accessor set is provided. */
#include <stdlib.h>   /* MSVC _byteswap_* */
static uint8_t g_vm_stub[65536];
extern "C" uint8_t* vm_base = g_vm_stub;   /* stvlx-class emissions use it raw */
#define VMOFF(a) ((uint32_t)(a) & 0xFFFFu)
extern "C" uint8_t  vm_read8 (uint64_t a) { return g_vm_stub[VMOFF(a)]; }
extern "C" uint16_t vm_read16(uint64_t a) { uint16_t v; memcpy(&v, g_vm_stub + VMOFF(a), 2); return _byteswap_ushort(v); }
extern "C" uint32_t vm_read32(uint64_t a) { uint32_t v; memcpy(&v, g_vm_stub + VMOFF(a), 4); return _byteswap_ulong(v); }
extern "C" uint64_t vm_read64(uint64_t a) { uint64_t v; memcpy(&v, g_vm_stub + VMOFF(a), 8); return _byteswap_uint64(v); }
extern "C" void vm_write8 (uint64_t a, uint8_t  v) { g_vm_stub[VMOFF(a)] = v; }
extern "C" void vm_write16(uint64_t a, uint16_t v) { v = _byteswap_ushort(v); memcpy(g_vm_stub + VMOFF(a), &v, 2); }
extern "C" void vm_write32(uint64_t a, uint32_t v) { v = _byteswap_ulong(v);  memcpy(g_vm_stub + VMOFF(a), &v, 4); }
extern "C" void vm_write64(uint64_t a, uint64_t v) { v = _byteswap_uint64(v); memcpy(g_vm_stub + VMOFF(a), &v, 8); }
""")
    out.append("int main(void) {")
    out.append("    ppu_context* ctx = &g_ctx;")
    n_encoding_skipped = 0
    for i, c in enumerate(cases):
        insn = ppu_disasm.decode(c["word"], 0x10000 + i * 4)
        exp_mn = c["name"].split()[0].rstrip(".")
        got_mn = insn.mnemonic.rstrip(".")
        if got_mn != exp_mn:
            print(f"ENCODING mismatch for {c['name']!r}: decoded as "
                  f"{insn.mnemonic} {insn.operands} (word {c['word']:#010x}) -- case skipped")
            n_encoding_skipped += 1
            continue
        code = lifter._translate(insn, dummy)
        if code.startswith("/*"):
            print(f"UNHANDLED by lifter: {c['name']!r} -> {code[:60]} -- case skipped")
            n_encoding_skipped += 1
            continue
        nm = c["name"].replace('"', "'")
        out.append(f'    {{ /* case {i}: {nm} | {insn.mnemonic} {insn.operands} */')
        out.append("      memset(ctx, 0, sizeof(*ctx));")
        if c.get("in_cr"):
            out.append(f"      ctx->cr = 0x{c['in_cr']:08X}U;")
        for reg, val in c["in_regs"].items():
            out.append(f"      ctx->gpr[{reg}] = 0x{val:016X}ULL;")
        for reg, pat in c["in_fprs"].items():
            out.append(f"      {{ uint64_t t = 0x{pat:016X}ULL; "
                       f"memcpy(&ctx->fpr[{reg}], &t, 8); }}")
        if c["in_ca"]:
            out.append("      ctx->xer |= (1u << 29);")
        body = [f"        {code}"]
        for reg, val, mask in c["expects"]:
            body.append(f'        check_reg("{nm}", {reg}, ctx->gpr[{reg}], '
                        f"0x{val:016X}ULL, 0x{mask:016X}ULL);")
        for reg, pat, mask in c["exp_fprs"]:
            body.append(f'        check_fpr("{nm}", {reg}, ctx->fpr[{reg}], '
                        f"0x{pat:016X}ULL, 0x{mask:016X}ULL);")
        if c["exp_ca"] is not None:
            body.append(f'        check_ca("{nm}", ctx->xer, {int(bool(c["exp_ca"]))});')
        if c.get("exp_ov") is not None:
            body.append(f'        check_ov("{nm}", ctx->xer, {int(bool(c["exp_ov"]))});')
        if c.get("exp_so") is not None:
            body.append(f'        check_so("{nm}", ctx->xer, {int(bool(c["exp_so"]))});')
        if c["exp_cr"] is not None:
            nib, shift = c["exp_cr"]
            body.append(f'        check_cr("{nm}", ctx->cr, {nib}, {shift});')
        if c.get("exp_crbit") is not None:
            bit, want = c["exp_crbit"]
            body.append(f'        check_crbit("{nm}", ctx->cr, {bit}, {want});')
        if c["may_trap"]:
            out.append("      __try {")
            out.extend(body)
            out.append("        g_pass++;")
            out.append("      } __except (1) {")
            out.append(f'        printf("FAIL {nm}: HOST TRAP (div?) -- lifted code must not fault\\n");')
            out.append("        g_fail++;")
            out.append("      }")
        else:
            out.extend(body)
        out.append("    }")

    # --- VMX endianness canary tranche (memory round-trips) ---
    for vi, vc in enumerate(vcases):
        # Lift each program word; skip the whole case if any word is unhandled.
        lifted = []
        skip = False
        for k, word in enumerate(vc["prog"]):
            insn = ppu_disasm.decode(word, 0x20000 + (vi * 16 + k) * 4)
            code = lifter._translate(insn, dummy)
            if code.startswith("/*"):
                print(f"UNHANDLED by lifter in VMX case {vc['name']!r}: "
                      f"{insn.mnemonic} {insn.operands} -> {code[:50]} -- case skipped")
                skip = True
                break
            lifted.append((insn, code))
        if skip:
            n_encoding_skipped += 1
            continue
        nm = vc["name"].replace('"', "'")
        out.append(f'    {{ /* vcase {vi}: {nm} */')
        out.append("      memset(ctx, 0, sizeof(*ctx));")
        out.append("      memset(g_vm_stub, 0, sizeof(g_vm_stub));")
        # base pointers: r10=A, r11=B, r12=C, r13=result
        out.append(f"      ctx->gpr[10] = 0x{A_ADDR:X}ULL; ctx->gpr[11] = 0x{B_ADDR:X}ULL;")
        out.append(f"      ctx->gpr[12] = 0x{C_ADDR:X}ULL; ctx->gpr[13] = 0x{R_ADDR:X}ULL;")
        for addr, blob in vc["vin"].items():
            byts = ", ".join(f"0x{b:02X}" for b in blob)
            out.append(f"      {{ static const uint8_t _in[16] = {{ {byts} }}; "
                       f"memcpy(g_vm_stub + 0x{addr:X}, _in, 16); }}")
        for insn, code in lifted:
            out.append(f"      /* {insn.mnemonic} {insn.operands} */")
            out.append(f"      {code}")
        for addr, blob in vc["vout"].items():
            byts = ", ".join(f"0x{b:02X}" for b in blob)
            out.append(f"      {{ static const uint8_t _want[16] = {{ {byts} }}; "
                       f'check_bytes("{nm}", g_vm_stub + 0x{addr:X}, _want); }}')
        if vc["exp_cr"] is not None:
            nib, shift = vc["exp_cr"]
            out.append(f'      check_cr("{nm}", ctx->cr, {nib}, {shift});')
        out.append("    }")

    out.append("""
    printf("\\n[ppu-conformance] %d checks passed, %d FAILED, %d skipped\\n",
           g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
""")
    with open(path, "w") as f:
        f.write("\n".join(out))
    print(f"wrote {path}: {len(cases) - n_encoding_skipped} cases "
          f"({n_encoding_skipped} skipped at generation)")

# ---------------------------------------------------------------------------

def _compile_and_run(cpath, epath, log, tag):
    """Shared compile+run+report path for both the static suite and --fuzz.
    Returns the process returncode (0 = all checks passed)."""
    vcvars = r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
    bat = os.path.join(ROOT, "scratch", f"{tag}_run.bat")
    with open(bat, "w") as f:
        f.write("@echo off\n")
        f.write(f'call "{vcvars}" >nul 2>nul\n')
        f.write(f'cd /d "{ROOT}"\n')
        f.write(f'cl /nologo /O1 /W3 /I runtime\\ppu /Fe:"{epath}" "{cpath}" > "{log}" 2>&1\n')
        f.write("if errorlevel 1 exit /b 2\n")
        f.write(f'"{epath}" >> "{log}" 2>&1\n')
    r = subprocess.run(["cmd", "/c", bat], cwd=ROOT)
    tail = open(log).read() if os.path.exists(log) else ""
    fails = [ln for ln in tail.splitlines() if ln.startswith("FAIL")]
    for ln in fails[:40]:
        print(ln)
    if len(fails) > 40:
        print(f"... and {len(fails) - 40} more failures")
    for ln in tail.splitlines():
        if "[ppu-conformance]" in ln or "error C" in ln:
            print(ln)
    if r.returncode == 2:
        print("COMPILE FAILED -- see", log)
    return r.returncode

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", action="store_true", help="only write the C file")
    ap.add_argument("--fuzz", nargs="?", const=200, default=None, type=int,
                     help="semantic fuzzer mode: N generated cases per covered "
                          "family (default 200) instead of the static suite")
    ap.add_argument("--seed", type=int, default=0,
                     help="fuzz PRNG seed (deterministic; default 0)")
    args = ap.parse_args()

    if args.fuzz is not None:
        print(f"[test_ppu_lift] --fuzz mode: N={args.fuzz} seed={args.seed}")
        build_fuzz_cases(args.fuzz, args.seed)
        print(f"[test_ppu_lift] generated {len(FUZZ_CASES)} fuzz cases")
        cpath = os.path.join(ROOT, "scratch", "ppu_fuzz.cpp")
        epath = os.path.join(ROOT, "scratch", "ppu_fuzz.exe")
        log = os.path.join(ROOT, "scratch", "ppu_fuzz.log")
        emit_c(cpath, cases=FUZZ_CASES, vcases=[])
        if args.emit:
            return
        rc = _compile_and_run(cpath, epath, log, "ppu_fuzz")
        print(f"[test_ppu_lift] --fuzz seed={args.seed} N={args.fuzz}: "
              f"{'GREEN' if rc == 0 else 'FAILURES FOUND'}")
        sys.exit(0 if rc == 0 else 1)

    cpath = os.path.join(ROOT, "scratch", "ppu_conformance.cpp")   # preamble is C++ (extern "C")
    epath = os.path.join(ROOT, "scratch", "ppu_conformance.exe")
    emit_c(cpath)
    if args.emit:
        return

    log = os.path.join(ROOT, "scratch", "ppu_conformance.log")
    rc = _compile_and_run(cpath, epath, log, "ppu_conformance")
    sys.exit(0 if rc == 0 else 1)

if __name__ == "__main__":
    main()
