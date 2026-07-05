#!/usr/bin/env python3
"""Stable import surface for SPU fuzz reference plug-ins.

A plug-in (tools/spu_fuzz_refs/spu_refs_<group>.py) does:

    from spu_refs_api import register, to_b, from_b, to_h, from_h, sext, M32

and calls register(mnemonic, form, ref, **opts) at import time. The harness
(tools/test_spu_lift.py) imports each plug-in and then reads REGISTRY.

This module deliberately holds NO semantics of its own -- the byte/half/word
lane helpers are re-exported straight from tools/test_spu_lift.py so there is a
single source of truth for byte order (word 0 = preferred slot; to_b() is the
big-endian byte view, byte 0 first). test_spu_lift.py inserts its own tools/
directory (and tools/spu_fuzz_refs/) onto sys.path before importing plug-ins,
so this import resolves regardless of which of the two directories is CWD-
relative at import time.

STANDING GATE, not scratch/: this directory is the fuzzer's permanent,
version-controlled oracle set (moved out of scratch/ 2026-07 once every
family had a ref, was fuzz-clean, and was independently verified -- see
tools/test_spu_lift.py's module docstring "Coverage" section for the
per-family status and any documented divergences).
"""

# The canonical lane helpers live in the harness; import them so every ref uses
# the exact byte order the driver seeds/reads. (test_spu_lift adds tools/ to
# sys.path, so `import test_spu_lift` works when the harness drives us. When a
# plug-in is imported standalone for a syntax check it will also resolve as long
# as tools/ is on the path.)
from test_spu_lift import (  # noqa: F401  (re-exported for plug-ins)
    to_b, from_b, to_h, from_h, to_q, from_q, sext, rotl32, rotl16,
    lo16s, hi16s, M32, Q_MASK,
)

# mnemonic -> dict(form=..., ref=..., opts={...}). Insertion-ordered so the
# fuzz report groups families in registration order.
REGISTRY = {}


def register(mnemonic, form, ref, **opts):
    """Plug one mnemonic into the fuzzer.

    mnemonic : SPU assembler mnemonic (str), e.g. "shufb".
    form     : one of the FORM names the harness understands (see FORMS in
               tools/test_spu_lift.py). It fixes the encoding AND the ref
               signature:
                 "rr"    ref(a, b)        RR, two source regs  (enc_rr, op11)
                 "rr1"   ref(a)           RR, one source reg   (enc_rr, op11)
                 "rrt"   ref(a, b, t)     RR, two srcs + RT read as input (op11)
                 "ri7"   ref(a, i7)       RI7, one src + 7-bit imm (enc_ri7, op11)
                 "ri10"  ref(a, imm)      RI10, one src + signed 10-bit (enc_ri10, op8)
                 "ri16"  ref(i16)         RI16, imm only        (enc_ri16, op9)
                 "ri16t" ref(t, i16)      RI16, imm + RT read as input (op9)
                 "ri18"  ref(i18)         RI18, imm only        (enc_ri18, op7)
                 "rrr"   ref(a, b, c)     RRR, three source regs (enc_rrr, op4)
                 "rrrt"  ref(a, b, c)     RRR where RC is read as the extra src
                                          (mpya-class), same wiring as "rrr".
                 "ri8"   ref(a, i8)       RI8, one src + 8-bit unsigned imm
                                          (enc_ri8, op10) -- the float<->int
                                          scale conversions (cflts/cfltu/
                                          csflt/cuflt); 10-bit opcode + 8-bit
                                          immediate, distinct field widths
                                          from "ri10"'s 8-bit-opcode +
                                          10-bit-immediate.
    ref      : Python function with the signature the form dictates. Operands are
               4-word register lists (word 0 = preferred); immediates arrive as
               the sign-extended int the ISA field holds. Returns a 4-word list.
    opts     : the opcode kwarg the encoder needs -- op11 / op8 / op9 / op7 /
               op4 / op10 -- plus optional generation hints:
                 imm_pool : explicit list of immediate ints to draw from (else a
                            form-appropriate default edge pool).
                 subtle   : True to mark the family as byte-order/quadword/
                            preferred-slot sensitive (informational).
                 no_nan   : True if this ref interprets a register operand as
                            an IEEE float and has no single well-defined
                            result for a NaN operand (arithmetic ops can
                            legally emit any NaN payload; float->int casts
                            can be outright C undefined behavior). Scrubs
                            NaN bit patterns from every register draw for
                            this mnemonic.
    """
    if mnemonic in REGISTRY:
        import sys
        print(f"[spu-fuzz] skipping duplicate ref for {mnemonic!r} "
              f"(keeping first registration)", file=sys.stderr)
        return REGISTRY[mnemonic]["ref"]
    REGISTRY[mnemonic] = dict(form=form, ref=ref, opts=opts)
    return ref
