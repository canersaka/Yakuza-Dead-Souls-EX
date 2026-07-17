#!/usr/bin/env python3
"""Synthetic regression tests for the 2026-07-17 SPU tool batch (3 fixes):

  FIX 1  spu_lifter.py: halt-on-condition family (heq/heqi/hgt/hgti/hlgt/
         hlgti) now emits a real conditional halt instead of a no-op comment.
  FIX 2  spu_disasm.py: bisled gets the same 2-operand (RT, RA) special case
         bisl already has, so the lifter's ops[-1] target read is no longer
         handed a spurious RB.
  FIX 3  spu_lifter.py compute_bi_r0_jumps() + find_spu_functions.py: the
         bi-$r0 computed-jump classifier now accepts il/ila writers (not just
         lqa/lqr) and looks through one `ai rX,rY,0` / `ori rX,rY,0` identity
         move; find_spu_functions.py seeds il-loaded (not just ila-loaded)
         addresses as candidate function starts.

This is deliberately a plain assert-based script (not pytest) to match the
project's existing style (test_ppu_lift.py, test_spu_lift.py, lift_parse_audit.py
are all standalone `python foo.py` scripts with a nonzero exit on failure).

test_spu_lift.py's own docstring scopes it to lifted DATA semantics and
explicitly excludes "stop/branch/link control flow (F18/F19)" -- halt-on-
condition and bi-$r0 classification are control-flow, so they get their own
file here rather than expanding that suite's scope.

Usage:
    py -3 tools\\test_spu_lift_batch3.py
Exits nonzero on any failure.
"""

import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

from spu_disasm import spu_decode                                    # noqa: E402
from spu_lifter import (                                             # noqa: E402
    SPULifter, LiftedFunction, SPUInstruction, compute_bi_r0_jumps,
)

FAILS = []


def check(name, cond, detail=""):
    if cond:
        print(f"PASS {name}")
    else:
        print(f"FAIL {name}  {detail}")
        FAILS.append(name)


# ---------------------------------------------------------------------------
# Encoders (same field layout as tools/test_spu_lift.py's enc_rr/enc_ri10).
# ---------------------------------------------------------------------------

def enc_rr(op11, rt, ra, rb=0):
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt


def enc_ri10(op8, rt, ra, i10):
    return (op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt


# op11/op8 values, cross-checked against tools/spu_disasm.py's own tables.
OP11_HEQ, OP11_HGT, OP11_HLGT = 0x3D8, 0x258, 0x2D8
OP8_HEQI, OP8_HGTI, OP8_HLGTI = 0x7F, 0x4F, 0x5F
OP11_BISLED = 0x1AB

RT, RA, RB = 3, 4, 5


# =============================================================================
# FIX 1: halt-on-condition emits a real conditional halt.
# =============================================================================

def fix1_tests():
    lifter = SPULifter()
    func = LiftedFunction(name="test", start_addr=0, end_addr=0x100)

    cases = [
        ("heq",  enc_rr(OP11_HEQ, RT, RA, RB), "==", None),
        ("hgt",  enc_rr(OP11_HGT, RT, RA, RB), ">", "(int32_t)"),
        ("hlgt", enc_rr(OP11_HLGT, RT, RA, RB), ">", "(uint32_t)"),
        ("heqi", enc_ri10(OP8_HEQI, RT, RA, 5), "==", None),
        ("hgti", enc_ri10(OP8_HGTI, RT, RA, 5), ">", "(int32_t)"),
        ("hlgti", enc_ri10(OP8_HLGTI, RT, RA, 5), ">", "(uint32_t)"),
    ]
    for mn, word, op, cast in cases:
        insn = spu_decode(word, 0x40)
        check(f"fix1-decode-{mn}", insn.mnemonic == mn,
              f"decoded {insn.mnemonic!r} operands={insn.operands!r}")
        c = lifter._translate(insn, func)
        check(f"fix1-not-noop-{mn}", "no-op in recomp" not in c, c)
        check(f"fix1-halts-{mn}",
              "spu_halt(ctx, SPU_STATUS_STOPPED_BY_HALT)" in c and "return;" in c,
              c)
        check(f"fix1-op-{mn}", f" {op} " in c or f"){op}(" in c, c)
        check(f"fix1-conditional-{mn}", c.strip().startswith("if ("), c)
        if cast:
            check(f"fix1-cast-{mn}", cast in c, c)
        # Preferred-slot (word 0) read on RA, per SPU ISA (heq/hgt/hlgt compare
        # the preferred slot, not the whole quadword).
        check(f"fix1-pref-slot-{mn}", f"ctx->gpr[{RA}]._u32[0]" in c, c)

    # Region-lift mode: pc must be materialized before the halt (same [REV]
    # convention as stop/stopd/channel ops/self-loop traps in this file).
    word = enc_rr(OP11_HEQ, RT, RA, RB)
    insn = spu_decode(word, 0x80)
    c = lifter._translate(insn, func, pc_to_region={0x80: 0})
    check("fix1-region-pc-materialized", "ctx->pc = 0x80" in c, c)

    # heqi is an equality test: cast choice must not affect result parity, but
    # the *_imm() text must appear verbatim as the RHS literal (RI10 already
    # sign-extends -- e.g. i10=-5 must show up as -5, not 0x3FB or similar).
    word = enc_ri10(OP8_HEQI, RT, RA, -5 & 0x3FF)
    insn = spu_decode(word, 0x40)
    check("fix1-heqi-operand-text", insn.operands.endswith(", -5"), insn.operands)
    c = lifter._translate(insn, func)
    check("fix1-heqi-negative-imm-emitted", "(-5)" in c, c)


# =============================================================================
# FIX 2: bisled disassembler operand (2-op, mirrors bisl).
# =============================================================================

def fix2_tests():
    word = enc_rr(OP11_BISLED, RT, RA, RB)
    insn = spu_decode(word, 0x100)
    check("fix2-mnemonic", insn.mnemonic == "bisled", insn.mnemonic)
    ops = [o.strip() for o in insn.operands.split(",") if o.strip()]
    check("fix2-two-operands", len(ops) == 2, ops)
    check("fix2-ops-last-is-ra-not-rb", ops[-1] == f"$r{RA}",
          f"ops={ops} (RA={RA}, RB={RB} -- RB must NOT leak into ops[-1])")

    # Round-trip through the lifter: the emitted dispatch must key off RA
    # (ctx->gpr[RA]), never RB, matching bisl's own convention.
    lifter = SPULifter()
    func = LiftedFunction(name="test", start_addr=0, end_addr=0x200)
    c = lifter._translate(insn, func)
    check("fix2-lift-uses-ra", f"ctx->gpr[{RA}]" in c, c)
    check("fix2-lift-does-not-use-rb", f"ctx->gpr[{RB}]" not in c, c)


# =============================================================================
# FIX 3a: compute_bi_r0_jumps -- il/ila writers + one-hop identity-move
# look-through (ai/ori with a zero immediate).
# =============================================================================

def mk(addr, mnemonic, operands, rt):
    """Build a synthetic SPUInstruction with raw's low 7 bits = rt (the only
    part of .raw compute_bi_r0_jumps's _dest_reg() reads for non-RRR ops)."""
    return SPUInstruction(addr=addr, raw=(rt & 0x7F), mnemonic=mnemonic,
                           operands=operands)


def fix3a_tests():
    bounds = [(0, 0x100)]

    # (A) il $r0, 0x40 ; bi $r0  -> computed jump (extended writer set).
    insns = [
        mk(0x00, "il", "$r0, 64", 0),
        mk(0x04, "bi", "$r0", 0),
    ]
    jumps = compute_bi_r0_jumps(insns, bounds)
    check("fix3a-il-direct-writer", 0x04 in jumps, jumps)

    # (B) ila $r0, 0x40 ; bi $r0  -> computed jump.
    insns = [
        mk(0x00, "ila", "$r0, 0x40", 0),
        mk(0x04, "bi", "$r0", 0),
    ]
    jumps = compute_bi_r0_jumps(insns, bounds)
    check("fix3a-ila-direct-writer", 0x04 in jumps, jumps)

    # (C) il $r5, 0x40 ; ai $r0, $r5, 0 ; bi $r0  -> jump via ai identity move.
    insns = [
        mk(0x00, "il", "$r5, 64", 5),
        mk(0x04, "ai", "$r0, $r5, 0", 0),
        mk(0x08, "bi", "$r0", 0),
    ]
    jumps = compute_bi_r0_jumps(insns, bounds)
    check("fix3a-ai-identity-lookthrough", 0x08 in jumps, jumps)

    # (D) ila $r5, 0x40 ; ori $r0, $r5, 0 ; bi $r0  -> jump via ori identity move.
    insns = [
        mk(0x00, "ila", "$r5, 0x40", 5),
        mk(0x04, "ori", "$r0, $r5, 0", 0),
        mk(0x08, "bi", "$r0", 0),
    ]
    jumps = compute_bi_r0_jumps(insns, bounds)
    check("fix3a-ori-identity-lookthrough", 0x08 in jumps, jumps)

    # (E) lqa $r5, 0x40 ; ai $r0, $r5, 4 ; bi $r0  -> NOT a jump: nonzero
    # immediate means a REAL add, not an identity move -- must stay a return.
    insns = [
        mk(0x00, "lqa", "$r5, 0x40", 5),
        mk(0x04, "ai", "$r0, $r5, 4", 0),
        mk(0x08, "bi", "$r0", 0),
    ]
    jumps = compute_bi_r0_jumps(insns, bounds)
    check("fix3a-nonzero-ai-stays-return", 0x08 not in jumps, jumps)

    # (F) baseline regression: lqa/lqr direct writers still classify as jumps
    # (the pre-existing behavior must be unchanged).
    insns = [
        mk(0x00, "lqa", "$r0, 0x40", 0),
        mk(0x04, "bi", "$r0", 0),
    ]
    jumps = compute_bi_r0_jumps(insns, bounds)
    check("fix3a-lqa-baseline-unchanged", 0x04 in jumps, jumps)

    # (G) baseline regression: a genuine return (link restored via lqd, or no
    # r0 writer at all) must stay classified as a return.
    insns = [
        mk(0x00, "lqd", "$r0, 0($r1)", 0),
        mk(0x04, "bi", "$r0", 0),
    ]
    jumps = compute_bi_r0_jumps(insns, bounds)
    check("fix3a-lqd-stays-return", 0x04 not in jumps, jumps)

    insns = [mk(0x00, "bi", "$r0", 0)]
    jumps = compute_bi_r0_jumps(insns, bounds)
    check("fix3a-no-writer-stays-return", 0x00 not in jumps, jumps)

    # (H) a second identity move (two hops) must NOT be chased -- conservative
    # by design: only exactly one look-through hop is honored.
    insns = [
        mk(0x00, "il", "$r6, 64", 6),
        mk(0x04, "ai", "$r5, $r6, 0", 5),   # hop 1: r5 <- r6 (identity)
        mk(0x08, "ai", "$r0, $r5, 0", 0),   # hop 2 target: r0 <- r5 (identity)
        mk(0x0C, "bi", "$r0", 0),
    ]
    jumps = compute_bi_r0_jumps(insns, bounds)
    check("fix3a-two-hop-not-chased", 0x0C not in jumps, jumps)


# =============================================================================
# FIX 3b: find_spu_functions.py il-seeding is DATAFLOW-GATED (only trusted as
# an address when it demonstrably feeds a bi/bisl-family branch on the same
# register) -- a naive "any in-range 4-aligned il value" first cut broke the
# mandatory before/after function-set-diff gate on 10/20 real SPU images
# (false hits were round-number SIZE constants like 0x4000, not addresses).
# =============================================================================

def fix3b_tests():
    import find_spu_functions as fsf   # noqa: E402

    # (a) il rX,addr immediately followed by bi rX -- the genuine idiom, must
    # be recognized as reaching a bi target.
    insns = [
        mk(0x00, "il", "$r3, 256", 3),
        mk(0x04, "bi", "$r3", 3),
    ]
    check("fix3b-reaches-direct-bi", fsf._reaches_bi_target(insns, 0, 3), None)

    # (b) il rX,addr ... bisl rX (indirect call form, target = last operand).
    insns = [
        mk(0x00, "il", "$r3, 256", 3),
        mk(0x04, "bisl", "$r0, $r3", 0),
    ]
    check("fix3b-reaches-bisl", fsf._reaches_bi_target(insns, 0, 3), None)

    # (c) il rX,SIZE with no downstream branch on rX at all -- must NOT be
    # treated as an address source (this is the exact false-positive class
    # the naive first cut hit: a size constant used only as a byte count).
    insns = [
        mk(0x00, "il", "$r3, 0x4000", 3),
        mk(0x04, "a", "$r5, $r5, $r3", 5),   # ordinary arithmetic use, no branch
        mk(0x08, "bi", "$r0", 0),            # unrelated return
    ]
    check("fix3b-size-constant-not-seeded", not fsf._reaches_bi_target(insns, 0, 3), None)

    # (d) rX is clobbered before the bi -- must NOT chase through the stale
    # value (dataflow, not "any bi anywhere in the window").
    insns = [
        mk(0x00, "il", "$r3, 256", 3),
        mk(0x04, "il", "$r3, 999", 3),   # rX rewritten
        mk(0x08, "bi", "$r3", 3),
    ]
    check("fix3b-clobber-breaks-chain", not fsf._reaches_bi_target(insns, 0, 3), None)

    # (e) end-to-end through the real seeding loop shape (mirrors the snippet
    # inside detect_functions()) for both the safe and false-positive cases.
    code_start, code_end = 0x0, 0x10000

    def run_il_seed_pass(insns):
        seed_starts = set()
        n = 0
        for idx, ins in enumerate(insns):
            if ins.mnemonic != "il":
                continue
            toks = ins.operands.split(",")
            if len(toks) < 2:
                continue
            try:
                v = int(toks[-1].strip(), 0)
            except ValueError:
                continue
            if not (code_start <= v < code_end and (v & 3) == 0 and v not in seed_starts):
                continue
            rt = fsf._il_dest_reg(toks[0])
            if rt is None or not fsf._reaches_bi_target(insns, idx, rt):
                continue
            seed_starts.add(v)
            n += 1
        return seed_starts, n

    seeds, n = run_il_seed_pass([mk(0x00, "il", "$r3, 256", 3), mk(0x04, "bi", "$r3", 3)])
    check("fix3b-e2e-genuine-idiom-seeded", 256 in seeds and n == 1, (seeds, n))

    seeds, n = run_il_seed_pass([mk(0x00, "il", "$r3, 0x4000", 3),
                                  mk(0x04, "a", "$r5, $r5, $r3", 5)])
    check("fix3b-e2e-size-constant-not-seeded", seeds == set() and n == 0, (seeds, n))


if __name__ == "__main__":
    fix1_tests()
    fix2_tests()
    fix3a_tests()
    fix3b_tests()
    print()
    if FAILS:
        print(f"{len(FAILS)} FAILED: {FAILS}")
        sys.exit(1)
    print("All batch-3 synthetic checks passed.")
