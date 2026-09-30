#!/usr/bin/env python3
"""
SPU (Synergistic Processing Unit) disassembler for PS3 binaries.

Decodes 32-bit fixed-width SPU instructions covering memory, integer,
logical, shift/rotate, branch, compare, channel, and hint-for-branch
operations.

Usage:
    python spu_disasm.py <input_file> [--base ADDR] [--offset N] [--json] [--length N]
"""

import argparse
import json
import struct

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def sign_extend(value: int, nbits: int) -> int:
    """Sign-extend *value* from *nbits* to a Python int."""
    if value & (1 << (nbits - 1)):
        value -= 1 << nbits
    return value


# ---------------------------------------------------------------------------
# Instruction representation
# ---------------------------------------------------------------------------

class SPUInstruction:
    """A decoded SPU instruction."""

    __slots__ = ("addr", "raw", "mnemonic", "operands")

    def __init__(self, addr: int = 0, raw: int = 0, mnemonic: str = "???",
                 operands: str = ""):
        self.addr = addr
        self.raw = raw
        self.mnemonic = mnemonic
        self.operands = operands

    def __str__(self) -> str:
        hexb = f"{self.raw:08X}"
        return f"{self.addr:08X}:  {hexb}  {self.mnemonic:<12s} {self.operands}"

# ---------------------------------------------------------------------------
# Decode tables
#
# SPU instructions are categorised by opcode field widths:
#   - 4-bit (bits 0-3)   -- not many
#   - 7-bit (bits 0-6)   -- RI18, RI16
#   - 8-bit (bits 0-7)   -- RI10
#   - 9-bit (bits 0-8)   -- RI8
#   - 11-bit (bits 0-10) -- RR, RRR, special
# ---------------------------------------------------------------------------

# RRR format, reading the word from the MSB: OP(4) RT(7) RB(7) RA(7) RC(7).
# The destination (RT) is the field right after the opcode; the low 7 bits
# are RC. Opcode values follow the SPU ISA: the cdd/cwd+shufb insert idiom
# only decodes sanely with selb=0b1000, shufb=0b1011.
RRR_TABLE: dict[int, str] = {
    0b1000: "selb",       # select bits
    0b1011: "shufb",      # shuffle bytes
    0b1100: "mpya",       # multiply and add
    0b1101: "fnms",       # floating negative multiply-subtract
    0b1110: "fma",        # floating multiply-add
    0b1111: "fms",        # floating multiply-subtract
}

# RI18 format: opcd(7) i18(18) rt(7)
RI18_TABLE: dict[int, str] = {
    0b0100001: "ila",     # immediate load address (unsigned 18-bit)
    0b0001000: "hbra",    # 0x08 hint for branch (absolute); pure perf hint
    0b0001001: "hbrr",    # 0x09 hint for branch (relative); pure perf hint
    # il/ilh/ilhu are RI16 (9-bit op), not RI18: their 7-bit prefix is 0x20,
    # which would shadow all three as "ilh" here (RI18 is checked before
    # RI16). They live in RI16_TABLE below; ila is the only RI16-prefix
    # immediate loader that is genuinely RI18.
    # br/brsl/bra/brasl are RI16 (9-bit op 0x060/0x062/0x064/0x066), not
    # RI18: placing them at 0x30-0x33 here would mis-decode them (and shadow
    # iohl 0xC1 as "br"). They live in RI16_TABLE, as do brnz/brz/brhnz/brhz.
}

# RI16 format: opcd(9) i16(16) rt(7)
# RI16 must be checked before RI10 in the decoder because some RI16
# opcodes share the top 8 bits with RI10 entries (e.g. brz op9=0x040 vs
# shlhi op8=0x20, brnz op9=0x042 vs shli op8=0x21).
RI16_TABLE: dict[int, str] = {
    # --- Built from the SPU ISA opcode table. lqr/stqr/fsmbi must be present
    # here or they mis-decode as RI10 rotate-immediates. op9 = (insn >> 23) & 0x1FF. ---
    # immediate loads
    0b010000001: "il",     # 0x081 immediate load word (sign-extended 16-bit)
    0b010000010: "ilhu",   # 0x082 immediate load halfword upper
    0b010000011: "ilh",    # 0x083 immediate load halfword
    0b011000001: "iohl",   # 0x0C1 immediate or halfword lower
    # branches relative / absolute (+ set link) + PC-relative/absolute loads
    0b001100000: "bra",    # 0x060 branch absolute
    0b001100001: "lqa",    # 0x061 load quadword (absolute address)
    0b001100010: "brasl",  # 0x062 branch absolute + set link
    0b001100100: "br",     # 0x064 branch relative
    0b001100101: "fsmbi",  # 0x065 form select mask byte immediate
    0b001100110: "brsl",   # 0x066 branch relative + set link
    0b001100111: "lqr",    # 0x067 load quadword (instruction-relative)
    # conditional branches + absolute/relative stores
    0b001000000: "brz",    # 0x040 branch if zero word
    0b001000001: "stqa",   # 0x041 store quadword (absolute address)
    0b001000010: "brnz",   # 0x042 branch if not zero word
    0b001000100: "brhz",   # 0x044 branch if zero halfword
    0b001000110: "brhnz",  # 0x046 branch if not zero halfword
    0b001000111: "stqr",   # 0x047 store quadword (instruction-relative)
}

# RI10 format: opcd(8) i10(10) ra(7) rt(7)
RI10_TABLE: dict[int, str] = {
    0b00110100: "lqd",    # load quadword d-form
    0b00100100: "stqd",   # store quadword d-form
    0b00011100: "ai",     # add word immediate
    0b00001100: "sfi",    # subtract from word immediate
    0b00011101: "ahi",    # add halfword immediate
    0b00001101: "sfhi",   # subtract from halfword immediate
    0b00010100: "andi",   # and word immediate
    0b00000100: "ori",    # or word immediate
    0b00000101: "orhi",   # or halfword immediate            (op8 0x05)
    0b00000110: "orbi",   # or byte immediate                (op8 0x06)
    0b00010101: "andhi",  # and halfword immediate           (op8 0x15)
    0b00010110: "andbi",  # and byte immediate               (op8 0x16)
    0b01000100: "xori",   # xor word immediate
    0b01000101: "xorhi",  # xor halfword immediate           (op8 0x45)
    0b01000110: "xorbi",  # xor byte immediate               (op8 0x46)
    0b01111100: "ceqi",   # compare equal word immediate
    0b01111101: "ceqhi",  # compare equal halfword immediate
    0b01111110: "ceqbi",  # compare equal byte immediate
    0b01111111: "heqi",   # halt if equal immediate              (op8 0x7F)
    0b01001100: "cgti",   # compare greater than word immediate
    0b01001101: "cgthi",  # compare greater than halfword immediate
    0b01001110: "cgtbi",  # compare greater than byte immediate
    0b01001111: "hgti",   # halt if greater than immediate       (op8 0x4F)
    0b01011100: "clgti",  # compare logical greater than word immediate
    0b01011101: "clgthi", # compare logical greater than halfword immediate
    0b01011110: "clgtbi", # compare logical greater than byte immediate
    0b01011111: "hlgti",  # halt if logically greater than immediate (op8 0x5F)
    0b01110100: "mpyi",   # multiply immediate
    0b01110101: "mpyui",  # multiply unsigned immediate
    # The rotate/shift immediate forms (roti/rotmi/rotmai/shli/rothi/
    # rothmi/rotmahi/shlhi) are RI7 (op11 0x78-0x7f), not RI10; they are
    # handled in the ri7_table inside spu_decode(). RI10 entries for them
    # would mis-decode fsmbi/lqr/stqr/cbd/orx.
}

# RR format: opcd(11) rb(7) ra(7) rt(7)
SPU_RR: dict[int, str] = {
    # Built from the SPU ISA opcode table. Key = 11-bit opcode (insn >> 21).
    # The RI8 float conversions (cflts/cfltu/csflt/cuflt) are decoded
    # separately in spu_decode(); RI7 rotate/shift/gen-control immediates live
    # in ri7_table. Operand strings for RR ops are display-only: the lifter
    # dispatches on the mnemonic and reads raw register fields.
    # control / channels
    0x000: "stop", 0x001: "lnop", 0x002: "sync", 0x003: "dsync",
    0x00c: "mfspr", 0x00d: "rdch", 0x00f: "rchcnt",
    0x10c: "mtspr", 0x10d: "wrch", 0x201: "nop", 0x140: "stopd",
    # integer arithmetic
    0x040: "sf", 0x041: "or", 0x042: "bg", 0x048: "sfh", 0x049: "nor",
    0x053: "absdb", 0x0c0: "a", 0x0c1: "and", 0x0c2: "cg", 0x0c8: "ah",
    0x0c9: "nand", 0x0d3: "avgb",
    0x340: "addx", 0x341: "sfx", 0x342: "cgx", 0x343: "bgx",
    0x346: "mpyhha", 0x34e: "mpyhhau",
    # rotate / shift (register-variable)
    0x058: "rot", 0x059: "rotm", 0x05a: "rotma", 0x05b: "shl",
    0x05c: "roth", 0x05d: "rothm", 0x05e: "rotmah", 0x05f: "shlh",
    0x1cc: "rotqbybi", 0x1cd: "rotqmbybi", 0x1cf: "shlqbybi",
    0x1d8: "rotqbi", 0x1d9: "rotqmbi", 0x1db: "shlqbi",
    0x1dc: "rotqby", 0x1dd: "rotqmby", 0x1df: "shlqby",
    # gather / form-select-mask / count / extend
    0x1b0: "gb", 0x1b1: "gbh", 0x1b2: "gbb",
    0x1b4: "fsm", 0x1b5: "fsmh", 0x1b6: "fsmb",
    0x2a5: "clz", 0x2a6: "xswd", 0x2ae: "xshw", 0x2b4: "cntb", 0x2b6: "xsbh",
    0x1f0: "orx",
    # generate-controls-for-insertion (x-form)
    0x1d4: "cbx", 0x1d5: "chx", 0x1d6: "cwx", 0x1d7: "cdx",
    # load/store indexed
    0x1c4: "lqx", 0x144: "stqx",
    # branches (register-indirect)
    0x1a8: "bi", 0x1a9: "bisl", 0x1aa: "iret", 0x1ab: "bisled",
    0x128: "biz", 0x129: "binz", 0x12a: "bihz", 0x12b: "bihnz", 0x1ac: "hbr",
    # logical
    0x241: "xor", 0x249: "eqv", 0x2c1: "andc", 0x2c9: "orc",
    # compares
    0x240: "cgt", 0x248: "cgth", 0x250: "cgtb", 0x253: "sumb", 0x258: "hgt",
    0x2c0: "clgt", 0x2c8: "clgth", 0x2d0: "clgtb", 0x2d8: "hlgt",
    0x3c0: "ceq", 0x3c8: "ceqh", 0x3d0: "ceqb", 0x3d8: "heq",
    # multiply
    0x3c4: "mpy", 0x3c5: "mpyh", 0x3c6: "mpyhh", 0x3c7: "mpys",
    0x3cc: "mpyu", 0x3ce: "mpyhhu",
    # floating point
    0x2c2: "fcgt", 0x2c3: "dfcgt", 0x2c4: "fa", 0x2c5: "fs", 0x2c6: "fm",
    0x2ca: "fcmgt", 0x2cb: "dfcmgt", 0x2cc: "dfa", 0x2cd: "dfs", 0x2ce: "dfm",
    0x35c: "dfma", 0x35d: "dfms", 0x35e: "dfnms", 0x35f: "dfnma",
    0x3c2: "fceq", 0x3c3: "dfceq", 0x3ca: "fcmeq", 0x3cb: "dfcmeq",
    0x3b8: "fesd", 0x3b9: "frds", 0x398: "fscrrd", 0x3ba: "fscrwr",
    0x3bf: "dftsv", 0x3d4: "fi", 0x1b8: "frest", 0x1b9: "frsqest",
}

# Channel names, per the Cell BE Architecture manual.
# Channel number is in bits 11-7 of rdch/wrch/rchcnt instructions.
CHANNEL_NAMES = {
    0:  "SPU_RdEventStat",
    1:  "SPU_WrEventMask",
    2:  "SPU_WrEventAck",
    3:  "SPU_RdSigNotify1",
    4:  "SPU_RdSigNotify2",
    7:  "SPU_WrDec",
    8:  "SPU_RdDec",
    9:  "MFC_WrMSSyncReq",
    11: "SPU_RdEventMask",
    13: "SPU_RdMachStat",
    14: "SPU_WrSRR0",
    15: "SPU_RdSRR0",
    16: "MFC_LSA",
    17: "MFC_EAH",
    18: "MFC_EAL",
    19: "MFC_Size",
    20: "MFC_TagID",
    21: "MFC_Cmd",
    22: "MFC_WrTagMask",
    23: "MFC_WrTagUpdate",
    24: "MFC_RdTagStat",
    25: "MFC_RdListStallStat",
    26: "MFC_WrListStallAck",
    27: "MFC_RdAtomicStat",
    28: "SPU_WrOutMbox",
    29: "SPU_RdInMbox",
    30: "SPU_WrOutIntrMbox",
}

# ---------------------------------------------------------------------------
# Decode
# ---------------------------------------------------------------------------

def spu_decode(insn: int, addr: int = 0) -> SPUInstruction:
    """Decode a single 32-bit SPU instruction."""
    result = SPUInstruction(addr=addr, raw=insn)

    # Extract various opcode widths
    op4 = (insn >> 28) & 0xF
    op7 = (insn >> 25) & 0x7F
    op8 = (insn >> 24) & 0xFF
    op9 = (insn >> 23) & 0x1FF
    op11 = (insn >> 21) & 0x7FF

    # Fields
    rt = insn & 0x7F
    ra = (insn >> 7) & 0x7F
    rb = (insn >> 14) & 0x7F
    rc = (insn >> 21) & 0x7F  # for RRR format

    i10 = sign_extend((insn >> 14) & 0x3FF, 10)
    i16 = sign_extend((insn >> 7) & 0xFFFF, 16)
    i18 = (insn >> 7) & 0x3FFFF

    # ---- RRR format (4-bit opcode) ----
    if op4 in RRR_TABLE:
        mne = RRR_TABLE[op4]
        result.mnemonic = mne
        # RRR destination is the high field (>>21); low 7 bits are RC.
        result.operands = f"$r{rc}, $r{ra}, $r{rb}, $r{rt}"
        return result

    # ---- RI18 format (7-bit opcode) ----
    if op7 in RI18_TABLE:
        mne = RI18_TABLE[op7]
        result.mnemonic = mne

        if mne == "ila":
            result.operands = f"$r{rt}, 0x{i18:X}"
        elif mne == "hbra":
            # hbra (a-form) is OP(7) R0H(2) I16(16) R0L(7). brinst (branch-
            # instruction address hint) = sign_extend(R0H||R0L,9)*4+PC (R0L
            # occupies the RT bit-position, 25-31); brtarg (branch-target
            # address hint) = I16*4, absolute (not PC-relative, unlike hbrr
            # below). Example: word 0x10111213 @0x45ac -> brinst=0x45f8
            # (=addr+76), brtarg=0x8890 (=0x2224*4). Display only:
            # tools/spu_lifter.py treats hbra as an unconditional no-op and
            # never reads either operand.
            r0h = (i18 >> 16) & 0x3
            i16_field = i18 & 0xFFFF
            brinst_raw = (r0h << 7) | rt
            brinst = sign_extend(brinst_raw, 9) * 4 + addr
            brtarg = i16_field * 4
            result.operands = f"0x{brinst & 0xFFFFFFFF:X}, 0x{brtarg & 0xFFFFFFFF:X}"
        elif mne == "hbrr":
            # hbrr (relative form) is laid out like hbra in the ISA diagram
            # but does not split its target field into a 2-bit R0H + 16-bit
            # I16 the way hbra does: brtarg is the full 18-bit i18 span as
            # one signed value (sign_extend(i18,18)*4+PC, masked to the 256KB
            # local-store address space with & 0x3FFFF). Splitting it into a
            # 16-bit low field gives wrong results, e.g. word 0x127ff797
            # @0x3054: low-16 (signed or unsigned) gives 0x43010, the full
            # 18-bit signed form gives the correct 0x3010. brinst (the
            # branch-instruction address hint) still reads bits 16-17 of i18
            # as R0H composited with RT; those bits overlap the ones used in
            # brtarg's full-18-bit read, and the two hint values are computed
            # independently from the overlapping ranges by design. Display
            # only: tools/spu_lifter.py treats hbrr as an unconditional no-op
            # and never reads either operand.
            roh = (i18 >> 16) & 0x3
            brinst_raw = (roh << 7) | rt
            brinst = sign_extend(brinst_raw, 9) * 4 + addr
            brtarg = (sign_extend(i18, 18) * 4 + addr) & 0x3FFFF
            result.operands = f"0x{brinst & 0xFFFFFFFF:X}, 0x{brtarg:X}"
        return result

    # ---- RI16 format (9-bit opcode) ----
    # Must be checked before RI10: some RI16 op9 values share top 8 bits
    # with RI10 entries (brz/shlhi both have op8=0x20; brnz/shli have op8=0x21).
    if op9 in RI16_TABLE:
        mne = RI16_TABLE[op9]
        result.mnemonic = mne
        if mne in ("lqa", "stqa"):
            # a-form absolute: LSA = I16 << 2, with no further alignment
            # masking. The 16-bit encoded field is scaled x4 to reconstruct
            # an 18-bit value whose own bottom 2 bits are zero; masking the
            # result to a 16-byte (&0x3FFF0) boundary would zero real address
            # bits whenever the scaled value is not itself a multiple of 16.
            # Example: word 0x30802ecb decodes to $r75,0x174 (0x5d<<2, no
            # mask). Display only for codegen impact: see the LQA/STQA
            # handling in tools/spu_lifter.py.
            lsa = i16 << 2
            result.operands = f"$r{rt}, 0x{lsa & 0x3FFFF:X}"
        elif mne == "il":
            # i16 is already sign-extended above.
            result.operands = f"$r{rt}, {i16}"
        elif mne in ("iohl", "ilh", "ilhu"):
            result.operands = f"$r{rt}, 0x{i16 & 0xFFFF:X}"
        elif mne in ("brz", "brnz", "brhz", "brhnz"):
            target = i16 * 4 + addr
            result.operands = f"$r{rt}, 0x{target & 0x3FFFF:X}"
        elif mne == "br":                 # relative, no link
            target = (i16 * 4 + addr) & 0x3FFFF
            result.operands = f"0x{target:X}"
        elif mne == "brsl":                # relative + set link: RT is the
            # link-destination register (holds PC+4 after the branch), a
            # real encoded operand. Display only: tools/spu_lifter.py
            # recovers it via `insn.raw & 0x7F` directly.
            target = (i16 * 4 + addr) & 0x3FFFF
            result.operands = f"$r{rt}, 0x{target:X}"
        elif mne == "bra":                 # absolute, no link
            target = (i16 * 4) & 0x3FFFF
            result.operands = f"0x{target:X}"
        elif mne == "brasl":               # absolute + set link (see brsl note)
            target = (i16 * 4) & 0x3FFFF
            result.operands = f"$r{rt}, 0x{target:X}"
        elif mne in ("lqr", "stqr"):
            # r-form: LSA = (pc + I16*4), masked only to the 256KB local-
            # store address space (& 0x3FFFF), not forced to a 16-byte
            # boundary (same as lqa/stqa above). I16 is sign-extended
            # (relative).
            target = (i16 * 4 + addr) & 0x3FFFF
            result.operands = f"$r{rt}, 0x{target:X}"
        else:
            result.operands = f"$r{rt}, 0x{i16 & 0xFFFF:X}"
        return result

    # ---- Channel instructions (checked before RI10 to avoid wrch/shli clash) ----
    # wrch (op11=0x10D) shares op8=0x21 with shli; must be identified by op11.
    # RDCH/WRCH/RCHCNT are RR-form per the SPU ISA: OP(11) RB(7) RA(7) RT(7).
    # The channel number is the full 7-bit RA field (bits 7-13), the same
    # field every other RR instruction uses for its second register operand,
    # not a narrower dedicated "channel" field; truncating it to 5 bits would
    # wrap any channel number >= 32 modulo 32.
    _ch_field = (insn >> 7) & 0x7F
    if op11 == 0b00000001101:   # rdch RT, CA    op11=0x00D=13
        _ch = CHANNEL_NAMES.get(_ch_field, f"ch{_ch_field}")
        result.mnemonic = "rdch"
        result.operands = f"$r{rt}, {_ch}"
        return result
    if op11 == 0b00100001101:   # wrch CA, RT    op11=0x10D=269
        _ch = CHANNEL_NAMES.get(_ch_field, f"ch{_ch_field}")
        result.mnemonic = "wrch"
        result.operands = f"{_ch}, $r{rt}"
        return result
    if op11 == 0b00000001111:   # rchcnt RT, CA  op11=0x00F=15
        _ch = CHANNEL_NAMES.get(_ch_field, f"ch{_ch_field}")
        result.mnemonic = "rchcnt"
        result.operands = f"$r{rt}, {_ch}"
        return result

    # ---- RI10 format (8-bit opcode) ----
    if op8 in RI10_TABLE:
        mne = RI10_TABLE[op8]
        result.mnemonic = mne
        if mne in ("lqd", "stqd"):
            offset = i10 << 4  # quadword offset
            if offset < 0:
                disp = f"-0x{-offset:X}"
            else:
                disp = f"0x{offset:X}"
            result.operands = f"$r{rt}, {disp}($r{ra})"
        else:
            result.operands = f"$r{rt}, $r{ra}, {i10}"
        return result

    # ---- RI8 format (10-bit opcode, 8-bit immediate): float<->int conversions ----
    op10 = (insn >> 22) & 0x3FF
    ri8_table = {0b0111011000: "cflts", 0b0111011001: "cfltu",
                 0b0111011010: "csflt", 0b0111011011: "cuflt"}
    if op10 in ri8_table:
        result.mnemonic = ri8_table[op10]
        i8 = (insn >> 14) & 0xFF
        result.operands = f"$r{rt}, $r{ra}, {i8}"
        return result

    # ---- RR format (11-bit opcode) ----
    if op11 in SPU_RR:
        mne = SPU_RR[op11]
        result.mnemonic = mne

        # Branches
        if mne == "bi":
            result.operands = f"$r{ra}"
            return result
        if mne in ("bisl", "bisled"):
            # bisl/bisled set the link register (RT) as well as branching to
            # RA; RT is a real encoded operand. Display only: tools/spu_lifter.py
            # recovers it via `insn.raw & 0x7F` directly. bisled shares bisl's
            # RR operand shape (RT, RA; RB unused); the generic 3-operand
            # default would hand the lifter's ops[-1]-based target read
            # (bisl/bisled emission in spu_lifter.py) RB instead of RA.
            result.operands = f"$r{rt}, $r{ra}"
            return result
        if mne in ("biz", "binz", "bihz", "bihnz"):
            result.operands = f"$r{rt}, $r{ra}"
            return result
        if mne == "hbr":
            # hbr (r-form) is OP(12 incl P-bit) /// ROH RA ROL, not a plain
            # RR-format $ra,$rb pair. brinst = sign_extend(ROH||ROL,9)*4+PC,
            # the same composite-with-RT-position math as hbra/hbrr's brinst
            # (ROL occupies the RT bit-position, 25-31); brtarg is the
            # runtime contents of RA's preferred slot, a register reference
            # rather than a static address, so it prints as a register. There
            # is no $rb in this format; those bits are ROH = (insn>>14)&0x3.
            # Example: word 0x3580000d @0x37b8 -> brinst=0x37ec, ra=lr.
            roh = (insn >> 14) & 0x3
            brinst_raw = (roh << 7) | rt
            brinst = sign_extend(brinst_raw, 9) * 4 + addr
            result.operands = f"0x{brinst & 0xFFFFFFFF:X}, $r{ra}"
            return result

        # Stop: carries a real 14-bit stop-and-signal code in the low bits of
        # the whole instruction word (SPU_Status StopCode = insn bits 18:31;
        # lv2's stop-code protocol dispatches on this: 0x0..0xFF user codes,
        # 0x100-0x120 reserved OS syscalls e.g.
        # SYS_SPU_THREAD_STOP_THREAD_EXIT=0x102). Display only:
        # tools/spu_lifter.py's `stop` codegen does not read this operand
        # (unconditional SPU_STATUS_STOPPED_BY_STOP).
        if mne == "stop":
            code = insn & 0x3FFF
            result.operands = f"0x{code:X}"
            return result
        # nop/sync/dsync: no real operand consumed by the ISA semantics.
        if mne in ("lnop", "nop", "sync", "dsync"):
            # sync carries a C feature bit (BE bit 11 => mask 1<<20): channel
            # synchronization before instruction synchronization. It is
            # decoded as "syncc" so the lifter can act on the bit. The lifter
            # handles "syncc" (sync family in spu_lifter.py); keep these two
            # in lockstep.
            if mne == "sync" and (insn & 0x00100000):
                result.mnemonic = "syncc"
            return result

        # Single-operand
        if mne in ("clz", "cntb", "gb", "gbh", "fscrrd", "orx", "fsm", "fsmb"):
            result.operands = f"$r{rt}, $r{ra}"
            return result

        # Default RR: 3-register
        result.operands = f"$r{rt}, $r{ra}, $r{rb}"
        return result

    # ---- Shift/rotate immediate forms (RI7 = 11-bit opcode, 7-bit immediate) ----
    # These have the format: opcd(11) i7(7) ra(7) rt(7)
    # Keyed by op11 = insn >> 21.
    ri7_table: dict[int, str] = {
        # word / halfword rotate & shift immediate (0x78-0x7f)
        0b00001111000: "roti",      # 0x78 rotate word immediate
        0b00001111001: "rotmi",     # 0x79 rotate & mask word immediate
        0b00001111010: "rotmai",    # 0x7a rotate & mask algebraic word immediate
        0b00001111011: "shli",      # 0x7b shift left word immediate
        0b00001111100: "rothi",     # 0x7c rotate halfword immediate
        0b00001111101: "rothmi",    # 0x7d rotate & mask halfword immediate
        0b00001111110: "rotmahi",   # 0x7e rotate & mask algebraic halfword immediate
        0b00001111111: "shlhi",     # 0x7f shift left halfword immediate
        # generate-controls-for-insertion d-form (0x1F4-0x1F7)
        0b00111110100: "cbd",       # 0x1F4 gen controls for byte insertion
        0b00111110101: "chd",       # 0x1F5 gen controls for halfword insertion
        0b00111110110: "cwd",       # 0x1F6 gen controls for word insertion
        0b00111110111: "cdd",       # 0x1F7 gen controls for doubleword insertion
        # quadword rotate & shift by bits/bytes immediate (0x1F8-0x1FF)
        0b00111111000: "rotqbii",   # 0x1F8 rotate quadword by bits immediate
        0b00111111001: "rotqmbii",  # 0x1F9 rotate & mask quadword by bits immediate
        0b00111111011: "shlqbii",   # 0x1FB shift left quadword by bits immediate
        0b00111111100: "rotqbyi",   # 0x1FC rotate quadword by bytes immediate
        0b00111111101: "rotqmbyi",  # 0x1FD rotate & mask quadword by bytes immediate
        0b00111111111: "shlqbyi",   # 0x1FF shift left quadword by bytes immediate
    }
    # I7 signedness per instruction, from the SPU assembly language
    # specification's valid-immediate table: rothi/roti/rotqbyi/rotmai/rotmi/
    # rothmi/rotmahi/rotqmbii/shli/shlhi/shlqbii/shlqbyi are in a signed class
    # (s7/s6/s3, all sub-ranges of the same signed 7-bit field); cbd/chd/cwd/
    # cdd are unsigned (u7, 0-127), since the assembler places no limits on
    # them and encodes the least significant bits. rotqbii is listed as u3
    # (0-7), but that is only the sane-input warning threshold for a rotate
    # by bits within a byte, not a different bit-width or display
    # difference, so it stays signed like its siblings.
    RI7_UNSIGNED = {"cbd", "chd", "cwd", "cdd"}
    if op11 in ri7_table:
        # Signed-class immediates with bit 6 set display as negative (roti
        # $rt,$ra,-64, not 64). The choice does not affect codegen: every
        # runtime/spu/spu_helpers.h consumer of the signed family masks the
        # shift/rotate amount with a power-of-two <=64 before use, and the
        # unsigned and signed forms differ by exactly 128, a multiple of every
        # such mask. cbd/chd/cwd/cdd stay unsigned (their runtime helpers add
        # i7 to a position then mask with &0xF, and 128 is a multiple of 16
        # too).
        mne = ri7_table[op11]
        if mne in RI7_UNSIGNED:
            i7 = rb  # bits 14-20, unsigned (u7)
        else:
            i7 = sign_extend(rb, 7)  # bits 14-20, signed (s-class)
        result.mnemonic = mne
        result.operands = f"$r{rt}, $r{ra}, {i7}"
        return result

    # ---- Fallback ----
    result.mnemonic = ".word"
    result.operands = f"0x{insn:08X}"
    return result

# ---------------------------------------------------------------------------
# Bulk disassembly
# ---------------------------------------------------------------------------

def disassemble_spu(data: bytes, base_addr: int = 0) -> list[SPUInstruction]:
    """Disassemble a buffer of SPU instructions (big-endian)."""
    instructions: list[SPUInstruction] = []
    for off in range(0, len(data) - 3, 4):
        raw = struct.unpack_from(">I", data, off)[0]
        addr = base_addr + off
        insn = spu_decode(raw, addr)
        instructions.append(insn)
    return instructions

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="SPU disassembler for PS3 binaries")
    parser.add_argument("input", help="Input binary or ELF file")
    parser.add_argument("--base", type=lambda x: int(x, 0), default=0,
                        help="Base address (hex ok)")
    parser.add_argument("--offset", type=lambda x: int(x, 0), default=0,
                        help="Start offset in file")
    parser.add_argument("--length", type=lambda x: int(x, 0), default=0,
                        help="Bytes to disassemble (0=all)")
    parser.add_argument("--json", action="store_true", help="Output as JSON")
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        file_data = f.read()

    data = file_data[args.offset:]
    if args.length:
        data = data[:args.length]

    instructions = disassemble_spu(data, args.base)

    if args.json:
        out = [{"addr": f"0x{i.addr:08X}", "hex": f"{i.raw:08X}",
                "mnemonic": i.mnemonic, "operands": i.operands}
               for i in instructions]
        print(json.dumps(out, indent=2))
    else:
        for i in instructions:
            print(i)


if __name__ == "__main__":
    main()
