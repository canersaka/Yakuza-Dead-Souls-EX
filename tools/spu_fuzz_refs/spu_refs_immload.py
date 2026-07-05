#!/usr/bin/env python3
"""SPU fuzz reference plug-in -- immload family (il, ilh, ilhu, ila, iohl).

Clean-room refs transcribed straight from the RTL blocks in
SPU_ISA_v1.2_27Jan2007_pub.pdf (Constant-Formation Instructions), cross-checked
against the immediate-form ranges in SPU_Assembly_Language_Spec_1.5.pdf
Table 2-6. NO lifter / helper / RPCS3 code was consulted for the semantics.

Register value = list of 4 uint32 SPU WORDS, word 0 = preferred slot = the
most-significant 32 bits of the architectural quadword. The ISA RTL writes the
same 32-bit value `t` into each of the four word slots (RT0:3, RT4:7, RT8:11,
RT12:15 -- byte ranges naming the four words), so every ref returns [t,t,t,t]
(iohl ORs per-word against the incoming RT).

Immediate arrival per the fuzz contract (already the sign-extended int the ISA
field holds):
  * il   -> I16 is an s16 field (Table 2-6: -32768..32767). Arrives signed; the
            RTL is RepLeftBit(I16,32) = sign-extend to 32. sext(i16,16)&M32.
  * ilh  -> I16 is a u16 field (Table 2-6 note: fsmbi/ilh/ilhu/iohl range is
            -32768..65535). Only the low 16 bits are used; the harness may draw a
            negative signed value, so mask i16 & 0xFFFF before use.
  * ilhu -> same u16 field; low 16 bits placed in the upper halfword of the word.
  * ila  -> I18 is a u18 field (0..262143); low 18 bits placed unchanged.
  * iohl -> u16 field; low 16 bits ORed into each word of the incoming RT.
"""

from spu_refs_api import register, to_b, from_b, to_h, from_h, sext, M32


# --- il  (Immediate Load Word; ISA v1.2 p52) --------------------------------
# RTL:  t <- RepLeftBit(I16, 32)
#       RT0:3 = RT4:7 = RT8:11 = RT12:15 = t
# I16 arrives already sign-extended to a signed Python int, so masking the
# sign-extended 32-bit value to 32 bits reproduces RepLeftBit(I16,32) exactly.
def ref_il(i16):
    t = sext(i16, 16) & M32
    return [t, t, t, t]

register("il", "ri16", ref_il, op9=0x081)


# --- ilh  (Immediate Load Halfword; ISA v1.2 p50) ---------------------------
# RTL (eight halfword slots):  s <- I16 ; RT0:1 = RT2:3 = ... = RT14:15 = s
# Each of the 8 architectural halfwords receives the low 16 bits of I16. As a
# word that is (v << 16) | v with v = I16 & 0xFFFF.
def ref_ilh(i16):
    v = i16 & 0xFFFF
    t = ((v << 16) | v) & M32
    return [t, t, t, t]

register("ilh", "ri16", ref_ilh, op9=0x083)


# --- ilhu  (Immediate Load Halfword Upper; ISA v1.2 p51) --------------------
# RTL:  t <- I16 || 0x0000  (I16 in the leftmost 16 bits, low 16 bits zero)
#       RT0:3 = RT4:7 = RT8:11 = RT12:15 = t
def ref_ilhu(i16):
    t = ((i16 & 0xFFFF) << 16) & M32
    return [t, t, t, t]

register("ilhu", "ri16", ref_ilhu, op9=0x082)


# --- ila  (Immediate Load Address; ISA v1.2 p53) ----------------------------
# RTL:  t <- (14)0 || I18   (14 zero bits followed by the 18-bit I18 field)
#       RT0:3 = RT4:7 = RT8:11 = RT12:15 = t
# I18 is unsigned (Table 2-6 u18); zero-extend the low 18 bits to 32.
def ref_ila(i18):
    t = i18 & 0x3FFFF
    return [t, t, t, t]

register("ila", "ri18", ref_ila, op7=0x21)


# --- iohl  (Immediate Or Halfword Lower; ISA v1.2 p54) ----------------------
# RTL:  t <- 0x0000 || I16   (I16 in the low 16 bits, high 16 bits zero)
#       RTk <- RTk | t   for each of the four word slots (RT read as input)
def ref_iohl(t_reg, i16):
    t = i16 & 0xFFFF
    return [(t_reg[i] | t) & M32 for i in range(4)]

register("iohl", "ri16t", ref_iohl, op9=0x0C1)
