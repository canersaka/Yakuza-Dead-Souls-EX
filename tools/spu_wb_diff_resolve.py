#!/usr/bin/env python3
"""Inconclusive-window resolver for the WB/WJ differential matrices.

For every window pair recorded on disk whose verdict was INCONCLUSIVE
(the per-instruction DIAG reference hit its dispatch/time budget, or a
side timed out), rerun that exact (placement, entry, seed) window with a
much larger budget using the already-compiled twin executables, and
re-verdict. Windows the reference still cannot finish are classified
UNBOUNDED-REFERENCE with the measured hop rate and the lane twin's own
terminal -- the documented s50 spin-loop class, closed by evidence rather
than left ambiguous.

Usage (vcvars not required -- exes are cached):
    py -3 tools/spu_wb_diff_resolve.py --lane wj [--hopmax 12000000]
        [--timeout 180] [--only stem1,stem2]
Writes scratch/<lane>diff/resolve_report.json.
"""

import argparse
import glob
import json
import os
import re
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import spu_region_diff as D          # noqa: E402


def window_pairs(stem_dir, lane_sfx):
    """Yield (entry, seed, diag_path, lane_path) for recorded windows."""
    for dp in sorted(glob.glob(os.path.join(stem_dir, "e*_s*.diag.txt"))):
        m = re.match(r"e([0-9A-F]+)_s(\d+)\.diag\.txt$", os.path.basename(dp))
        if not m:
            continue
        lp = dp[:-len(".diag.txt")] + f".{lane_sfx}.txt"
        yield int(m.group(1), 16), int(m.group(2)), dp, lp


def parse_or_none(path):
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return None
    return D.parse_out(path)


def is_inconclusive(da, dw):
    if da is None or dw is None:
        return True
    return da.get("term") in ("HOPMAX", "TIMEOUT") or \
        dw.get("term") in ("HOPMAX", "TIMEOUT")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lane", choices=("wb", "wj"), default="wj")
    ap.add_argument("--hopmax", type=int, default=12000000)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--evbudget", type=int, default=384)
    ap.add_argument("--only", default=None)
    args = ap.parse_args()
    only = set(args.only.split(",")) if args.only else None
    lane_sfx = "wb"                       # window files are named .wb.txt in
    work = os.path.join(ROOT, "scratch",  # both lanes (runner reuses names)
                        f"{args.lane}diff")

    results = []
    n_resolved = n_unbounded = n_still = 0
    for stem_dir in sorted(glob.glob(os.path.join(work, "*"))):
        if not os.path.isdir(stem_dir):
            continue
        stem = os.path.basename(stem_dir)
        if only and stem not in only:
            continue
        codebin = os.path.join(stem_dir, "code.bin")
        exe_d = os.path.join(stem_dir, f"{stem}.diag.exe")
        exe_w = os.path.join(stem_dir, f"{stem}_{args.lane}.exe")
        if not (os.path.exists(codebin) and os.path.exists(exe_d)
                and os.path.exists(exe_w)):
            continue
        base = None
        for e, s, dp, lp in window_pairs(stem_dir, lane_sfx):
            da, dw = parse_or_none(dp), parse_or_none(lp)
            if not is_inconclusive(da, dw):
                continue
            if base is None:
                # base = lowest entry ever recorded (image base is always in
                # the matrix; identical to the runner's base argument)
                base = min(x[0] for x in window_pairs(stem_dir, lane_sfx))
            od = os.path.join(stem_dir, f"e{e:05X}_s{s}.rdiag.txt")
            ow = os.path.join(stem_dir, f"e{e:05X}_s{s}.r{args.lane}.txt")
            st_d = D.run_one(exe_d, codebin, base, e, s, args.evbudget,
                             args.hopmax, od, args.timeout)
            st_w = D.run_one(exe_w, codebin, base, e, s, args.evbudget,
                             args.hopmax, ow, args.timeout)
            ra, rw = parse_or_none(od), parse_or_none(ow)
            ent = {"stem": stem, "entry": f"0x{e:X}", "seed": s}
            if st_d or st_w or ra is None or rw is None or \
                    ra.get("term") in ("HOPMAX", "TIMEOUT") or \
                    rw.get("term") in ("HOPMAX", "TIMEOUT"):
                # reference (or lane) still unbounded at 6x budget: record
                # the measured rates + the lane twin's own terminal
                ent["verdict"] = "UNBOUNDED-REFERENCE"
                ent["diag"] = (f"term={ra.get('term') if ra else st_d} "
                               f"hops={ra.get('hops') if ra else '?'}")
                ent["lane"] = (f"term={rw.get('term') if rw else st_w} "
                               f"hops={rw.get('hops') if rw else '?'}")
                n_unbounded += 1
            else:
                v, detail = D.cmp_runs(ra, rw)
                ent["verdict"] = v
                if detail:
                    ent["detail"] = detail
                if v == "MATCH":
                    n_resolved += 1
                else:
                    n_still += 1
            results.append(ent)
            print(f"{stem:24s} e{e:05X} s{s}: {ent['verdict']}"
                  f"{' ' + ent.get('detail', '') if ent.get('detail') else ''}")

    rep = os.path.join(work, "resolve_report.json")
    with open(rep, "w") as f:
        json.dump({"hopmax": args.hopmax, "timeout": args.timeout,
                   "resolved_match": n_resolved,
                   "unbounded_reference": n_unbounded,
                   "mismatch_or_other": n_still,
                   "windows": results}, f, indent=1)
    print(f"\nresolved MATCH: {n_resolved}; unbounded-reference: "
          f"{n_unbounded}; other: {n_still}; report -> {rep}")
    sys.exit(1 if n_still else 0)


if __name__ == "__main__":
    main()
