#!/usr/bin/env python3
"""SPU fuzz reference plug-in -- WORKED EXAMPLES (il, a, shufb).

This is the TEMPLATE a Phase-2 family agent copies to
tools/spu_fuzz_refs/spu_refs_<group>.py.
tools/test_spu_lift.py --fuzz auto-discovers every
tools/spu_fuzz_refs/spu_refs_*.py, imports it, and calls its module-level
register() calls (executed at import time) to plug each mnemonic's reference
semantics into the fuzzer.

READ tools/test_spu_lift.py's REF-FUNCTION CONTRACT docstring (top of file, the
"SPU FUZZ REF-FUNCTION CONTRACT" block) before writing a ref. The contract, in
brief:

  * A register value is a Python list of 4 uint32 SPU WORDS, index 0 = word 0 =
    PREFERRED SLOT. This is exactly ctx->gpr[r]._u32[i] in the runtime. Word 0
    is the most-significant (big-endian leftmost) 32 bits of the architectural
    quadword. Byte 0 of the quadword = bits 24..31 of word 0 -- use the
    to_b()/from_b() big-endian byte view helpers, never raw host-order casts.
  * A ref returns a 4-word list (word 0..3). The harness compares all four words.
  * Immediates arrive as the ALREADY-SIGN-EXTENDED Python int the ISA field
    holds (e.g. il's I16 arrives as a signed value in [-32768, 32767]); the ref
    masks/re-expands per the ISA pseudocode, same as it would for the real field.
  * register(mnemonic, form, ref, **opts) wires one mnemonic. `form` names the
    encoding + which operands the ref takes (see FORMS in the harness). The
    fuzzer draws edge-biased operands, encodes via the harness's own enc_* (so an
    encoding slip is reported as ENCODING, not a false lifter failure), lifts,
    compiles, runs, and compares.

The three refs below are transcribed byte-for-byte from SPU_ISA_v1.2_27Jan2007
(il p52, a p60, shufb p116) -- the RTL blocks, not the prose. shufb is the
byte-order template: it concatenates RA||RB into 32 bytes (big-endian byte 0
first), then for each of 16 selector bytes applies Table 5-1 / the p116 RTL.
"""

from spu_refs_api import register, to_b, from_b, sext, M32


# --- il  (Immediate Load Word; ISA p52) -------------------------------------
# RTL: t <- RepLeftBit(I16, 32); RT0:3 = RT4:7 = RT8:11 = RT12:15 = t.
# I16 arrives already sign-extended to a signed Python int per the contract, so
# masking to 32 bits reproduces RepLeftBit(I16,32) exactly.
def ref_il(i16):
    return [sext(i16, 16) & M32] * 4

register("il", "ri16", ref_il, op9=0x081)


# --- a  (Add Word; ISA p60) -------------------------------------------------
# RTL: RTk = RAk + RBk for each of four 32-bit word slots; carries discarded.
def ref_a(a, b):
    return [(a[i] + b[i]) & M32 for i in range(4)]

register("a", "rr", ref_a, op11=0x0C0)


# --- shufb  (Shuffle Bytes; ISA p116) ---------------------------------------
# RTL: Rconcat = RA || RB  (byte 0 = MSByte of RA's word 0 ... byte 31 = LSByte
# of RB's word 3). For j in 0..15, b = RCj (one selector byte):
#     b0:1 = 0b10  -> 0x00      (mask (b & 0xC0) == 0x80)
#     b0:2 = 0b110 -> 0xFF      (mask (b & 0xE0) == 0xC0)
#     b0:2 = 0b111 -> 0x80      (mask (b & 0xE0) == 0xE0)
#     otherwise    -> Rconcat[b & 0x1F]
# to_b() gives the big-endian byte view (byte 0 first), so concatenation is a
# plain list append and Rconcat[i] indexes the true architectural byte i.
def ref_shufb(a, b, c):
    cat = to_b(a) + to_b(b)                 # 32 bytes, big-endian byte 0 first
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

register("shufb", "rrr", ref_shufb, op4=0xB)
