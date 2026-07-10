#!/usr/bin/env python3
"""bootdiff.py - the standing boot-choreography differential.

Turns "find where our boot first diverges from RPCS3" into one command.

CONCEPT
-------
Both runtimes emit a stream of boot-choreography events (file opens, audio
port starts, workload signal raises, jobchain round starts, gcm-label
publishes, user-command dispatches, flips, main-loop iterations). This tool
normalizes each side's raw log(s) into ordered per-type event sequences and
reports the first point where our sequence stops progressing relative to
the reference (RPCS3) sequence - the same comparison that has, by hand,
found every wall since session 13.

INPUTS
------
Our side is captured as TWO separate files (see e.g. scratch/boot_s25ride.ps1):
a stdout capture (``--ours-out``) and a stderr capture (``--ours-err``),
redirected independently by PowerShell's Start-Process. There is NO shared
clock between them (confirmed: neither stream carries a timestamp that
correlates with the other), so this tool does not claim byte-exact
interleaving across the stdout/stderr boundary - it reports per-stream
ordinal sequences and flags cross-stream ordering claims as unavailable.
This is deliberate: session17 lost time to exactly this trap (an
unbuffered-vs-buffered emitter mismatch produced a false ordering).

The reference side is one or more already-captured RPCS3-side logs, given
as ``--ref PATH:KIND`` (repeatable). KIND is one of:
  - ``rpcs3log``  - a normal RPCS3.log (elapsed-time-prefixed lines)
  - ``probelog``  - one of this project's yz_*_oracle.log probe captures
                    (raw ``t=<uint>`` guest-clock, no elapsed-time prefix)
Each ``--ref`` file is treated as its OWN run/clock domain - this tool does
NOT merge timestamps across different --ref files (they are frequently
different boots on different days; see scratch/rpcs3_stopper_oracle.md).
Cross-type ordering claims ("JOB_ROUND 3 precedes FILE_OPEN X") are only
meaningful *within* a single --ref file's own timeline.

INSTRUMENT-CAP DISCIPLINE (LESSONS #21 / evidence-discipline rule)
--------------------------------------------------------------------
Several of these probes are deliberately rate-limited (they'd flood the log
otherwise) and the raw per-line occurrence count is NOT the true total. The
known case handled specially here: our own ``[chain]`` probe
(yakuza/main.cpp's yz_chain_probe) prints an individual ``hit#N`` line only
for N<=4 per address, then relies on a separate, uncapped
``[chain] census(...)`` line (printed every 5s) that carries the TRUE
running count for every watched address. This tool parses the census lines
and uses the last-seen watermark as the JOB_ROUND / JOB_ENTRY_WRITE /
MAINLOOP_ITER count instead of counting ``hit#`` lines (which would silently
read as "4" forever). The reference-side ``[yzround]`` probe carries its own
documented cap too (``cap=first200+every100th+60s-after-first-ROUNDDRIVER``,
scratch/rpcs3_stopper_oracle.md) - this tool prints that ARMED banner
verbatim rather than asserting a corrected ceiling for it, since (unlike our
own probe) there is no separate uncapped watermark line for it on disk.

EVENT VOCABULARY
-----------------
FILE_OPEN, AUDIO_PORT_START, WKL_RAISE, JOB_ROUND, JOB_ENTRY_WRITE, SPUP17,
LABEL_PUBLISH, UCMD, FLIP, MAINLOOP_ITER. See OUR_PATTERNS /
REF_RPCS3LOG_PATTERNS / REF_PROBELOG_PATTERNS below - each entry is one
line to add a new event type or a new log-line shape for an existing type.

USAGE
-----
  py -3 tools\\bootdiff.py --ours-err scratch\\s25ride.err --ours-out scratch\\s25ride.out ^
      --ref "C:\\Users\\csaka\\Downloads\\rpcs3clone\\rpcs3\\bin\\log\\RPCS3.log:rpcs3log" ^
      --ref "C:\\Users\\csaka\\Downloads\\rpcs3clone\\rpcs3\\bin\\yz_rounddriver_oracle.log:probelog" ^
      --context 20

See CHEATSHEET.md's "boot-choreography differential" section for the full
runbook (how to capture both sides, how to run, how to read the output).
"""

import argparse
import os
import re
import sys
from collections import defaultdict

# ===========================================================================
# Event record
# ===========================================================================


class Event(object):
    __slots__ = ("etype", "ordinal", "args", "raw", "source", "lineno", "clock")

    def __init__(self, etype, ordinal, args, raw, source, lineno, clock=None):
        self.etype = etype
        self.ordinal = ordinal      # 1-based occurrence index of this etype in this source
        self.args = args            # tuple of strings, event-specific
        self.raw = raw              # original line (trimmed)
        self.source = source        # file path this event came from
        self.lineno = lineno        # line number within `source`
        self.clock = clock          # float seconds (rpcs3log) or int t= (probelog), or None

    def __repr__(self):
        return "Event(%s#%d args=%r @%s:%d)" % (
            self.etype, self.ordinal, self.args, os.path.basename(self.source), self.lineno)


# ===========================================================================
# OUR-SIDE PATTERNS
#
# One entry per (event_type, line shape). `stream` picks which of the two
# capture files (--ours-err / --ours-out) this pattern applies to. Add a new
# event type by adding one dict here - nothing else needs to change.
#
# NOTE: JOB_ROUND / JOB_ENTRY_WRITE / MAINLOOP_ITER are NOT counted from
# these hit#-line patterns in the final report (see CHAIN_CENSUS handling
# below) - the hit# lines are kept here only so the tail-context printer has
# individual raw samples to show; the authoritative count comes from the
# uncapped [chain] census line.
# ===========================================================================

OUR_PATTERNS = [
    dict(etype="FILE_OPEN", stream="out",
         regex=re.compile(r"\[cellFs\] t(?P<tid>\d+) Open\(path='(?P<path>[^']+)'"),
         args=lambda m: (os.path.basename(m.group("path")),)),
    dict(etype="AUDIO_PORT_START", stream="out",
         regex=re.compile(r"\[cellAudio\] PortStart\(port=(?P<port>\d+)\)"),
         args=lambda m: (m.group("port"),)),
    dict(etype="WKL_RAISE", stream="err",
         regex=re.compile(
             r"\[widsig\] t(?P<tid>\d+) raise=(?P<raise>[0-9A-Fa-f]+) "
             r"old=(?P<old>[0-9A-Fa-f]+) new=(?P<new>[0-9A-Fa-f]+)"),
         args=lambda m: (m.group("raise"),)),
    # JOB_ROUND: ROUNDDRIVER (func_00A9F8AC) is the per-round header writer -
    # the SAME address RPCS3-side yz_rounddriver_oracle.log's [yzround]
    # tag=ROUNDDRIVER probe watches (scratch/rpcs3_stopper_oracle.md), so
    # counts are directly comparable across the two engines.
    dict(etype="JOB_ROUND", stream="err",
         regex=re.compile(r"\[chain\] 0x00A9F8AC hit#(?P<n>\d+)"),
         args=lambda m: ()),
    # JOBWR (func_00E5F094): the per-job-entry writer inside a round, also
    # PC-matched to the RPCS3-side [yzround] tag=JOBWR probe.
    dict(etype="JOB_ENTRY_WRITE", stream="err",
         regex=re.compile(r"\[chain\] 0x00E5F094 hit#(?P<n>\d+)"),
         args=lambda m: ()),
    dict(etype="MAINLOOP_ITER", stream="err",
         regex=re.compile(r"\[chain\] 0x00D1E838 hit#(?P<n>\d+)"),
         args=lambda m: ()),
    dict(etype="LABEL_PUBLISH", stream="err",
         regex=re.compile(
             r"\[vbl\] tick=(?P<tick>\d+) pending=\[(?P<p0>-?\d+) (?P<p1>-?\d+)\] "
             r"label@0x10200010=0x(?P<val>[0-9A-Fa-f]+)"),
         args=lambda m: (m.group("val"),)),
    dict(etype="UCMD", stream="err",
         regex=re.compile(
             r"\[ucmd\] n=(?P<n>\d+) cause=0x(?P<cause>[0-9A-Fa-f]+) "
             r"handlers=0x(?P<handlers>[0-9A-Fa-f]+) send=(?P<send>-?\d+)"),
         args=lambda m: (m.group("cause"), m.group("send"))),
    dict(etype="FLIP", stream="err",
         regex=re.compile(
             r"\[live-draw\] frame (?P<n>\d+) presented: draws=(?P<draws>\d+) clears=(?P<clears>\d+)"),
         args=lambda m: (m.group("draws"), m.group("clears"))),
]

# ARMED / armed banners that establish liveness for a given our-side tag.
OUR_ARM_BANNERS = {
    "[chain]": re.compile(r"\[chainprobe\] ARMED"),
    "[widsig]": re.compile(r"\[widsig\] armed", re.IGNORECASE),
    "[ucmd]": re.compile(r"\[ucmd\] armed", re.IGNORECASE),
}

# The [chain] census line: uncapped per-address running totals, printed
# every 5s from the probe plus periodically from the watchdog. This is the
# ONLY reliable source for JOB_ROUND / JOB_ENTRY_WRITE / MAINLOOP_ITER
# counts - the hit#-line pattern above caps at 4 per address (yakuza/main.cpp
# yz_chain_probe: `if (n <= 4) fprintf(...)`).
CENSUS_LINE_RE = re.compile(r"\[chain\] census\((?P<tag>[^)]+)\) t=(?P<t>\d+)ms:(?P<body>.*)")
CENSUS_ADDR_RE = re.compile(r"(0x[0-9A-Fa-f]{8})=(\d+)")
CHAIN_ADDR_TO_ETYPE = {
    "0x00A9F8AC": "JOB_ROUND",
    "0x00E5F094": "JOB_ENTRY_WRITE",
    "0x00D1E838": "MAINLOOP_ITER",
}


# ===========================================================================
# REFERENCE-SIDE PATTERNS
# ===========================================================================

# RPCS3.log-style lines: "<marker> H:MM:SS.ffffff {thread} module: message"
RPCS3LOG_TIME_RE = re.compile(r"^\W*(?P<h>\d+):(?P<m>\d+):(?P<s>\d+(?:\.\d+)?)")


def rpcs3log_elapsed_seconds(line):
    m = RPCS3LOG_TIME_RE.match(line)
    if not m:
        return None
    return int(m.group("h")) * 3600 + int(m.group("m")) * 60 + float(m.group("s"))


REF_RPCS3LOG_PATTERNS = [
    dict(etype="FILE_OPEN",
         regex=re.compile(r"sys_fs_open\(\):.*?,\s*'(?P<path>/[^']+)'"),
         args=lambda m: (os.path.basename(m.group("path")),)),
    dict(etype="AUDIO_PORT_START",
         regex=re.compile(r"cellAudio:\s*cellAudioPortStart\(portNum=(?P<port>\d+)\)"),
         args=lambda m: (m.group("port"),)),
    # RPCS3-side workload-raise probe on disk this pass is scoped to wid4
    # ONLY (w4raise/w4follow, s24-late build) - not a general wid0-15 watch.
    # See scratch/s25_bootdiff.md's "missing reference-side events" list.
    dict(etype="WKL_RAISE",
         regex=re.compile(
             r"\[w4raise\] wid4 SIGNAL-RAISE wklSignal1 old=0x(?P<old>[0-9A-Fa-f]+) "
             r"new=0x(?P<new>[0-9A-Fa-f]+)"),
         args=lambda m: ("wid4:" + m.group("new"),)),
    dict(etype="LABEL_PUBLISH",
         regex=re.compile(
             r"\[yzlabel\] POLL vblank_count=(?P<vc>\d+) ea=(?P<ea>0x[0-9A-Fa-f]+) "
             r"val=(?P<val>0x[0-9A-Fa-f]+)"),
         args=lambda m: (m.group("val"),)),
    # int_flip_index from the SAME [yzlabel] POLL line - the RPCS3-side flip
    # proxy on disk this pass (no dedicated per-flip probe log was found).
    dict(etype="FLIP",
         regex=re.compile(r"\[yzlabel\] POLL .*? int_flip_index=(?P<idx>\d+)"),
         args=lambda m: (m.group("idx"),)),
]

# etype -> regex for the ARMED banner that proves the probe was live in this
# RPCS3.log. None means "no dedicated banner; the pattern itself is a core
# always-on RPCS3 log line" (sys_fs_open, cellAudioPortStart HLE trace).
RPCS3LOG_ARM_BANNERS = {
    "FILE_OPEN": None,
    "AUDIO_PORT_START": None,
    "WKL_RAISE": re.compile(r"\[w4raise\] ARMED"),
    "LABEL_PUBLISH": re.compile(r"\[yzlabel\] ARMED"),
    "FLIP": re.compile(r"\[yzlabel\] ARMED"),
}

# yz_*_oracle.log probe-style lines: "[tag] ... t=<uint> ..." (get_system_time(),
# monotonic WITHIN one RPCS3 process/run only - never compare `t=` across
# two different --ref probelog files, and never across probelog vs rpcs3log).
PROBELOG_T_RE = re.compile(r"\bt=(?P<t>\d+)\b")

REF_PROBELOG_PATTERNS = [
    dict(etype="JOB_ROUND",
         regex=re.compile(r"\[yzround\] tag=ROUNDDRIVER n=(?P<n>\d+)"),
         args=lambda m: ()),
    dict(etype="JOB_ENTRY_WRITE",
         regex=re.compile(r"\[yzround\] tag=JOBWR n=(?P<n>\d+)"),
         args=lambda m: ()),
]

PROBELOG_ARM_BANNERS = {
    "JOB_ROUND": re.compile(r"\[yzround\] ARMED"),
    "JOB_ENTRY_WRITE": re.compile(r"\[yzround\] ARMED"),
}
# Verbatim cap text lifted from the ARMED banner itself (not asserted by
# this tool) - printed alongside counts for these two types so nobody
# mistakes "last n= seen" for a true ceiling.
PROBELOG_ARM_BANNER_FULL = re.compile(r"\[yzround\] ARMED:.*")

# The full vocabulary the design brief asks for. Types with no pattern on a
# given side are reported as "not captured" rather than silently skipped -
# this is the "missing reference-side events" deliverable.
FULL_VOCAB = [
    "FILE_OPEN", "AUDIO_PORT_START", "WKL_RAISE", "JOB_ROUND", "JOB_ENTRY_WRITE",
    "SPUP17", "LABEL_PUBLISH", "UCMD", "FLIP", "MAINLOOP_ITER",
]

CENSUS_BACKED_TYPES = set(CHAIN_ADDR_TO_ETYPE.values())


# ===========================================================================
# Parsing
# ===========================================================================


def _read_lines(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.readlines()


def parse_our_side(err_path, out_path):
    """Returns (events_by_type, armed_tags, census_watermark, census_banner)."""
    events = defaultdict(list)
    armed_tags = set()
    counters = defaultdict(int)
    # etype -> (count, t_ms, lineno) of the LAST census line that mentioned it
    census_watermark = {}
    census_banner = None

    for stream_name, path in (("err", err_path), ("out", out_path)):
        if not path:
            continue
        lines = _read_lines(path)
        patterns = [p for p in OUR_PATTERNS if p["stream"] == stream_name]
        for lineno, line in enumerate(lines, 1):
            for tag, banner_re in OUR_ARM_BANNERS.items():
                if banner_re.search(line):
                    armed_tags.add(tag)

            cm = CENSUS_LINE_RE.search(line)
            if cm:
                if census_banner is None:
                    census_banner = line.strip()
                t_ms = int(cm.group("t"))
                for addr, cnt in CENSUS_ADDR_RE.findall(cm.group("body")):
                    et = CHAIN_ADDR_TO_ETYPE.get(addr.upper().replace("0X", "0x"))
                    # findall gives addr already as "0x........"; normalize case
                    et = CHAIN_ADDR_TO_ETYPE.get(addr)
                    if et:
                        census_watermark[et] = (int(cnt), t_ms, lineno)

            for p in patterns:
                m = p["regex"].search(line)
                if not m:
                    continue
                counters[p["etype"]] += 1
                ev = Event(
                    etype=p["etype"],
                    ordinal=counters[p["etype"]],
                    args=p["args"](m),
                    raw=line.rstrip("\n"),
                    source=path,
                    lineno=lineno,
                )
                events[p["etype"]].append(ev)
    return events, armed_tags, census_watermark, census_banner


def parse_ref_file(path, kind):
    """Returns (events_by_type, armed_types, banner_text) for one reference file."""
    events = defaultdict(list)
    armed_types = set()
    counters = defaultdict(int)
    banner_text = {}

    if kind == "rpcs3log":
        patterns = REF_RPCS3LOG_PATTERNS
        arm_banners = RPCS3LOG_ARM_BANNERS
        clock_fn = rpcs3log_elapsed_seconds
    elif kind == "probelog":
        patterns = REF_PROBELOG_PATTERNS
        arm_banners = PROBELOG_ARM_BANNERS

        def clock_fn(line):
            m = PROBELOG_T_RE.search(line)
            return int(m.group("t")) if m else None
    else:
        raise ValueError("unknown ref kind %r (expected rpcs3log|probelog)" % kind)

    lines = _read_lines(path)
    for etype, banner_re in arm_banners.items():
        if banner_re is None:
            armed_types.add(etype)  # no banner defined -> core log line, always live
            continue
        for line in lines:
            if banner_re.search(line):
                armed_types.add(etype)
                banner_text[etype] = line.strip()
                break

    for lineno, line in enumerate(lines, 1):
        for p in patterns:
            m = p["regex"].search(line)
            if not m:
                continue
            counters[p["etype"]] += 1
            ev = Event(
                etype=p["etype"],
                ordinal=counters[p["etype"]],
                args=p["args"](m),
                raw=line.rstrip("\n"),
                source=path,
                lineno=lineno,
                clock=clock_fn(line),
            )
            events[p["etype"]].append(ev)
    return events, armed_types, banner_text


# ===========================================================================
# Comparison / report
# ===========================================================================


def count_divergence(ours_n, ref_n):
    """Pure count-based divergence: which side stalls first, by ordinal."""
    if ours_n == ref_n:
        return None
    if ours_n < ref_n:
        return dict(kind="missing", ours_n=ours_n, ref_n=ref_n)
    return dict(kind="extra", ours_n=ours_n, ref_n=ref_n)


def file_open_set_diff(ours_list, ref_list):
    """Order-independent: which basenames appear on ref but never on ours,
    in the order ref first saw them (and vice versa). Positional comparison
    across two different boots is meaningless here (thread interleaving
    differs run to run) - set membership is the meaningful question."""
    ours_seen = []
    ours_set = set()
    for ev in ours_list:
        name = ev.args[0]
        if name not in ours_set:
            ours_set.add(name)
            ours_seen.append(name)
    ref_seen = []
    ref_set = set()
    for ev in ref_list:
        name = ev.args[0]
        if name not in ref_set:
            ref_set.add(name)
            ref_seen.append(name)
    missing_in_ours = [n for n in ref_seen if n not in ours_set]
    extra_in_ours = [n for n in ours_seen if n not in ref_set]
    return missing_in_ours, extra_in_ours


def format_event_brief(ev):
    if ev is None:
        return "(none)"
    where = "%s:%d" % (os.path.basename(ev.source), ev.lineno)
    clock = "" if ev.clock is None else " clock=%s" % ev.clock
    return "#%d args=%s @%s%s" % (ev.ordinal, ev.args, where, clock)


def print_report(ours_events, ours_armed, census_watermark, census_banner,
                  ref_sources, context_n):
    print("=" * 78)
    print("BOOT-CHOREOGRAPHY DIFFERENTIAL")
    print("=" * 78)

    if census_banner:
        print("\n[chain] census line seen (uncapped watermark source):")
        print("  " + census_banner)

    print("\n--- per-event-type summary ---\n")
    header = "%-16s %10s %14s  %s" % ("EVENT", "ours_n", "ref_n(best)", "verdict")
    print(header)
    print("-" * len(header))

    for etype in FULL_VOCAB:
        ours_list = ours_events.get(etype, [])
        if etype in CENSUS_BACKED_TYPES and etype in census_watermark:
            ours_n, t_ms, lineno = census_watermark[etype]
            ours_note = "%d*" % ours_n  # '*' = census-corrected, not hit#-line count
        else:
            ours_n = len(ours_list)
            ours_note = str(ours_n)

        any_ref_pattern = any(etype == p["etype"] for p in REF_RPCS3LOG_PATTERNS) or \
            any(etype == p["etype"] for p in REF_PROBELOG_PATTERNS)

        if etype not in [p["etype"] for p in OUR_PATTERNS] and etype not in CENSUS_BACKED_TYPES:
            ours_note = "NOT INSTRUMENTED (ours)"

        if not any_ref_pattern:
            verdict = "NO REFERENCE PROBE ON DISK for this type"
            ref_n_str = "-"
        else:
            best_label, best_n, best_armed = None, -1, False
            for label, (ref_events, armed_types, _banner) in ref_sources:
                rl = ref_events.get(etype, [])
                if len(rl) > best_n:
                    best_label, best_n, best_armed = label, len(rl), etype in armed_types

            if best_n <= 0:
                verdict = ("ref count=0 (MEASURED zero, ARMED banner present)"
                           if best_armed else
                           "ref count=0 (PLAUSIBLE, no ARMED banner seen)")
                ref_n_str = "0"
            else:
                ref_n_str = "%d(%s)" % (best_n, best_label)
                if ours_n == 0:
                    verdict = "OURS NEVER FIRES (ref has %d)" % best_n
                elif ours_n < best_n:
                    verdict = "ours stalls at #%d, ref reaches #%d" % (ours_n, best_n)
                else:
                    verdict = "ours >= best ref sample on disk (ref may itself be capped)"

        print("%-16s %10s %14s  %s" % (etype, ours_note, ref_n_str, verdict))

    print("\n  '*' = corrected via the uncapped [chain] census watermark, NOT the")
    print("        hit#-line count (that line is capped at 4 per address).")

    # --- FILE_OPEN set-diff (the meaningful cross-run comparison for this type) --
    print("\n--- FILE_OPEN: files ref opened that ours never reached ---\n")
    ours_fo = ours_events.get("FILE_OPEN", [])
    for label, (ref_events, _armed, _banner) in ref_sources:
        ref_fo = ref_events.get("FILE_OPEN", [])
        if not ref_fo:
            continue
        missing, extra = file_open_set_diff(ours_fo, ref_fo)
        print("  vs %s (ref opened %d distinct files, ours opened %d distinct files):"
              % (label, len({e.args[0] for e in ref_fo}), len({e.args[0] for e in ours_fo})))
        if missing:
            print("    ref-only (first %d of %d): %s" % (min(10, len(missing)), len(missing), missing[:10]))
        else:
            print("    (no files seen on ref that ours missed)")
        if extra:
            print("    ours-only (first %d of %d): %s" % (min(10, len(extra)), len(extra), extra[:10]))

    # --- first divergence per type (count-based) ----------------------------
    print("\n--- first divergence per type (count-based) ---\n")
    any_div = False
    for etype in FULL_VOCAB:
        if etype in CENSUS_BACKED_TYPES and etype in census_watermark:
            ours_n = census_watermark[etype][0]
        else:
            ours_n = len(ours_events.get(etype, []))
        best_n = 0
        best_label = None
        for label, (ref_events, _armed, _banner) in ref_sources:
            n = len(ref_events.get(etype, []))
            if n > best_n:
                best_n, best_label = n, label
        if best_n == 0:
            continue
        div = count_divergence(ours_n, best_n)
        if div is not None and div["kind"] == "missing":
            any_div = True
            print("[%s] ours stops at ordinal #%d; ref (%s) reaches #%d (first missing = ours #%d)"
                  % (etype, ours_n, best_label, best_n, ours_n + 1))
    if not any_div:
        print("(no type shows ours falling behind an available reference count)")

    # --- tail context: our own last N recognized events before the wedge ---
    print("\n--- our own tail (last %d recognized per-line events, PER STREAM - " % context_n
          + "err and out have no shared clock, see module docstring) ---\n")
    for stream_label, suffix in (("stderr", ".err"), ("stdout", ".out")):
        all_ours = [ev for lst in ours_events.values() for ev in lst if ev.source.endswith(suffix)]
        all_ours.sort(key=lambda ev: ev.lineno)
        print("  [%s tail]" % stream_label)
        for ev in all_ours[-context_n:]:
            print("    %-16s %s" % (ev.etype, format_event_brief(ev)))

    print("\n--- our-side ARMED tags seen (liveness banners) ---")
    print("  " + (", ".join(sorted(ours_armed)) if ours_armed else "(none seen)"))

    print("\n--- reference-side ARMED banners seen, verbatim ---")
    for label, (_ref_events, _armed, banner_text) in ref_sources:
        for etype, text in banner_text.items():
            print("  [%s] %s: %s" % (label, etype, text))


# ===========================================================================
# CLI
# ===========================================================================


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ours-err", required=True, help="our port's stderr capture (scratch\\*.err)")
    ap.add_argument("--ours-out", required=False, default=None,
                     help="our port's stdout capture (scratch\\*.out) - needed for FILE_OPEN/AUDIO_PORT_START")
    ap.add_argument("--ref", action="append", default=[], metavar="PATH:KIND",
                     help="reference log, KIND is rpcs3log or probelog. Repeatable.")
    ap.add_argument("--context", type=int, default=20, help="tail context size (default 20)")
    args = ap.parse_args(argv)

    ours_events, ours_armed, census_watermark, census_banner = parse_our_side(
        args.ours_err, args.ours_out)

    ref_sources = []
    for spec in args.ref:
        if ":" not in spec:
            ap.error("--ref must be PATH:KIND, got %r" % spec)
        path, kind = spec.rsplit(":", 1)
        if not os.path.isfile(path):
            ap.error("--ref path does not exist: %s" % path)
        ref_events, ref_armed, banner_text = parse_ref_file(path, kind)
        label = os.path.basename(path)
        ref_sources.append((label, (ref_events, ref_armed, banner_text)))

    if not ref_sources:
        print("WARNING: no --ref given; printing our-side event counts only.\n", file=sys.stderr)

    print_report(ours_events, ours_armed, census_watermark, census_banner, ref_sources, args.context)


if __name__ == "__main__":
    main()
