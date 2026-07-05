#!/usr/bin/env python3
"""SPU fuzz reference plug-in -- LOGICAL family.

Clean-room refs for the SPU bitwise/logical group, transcribed from the RTL
blocks of SPU_ISA_v1.2_27Jan2007_pub.pdf (pages 97-114 in the PDF's own page
numbering) and cross-checked against SPU_Assembly_Language_Spec_1.5.pdf's
Table 2-6 immediate-form prose:

  RR word ops (ISA pp. 97/103/108/112/113/114, 99/104 for -c forms):
    and   RT = RA & RB                     (op11 0x0C1, p97 "And")
    or    RT = RA | RB                     (op11 0x041, p101 "Or")
    xor   RT = RA ^ RB                     (op11 0x241, p107 "Exclusive Or")
    nand  RT = ~(RA & RB)                  (op11 0x0C9, p111 "Nand")
    nor   RT = ~(RA | RB)                  (op11 0x049, p112 "Nor")
    eqv   RT = RA ^ (~RB)                  (op11 0x249, p113 "Equivalent")
    andc  RT = RA & (~RB)                  (op11 0x2C1, p97  "And w/ Complement")
    orc   RT = RA | (~RB)                  (op11 0x2C9, p102 "Or w/ Complement")

  These are all plain bitwise over the whole 128-bit quadword; the ISA writes
  them as four independent 32-bit word slots but the result is identical to a
  per-word (or per-byte) bitwise op. We compute per-word to match the 4-word
  register list exactly.

  RI10 immediate ops -- the SUBTLE part (I10 extension differs by width):
    WORD  (andi/ori/xori, op8 0x14/0x04/0x44): t = RepLeftBit(I10,32) i.e.
          SIGN-EXTEND s10 to 32 bits, then op with each 32-bit word.
    HALF  (andhi/orhi/xorhi, op8 0x15/0x05/0x45): t = RepLeftBit(I10,16) i.e.
          SIGN-EXTEND s10 to 16 bits, then op with each 16-bit halfword.
    BYTE  (andbi/orbi/xorbi, op8 0x16/0x06/0x46): b = I10 & 0x00FF i.e. the LOW
          8 BITS of s10 (NO sign extension), replicated to all 16 byte slots,
          then op with each byte.

  Per the contract, the ri10 immediate arrives ALREADY sign-extended to the
  signed Python int the 10-bit field holds (range [-512, 511]); sext(imm,10)
  reproduces RepLeftBit(I10,...) and (imm & 0xFF) reproduces the byte mask.
"""

from spu_refs_api import register, to_b, from_b, to_h, from_h, sext, M32


# --- RR word logical ops (whole-quadword bitwise; ISA pp. 97-114) -----------
def ref_and(a, b):   return [(a[i] & b[i]) & M32 for i in range(4)]
def ref_or(a, b):    return [(a[i] | b[i]) & M32 for i in range(4)]
def ref_xor(a, b):   return [(a[i] ^ b[i]) & M32 for i in range(4)]
def ref_nand(a, b):  return [(~(a[i] & b[i])) & M32 for i in range(4)]
def ref_nor(a, b):   return [(~(a[i] | b[i])) & M32 for i in range(4)]
def ref_eqv(a, b):   return [(a[i] ^ (~b[i] & M32)) & M32 for i in range(4)]
def ref_andc(a, b):  return [(a[i] & (~b[i] & M32)) & M32 for i in range(4)]
def ref_orc(a, b):   return [(a[i] | (~b[i] & M32)) & M32 for i in range(4)]

register("and",  "rr", ref_and,  op11=0x0C1)
register("or",   "rr", ref_or,   op11=0x041)
register("xor",  "rr", ref_xor,  op11=0x241)
register("nand", "rr", ref_nand, op11=0x0C9)
register("nor",  "rr", ref_nor,  op11=0x049)
register("eqv",  "rr", ref_eqv,  op11=0x249)
register("andc", "rr", ref_andc, op11=0x2C1)
register("orc",  "rr", ref_orc,  op11=0x2C9)


# --- RI10 WORD immediate: RepLeftBit(I10,32) = sign-extend s10 to 32 --------
def _word_imm(a, imm, op):
    t = sext(imm, 10) & M32
    return [op(a[i], t) & M32 for i in range(4)]

def ref_andi(a, imm):  return _word_imm(a, imm, lambda x, t: x & t)
def ref_ori(a, imm):   return _word_imm(a, imm, lambda x, t: x | t)
def ref_xori(a, imm):  return _word_imm(a, imm, lambda x, t: x ^ t)

register("andi", "ri10", ref_andi, op8=0x14)
register("ori",  "ri10", ref_ori,  op8=0x04)
register("xori", "ri10", ref_xori, op8=0x44)


# --- RI10 HALFWORD immediate: RepLeftBit(I10,16) = sign-extend s10 to 16 ----
def _half_imm(a, imm, op):
    t = sext(imm, 10) & 0xFFFF
    return from_h([op(h, t) & 0xFFFF for h in to_h(a)])

def ref_andhi(a, imm):  return _half_imm(a, imm, lambda h, t: h & t)
def ref_orhi(a, imm):   return _half_imm(a, imm, lambda h, t: h | t)
def ref_xorhi(a, imm):  return _half_imm(a, imm, lambda h, t: h ^ t)

register("andhi", "ri10", ref_andhi, op8=0x15, subtle=True)
register("orhi",  "ri10", ref_orhi,  op8=0x05, subtle=True)
register("xorhi", "ri10", ref_xorhi, op8=0x45, subtle=True)


# --- RI10 BYTE immediate: b = I10 & 0x00FF (LOW 8 bits, NOT sign-extended) --
def _byte_imm(a, imm, op):
    b = imm & 0xFF
    return from_b([op(x, b) & 0xFF for x in to_b(a)])

def ref_andbi(a, imm):  return _byte_imm(a, imm, lambda x, b: x & b)
def ref_orbi(a, imm):   return _byte_imm(a, imm, lambda x, b: x | b)
def ref_xorbi(a, imm):  return _byte_imm(a, imm, lambda x, b: x ^ b)

register("andbi", "ri10", ref_andbi, op8=0x16, subtle=True)
register("orbi",  "ri10", ref_orbi,  op8=0x06, subtle=True)
register("xorbi", "ri10", ref_xorbi, op8=0x46, subtle=True)
