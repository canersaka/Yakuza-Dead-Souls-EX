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

def case(name, word, in_regs, expects, in_ca=None, exp_ca=None, exp_cr=None, may_trap=False):
    CASES.append(dict(name=name, word=word, in_regs=in_regs, expects=expects,
                      in_ca=in_ca, exp_ca=exp_ca, exp_cr=exp_cr, may_trap=may_trap))

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

build_cases()

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
        for reg, val in c["in_regs"].items():
            out.append(f"      ctx->gpr[{reg}] = 0x{val:016X}ULL;")
        if c["in_ca"]:
            out.append("      ctx->xer |= (1u << 29);")
        body = [f"        {code}"]
        for reg, val, mask in c["expects"]:
            body.append(f'        check_reg("{nm}", {reg}, ctx->gpr[{reg}], '
                        f"0x{val:016X}ULL, 0x{mask:016X}ULL);")
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
