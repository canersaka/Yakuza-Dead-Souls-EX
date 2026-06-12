#!/usr/bin/env python3
"""
PPU function boundary detector for PS3 binaries.

Analyses a PPU ELF or raw binary for function entry/exit points by:
  - Seeding from the .opd function-descriptor table (ELF inputs) -- the
    authoritative list of every address-taken function, located by scanning
    data segments for {code, TOC} descriptor pairs
  - Seeding the ELF entry point (also an OPD descriptor)
  - Detecting standard PPU prologues (mflr r0; stw/std r0, X(r1); stwu/stdu r1, -Y(r1))
  - Detecting epilogues (lwz/ld r0, X(r1); mtlr r0; blr)
  - Following bl (branch-and-link) targets to discover called functions
  - Clipping overlapping ranges so every byte belongs to at most one function
    (inter-function padding/data is deliberately left unattributed; the
    lifter's discovery pass picks up any genuinely reachable stragglers)
  - Building a call graph

Usage:
    python find_functions.py <input_elf_or_bin> [--raw] [--base ADDR] [--json]
                             [--call-graph] [--min-size N] [--no-opd]
"""

import argparse
import json
import os
import struct
import sys
import time
from collections import Counter
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ppu_disasm import (Instruction, disassemble_bytes, decode, sign_extend,
                        bits, bit)

# ---------------------------------------------------------------------------
# Instruction classification helpers
# ---------------------------------------------------------------------------

def _is_mflr_r0(insn: Instruction) -> bool:
    return insn.mnemonic == "mflr" and insn.operands.strip() == "r0"


def _is_mtlr_r0(insn: Instruction) -> bool:
    return insn.mnemonic == "mtlr" and insn.operands.strip() == "r0"


def _is_blr(insn: Instruction) -> bool:
    return insn.mnemonic == "blr"


def _is_store_lr(insn: Instruction) -> bool:
    """Check for stw r0, X(r1) or std r0, X(r1) -- saving LR to stack."""
    if insn.mnemonic in ("stw", "std"):
        ops = insn.operands.replace(" ", "")
        if ops.startswith("r0,") and "(r1)" in ops:
            return True
    return False


def _is_load_lr(insn: Instruction) -> bool:
    """Check for lwz r0, X(r1) or ld r0, X(r1) -- restoring LR from stack."""
    if insn.mnemonic in ("lwz", "ld"):
        ops = insn.operands.replace(" ", "")
        if ops.startswith("r0,") and "(r1)" in ops:
            return True
    return False


def _is_stack_alloc(insn: Instruction) -> bool:
    """Check for stwu r1, -X(r1) or stdu r1, -X(r1) -- stack frame creation."""
    if insn.mnemonic in ("stwu", "stdu"):
        ops = insn.operands.replace(" ", "")
        if ops.startswith("r1,") and "(r1)" in ops and "-" in ops:
            return True
    return False


def _is_bl(insn: Instruction) -> bool:
    """Check for bl (branch-and-link, non-absolute)."""
    return insn.mnemonic == "bl"


def _bl_target(insn: Instruction) -> int | None:
    """Extract the target address of a bl instruction."""
    if insn.mnemonic != "bl":
        return None
    ops = insn.operands.strip()
    try:
        return int(ops, 16)
    except ValueError:
        return None

# ---------------------------------------------------------------------------
# Function data
# ---------------------------------------------------------------------------

@dataclass
class Function:
    """A detected function with start/end addresses and metadata."""
    start: int = 0
    end: int = 0          # address after last instruction
    stack_size: int = 0   # frame size if detected
    calls: list[int] = field(default_factory=list)  # addresses this function calls
    callers: list[int] = field(default_factory=list)  # addresses that call this function
    has_prologue: bool = False
    has_epilogue: bool = False
    is_leaf: bool = False  # no bl instructions, no LR save

    @property
    def size(self) -> int:
        return self.end - self.start

    def to_dict(self) -> dict:
        return {
            "start": f"0x{self.start:08X}",
            "end": f"0x{self.end:08X}",
            "size": self.size,
            "stack_size": self.stack_size,
            "has_prologue": self.has_prologue,
            "has_epilogue": self.has_epilogue,
            "is_leaf": self.is_leaf,
            "calls": [f"0x{a:08X}" for a in self.calls],
            "num_callers": len(self.callers),
        }

# ---------------------------------------------------------------------------
# Function finder
# ---------------------------------------------------------------------------

def _note(msg: str) -> None:
    """Progress heartbeat on stderr (stdout stays clean for --json)."""
    print(f"[find_functions] {msg}", file=sys.stderr, flush=True)


class FunctionFinder:
    """Detect function boundaries in a PPU instruction stream."""

    def __init__(self, instructions: list[Instruction], min_size: int = 8,
                 seeds: set[int] | None = None,
                 exec_ranges: list[tuple[int, int]] | None = None):
        self.instructions = instructions
        self.min_size = min_size  # minimum function size in bytes
        self.seeds = seeds or set()           # known-true starts (.opd, entry)
        self.exec_ranges = exec_ranges or []  # [(lo, hi)] of executable text

        # Build addr -> index lookup
        self._addr_to_idx: dict[int, int] = {}
        for i, insn in enumerate(instructions):
            self._addr_to_idx[insn.addr] = i

        self.functions: dict[int, Function] = {}  # start_addr -> Function

    def find_prologue_functions(self) -> None:
        """Scan for standard function prologues and their matching epilogues."""
        n = len(self.instructions)

        i = 0
        while i < n:
            insn = self.instructions[i]

            # Look for: mflr r0
            if not _is_mflr_r0(insn):
                i += 1
                continue

            # Check next 1-3 instructions for stw/std r0, X(r1)
            found_store = False
            found_stack = False
            stack_size = 0
            scan_end = min(i + 5, n)

            for j in range(i + 1, scan_end):
                nxt = self.instructions[j]
                if _is_store_lr(nxt):
                    found_store = True
                if _is_stack_alloc(nxt):
                    found_stack = True
                    # Extract stack size from operands
                    ops = nxt.operands.replace(" ", "")
                    try:
                        # "r1,-0x50(r1)" -> extract -0x50
                        disp_str = ops.split(",")[1].split("(")[0]
                        stack_size = abs(int(disp_str, 0))
                    except (ValueError, IndexError):
                        stack_size = 0
                    break

            if not found_store:
                i += 1
                continue

            # The PPC64 ELFv1 prologue may allocate the frame *before* saving
            # LR, i.e. `stdu r1,-X(r1)` then `mflr r0`. In that case the true
            # function entry (and the address used as a bl target) is the stdu,
            # one instruction earlier — anchoring on mflr registers the start
            # 4 bytes late, which later makes find_leaf_functions discard the
            # real entry as a sub-min_size sliver. Back up to the stdu so the
            # start matches the actual call target.
            start_idx = i
            if i > 0 and _is_stack_alloc(self.instructions[i - 1]):
                start_idx = i - 1
                prev = self.instructions[i - 1]
                ops = prev.operands.replace(" ", "")
                try:
                    stack_size = abs(int(ops.split(",")[1].split("(")[0], 0))
                except (ValueError, IndexError):
                    pass

            func_start = self.instructions[start_idx].addr
            func = Function(start=func_start, stack_size=stack_size,
                            has_prologue=True)

            # Scan forward for epilogue (mtlr r0; blr) or just blr
            func_end = None
            for j in range(i + 1, n):
                curr = self.instructions[j]

                # Collect bl targets
                if _is_bl(curr):
                    target = _bl_target(curr)
                    if target is not None:
                        func.calls.append(target)

                # Check for epilogue pattern
                if _is_blr(curr):
                    # Look back 1-3 instructions for mtlr r0
                    for k in range(max(0, j - 3), j):
                        if _is_mtlr_r0(self.instructions[k]):
                            func.has_epilogue = True
                            break

                    func_end = curr.addr + 4
                    break

                # Safety: if we hit another prologue, stop
                if j > i + 2 and _is_mflr_r0(curr):
                    func_end = curr.addr
                    break

            if func_end is None:
                # No blr found, estimate end
                func_end = self.instructions[min(i + 100, n - 1)].addr + 4

            func.end = func_end

            if func.size >= self.min_size:
                self.functions[func_start] = func

            # Advance past this function
            end_idx = self._addr_to_idx.get(func_end, i + 1)
            i = end_idx if end_idx > i else i + 1
            continue

    def find_leaf_functions(self) -> None:
        """Detect leaf functions that don't save LR.

        These are identified as bl targets that don't appear to have a
        standard prologue.  They end at the first blr.
        """
        # Collect all bl targets
        bl_targets: set[int] = set()
        for func in self.functions.values():
            bl_targets.update(func.calls)

        # Also scan instructions for bl -- but only trust calls that sit
        # inside an already-detected function (raw data in the executable
        # segment decodes into phantom bl instructions otherwise).
        covered = self._coverage_fn()
        for insn in self.instructions:
            if _is_bl(insn) and covered(insn.addr):
                target = _bl_target(insn)
                if target is not None:
                    bl_targets.add(target)

        # For each target not already a known function, scan for blr
        for target in bl_targets:
            if target in self.functions:
                continue
            idx = self._addr_to_idx.get(target)
            if idx is None:
                continue

            # Scan for blr (max 200 instructions for a leaf)
            func = Function(start=target, is_leaf=True)
            found_end = False
            for j in range(idx, min(idx + 200, len(self.instructions))):
                curr = self.instructions[j]
                if _is_bl(curr):
                    t = _bl_target(curr)
                    if t is not None:
                        func.calls.append(t)
                        func.is_leaf = False
                if _is_blr(curr):
                    func.end = curr.addr + 4
                    found_end = True
                    break
                # If we hit another known function start, stop
                if curr.addr != target and curr.addr in self.functions:
                    func.end = curr.addr
                    found_end = True
                    break

            if found_end and func.size >= self.min_size:
                self.functions[target] = func

    def build_call_graph(self) -> None:
        """Populate caller lists from call lists."""
        for addr, func in self.functions.items():
            for call_target in func.calls:
                if call_target in self.functions:
                    self.functions[call_target].callers.append(addr)

    def _coverage_fn(self):
        """Return a fast addr-inside-a-known-function predicate."""
        import bisect
        starts_sorted = sorted(self.functions)
        ends = [self.functions[s].end for s in starts_sorted]

        def covered(addr: int) -> bool:
            i = bisect.bisect_right(starts_sorted, addr) - 1
            return i >= 0 and addr < ends[i]

        return covered

    def find_branch_target_functions(self) -> None:
        """Third pass: find branch targets that fall outside known function boundaries.

        When the lifter generates code, any branch target that isn't inside a
        known function becomes a missing stub (empty function). This pass scans
        for branch targets (b, bc, etc.) and creates minimal function entries
        for any that aren't already covered.

        Only branches that themselves sit INSIDE a detected function are
        trusted as sources: executable segments also contain read-only data,
        and raw data decoded as instructions yields phantom branch targets.
        Real code never branches into data, so filtering by source kills the
        phantoms without losing real targets.
        """
        covered = self._coverage_fn()

        all_targets: set[int] = set()
        for insn in self.instructions:
            mn = insn.mnemonic
            # Collect targets of conditional and unconditional branches
            if mn.startswith("b") and mn not in ("bl", "blr", "bctr", "bctrl", "blrl"):
                if not covered(insn.addr):
                    continue
                ops = insn.operands.strip()
                # Target is usually the last operand
                parts = [p.strip() for p in ops.split(",")]
                for p in parts:
                    if p.startswith("0x"):
                        try:
                            all_targets.add(int(p, 16))
                        except ValueError:
                            pass

        new_targets: list[int] = []
        for target in sorted(all_targets):
            if not covered(target) and target not in self.functions:
                new_targets.append(target)

        # Create minimal functions for uncovered targets
        for target in new_targets:
            idx = self._addr_to_idx.get(target)
            if idx is None:
                continue

            func = Function(start=target)
            # Scan forward for blr or next known function
            found_end = False
            for j in range(idx, min(idx + 500, len(self.instructions))):
                curr = self.instructions[j]
                if _is_blr(curr):
                    func.end = curr.addr + 4
                    found_end = True
                    break
                # Stop at known function boundaries
                if curr.addr != target and curr.addr in self.functions:
                    func.end = curr.addr
                    found_end = True
                    break

            if found_end and func.size >= 4:
                self.functions[target] = func

    def _all_bl_targets(self) -> set[int]:
        targets: set[int] = set()
        for insn in self.instructions:
            if _is_bl(insn):
                t = _bl_target(insn)
                if t is not None:
                    targets.add(t)
        return targets

    def apply_seeds(self) -> int:
        """Register every seed address (.opd descriptors, ELF entry) as a
        function start. Seeds are ground truth, so they bypass min_size.
        Each seeded function ends at its first blr (a return), bounded by
        the next known start -- the same rule the other passes use. Code
        reachable past a first blr is picked up as branch-target entries or
        by the lifter's discovery pass, never by blind range extension
        (extending ranges sweeps inter-function padding/data in as garbage
        instructions)."""
        import bisect
        live_seeds = [a for a in self.seeds
                      if a in self._addr_to_idx and a not in self.functions]
        all_starts = sorted(set(self.functions.keys()) | set(live_seeds))
        added = 0
        for addr in live_seeds:
            k = bisect.bisect_right(all_starts, addr)
            nxt = all_starts[k] if k < len(all_starts) else None
            idx = self._addr_to_idx[addr]
            end = None
            for j in range(idx, min(idx + 2000, len(self.instructions))):
                cur = self.instructions[j]
                if nxt is not None and cur.addr >= nxt:
                    end = nxt
                    break
                if _is_blr(cur):
                    end = cur.addr + 4
                    break
            if end is None:
                end = nxt if nxt is not None else addr + 4
            self.functions[addr] = Function(start=addr, end=max(end, addr + 4))
            added += 1
        return added

    def drop_prologue_slivers(self, bl_targets: set[int]) -> int:
        """Remove heuristic-only starts that sit a few instructions after a
        seeded start with no intervening return -- those are mid-prologue
        anchors (e.g. the mflr after a stdu), not real functions. Starts
        that are themselves seeds or bl targets are kept."""
        if not self.seeds:
            return 0
        seed_sorted = sorted(self.seeds)
        import bisect
        dropped = 0
        for start in list(self.functions.keys()):
            if start in self.seeds or start in bl_targets:
                continue
            i = bisect.bisect_right(seed_sorted, start) - 1
            if i < 0:
                continue
            seed = seed_sorted[i]
            if seed == start or start - seed > 32:
                continue
            idx0 = self._addr_to_idx.get(seed)
            idx1 = self._addr_to_idx.get(start)
            if idx0 is None or idx1 is None:
                continue
            if any(self.instructions[k].mnemonic in ("blr", "b", "rfid")
                   for k in range(idx0, idx1)):
                continue
            del self.functions[start]
            dropped += 1
        return dropped

    def clip_overlaps(self) -> int:
        """Trim any function whose detected end runs past the next function's
        start, so every byte belongs to at most one function. Gaps between
        functions (alignment padding, embedded data) are left unattributed
        on purpose -- lifting them produces garbage code."""
        if not self.functions:
            return 0
        starts = sorted(self.functions.keys())
        clipped = 0
        for i, start in enumerate(starts):
            func = self.functions[start]
            nxt = starts[i + 1] if i + 1 < len(starts) else None
            if nxt is not None and func.end > nxt:
                func.end = nxt
                clipped += 1
            if func.end <= start:
                func.end = start + 4 if nxt is None else min(start + 4, nxt)
        return clipped

    def run(self) -> list[Function]:
        """Run all detection passes and return sorted function list."""
        t0 = time.time()
        self.find_prologue_functions()
        _note(f"prologue pass: {len(self.functions)} functions "
              f"({time.time() - t0:.1f}s)")

        t = time.time()
        self.find_leaf_functions()
        _note(f"leaf pass: {len(self.functions)} functions "
              f"({time.time() - t:.1f}s)")

        t = time.time()
        self.find_branch_target_functions()
        _note(f"branch-target pass: {len(self.functions)} functions "
              f"({time.time() - t:.1f}s)")

        if self.seeds:
            t = time.time()
            added = self.apply_seeds()
            bl_targets = self._all_bl_targets()
            dropped = self.drop_prologue_slivers(bl_targets)
            _note(f"seed pass: +{added} seeded starts, "
                  f"-{dropped} prologue slivers -> {len(self.functions)} "
                  f"({time.time() - t:.1f}s)")

        t = time.time()
        clipped = self.clip_overlaps()
        _note(f"overlap clipping: {clipped} ranges trimmed ({time.time() - t:.1f}s)")

        self.build_call_graph()
        _note(f"done: {len(self.functions)} functions total "
              f"({time.time() - t0:.1f}s)")
        return sorted(self.functions.values(), key=lambda f: f.start)

# ---------------------------------------------------------------------------
# OPD descriptor seeding (ELF inputs)
# ---------------------------------------------------------------------------

def collect_opd_seeds(elf, exec_ranges: list[tuple[int, int]],
                      big_endian: bool = True) -> set[int]:
    """Find function starts listed in the .opd descriptor table.

    PPC64 ELFv1 requires a {code, TOC} descriptor for every address-taken
    function; PS3 EBOOTs keep them in .opd within a data segment. Section
    names are unreliable in stripped binaries, so locate descriptors by
    shape instead: scan data segments for pairs where the first word is an
    aligned text address and the second is a data address, keep only TOC
    values that repeat (real .opd tables share a handful of TOC values),
    then collect every code address paired with one of those TOCs.
    """
    from elf_parser import PT_LOAD

    fmt = ">II" if big_endian else "<II"
    data_segs: list[tuple[int, bytes]] = []
    for i, ph in enumerate(elf.program_headers):
        if ph.p_type == PT_LOAD and ph.p_filesz > 0 and not (ph.p_flags & 1):
            data_segs.append((ph.p_vaddr, elf.get_segment_data(i)))

    data_ranges = [(v, v + len(d)) for v, d in data_segs]

    def in_exec(a: int) -> bool:
        return any(lo <= a < hi for lo, hi in exec_ranges)

    def in_data(a: int) -> bool:
        return any(lo <= a < hi for lo, hi in data_ranges)

    toc_freq: Counter = Counter()
    for vaddr, dat in data_segs:
        for off in range(0, len(dat) - 7, 4):
            code, toc = struct.unpack_from(fmt, dat, off)
            if code % 4 == 0 and in_exec(code) and in_data(toc):
                toc_freq[toc] += 1

    keep = {t for t, c in toc_freq.items() if c >= 16}
    seeds: set[int] = set()
    if keep:
        for vaddr, dat in data_segs:
            for off in range(0, len(dat) - 7, 4):
                code, toc = struct.unpack_from(fmt, dat, off)
                if toc in keep and code % 4 == 0 and in_exec(code):
                    seeds.add(code)

    # The ELF entry is a descriptor too; resolve and include its code addr.
    entry = elf.elf_header.e_entry
    for vaddr, dat in data_segs:
        if vaddr <= entry and entry + 4 <= vaddr + len(dat):
            code = struct.unpack_from(fmt[0] + "I", dat, entry - vaddr)[0]
            if in_exec(code) and code % 4 == 0:
                seeds.add(code)
            break
    if in_exec(entry):
        seeds.add(entry)

    return seeds


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="PPU function boundary detector for PS3 binaries"
    )
    parser.add_argument("input", help="Input ELF file or raw binary")
    parser.add_argument("--raw", action="store_true",
                        help="Treat input as raw binary")
    parser.add_argument("--base", type=lambda x: int(x, 0), default=0,
                        help="Base address (for raw binary)")
    parser.add_argument("--little-endian", action="store_true")
    parser.add_argument("--json", action="store_true",
                        help="Output as JSON")
    parser.add_argument("--call-graph", action="store_true",
                        help="Include call graph in output")
    parser.add_argument("--min-size", type=int, default=8,
                        help="Minimum function size in bytes (default: 8)")
    parser.add_argument("--no-opd", action="store_true",
                        help="Skip .opd descriptor seeding (ELF inputs)")
    parser.add_argument("--output", "-o", metavar="FILE",
                        help="Write function list to JSON file (for use with ppu_lifter)")
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        file_data = f.read()

    big_endian = not args.little_endian
    base_addr = args.base

    seeds: set[int] = set()
    exec_ranges: list[tuple[int, int]] = []

    if args.raw:
        all_insns = disassemble_bytes(file_data, base_addr, big_endian)
        exec_ranges = [(base_addr, base_addr + len(file_data))]
    else:
        try:
            from elf_parser import ELFFile, PT_LOAD
            elf = ELFFile(args.input)
            elf.load()
            big_endian = elf.elf_header.big_endian
            all_insns = []
            for ph in elf.program_headers:
                if ph.p_type == PT_LOAD and (ph.p_flags & 1):
                    seg_data = elf.get_segment_data(elf.program_headers.index(ph))
                    all_insns.extend(disassemble_bytes(seg_data, ph.p_vaddr, big_endian))
                    exec_ranges.append((ph.p_vaddr, ph.p_vaddr + ph.p_filesz))
            if not all_insns:
                for ph in elf.program_headers:
                    if ph.p_type == PT_LOAD and ph.p_filesz > 0:
                        seg_data = elf.get_segment_data(elf.program_headers.index(ph))
                        all_insns.extend(disassemble_bytes(seg_data, ph.p_vaddr, big_endian))
                        exec_ranges.append((ph.p_vaddr, ph.p_vaddr + ph.p_filesz))
                        break
            if not args.no_opd:
                t = time.time()
                seeds = collect_opd_seeds(elf, exec_ranges, big_endian)
                _note(f"opd scan: {len(seeds)} descriptor code addresses "
                      f"({time.time() - t:.1f}s)")
        except Exception as exc:
            print(f"Warning: ELF parse failed ({exc}), treating as raw", file=sys.stderr)
            all_insns = disassemble_bytes(file_data, base_addr, big_endian)
            exec_ranges = [(base_addr, base_addr + len(file_data))]

    if not all_insns:
        print("No instructions to analyse.", file=sys.stderr)
        sys.exit(1)

    _note(f"disassembled {len(all_insns)} instructions across "
          f"{len(exec_ranges)} executable segment(s)")

    finder = FunctionFinder(all_insns, min_size=args.min_size, seeds=seeds,
                            exec_ranges=exec_ranges)
    functions = finder.run()

    if seeds:
        starts = {f.start for f in functions}
        unseated = [a for a in seeds if a not in starts]
        if unseated:
            _note(f"WARNING: {len(unseated)} seed addresses are not function "
                  f"starts (first: 0x{min(unseated):08X})")
        else:
            _note("verified: every .opd descriptor address is a function start")

    if args.output:
        # Write function list for ppu_lifter
        out_list = [{"start": f"0x{f.start:08X}", "end": f"0x{f.end:08X}"} for f in functions]
        with open(args.output, "w") as f:
            json.dump(out_list, f, indent=2)
        print(f"Wrote {len(functions)} functions to {args.output}")
        return

    if args.json:
        out: dict = {
            "total_functions": len(functions),
            "functions": [f.to_dict() for f in functions],
        }

        if args.call_graph:
            edges: list[dict] = []
            for func in functions:
                for call_target in func.calls:
                    edges.append({
                        "from": f"0x{func.start:08X}",
                        "to": f"0x{call_target:08X}",
                    })
            out["call_graph"] = edges

        print(json.dumps(out, indent=2))

    else:
        print(f"Found {len(functions)} functions:\n")
        print(f"{'Start':>12s}  {'End':>12s}  {'Size':>8s}  {'Stack':>6s}  "
              f"{'Pro':>3s}  {'Epi':>3s}  {'Leaf':>4s}  {'Calls':>5s}  {'Callers':>7s}")
        print("-" * 80)

        for func in functions:
            pro = "Y" if func.has_prologue else "-"
            epi = "Y" if func.has_epilogue else "-"
            leaf = "Y" if func.is_leaf else "-"
            print(f"0x{func.start:08X}  0x{func.end:08X}  {func.size:8d}  "
                  f"{func.stack_size:6d}  {pro:>3s}  {epi:>3s}  {leaf:>4s}  "
                  f"{len(func.calls):5d}  {len(func.callers):7d}")

        if args.call_graph:
            print(f"\nCall graph ({sum(len(f.calls) for f in functions)} edges):")
            for func in functions:
                if func.calls:
                    targets = ", ".join(f"0x{t:08X}" for t in func.calls[:10])
                    more = f" ... (+{len(func.calls) - 10})" if len(func.calls) > 10 else ""
                    print(f"  0x{func.start:08X} -> {targets}{more}")


if __name__ == "__main__":
    main()
