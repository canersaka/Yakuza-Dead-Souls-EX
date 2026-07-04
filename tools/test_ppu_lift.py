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
import os
import random
import struct
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import ppu_disasm                    # noqa: E402
from ppu_lifter import PPULifter, LiftedFunction   # noqa: E402

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

def ref_rlwinm(a, sh, mb, me):   return rotl32(a, sh) & mask32(mb, me), None
def ref_rlwimi(ra, rs, sh, mb, me):
    m = mask32(mb, me)
    return ((rotl32(rs, sh) & m) | (ra & MASK32 & ~m)) & MASK32, None   # low32 compare
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
         in_fprs=None, exp_fprs=None, in_cr=0):
    CASES.append(dict(name=name, word=word, in_regs=in_regs, expects=expects,
                      in_ca=in_ca, exp_ca=exp_ca, exp_cr=exp_cr, may_trap=may_trap,
                      in_fprs=in_fprs or {}, exp_fprs=exp_fprs or [], in_cr=in_cr))

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
                 {R[0]: a}, [(R[1], v, MASK32)])
            r0 = 0xAAAAAAAABBBBBBBB
            v2, _ = ref_rlwimi(r0, a, sh, mb, me)
            case(f"rlwimi a={a:#x} sh={sh} mb={mb} me={me}",
                 m_form(20, R[0], R[1], sh, mb, me),
                 {R[0]: a, R[1]: r0}, [(R[1], v2, MASK32)])
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
# C driver generation
# ---------------------------------------------------------------------------

def emit_c(path):
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
static void check_cr(const char* name, uint32_t cr, int nib, int shift) {
    int got = (cr >> shift) & 0xF;
    /* only LT/GT/EQ (top 3 bits of the nibble); SO passthrough not asserted */
    if ((got & 0xE) != (nib & 0xE)) {
        printf("FAIL %s: CR nibble@%d = %X want %X\\n", name, shift, got, nib); g_fail++;
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
    for i, c in enumerate(CASES):
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
        if c["exp_cr"] is not None:
            nib, shift = c["exp_cr"]
            body.append(f'        check_cr("{nm}", ctx->cr, {nib}, {shift});')
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
    for vi, vc in enumerate(VCASES):
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
    print(f"wrote {path}: {len(CASES) - n_encoding_skipped} cases "
          f"({n_encoding_skipped} skipped at generation)")

# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", action="store_true", help="only write the C file")
    args = ap.parse_args()

    cpath = os.path.join(ROOT, "scratch", "ppu_conformance.cpp")   # preamble is C++ (extern "C")
    epath = os.path.join(ROOT, "scratch", "ppu_conformance.exe")
    emit_c(cpath)
    if args.emit:
        return

    vcvars = r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
    bat = os.path.join(ROOT, "scratch", "ppu_conformance_run.bat")
    log = os.path.join(ROOT, "scratch", "ppu_conformance.log")
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
    sys.exit(0 if r.returncode == 0 else 1)

if __name__ == "__main__":
    main()
