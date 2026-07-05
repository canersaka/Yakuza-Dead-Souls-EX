#!/usr/bin/env python3
"""
CPU differential trace-diff: find the FIRST instruction where our lifted guest
execution (PPU or SPU) diverges from RPCS3's (the register/memory oracle).

This is the general bug-finder the project's "Verification reality" calls for:
a wrong lift compiles and silently computes the wrong value, only surfacing when
a downstream consumer crashes (the VMX scramble, the gs_task null-pointer wall,
the `5882fe4` dispatch bug -- all the same class). Running the SAME code on both
engines and diffing the per-instruction register trace pinpoints the exact
instruction that first goes wrong -- no crash required -- and surfaces every such
bug up to that point in one pass, instead of one boot-wall at a time.

Engine-agnostic: it diffs any trace in the format below, so the SAME tool covers
SPU images (128-bit regs, two hex words) and PPU threads (64-bit GPRs, one word;
FPR/CR/LR/... by name). Drivers: tools/diverge.ps1 (SPU) and tools/ppu_diverge.ps1 (PPU).

TRACE FORMAT (both engines emit this)
    <PC hex>                         one line per retired instruction (PC about to run)
      <reg> <word...>                optional: a register written, post-state.
                                     <reg> is a name (r5, f3, cr, lr, v10, ...);
                                     <word...> is 1..4 big-endian hex words (SPU=2, GPR=1).
    # ...                            comment / header lines are ignored

USAGE
    py -3 tools/tracediff.py ours.txt rpcs3.txt
    py -3 tools/tracediff.py ours.txt rpcs3.txt --align-pc 3050 --context 12
    py -3 tools/tracediff.py --self-test

Exit code: 0 if the traces agree over the window, 1 on the first divergence (so
it slots into CI), 2 on a usage/parse error.
"""

import argparse
import re
import sys

# --------------------------------------------------------------------------- #
# Parsing
# --------------------------------------------------------------------------- #

# A PC line: hex PC at column 0, optionally followed by whitespace + a marker/comment
# (our emitter appends " |P"/"|K" for the policy/kernel LS boundary; RPCS3 emits bare).
_PC_RE = re.compile(r"^([0-9A-Fa-f]{1,16})(?:\s.*)?$")
# A register write: an indented "<name> <hexword> [<hexword> ...]".
_RT_RE = re.compile(r"^\s+([A-Za-z][A-Za-z0-9]*)\s+((?:[0-9A-Fa-f]+\s*)+)$")


class Event:
    """One retired instruction: a PC plus 0..N register writes (name -> value words)."""

    __slots__ = ("pc", "writes", "shadow")

    def __init__(self, pc):
        self.pc = pc
        self.writes = []  # list[(name:str, values:tuple[int,...])]
        self.shadow = ()  # writes made by out-of-window events since the previous
                          # kept event (see apply_pc_range) -- the OTHER side's
                          # reconstructed state for these regs is stale, not wrong

    def __repr__(self):
        w = " ".join(f"{n}={':'.join(f'{v:X}' for v in vs)}" for n, vs in self.writes)
        return f"{self.pc:X}" + (f"  {w}" if w else "")


def parse_trace(path):
    """Parse a trace file into a list[Event]. Comment/blank lines are skipped."""
    events = []
    cur = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\r\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            m = _RT_RE.match(line)
            if m:
                if cur is None:
                    cur = Event(-1)
                    events.append(cur)
                vals = tuple(int(w, 16) for w in m.group(2).split())
                cur.writes.append((m.group(1), vals))
                continue
            m = _PC_RE.match(line)
            if m:
                cur = Event(int(m.group(1), 16))
                events.append(cur)
                continue
            # Unrecognized line: ignore but keep going (traces may carry stray logs).
    return events


def apply_pc_range(events, lo, hi):
    """Keep only events with lo<=pc<hi, but carry each dropped event's register
    writes as `shadow` on the next kept event. STALE-REF GUARD: both engines run
    the same instruction stream, so when OUR trace shows out-of-window code
    writing a register, the (window-bounded) reference engine also wrote it but
    could not record it -- its reconstructed value is STALE from that point.
    The differ uses `shadow` to invalidate the other side's state instead of
    flagging a false 'compute' divergence (the 0x51C4 gs_task artifact class)."""
    kept, pending = [], []
    for e in events:
        if lo <= e.pc < hi:
            e.shadow = tuple(pending)
            pending = []
            kept.append(e)
        else:
            pending.extend(e.writes)
    return kept


# --------------------------------------------------------------------------- #
# Diff
# --------------------------------------------------------------------------- #

def _apply_shadow(e, own_state, other_state):
    """Fold an event's out-of-window shadow writes into its own side's state and
    INVALIDATE the other side's (it executed the same code unrecorded)."""
    for n, v in e.shadow:
        own_state[n] = v
        other_state.pop(n, None)


class Divergence:
    def __init__(self, kind, index, ours, ref, detail):
        self.kind = kind        # "control-flow" | "compute" | "align"
        self.index = index
        self.ours = ours        # Event or None
        self.ref = ref          # Event or None
        self.detail = detail    # human string


def _resync(a, b, i, j, window):
    """After a PC mismatch, look for a run of >=3 matching PCs within `window` on
    either side (handles one engine emitting an extra hint/nop event). Returns the
    (di, dj) skip that realigns, or None."""
    def run_len(ii, jj):
        n = 0
        while ii + n < len(a) and jj + n < len(b) and a[ii + n].pc == b[jj + n].pc:
            n += 1
        return n

    best = None
    for di in range(0, window + 1):
        for dj in range(0, window + 1):
            if di == 0 and dj == 0:
                continue
            ii, jj = i + di, j + dj
            if ii >= len(a) or jj >= len(b):
                continue
            if a[ii].pc == b[jj].pc and run_len(ii, jj) >= 3:
                cost = di + dj
                if best is None or cost < best[0]:
                    best = (cost, di, dj)
    return None if best is None else (best[1], best[2])


def diff(ours, ref, align_pc=None, context=8, limit=None, resync_window=8, ignore_regs=False):
    """Compare two event streams. Returns (Divergence|None, skips:list[str]).

    ignore_regs: compare only the PC sequence (control flow), not register writes.
    Use when the two emitters annotate register writes differently (e.g. our
    --trace tags the dest as insn&0x7F, wrong for RRR-format ops, while RPCS3
    emits the actually-changed reg) -- then a control-flow (loop/branch) bug still
    shows cleanly as a PC divergence."""
    i = j = 0
    if align_pc is not None:
        while i < len(ours) and ours[i].pc != align_pc:
            i += 1
        while j < len(ref) and ref[j].pc != align_pc:
            j += 1
        if i == len(ours) or j == len(ref):
            where = "ours" if i == len(ours) else "ref"
            return Divergence("align", 0, None, None,
                              f"--align-pc 0x{align_pc:X} not found in {where}"), []

    skips = []
    compared = 0
    state_a, state_b = {}, {}   # running register files (name -> value tuple)
    while i < len(ours) and j < len(ref):
        if limit is not None and compared >= limit:
            break
        ea, eb = ours[i], ref[j]
        if ea.pc != eb.pc:
            rs = _resync(ours, ref, i, j, resync_window)
            if rs is None:
                last = ours[i - 1].pc if i else -1
                return Divergence("control-flow", compared, ea, eb,
                                  f"PC diverged: ours=0x{ea.pc:X} ref=0x{eb.pc:X}. "
                                  f"The branch/condition that chose this path is at or "
                                  f"just before the last common PC 0x{last:X}."), skips
            di, dj = rs
            if di:
                skips.append(f"ours emitted {di} extra event(s) near 0x{ea.pc:X}")
                for k in range(di):
                    _apply_shadow(ours[i + k], state_a, state_b)
                    for n, vals in ours[i + k].writes:
                        state_a[n] = vals
            if dj:
                skips.append(f"ref emitted {dj} extra event(s) near 0x{eb.pc:X}")
                for k in range(dj):
                    _apply_shadow(ref[j + k], state_b, state_a)
                    for n, vals in ref[j + k].writes:
                        state_b[n] = vals
            i += di
            j += dj
            continue
        # Apply this instruction's writes to each running register file, then
        # compare only the registers touched here that BOTH sides have a known
        # value for. Robust to emit conventions: our --trace logs every rt-write,
        # RPCS3 only changed regs -- an unchanged write converges to the same
        # state, so it is not a false divergence.
        _apply_shadow(ea, state_a, state_b)
        _apply_shadow(eb, state_b, state_a)
        for n, vals in ea.writes:
            state_a[n] = vals
        for n, vals in eb.writes:
            state_b[n] = vals
        if not ignore_regs:
            touched = {n for n, _ in ea.writes} | {n for n, _ in eb.writes}
            bad = [n for n in touched
                   if n in state_a and n in state_b and state_a[n] != state_b[n]]
            if bad:
                return Divergence("compute", compared, ea, eb,
                                  _describe_state_diff(ea.pc, sorted(bad), state_a, state_b)), skips
        i += 1
        j += 1
        compared += 1
    return None, skips


def _fmt(vs):
    return ":".join(f"{v:X}" for v in vs) if vs is not None else "(no write)"


def _describe_write_diff(ea, eb):
    da = {n: vs for n, vs in ea.writes}
    db = {n: vs for n, vs in eb.writes}
    parts = [f"compute diverged at PC 0x{ea.pc:X}:"]
    for n in sorted(set(da) | set(db)):
        if da.get(n) != db.get(n):
            parts.append(f"  {n}: ours={_fmt(da.get(n))}  ref={_fmt(db.get(n))}")
    return "\n".join(parts)


def _describe_state_diff(pc, regs, state_a, state_b):
    parts = [f"compute diverged at PC 0x{pc:X} (reconstructed register state):"]
    for n in regs:
        parts.append(f"  {n}: ours={_fmt(state_a.get(n))}  ref={_fmt(state_b.get(n))}")
    return "\n".join(parts)


# --------------------------------------------------------------------------- #
# Reporting
# --------------------------------------------------------------------------- #

def _window(events, center, context):
    return max(0, center - context), min(len(events), center + context + 1)


def report(div, ours, ref, context):
    if div is None:
        print("No divergence over the compared window. Traces AGREE.")
        return 0
    if div.kind == "align":
        print(f"ALIGN ERROR: {div.detail}")
        return 2
    print("=" * 72)
    print(f"FIRST DIVERGENCE ({div.kind}) at compared index {div.index}")
    print("=" * 72)
    print(div.detail)
    print()
    oi = ours.index(div.ours) if div.ours in ours else None
    ri = ref.index(div.ref) if div.ref in ref else None
    if oi is not None:
        lo, hi = _window(ours, oi, context)
        print(f"--- OURS  [{lo}..{hi}) ---")
        for k in range(lo, hi):
            print(f"{'>>' if k == oi else '  '} {ours[k]!r}")
    print()
    if ri is not None:
        lo, hi = _window(ref, ri, context)
        print(f"--- RPCS3 [{lo}..{hi}) ---")
        for k in range(lo, hi):
            print(f"{'>>' if k == ri else '  '} {ref[k]!r}")
    print()
    if div.kind == "compute":
        print(f"=> Inspect the lift of PC 0x{div.ours.pc:X} (the instruction whose "
              f"result differs, or its input).")
    else:
        print("=> A branch was taken differently; the mis-computed condition is "
              "usually a few instructions earlier in the OURS window above.")
    return 1


# --------------------------------------------------------------------------- #
# Wide scan: find ALL divergences in one pass (not just the first)
# --------------------------------------------------------------------------- #

def scan(ours, ref, align_pc=None, resync_window=8, ignore_regs=False, max_findings=2000):
    """Find EVERY divergence in one pass, deduped by PC. After each finding it
    resyncs so later findings are INDEPENDENT (not cascades of the first):
      - compute  -> adopt the oracle's value into our reconstructed state, so the
                    next compare is measured against known-correct upstream state;
      - control-flow -> realign the two PC streams (lookahead) and continue.
    Returns a list[Divergence] (ordered by first occurrence)."""
    i = j = 0
    if align_pc is not None:
        while i < len(ours) and ours[i].pc != align_pc: i += 1
        while j < len(ref) and ref[j].pc != align_pc: j += 1
        if i == len(ours) or j == len(ref):
            return [Divergence("align", 0, None, None, f"--align-pc 0x{align_pc:X} not found")]
    findings, seen = [], set()
    state_a, state_b = {}, {}
    while i < len(ours) and j < len(ref) and len(findings) < max_findings:
        ea, eb = ours[i], ref[j]
        if ea.pc != eb.pc:
            if ea.pc not in seen:
                seen.add(ea.pc)
                last = ours[i - 1].pc if i else -1
                findings.append(Divergence("control-flow", 0, ea, eb,
                    f"ours=0x{ea.pc:X} ref=0x{eb.pc:X} (diverged after common 0x{last:X})"))
            rs = _resync(ours, ref, i, j, resync_window) or _resync(ours, ref, i, j, resync_window * 16)
            if rs is None:
                break
            di, dj = rs
            for k in range(di):
                _apply_shadow(ours[i + k], state_a, state_b)
                for n, v in ours[i + k].writes: state_a[n] = v
            for k in range(dj):
                _apply_shadow(ref[j + k], state_b, state_a)
                for n, v in ref[j + k].writes: state_b[n] = v
            i += di; j += dj
            continue
        _apply_shadow(ea, state_a, state_b)
        _apply_shadow(eb, state_b, state_a)
        for n, v in ea.writes: state_a[n] = v
        for n, v in eb.writes: state_b[n] = v
        if not ignore_regs:
            touched = {n for n, _ in ea.writes} | {n for n, _ in eb.writes}
            bad = [n for n in touched if n in state_a and n in state_b and state_a[n] != state_b[n]]
            if bad:
                if ea.pc not in seen:
                    seen.add(ea.pc)
                    findings.append(Divergence("compute", 0, ea, eb,
                        _describe_state_diff(ea.pc, sorted(bad), state_a, state_b)))
                for n in bad:
                    state_a[n] = state_b[n]   # oracle-resync: adopt truth, keep scanning
        i += 1; j += 1
    return findings


def report_scan(findings):
    real = [f for f in findings if f.kind != "align"]
    if findings and findings[0].kind == "align":
        print(f"ALIGN ERROR: {findings[0].detail}"); return 2
    if not real:
        print("No divergences found over the compared window. Traces AGREE.")
        return 0
    print(f"{len(real)} divergent PC(s), earliest first "
          f"(compute values are ours-vs-ref at that instruction):\n")
    for n, f in enumerate(real, 1):
        pc = f.ours.pc if f.ours else 0
        head = (f.detail.splitlines()[0] if f.detail else "")
        print(f"{n:3}. [{f.kind:12}] PC 0x{pc:05X}  {head.replace('compute diverged at PC 0x%X (reconstructed register state):' % pc, '').strip()}")
        for line in (f.detail.splitlines()[1:] if f.detail else []):
            if line.strip():
                print(f"          {line.strip()}")
    print(f"\nFix these in the lifter (earliest first -- later ones may resolve once "
          f"the first is fixed), then re-run.")
    return 1


# --------------------------------------------------------------------------- #
# Self-test (validates the aligner with no build, for both SPU and PPU shapes)
# --------------------------------------------------------------------------- #

def _mk(lines):
    ev, cur = [], None
    for line in lines:
        m = _RT_RE.match(line)
        if m:
            cur.writes.append((m.group(1), tuple(int(w, 16) for w in m.group(2).split())))
            continue
        m = _PC_RE.match(line)
        if m:
            cur = Event(int(m.group(1), 16))
            ev.append(cur)
    return ev


def self_test():
    ok = True

    def check(name, cond):
        nonlocal ok
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
        ok = ok and cond

    # SPU-shaped trace (128-bit regs = two hex words).
    spu = ["03050", "  r3  0000000000000000 000000000000C2D0",
           "03054", "0AB60", "  r6  0000000000000000 000000000000C350"]
    check("SPU identical -> agree", diff(_mk(spu), _mk(spu))[0] is None)
    bad = list(spu); bad[4] = "  r6  0000000000000000 0000000000000000"
    d, _ = diff(_mk(bad), _mk(spu))
    check("SPU value mismatch -> compute divergence @0xAB60",
          d and d.kind == "compute" and d.ours.pc == 0x0AB60)

    # PPU-shaped trace (64-bit GPRs = one word; named regs).
    ppu = ["100004A0", "  r3 00000000000003E8",
           "100004A4", "  r4 FFFFFFFFD0100950",
           "100004A8", "  lr 00000000100004AC"]
    check("PPU identical -> agree", diff(_mk(ppu), _mk(ppu))[0] is None)
    pbad = list(ppu); pbad[3] = "  r4 00000000D0100950"   # sign-extension bug shape
    d, _ = diff(_mk(pbad), _mk(ppu))
    check("PPU value mismatch -> compute divergence @0x100004A4",
          d and d.kind == "compute" and d.ours.pc == 0x100004A4 and d.ours.writes[0][0] == "r4")

    # Control-flow split.
    d, _ = diff(_mk(["03050", "07C10"]), _mk(["03050", "0AB60"]))
    check("PC split -> control-flow divergence", d and d.kind == "control-flow")

    # Resync past one extra event; and --align-pc.
    d, sk = diff(_mk(["03050", "0AAAA", "03058", "0305C", "03060"]),
                 _mk(["03050", "03058", "0305C", "03060"]))
    check("single extra event -> resync, no false divergence", d is None and bool(sk))
    d, _ = diff(_mk(["09000", "03050", "03054"]), _mk(["01111", "03050", "03054"]),
                align_pc=0x03050)
    check("--align-pc skips mismatched prologue", d is None)

    # 7. Real-emitter quirk: PC lines carry a " |P"/"|K" LS-boundary marker.
    mk = _mk(["03050 |P", "  r6  0003FFD0 0003FFD0", "03054 |K"])
    check("PC lines with ' |P' marker parse to bare PCs",
          len(mk) == 2 and mk[0].pc == 0x3050 and mk[1].pc == 0x3054)

    # 8. Emit convention: ours re-logs an UNCHANGED reg that ref omits -> NOT a divergence.
    d, _ = diff(_mk(["03050", "  r5 C2D0", "03054", "  r5 C2D0"]),
                _mk(["03050", "  r5 C2D0", "03054"]))
    check("unchanged rt-write on one side only -> no false divergence", d is None)

    # 9. ours CHANGES a reg that ref leaves unchanged -> real divergence, caught via state.
    d, _ = diff(_mk(["03050", "  r5 C2D0", "03054", "  r5 0"]),
                _mk(["03050", "  r5 C2D0", "03054"]))
    check("ours changes a reg ref leaves unchanged -> compute divergence",
          d is not None and d.kind == "compute")

    # 10. STALE-REF GUARD: ours runs out-of-window code that rewrites r5, then an
    #     in-window instr re-logs it; the windowed ref never saw the rewrite. The
    #     ref-side state is STALE, not divergent -> guard must swallow it.
    go = apply_pc_range(_mk(["03050", "  r5 4", "0B40C", "  r5 0",
                             "03054", "  r5 0"]), 0x3000, 0x8000)
    gr = apply_pc_range(_mk(["03050", "  r5 4",
                             "03054"]), 0x3000, 0x8000)
    d, _ = diff(go, gr)
    check("stale-ref guard: out-of-window rewrite -> no false compute divergence",
          d is None)
    #     ...but a REAL divergence after the window gap is still caught once the
    #     ref side writes the register again in-window.
    go2 = apply_pc_range(_mk(["03050", "  r5 4", "0B40C", "  r5 0",
                              "03054", "  r5 7"]), 0x3000, 0x8000)
    gr2 = apply_pc_range(_mk(["03050", "  r5 4",
                              "03054", "  r5 9"]), 0x3000, 0x8000)
    d, _ = diff(go2, gr2)
    check("stale-ref guard: real post-gap divergence still caught",
          d is not None and d.kind == "compute" and d.ours.pc == 0x3054)

    # 11. WIDE SCAN finds multiple INDEPENDENT divergences (oracle-resync between them).
    so = ["03050", "  r5 1", "03054", "  r6 2", "03058", "  r7 3"]
    sr = ["03050", "  r5 9", "03054", "  r6 2", "03058", "  r7 8"]   # r5,r7 diverge; r6 agrees
    fs = [f for f in scan(_mk(so), _mk(sr)) if f.kind == "compute"]
    check("wide scan finds both independent divergences (0x3050, 0x3058)",
          len(fs) == 2 and fs[0].ours.pc == 0x3050 and fs[1].ours.pc == 0x3058)

    print("SELF-TEST:", "ALL PASS" if ok else "FAILURES")
    return 0 if ok else 1


# --------------------------------------------------------------------------- #

def main(argv=None):
    p = argparse.ArgumentParser(description="Diff two guest instruction traces (PPU or SPU) to the first divergence.")
    p.add_argument("ours", nargs="?", help="our lifted trace")
    p.add_argument("ref", nargs="?", help="RPCS3 reference trace")
    p.add_argument("--align-pc", type=lambda s: int(s, 16), default=None,
                   help="skip both traces to the first occurrence of this PC (hex) before diffing")
    p.add_argument("--context", type=int, default=8, help="context lines around the divergence")
    p.add_argument("--limit", type=int, default=None, help="stop after N compared instructions")
    p.add_argument("--resync-window", type=int, default=8,
                   help="lookahead used to re-align after a single-side extra event")
    p.add_argument("--pc-range", default=None,
                   help="only compare events with LO<=pc<HI (hex 'LO:HI', e.g. 3000:8000) -- match a "
                        "bounded reference tracer so calls OUT of the window aren't seen as divergences")
    p.add_argument("--pc-only", action="store_true",
                   help="compare only the PC sequence (control flow), ignoring register writes -- "
                        "use when the two emitters annotate register dests differently")
    p.add_argument("--scan", action="store_true",
                   help="WIDE SCAN: report EVERY divergence in one pass (deduped by PC), "
                        "oracle-resyncing after each so findings are independent, not cascades")
    p.add_argument("--max-findings", type=int, default=2000, help="cap for --scan")
    p.add_argument("--self-test", action="store_true", help="run the built-in aligner tests and exit")
    args = p.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.ours or not args.ref:
        p.error("need two trace files (or --self-test)")

    try:
        ours = parse_trace(args.ours)
        ref = parse_trace(args.ref)
    except OSError as e:
        print(f"parse error: {e}", file=sys.stderr)
        return 2
    if args.pc_range:
        lo, hi = (int(x, 16) for x in args.pc_range.split(":"))
        ours = apply_pc_range(ours, lo, hi)
        ref = apply_pc_range(ref, lo, hi)
        print(f"[pc-range] kept events in [0x{lo:X},0x{hi:X}) "
              f"(out-of-window writes invalidate the other side's state, not compared)")
    print(f"ours: {len(ours)} events ({args.ours})")
    print(f"ref : {len(ref)} events ({args.ref})")
    if args.scan:
        findings = scan(ours, ref, align_pc=args.align_pc, resync_window=args.resync_window,
                        ignore_regs=args.pc_only, max_findings=args.max_findings)
        return report_scan(findings)
    div, skips = diff(ours, ref, align_pc=args.align_pc, context=args.context,
                      limit=args.limit, resync_window=args.resync_window, ignore_regs=args.pc_only)
    for s in skips:
        print(f"[resync] {s}")
    return report(div, ours, ref, args.context)


if __name__ == "__main__":
    sys.exit(main())
