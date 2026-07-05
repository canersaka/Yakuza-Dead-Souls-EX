#!/usr/bin/env python3
"""SPU fuzz reference plug-in -- group 'select' (SUBTLE: byte-order/quadword).

Mnemonics: selb, fsm, fsmh, fsmb, fsmbi, gb, gbh, gbb, orx.

Clean-room. Every ref below is transcribed directly from the RTL blocks of
SPU_ISA_v1.2_27Jan2007_pub.pdf (page cites in each ref) -- NOT from the lifter,
the runtime helpers, or the harness's own static-suite refs. Encodings and
immediate forms are from the same ISA pages (op-field binary strings) and cross
-checked against SPU_Assembly_Language_Spec_1.5.pdf Table 2-2.

Big-endian numbering reminder (this is the whole reason the family is SUBTLE):
  * Quadword bit 0 = MSB, bit 127 = LSB.
  * Register value is [w0,w1,w2,w3], word 0 = preferred slot = the MSB word.
    RA28:31 are the four LOW bits of word 0; RA2:3 (byte numbering) is the low
    16 bits of word 0; "rightmost bit of word j" is (wj & 1).
  * to_b() = big-endian byte view (byte 0 first = MSByte of word 0). to_h() =
    big-endian halfword view (hw 0 first). Always index the architectural
    position through these helpers -- never a host-order cast.
"""

from spu_refs_api import register, to_b, from_b, to_h, from_h, M32


# --- selb  (Select Bits; ISA p115, RRR op4=0x8) -----------------------------
# RTL: RT0:127 <- RC0:127 & RB0:127 | (~RC0:127) & RA0:127.
# Bit-for-bit: where RC bit is 1 take RB, where 0 take RA. Per-word suffices
# because the operation is bitwise and lane-independent. form "rrr": (a,b,c) =
# (RA, RB, RC).
def ref_selb(a, b, c):
    return [((c[i] & b[i]) | ((~c[i] & M32) & a[i])) & M32 for i in range(4)]

register("selb", "rrr", ref_selb, op4=0x8, subtle=True)


# --- fsm  (Form Select Mask for Words; ISA p87, RR op11=0x1B4) ---------------
# RTL: s <- RA28:31 ; for j=0..3: if s_j=0 then r_{k::4}<-0 else <-0xFFFFFFFF.
# RA28:31 are the low 4 bits of word 0, MSB-first: s_0 = bit 28 = (w0>>3)&1 ...
# s_3 = bit 31 = (w0>>0)&1. r is filled word-by-word left-to-right, so result
# word j gets s_j.
def ref_fsm(a):
    s = a[0] & 0xF
    return [M32 if (s >> (3 - j)) & 1 else 0 for j in range(4)]

register("fsm", "rr1", ref_fsm, op11=0x1B4, subtle=True)


# --- fsmh  (Form Select Mask for Halfwords; ISA p86, RR op11=0x1B5) ----------
# RTL: s <- RA3 (the rightmost 8 bits of the preferred slot = low byte of word
# 0). for j=0..7: if s_j=0 then halfword <-0x0000 else 0xFFFF, left-to-right.
# s_0 = MSB of that byte = (v>>7)&1.
def ref_fsmh(a):
    v = a[0] & 0xFF
    return from_h([0xFFFF if (v >> (7 - j)) & 1 else 0 for j in range(8)])

register("fsmh", "rr1", ref_fsmh, op11=0x1B5, subtle=True)


# --- fsmb  (Form Select Mask for Bytes; ISA p85, RR op11=0x1B6) --------------
# RTL: s <- RA2:3 (rightmost 16 bits of the preferred slot = w0 & 0xFFFF).
# for j=0..15: if s_j=0 then byte<-0x00 else 0xFF, left-to-right. s_0 = MSB of
# the 16-bit value = (v>>15)&1.
def _fsm_bytes(v):
    v &= 0xFFFF
    return from_b([0xFF if (v >> (15 - j)) & 1 else 0x00 for j in range(16)])

def ref_fsmb(a):
    return _fsm_bytes(a[0])

register("fsmb", "rr1", ref_fsmb, op11=0x1B6, subtle=True)


# --- fsmbi  (Form Select Mask for Bytes Immediate; ISA p55, RI16 op9=0x065) --
# RTL: s <- I16 ; identical byte-replication to fsmb but the 16-bit source is
# the immediate. I16 arrives sign-extended per the contract; only the low 16
# bits are the mask source (s <- I16 is a 16-bit field).
def ref_fsmbi(i16):
    return _fsm_bytes(i16 & 0xFFFF)

register("fsmbi", "ri16", ref_fsmbi, op9=0x065, subtle=True)


# --- gb  (Gather Bits from Words; ISA p90, RR op11=0x1B0) --------------------
# RTL: for j=31,63,95,127 (step 32) s_k <- RA_j ; RT0:3 <- 0x0000000 || s ;
# RT4:15 <- 0. RA_j at j=31 = LSB of word 0 = (w0&1) -> s_0 (MSB of the 4-bit
# field). Result = (w0&1)<<3 | (w1&1)<<2 | (w2&1)<<1 | (w3&1)<<0 in word 0.
def ref_gb(a):
    v = 0
    for i in range(4):
        v |= (a[i] & 1) << (3 - i)
    return [v, 0, 0, 0]

register("gb", "rr1", ref_gb, op11=0x1B0, subtle=True)


# --- gbh  (Gather Bits from Halfwords; ISA p89, RR op11=0x1B1) ---------------
# RTL: for j=15,31,...,127 (step 16) gather the rightmost bit of each of 8
# halfwords into the rightmost byte of the preferred slot; s_0 = LSB of hw 0 =
# MSB of the 8-bit field. Other bits/slots zero.
def ref_gbh(a):
    h = to_h(a)
    v = 0
    for j in range(8):
        v |= (h[j] & 1) << (7 - j)
    return [v, 0, 0, 0]

register("gbh", "rr1", ref_gbh, op11=0x1B1, subtle=True)


# --- gbb  (Gather Bits from Bytes; ISA p88, RR op11=0x1B2) -------------------
# RTL: for j=7,15,...,127 (step 8) gather the rightmost bit of each of 16 bytes
# into the rightmost 16 bits of the preferred slot; s_0 = LSB of byte 0 = MSB
# of the 16-bit field. Other bits/slots zero.
def ref_gbb(a):
    b = to_b(a)
    v = 0
    for p in range(16):
        v |= (b[p] & 1) << (15 - p)
    return [v, 0, 0, 0]

register("gbb", "rr1", ref_gbb, op11=0x1B2, subtle=True)


# --- orx  (Or Across; ISA p107, RR op11=0x1F0) ------------------------------
# RTL: RT0:3 <- RA0:3 | RA4:7 | RA8:11 | RA12:15 ; RT4:15 <- 0. i.e. OR the four
# words into the preferred slot, zero the rest.
def ref_orx(a):
    return [(a[0] | a[1] | a[2] | a[3]) & M32, 0, 0, 0]

register("orx", "rr1", ref_orx, op11=0x1F0, subtle=True)
