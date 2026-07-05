#!/usr/bin/env python3
"""SPU fuzz reference plug-in -- extend family (xsbh, xshw, xswd).

Clean-room refs transcribed from the RTL blocks of
SPU_ISA_v1.2_27Jan2007_pub.pdf (NOT the prose):
    xsbh  -- Extend Sign Byte to Halfword    (ISA p94, op11 0x2B6, RR 1-src)
    xshw  -- Extend Sign Halfword to Word    (ISA p95, op11 0x2AE, RR 1-src)
    xswd  -- Extend Sign Word to Doubleword  (ISA p96, op11 0x2A6, RR 1-src)

RTL (verbatim bit ranges, RA/RT numbered MSb=0 architectural bit order):
    xsbh: for each of 8 halfword slots, RTj0:1 <- RepLeftBit(RAj*2+1, 16)
          i.e. sign of the RIGHT byte of each halfword propagates left; the
          16-bit result (sign-extended byte) is stored in that halfword.
    xshw: RT0:3 <- RepLeftBit(RA2:3,32); RT4:7 <- RepLeftBit(RA6:7,32); ...
          i.e. sign of the RIGHT halfword of each word propagates left; the
          32-bit result (sign-extended halfword) replaces that word.
    xswd: RT0:7 <- RepLeftBit(RA4:7,64); RT8:15 <- RepLeftBit(RA12:15,64)
          i.e. of each doubleword pair (words 0,1) and (words 2,3), the RIGHT
          word (1, then 3) sign-extends to fill the 64-bit result; the LEFT
          word of each pair (0, then 2) becomes the all-0/all-1 sign fill.

This is the identical formula already used (green, non-fuzzed) in the
static suite at the top of tools/test_spu_lift.py (ref_xsbh/ref_xshw/
ref_xswd) -- ported here verbatim so the fuzzer covers the SAME semantics
with many more edge-biased draws. xswd carries a KNOWN-RED note: the lifter
route (rr_un "xswd" -> "spu_xswd") is present, but a prior audit flagged the
runtime helper's word-source/sign-placement wiring as suspect (F2); this ref
being independently re-derived from the RTL (not from the helper) is exactly
the check that would catch it again if it regresses.
"""

from spu_refs_api import register, to_h, from_h, sext, M32


# --- xsbh (Extend Sign Byte to Halfword; ISA p94) ---------------------------
# For each of 8 halfword slots: sign of the halfword's right (low) byte
# propagates to fill the whole 16-bit halfword.
def ref_xsbh(a):
    return from_h([sext(x & 0xFF, 8) & 0xFFFF for x in to_h(a)])


register("xsbh", "rr1", ref_xsbh, op11=0x2B6, subtle=True)


# --- xshw (Extend Sign Halfword to Word; ISA p95) ---------------------------
# For each of 4 word slots: sign of the word's right (low) halfword
# propagates to fill the whole 32-bit word.
def ref_xshw(a):
    return [sext(w & 0xFFFF, 16) & M32 for w in a]


register("xshw", "rr1", ref_xshw, op11=0x2AE, subtle=True)


# --- xswd (Extend Sign Word to Doubleword; ISA p96) -------------------------
# For each of 2 doubleword slots (words 0,1) and (words 2,3): the RIGHT word
# of the pair (word 1, then word 3) sign-extends across the full 64 bits, so
# the LEFT word of the result pair (0, then 2) becomes the sign fill and the
# RIGHT word of the result pair keeps the original value.
def ref_xswd(a):
    r = []
    for d in (0, 1):
        v = a[2 * d + 1] & M32
        r += [M32 if (v >> 31) else 0, v]
    return r


register("xswd", "rr1", ref_xswd, op11=0x2A6, subtle=True)
