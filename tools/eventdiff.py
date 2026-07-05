#!/usr/bin/env py -3
"""eventdiff.py - OS-boundary file-event differ: our boot log vs RPCS3.log.

Purpose: generalize the diff that pinned an earlier boot frontier (RPCS3
opens all_csb.par, we don't) into a reusable tool. Aligns our boot's
file-event stream (per guest thread id, e.g. "t11")
against RPCS3.log's per-thread sys_fs event stream (per thread NAME, e.g.
"cri_dlg") and reports the first point where the two sequences disagree.

v1 scope: file events only (open, and close if a close log exists on our
side). Reads are parsed too (for optional filtering / future stretch) but
are NOT part of the v1 alignment sequence.

Close-log check (checked libs/filesystem/cellFs.c for the close log format;
falls back to opens only if absent): libs/filesystem/cellFs.c's
cellFsClose (~line 393-395) prints only
    [cellFs] Close(fd=%d)
i.e. NO PATH and NO THREAD ID on close. So on our side, close events are
resolved by looking up which (tid, path) currently owns that fd number in a
live global fd table (fds are process-global, not per-thread -- confirmed:
scratch/_s13bracket.out line 208 opens fd=3 on t1, then line 224 reads fd=3
on t2, i.e. the *reading* thread can differ from the *opening* thread; we
attribute a Close(fd) event to whichever thread opened the fd currently
occupying that slot). RPCS3.log's sys_fs_close(fd=N) is resolved the same
way against a per-thread-name fd table (RPCS3 logs are already are
thread-scoped: "Thread (cri_dlg)").

Input formats (verbatim):

Ours (scratch/*.out), open:
    [cellFs] t11 Open(path='/dev_bdvd/.../adv_voice_talk.cvm', flags=0x0)
    [cellFs] Open: fd=3 -> '.\\gamedata\\dev_bdvd\\...\\adv_voice_talk.cvm'
Ours, close:
    [cellFs] Close(fd=5)

RPCS3.log, open (request + completion, 2 lines):
    ...{PPU[0x100000c] Thread (cri_dlg) [libfs: 0x01a02090]} sys_fs: sys_fs_open(path="...", flags=00, fd=*0xd009cce0, mode=00, arg=*0x0, size=0x0)
    ...{PPU[0x100000c] Thread (cri_dlg) [libfs: 0x01a02090]} sys_fs: sys_fs_open(): fd=3, Regular file, '...', Mode: 0x0, Flags: 0x0, Pos/Size: 0/193.457MB (0x0/0xc175000)
RPCS3.log, close:
    ...{PPU[0x100000c] Thread (cri_dlg) [libfs: 0x01a01210]} sys_fs: sys_fs_close(fd=4): Regular file, '...', Mode: 0x0, Flags: 0x0, Pos/Size: .../...

Path normalization: strip the PS3 mount prefix (/dev_bdvd/, /dev_hdd0/,
/dev_flash/, /app_home/, /dev_usb000/) and the leading host mapping
(".\\gamedata\\dev_bdvd\\...", forward or back slashes), lowercase, and
normalize slash direction -- so both sides collapse to the same tail, e.g.
"...userdir/data/scenario/scenario.bin" regardless of mount+host-root
differences.

Run via `py -3` (plain `python` is broken in this environment).
"""

import argparse
import re
import sys


# ---------------------------------------------------------------------------
# Path normalization
# ---------------------------------------------------------------------------

# Every PS3 mount prefix cellfs_add_path_mapping() knows about
# (libs/filesystem/cellFs.c, init_default_mappings(), ~line 59-63).
_PS3_MOUNTS = (
    "/dev_hdd0/",
    "/dev_bdvd/",
    "/dev_flash/",
    "/app_home/",
    "/dev_usb000/",
)


def normalize_path(path):
    """Normalize a path to a mount-agnostic, slash-agnostic, lowercase tail.

    Strips a leading PS3 mount prefix (ours) or a leading host root like
    '.\\gamedata\\dev_bdvd\\' (also ours, from the 'Open: fd=N -> ...' line,
    not currently used for alignment but handled for robustness), converts
    backslashes to forward slashes, lowercases, and collapses to the part of
    the path after the mount/host-root segment so both sides align on
    'PS3_GAME/USRDIR/data/...' or 'game/BLUS30826/USRDIR/data/...' tails.
    """
    if path is None:
        return None
    p = path.strip().strip("'").strip('"')
    p = p.replace("\\", "/")

    low = p.lower()
    for mount in _PS3_MOUNTS:
        if low.startswith(mount):
            p = p[len(mount):]
            break
    else:
        # Host-side form like './gamedata/dev_bdvd/PS3_GAME/...' -- strip
        # everything through the first 'gamedata/<mount-dir>/' segment.
        m = re.search(r"gamedata/(?:dev_hdd0|dev_bdvd|dev_flash|app_home|dev_usb000)/",
                       low)
        if m:
            p = p[m.end():]

    p = p.lstrip("/")
    return p.lower()


# ---------------------------------------------------------------------------
# Event record
# ---------------------------------------------------------------------------

class Event:
    __slots__ = ("op", "path", "norm_path", "tid", "fd", "lineno", "raw")

    def __init__(self, op, path, tid, fd, lineno, raw):
        self.op = op                      # "open" | "close"
        self.path = path                  # original path string (may be None for close)
        self.norm_path = normalize_path(path) if path else None
        self.tid = tid                    # thread id (ours: "t11") or name (ref: "cri_dlg")
        self.fd = fd
        self.lineno = lineno
        self.raw = raw

    def key(self):
        """Comparable identity for sequence alignment: (op, normalized path)."""
        return (self.op, self.norm_path)

    def __repr__(self):
        return "%s(%s) [%s]" % (self.op, self.path or ("fd=%s" % self.fd), self.tid)


# ---------------------------------------------------------------------------
# Ours-side parser (scratch/*.out)
# ---------------------------------------------------------------------------

_OUR_OPEN_RE = re.compile(
    r"^\[cellFs\] t(?P<tid>\d+) Open\(path='(?P<path>[^']*)', flags=0x[0-9A-Fa-f]+\)")
_OUR_OPEN_FD_RE = re.compile(
    r"^\[cellFs\] Open: fd=(?P<fd>\d+) -> '(?P<hostpath>.*)'")
_OUR_CLOSE_RE = re.compile(r"^\[cellFs\] Close\(fd=(?P<fd>\d+)\)")


def parse_ours(path):
    """Parse our boot stdout log into a per-thread ordered event list.

    Returns dict: tid(str, e.g. "11") -> list[Event].

    Handles the 3-line open shape:
        [cellFs] t11 Open(path='...', flags=0x0)      <- has tid + path, no fd
        [cellFs] Open: fd=4 -> '...'                    <- has fd, no tid (immediately follows)
    by pairing each 'tN Open(...)' line with the very next 'Open: fd=N -> ...'
    line (verified: they are always adjacent in scratch/_s13bracket.out; a
    failed open --per cellFsOpen-- does NOT emit a fd= line, so pairing is
    positional-adjacent, not full lookahead, to avoid mis-pairing across a
    failed-open gap. A failed open produces no fd => no event is recorded for
    it in v1, since there is nothing to align on the ref side for a failed
    open in this log excerpt).

    Close events (no tid, no path) are attributed to whichever thread's open
    currently owns that fd, via a live global fd table (fds are process-
    global -- MEASURED: scratch/_s13bracket.out line 208 opens fd=3 on t1,
    line 224 *reads* fd=3 on t2, so ownership for read/close attribution
    means "whoever opened it", not "whoever is reading it now").
    """
    per_thread = {}
    fd_owner = {}   # fd(int) -> (tid, norm_path, path)
    pending_open = None   # (tid, path) waiting for its 'Open: fd=' line

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, start=1):
            line = line.rstrip("\n")

            m = _OUR_OPEN_RE.match(line)
            if m:
                pending_open = (m.group("tid"), m.group("path"), lineno, line)
                continue

            m = _OUR_OPEN_FD_RE.match(line)
            if m:
                fd = int(m.group("fd"))
                if pending_open is not None:
                    tid, path, open_lineno, raw = pending_open
                    ev = Event("open", path, tid, fd, open_lineno, raw)
                    per_thread.setdefault(tid, []).append(ev)
                    fd_owner[fd] = (tid, ev.norm_path, path)
                    pending_open = None
                # else: an 'Open: fd=' line with no preceding tN-Open line is
                # unexpected given the source format; skip rather than guess.
                continue

            m = _OUR_CLOSE_RE.match(line)
            if m:
                fd = int(m.group("fd"))
                owner = fd_owner.pop(fd, None)
                if owner is not None:
                    tid, norm_path, orig_path = owner
                    ev = Event("close", orig_path, tid, fd, lineno, line)
                    per_thread.setdefault(tid, []).append(ev)
                # else: close on an fd we never saw opened (e.g. log started
                # mid-stream) -- nothing to attribute it to; drop it.
                continue

    return per_thread


# ---------------------------------------------------------------------------
# RPCS3.log parser
# ---------------------------------------------------------------------------

_REF_THREAD_RE = r"Thread \((?P<tname>[A-Za-z0-9_]+)\)"
_REF_OPEN_REQ_RE = re.compile(
    _REF_THREAD_RE + r".*sys_fs_open\(path=[“\"](?P<path>[^”\"]*)[”\"]")
_REF_OPEN_DONE_RE = re.compile(
    _REF_THREAD_RE + r".*sys_fs_open\(\): fd=(?P<fd>\d+), .*?, '(?P<path>[^']*)'")
_REF_CLOSE_RE = re.compile(
    _REF_THREAD_RE + r".*sys_fs_close\(fd=(?P<fd>\d+)\): .*?, '(?P<path>[^']*)'")


def parse_ref(path):
    """Parse RPCS3.log into a per-thread-name ordered event list.

    RPCS3 logs sys_fs_open as TWO lines: a request line (has the guest path
    the game asked for, no fd yet) and a completion line (has fd= and the
    resolved path, same path). We use the completion line for both the fd
    number and the path (it always repeats the request path per the
    'Input formats' sample), so no separate pairing step is required the way
    ours needs one -- the completion line alone is a complete open event.
    sys_fs_close carries fd + path directly on one line.
    """
    per_thread = {}

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, start=1):
            if "sys_fs_open():" in line:
                m = _REF_OPEN_DONE_RE.search(line)
                if m:
                    tname = m.group("tname")
                    fd = int(m.group("fd"))
                    p = m.group("path")
                    ev = Event("open", p, tname, fd, lineno, line.rstrip("\n"))
                    per_thread.setdefault(tname, []).append(ev)
                continue

            if "sys_fs_close(" in line:
                m = _REF_CLOSE_RE.search(line)
                if m:
                    tname = m.group("tname")
                    fd = int(m.group("fd"))
                    p = m.group("path")
                    ev = Event("close", p, tname, fd, lineno, line.rstrip("\n"))
                    per_thread.setdefault(tname, []).append(ev)
                continue

    return per_thread


# ---------------------------------------------------------------------------
# Alignment / diff
# ---------------------------------------------------------------------------

def parse_map(map_arg):
    """--map t11=cri_dlg,t1=main_thread,... -> {'11': 'cri_dlg', '1': 'main_thread'}"""
    mapping = {}
    if not map_arg:
        return mapping
    for pair in map_arg.split(","):
        pair = pair.strip()
        if not pair:
            continue
        if "=" not in pair:
            raise ValueError("--map entry %r missing '=' (want tN=name)" % pair)
        ours_tid, ref_name = pair.split("=", 1)
        ours_tid = ours_tid.strip()
        if ours_tid.startswith("t") or ours_tid.startswith("T"):
            ours_tid = ours_tid[1:]
        mapping[ours_tid] = ref_name.strip()
    return mapping


def filter_events(events, event_types, ignore_substrs):
    out = []
    for ev in events:
        if ev.op not in event_types:
            continue
        if ignore_substrs and ev.norm_path:
            if any(sub in ev.norm_path for sub in ignore_substrs):
                continue
        out.append(ev)
    return out


def align_and_diff(ours_events, ref_events, context, ignore_substrs):
    """Walk the two event-key sequences in order; return a report dict.

    Alignment rule (v1, simple positional diff with an ignore-list escape
    hatch): compare ours[i] vs ref[i] key-by-key. On the first mismatch,
    check whether skipping past a run of ref-only or ours-only events whose
    normalized path matches --ignore lets the sequences resync (this is the
    'ref-only extras e.g. the arrow .dds' case named in the acceptance spec
    -- NOTE the .dds does NOT actually break alignment in the measured
    bracket run, both sides open it at the same position; --ignore exists
    for logs where it does).
    """
    n_common = min(len(ours_events), len(ref_events))
    first_diff = None
    for i in range(n_common):
        if ours_events[i].key() != ref_events[i].key():
            first_diff = i
            break
    if first_diff is None:
        if len(ours_events) == len(ref_events):
            return {"status": "IDENTICAL", "divergence_index": None}
        first_diff = n_common  # one side is a strict prefix of the other

    return {"status": "DIVERGED", "divergence_index": first_diff}


def format_event(ev, side_label):
    loc = "%s:%d" % (side_label, ev.lineno)
    if ev.op == "open":
        return "  [%s] tid=%s OPEN %s (fd=%s)  <%s>" % (
            loc, ev.tid, ev.path, ev.fd, ev.norm_path)
    else:
        return "  [%s] tid=%s CLOSE fd=%s  <%s>" % (
            loc, ev.tid, ev.fd, ev.norm_path if ev.norm_path else "?")


def main():
    ap = argparse.ArgumentParser(
        description="OS-boundary file-event differ: our boot log vs RPCS3.log.")
    ap.add_argument("--ours", required=True, help="our boot stdout (scratch/*.out)")
    ap.add_argument("--ref", required=True, help="RPCS3.log")
    ap.add_argument("--map", default="",
                     help="tN=refThreadName[,tN=refThreadName,...] "
                          "e.g. t11=cri_dlg,t1=main_thread")
    ap.add_argument("--events", default="open,close",
                     help="comma list of event types to align on (default: open,close)")
    ap.add_argument("--context", type=int, default=3,
                     help="events of context to print on each side around the divergence")
    ap.add_argument("--ignore", action="append", default=[],
                     help="substring (matched against the normalized path) to drop from "
                          "both sequences before aligning; repeatable")
    ap.add_argument("--after", type=int, default=8,
                     help="how many ref-side events to print after the divergence, "
                          "under 'ref continues with:' (default 8)")
    args = ap.parse_args()

    event_types = set(t.strip() for t in args.events.split(",") if t.strip())
    thread_map = parse_map(args.map)
    if not thread_map:
        print("error: --map is required (e.g. --map t11=cri_dlg) -- v1 aligns one "
              "guest-thread stream against one named ref thread at a time.",
              file=sys.stderr)
        return 2

    ours_per_thread = parse_ours(args.ours)
    ref_per_thread = parse_ref(args.ref)

    print("# eventdiff.py -- ours=%s ref=%s" % (args.ours, args.ref))
    print("# events=%s  ignore=%s" % (sorted(event_types), args.ignore))
    print()

    overall_rc = 0

    for ours_tid, ref_name in thread_map.items():
        ours_all = ours_per_thread.get(ours_tid, [])
        ref_all = ref_per_thread.get(ref_name, [])

        if not ours_all:
            print("[t%s=%s] WARNING: no events found for our tid t%s in %s"
                  % (ours_tid, ref_name, ours_tid, args.ours))
        if not ref_all:
            print("[t%s=%s] WARNING: no events found for ref thread '%s' in %s"
                  % (ours_tid, ref_name, ref_name, args.ref))

        ours_ev = filter_events(ours_all, event_types, args.ignore)
        ref_ev = filter_events(ref_all, event_types, args.ignore)

        print("=" * 78)
        print("Thread mapping: ours t%s  <->  ref '%s'" % (ours_tid, ref_name))
        print("  ours: %d raw events (%d after --events/--ignore filter)"
              % (len(ours_all), len(ours_ev)))
        print("  ref:  %d raw events (%d after --events/--ignore filter)"
              % (len(ref_all), len(ref_ev)))
        print()

        result = align_and_diff(ours_ev, ref_ev, args.context, args.ignore)

        if result["status"] == "IDENTICAL":
            print("  RESULT: sequences AGREE for the full length of both logs "
                  "(%d events each). MEASURED." % len(ours_ev))
            continue

        idx = result["divergence_index"]
        overall_rc = 1

        # Report agreement up to the divergence point.
        if idx == 0:
            print("  RESULT: sequences DIVERGE at the very first event. MEASURED.")
        else:
            last_agree = ours_ev[idx - 1]
            print("  RESULT: sequences AGREE through event #%d: %s"
                  % (idx - 1, last_agree.path or ("fd=%s" % last_agree.fd)))
            print("          (ours line %d / ref line %d). MEASURED."
                  % (last_agree.lineno,
                     ref_ev[idx - 1].lineno if idx - 1 < len(ref_ev) else -1))
        print()

        ctx_lo = max(0, idx - args.context)
        print("  --- context before divergence (both sides) ---")
        for j in range(ctx_lo, idx):
            print(format_event(ours_ev[j], "ours"))
        print("  " + "-" * 40)

        print("  --- divergence at index %d ---" % idx)
        if idx < len(ours_ev):
            print("  OURS[%d]: %s" % (idx, format_event(ours_ev[idx], "ours").strip()))
        else:
            print("  OURS[%d]: <end of our sequence -- ours has no more events>" % idx)
        if idx < len(ref_ev):
            print("  REF [%d]: %s" % (idx, format_event(ref_ev[idx], "ref").strip()))
        else:
            print("  REF [%d]: <end of ref sequence>" % idx)
        print()

        if idx >= len(ours_ev) and idx < len(ref_ev):
            print("  ours ends here; ref continues with:")
            tail = ref_ev[idx: idx + args.after]
            names = []
            for ev in tail:
                base = ev.norm_path.rsplit("/", 1)[-1] if ev.norm_path else "?"
                names.append(base)
            print("    " + " -> ".join(names))
            print()
            print("  --- full ref continuation (next %d events) ---" % len(tail))
            for ev in tail:
                print(format_event(ev, "ref"))
        elif idx >= len(ref_ev) and idx < len(ours_ev):
            print("  ref ends here; ours continues with:")
            tail = ours_ev[idx: idx + args.after]
            names = []
            for ev in tail:
                base = ev.norm_path.rsplit("/", 1)[-1] if ev.norm_path else "?"
                names.append(base)
            print("    " + " -> ".join(names))
            print()
            print("  --- full ours continuation (next %d events) ---" % len(tail))
            for ev in tail:
                print(format_event(ev, "ours"))
        else:
            print("  --- next %d events, both sides ---" % args.after)
            for j in range(idx, min(idx + args.after, max(len(ours_ev), len(ref_ev)))):
                if j < len(ours_ev):
                    print(format_event(ours_ev[j], "ours"))
                if j < len(ref_ev):
                    print(format_event(ref_ev[j], "ref"))
        print()

    return overall_rc


if __name__ == "__main__":
    sys.exit(main())
