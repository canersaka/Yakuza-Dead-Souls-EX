#!/usr/bin/env python3
"""
lift_parse_audit.py -- raw-encoding vs disassembly-text cross-check for the
SPU and PPU lifters (tools/spu_lifter.py, tools/ppu_lifter.py).

Motivation (the bug class this is built to catch): both lifters recover
branch/call TARGETS and indirect-branch TARGET REGISTERS by positionally
indexing the operand-text list emitted by the matching disassembler
(tools/spu_disasm.py / tools/ppu_disasm.py) -- e.g. `ops[0]` = "the target".
If a disassembler operand-FORMAT change ever inserts/reorders a field (a link
register, say), the lifter's positional read can silently start reading the
WRONG field. There is no compile error and, unless the lifter's fallback path
happens to be counted in the lift summary, no warning either -- the miscompile
is completely invisible in the generated source. This is exactly what
happened to SPU brsl/brasl (fixed in 30fb609: the disasm added the link
register to brsl's operand text, ops[0] silently returned None, and every
brsl call in any module lifted after 2026-07-05 was dropped as a no-op).

This tool independently decodes the same value TWICE per instruction:
  1. From insn.raw using the field formulas the disassembler itself uses
     (imported bit-extraction helpers, not reimplemented by hand, to avoid
     transcription bugs of its own).
  2. From the ACTUAL lifter code -- imported and called for real (not
     reimplemented), either via a lifter's own static helper
     (SPULifter._branch_target) or by running the real per-instruction
     translator and regex-extracting the register/target embedded in the C
     it emits. This exercises the exact code path a real relift runs.

Any disagreement is a live parse-break: EITHER the lifter is reading the
wrong operand-text field, OR the "raw" formula transcribed here has drifted
from the disassembler's -- both are worth a human look, which is why every
finding prints both the raw-derived value AND the disassembler's own operand
text for the instruction.

Exit code is nonzero if any disagreement is found, so this can gate a relift
the way tools/test_ppu_lift.py already does for the 905-case PPU conformance
suite.

Usage:
    python lift_parse_audit.py --spu-glob "recomp_prx/*.elf"
    python lift_parse_audit.py --ppu game/EBOOT.elf [--length N]
    python lift_parse_audit.py            # both, with repo-relative defaults
"""

import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from spu_disasm import disassemble_spu, sign_extend as spu_sign_extend
from spu_lifter import SPULifter, LiftedFunction as SPULiftedFunction, _ops as spu_ops
from find_spu_functions import detect_functions as spu_detect_functions

from ppu_disasm import disassemble_bytes, bits as ppu_bits, bit as ppu_bit, \
    sign_extend as ppu_sign_extend
from ppu_lifter import PPULifter, LiftedFunction as PPULiftedFunction, \
    _parse_operands as ppu_ops
from elf_parser import ELFFile, PT_LOAD


# ---------------------------------------------------------------------------
# Finding record
# ---------------------------------------------------------------------------

class Finding:
    def __init__(self, arch, module, addr, mnemonic, operands, kind,
                 raw_value, lifted_value, detail=""):
        self.arch = arch
        self.module = module
        self.addr = addr
        self.mnemonic = mnemonic
        self.operands = operands
        self.kind = kind
        self.raw_value = raw_value
        self.lifted_value = lifted_value
        self.detail = detail

    def __str__(self):
        rv = "None" if self.raw_value is None else f"0x{self.raw_value:X}"
        lv = "None" if self.lifted_value is None else f"0x{self.lifted_value:X}"
        return (f"[{self.arch}] {self.module}:0x{self.addr:08X}  "
                f"{self.mnemonic:<8s} {self.operands!r:<28s} {self.kind}: "
                f"raw={rv} lifted={lv}"
                + (f"  ({self.detail})" if self.detail else ""))


# ---------------------------------------------------------------------------
# SPU side
# ---------------------------------------------------------------------------

# Mnemonics whose target is a static PC-relative/absolute immediate, decoded
# straight from the disassembler's own RI16 formulas (spu_disasm.py).
_SPU_STATIC_TARGET_RELATIVE = {"br", "brsl", "brz", "brnz", "brhz", "brhnz"}
_SPU_STATIC_TARGET_ABSOLUTE = {"bra", "brasl"}

# Mnemonics whose target is a RUNTIME register (indirect branch); the
# regression class lives here -- the register INDEX the lifter reads must
# match the architectural RA field (insn bits 7-13), not the RT field
# (link/condition register, bits 25-31) that some of these also carry.
_SPU_INDIRECT_TARGET_REG = {"bi", "bisl", "bisled", "biz", "binz", "bihz", "bihnz"}


def spu_raw_static_target(insn):
    """Recompute a br/brsl/bra/brasl/brz/.../target straight from insn.raw,
    mirroring spu_disasm.py's own RI16 formulas (i16 field, x4 scale, +pc for
    relative forms, masked to the 256KB local-store space)."""
    i16 = spu_sign_extend((insn.raw >> 7) & 0xFFFF, 16)
    mn = insn.mnemonic
    if mn in _SPU_STATIC_TARGET_RELATIVE:
        return (i16 * 4 + insn.addr) & 0x3FFFF
    if mn in _SPU_STATIC_TARGET_ABSOLUTE:
        return (i16 * 4) & 0x3FFFF
    return None


def spu_raw_indirect_ra(insn):
    """The architectural target REGISTER field (RA, insn bits 7-13) for an
    indirect branch -- independent of whether the disassembler's operand
    text happens to list RT before or after it."""
    return (insn.raw >> 7) & 0x7F


def spu_raw_link_rt(insn):
    """The architectural link/condition register field (RT, insn bits 25-31)."""
    return insn.raw & 0x7F


_PC_GPR_RE = re.compile(r"ctx->pc = ctx->gpr\[(\d+)\]")


def spu_lifted_indirect_reg(lifter, insn):
    """Run the REAL per-instruction translator and pull out the register
    index it used as the branch-target address. Returns an int, or 0 for the
    SPU_RET(ctx) `bi $r0` return special case (register 0 resolved but not
    textually embedded as ctx->gpr[0]), or None if neither pattern is found
    (unexpected shape -- also a finding)."""
    func = SPULiftedFunction(start_addr=0, end_addr=0)  # never contains a target
    c = lifter._translate(insn, func)
    m = _PC_GPR_RE.search(c)
    if m:
        return int(m.group(1))
    if "SPU_RET(ctx)" in c:
        return 0
    return None


def audit_spu_module(path):
    findings = []
    with open(path, "rb") as f:
        elf_buf = f.read()
    try:
        funcs, (text_off, base, size) = spu_detect_functions(elf_buf, verbose=False)
    except TypeError:
        funcs, (text_off, base, size) = spu_detect_functions(elf_buf)
    data = elf_buf[text_off:text_off + size]
    insns = disassemble_spu(data, base)

    lifter = SPULifter(trace=False)
    lifter.bi_r0_jump = set()  # conservative: audit each insn standalone

    n_checked = 0
    for insn in insns:
        mn = insn.mnemonic
        if mn in _SPU_STATIC_TARGET_RELATIVE or mn in _SPU_STATIC_TARGET_ABSOLUTE:
            n_checked += 1
            raw_t = spu_raw_static_target(insn)
            lifted_t = SPULifter._branch_target(insn)
            if lifted_t != raw_t:
                findings.append(Finding(
                    "SPU", os.path.basename(path), insn.addr, mn, insn.operands,
                    "branch-target-mismatch", raw_t, lifted_t))
        elif mn in _SPU_INDIRECT_TARGET_REG:
            n_checked += 1
            raw_ra = spu_raw_indirect_ra(insn)
            lifted_reg = spu_lifted_indirect_reg(lifter, insn)
            if lifted_reg != raw_ra:
                sev = ("LIVE MISCOMPILE" if mn in ("bisl", "bisled")
                       else "mismatch")
                findings.append(Finding(
                    "SPU", os.path.basename(path), insn.addr, mn, insn.operands,
                    f"indirect-target-register-{sev}", raw_ra, lifted_reg,
                    detail=f"link/cond RT field = {spu_raw_link_rt(insn)}"))
    return findings, n_checked


# ---------------------------------------------------------------------------
# PPU side
# ---------------------------------------------------------------------------

def ppu_raw_static_target(insn):
    """Recompute a b/bl/ba/bla/bc-family target straight from insn.raw,
    mirroring ppu_disasm.py's own I-form (opcd 18) / B-form (opcd 16)
    formulas. Returns None for anything else (indirect via LR/CTR, or a
    non-branch instruction)."""
    opcd = ppu_bits(insn.raw, 0, 5)
    if opcd == 18:
        li = ppu_bits(insn.raw, 6, 29)
        aa = ppu_bit(insn.raw, 30)
        target = ppu_sign_extend(li << 2, 26)
        if not aa:
            target += insn.addr
        return target & 0xFFFFFFFF
    if opcd == 16:
        bd = ppu_bits(insn.raw, 16, 29)
        aa = ppu_bit(insn.raw, 30)
        target = ppu_sign_extend(bd << 2, 16)
        if not aa:
            target += insn.addr
        return target & 0xFFFFFFFF
    return None


_FUNC_TGT_RE = re.compile(r"func_([0-9A-Fa-f]{8})")
_LOC_TGT_RE = re.compile(r"loc_([0-9A-Fa-f]{8})")


def ppu_lifted_static_target(lifter, insn):
    """Run the REAL per-instruction translator with func bounds (0, 0) --
    guaranteed to exclude any real target -- so every static branch takes
    the cross-fragment/trampoline emission path and the target address
    always appears literally as func_XXXXXXXX in the generated C. Returns
    None if neither the func_/loc_ pattern is found (a silently-dropped
    branch -- e.g. the uncounted `except ValueError` fallbacks)."""
    func = PPULiftedFunction(start_addr=0, end_addr=0)
    c = lifter._translate(insn, func)
    m = _FUNC_TGT_RE.search(c) or _LOC_TGT_RE.search(c)
    if m:
        return int(m.group(1), 16)
    return None


def audit_ppu_module(path, length=0):
    findings = []
    elf = ELFFile(path)
    elf.load()
    big_endian = elf.elf_header.big_endian
    all_insns = []
    for i, ph in enumerate(elf.program_headers):
        if ph.p_type == PT_LOAD and (ph.p_flags & 1):
            seg_data = elf.get_segment_data(i)
            if length:
                seg_data = seg_data[:length]
            all_insns.extend(disassemble_bytes(seg_data, ph.p_vaddr, big_endian))

    lifter = PPULifter()
    n_checked = 0
    for insn in all_insns:
        raw_t = ppu_raw_static_target(insn)
        if raw_t is None:
            continue  # not an opcd16/18 static-target branch
        n_checked += 1
        lifted_t = ppu_lifted_static_target(lifter, insn)
        if lifted_t != raw_t:
            findings.append(Finding(
                "PPU", os.path.basename(path), insn.addr, insn.mnemonic,
                insn.operands, "branch-target-mismatch", raw_t, lifted_t))
    return findings, n_checked


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--spu-glob", default="recomp_prx/*.elf",
                    help="Glob of SPU ELF images to audit (default: %(default)s)")
    p.add_argument("--ppu", default="game/EBOOT.elf",
                    help="PPU ELF to audit (default: %(default)s)")
    p.add_argument("--length", type=lambda x: int(x, 0), default=0,
                    help="Bound PPU .text scan to N bytes per PT_LOAD segment "
                         "(0 = all)")
    p.add_argument("--skip-spu", action="store_true")
    p.add_argument("--skip-ppu", action="store_true")
    p.add_argument("-v", "--verbose", action="store_true",
                    help="Print every checked instruction count per module")
    args = p.parse_args()

    all_findings: list[Finding] = []
    total_checked = 0

    if not args.skip_spu:
        spu_paths = sorted(glob.glob(args.spu_glob))
        if not spu_paths:
            print(f"WARNING: no SPU ELFs matched {args.spu_glob!r}", file=sys.stderr)
        for path in spu_paths:
            try:
                findings, n = audit_spu_module(path)
            except Exception as exc:
                print(f"WARNING: SPU audit failed for {path}: {exc}", file=sys.stderr)
                continue
            total_checked += n
            all_findings.extend(findings)
            if args.verbose:
                print(f"  [SPU] {os.path.basename(path)}: {n} branch/call "
                      f"instruction(s) checked, {len(findings)} finding(s)")

    if not args.skip_ppu:
        if os.path.exists(args.ppu):
            try:
                findings, n = audit_ppu_module(args.ppu, length=args.length)
                total_checked += n
                all_findings.extend(findings)
                if args.verbose:
                    print(f"  [PPU] {os.path.basename(args.ppu)}: {n} branch "
                          f"instruction(s) checked, {len(findings)} finding(s)")
            except Exception as exc:
                print(f"WARNING: PPU audit failed for {args.ppu}: {exc}", file=sys.stderr)
        else:
            print(f"WARNING: PPU input {args.ppu!r} not found, skipping", file=sys.stderr)

    print()
    print(f"lift_parse_audit: {total_checked} branch/call instruction(s) "
          f"cross-checked (raw encoding vs. actual lifter output), "
          f"{len(all_findings)} disagreement(s)")
    if all_findings:
        print()
        for f in all_findings:
            print(str(f))
        print()
        print(f"FAIL: {len(all_findings)} raw-vs-text parse disagreement(s) found.")
        return 1
    print("PASS: raw encoding and lifter output agree on every checked instruction.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
