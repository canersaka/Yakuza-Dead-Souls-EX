#!/usr/bin/env python3
"""SPU fuzz reference plug-in -- CARRY/BORROW family (cg, cgx, bg, bgx).

Transcribed clean-room from SPU_ISA_v1.2_27Jan2007_pub.pdf (the RTL blocks, not
the prose) and cross-checked against SPU_Assembly_Language_Spec_1.5.pdf Table 2-2
for encoding/operand order. All four are RR-form, rt,ra,rb, and operate on four
independent 32-bit WORD slots. cgx/bgx additionally read RT as an input (the
per-word carry/borrow bit), so they use the "rrt" form -- the harness seeds RT
and passes it as the 3rd arg.

SUBTLE (byte-order/quadword): the carry-in / borrow-in bit for cgx and bgx is
"RT bit (j*8+31)" for byte-slot j in {0,4,8,12}. Bit index j*8+31 is bit 31
(word 0 LSB), 63 (word 1 LSB), 95 (word 2 LSB), 127 (word 3 LSB) -- i.e. the
LEAST-significant bit of each 32-bit word. In the 4-word list [w0,w1,w2,w3]
(word 0 = preferred slot = most-significant) that is simply `t[k] & 1`. Getting
the RT-LSB per-word (and not, say, the whole-quadword LSB) is the trap here.

Opcodes (11-bit RR op, MSB->LSB), verified against tools/spu_disasm.py's
SPU_RR_OP11 table (cg=0x0C2, bg=0x042, cgx=0x342, bgx=0x343):
  cg  01100000010b = 0x0C2
  cgx 01101000010b = 0x342
  bg  00001000010b = 0x042
  bgx 01101000011b = 0x343
"""

from spu_refs_api import register, M32


# --- cg  (Carry Generate; ISA v1.2 p67) -------------------------------------
# RTL, for each word slot:
#   t[0:32] = (0 || RA_word) + (0 || RB_word)      # 33-bit unsigned sum
#   RT_word = 31'b0 || t[0]                         # carry-out into LSB, rest 0
# The carry-out is bit 32 of the 33-bit sum, i.e. (a+b) >> 32, which is 0 or 1.
def ref_cg(a, b):
    return [((a[i] + b[i]) >> 32) & 1 for i in range(4)]


register("cg", "rr", ref_cg, op11=0x0C2, subtle=True)


# --- cgx (Carry Generate Extended; ISA v1.2 p68) ----------------------------
# RTL, for each word slot j:
#   t[0:32] = (0 || RA_word) + (0 || RB_word) + (32'b0 || RT[j*8+31])
#   RT_word = 31'b0 || t[0]
# RT[j*8+31] is the LSB of the RT input word for that slot = (t_word & 1).
# The 33-bit sum's carry (bit 32) is the new carry-out. Max a+b+1 = 2^33 - 1, so
# the result is 0 or 1.
def ref_cgx(a, b, t):
    return [((a[i] + b[i] + (t[i] & 1)) >> 32) & 1 for i in range(4)]


register("cgx", "rrt", ref_cgx, op11=0x342, subtle=True)


# --- bg  (Borrow Generate; ISA v1.2 p70) ------------------------------------
# RTL, for each word slot:
#   if (RB_word >=u RA_word) then RT_word = 1 else RT_word = 0
# I.e. borrow-not for RB - RA: no borrow (result >= 0) => 1, borrow => 0.
def ref_bg(a, b):
    return [1 if (b[i] & M32) >= (a[i] & M32) else 0 for i in range(4)]


register("bg", "rr", ref_bg, op11=0x042, subtle=True)


# --- bgx (Borrow Generate Extended; ISA v1.2 p71) ---------------------------
# RTL, for each word slot j:
#   if (RT[j*8+31]) then                     # borrow-in bit == 1 (no incoming borrow)
#       if (RB_word >=u RA_word) then 1 else 0
#   else                                     # borrow-in bit == 0 (incoming borrow)
#       if (RB_word >u  RA_word) then 1 else 0
# RT[j*8+31] is the LSB of the RT input word = (t_word & 1).
def ref_bgx(a, b, t):
    out = []
    for i in range(4):
        av = a[i] & M32
        bv = b[i] & M32
        if t[i] & 1:
            out.append(1 if bv >= av else 0)
        else:
            out.append(1 if bv > av else 0)
    return out


register("bgx", "rrt", ref_bgx, op11=0x343, subtle=True)
