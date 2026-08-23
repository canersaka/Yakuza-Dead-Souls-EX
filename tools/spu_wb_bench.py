#!/usr/bin/env python3
"""WB microbenchmark: DIAG vs FAST vs WB on real lifted blocks.

Compiles three bench executables per placement (spu_wb_bench_driver.c +
the respective twin TU(s), same recipes as the differential runner) and
times identical architectural windows -- the entry-pc matrix filtered to
windows where every twin terminates the same way with the same event
count (unequal work is excluded, not averaged in).

Reports per window: diag/fast/wb microseconds, dispatch counts, and the
FAST->WB and DIAG->WB speedups; per placement: geometric means.

Run from a vcvars64 shell after gen_spu_wb.py + spu_wb_diff.py.
Usage:
    py -3 tools/spu_wb_bench.py --only job_bin_a_e400 [--reps 9]
"""

import argparse
import json
import math
import os
import re
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import gen_spu_regions as G          # noqa: E402
import spu_region_diff as D          # noqa: E402
import gen_spu_wb as GW              # noqa: E402
import spu_wb_diff as WD             # noqa: E402

WORK = os.path.join(ROOT, "scratch", "wbbench")
BENCH = os.path.join(TOOLS, "spu_wb_bench_driver.c")
FAST = WD.FAST
WB = WD.WB


def compile_bench(stem, twin, register, outdir):
    """twin: 'diag' (instruction TU), 'fast' (region TU), 'wb' (wb+fast)."""
    exe = os.path.join(outdir, f"{stem}.bench.{twin}.exe")
    objdir = os.path.join(outdir, f"benchobj_{twin}")
    os.makedirs(objdir, exist_ok=True)
    inc = ["/I", os.path.join(ROOT, "include"),
           "/I", os.path.join(ROOT, "runtime", "spu")]
    if twin == "diag":
        diag_c = compile_bench.diag_c
        h_dir, h_name = compile_bench.diag_h_dir, compile_bench.diag_h_name
        srcs = [[BENCH, diag_c]]
        flags = [D.CLFLAGS + inc + ["/I", h_dir,
                                    f"/DTWIN_HEADER={h_name}",
                                    f"/DREGISTER_FN={register}"]]
    elif twin == "fast":
        srcs = [[BENCH, os.path.join(FAST, f"{stem}_fast.c")]]
        flags = [D.CLFLAGS + inc + ["/I", FAST,
                                    f"/DTWIN_HEADER={stem}_fast.h",
                                    f"/DREGISTER_FN={register}"]]
    else:
        # three-object: bench driver (wb header), renamed fast, AVX2 wb
        steps = [
            (D.CLFLAGS + inc + ["/I", WB, f"/DTWIN_HEADER={stem}_wb.h",
                                f"/DREGISTER_FN={register}",
                                "/c", BENCH, f"/Fo{objdir}\\bench.obj"]),
            (D.CLFLAGS + inc + ["/I", FAST,
                                f"/D{register}={register}_fastbase",
                                "/c", os.path.join(FAST, f"{stem}_fast.c"),
                                f"/Fo{objdir}\\fast_rn.obj"]),
            (D.CLFLAGS + WD.WB_EXTRA + inc +
             ["/I", WB, "/c", os.path.join(WB, f"{stem}_wb.c"),
              f"/Fo{objdir}\\wb.obj"]),
        ]
        for s in steps:
            r = WD.cl_run(s)
            if r.returncode != 0:
                return None, r.stdout + r.stderr
        r = WD.cl_run(["/nologo", "/MT", f"{objdir}\\bench.obj",
                       f"{objdir}\\fast_rn.obj", f"{objdir}\\wb.obj",
                       f"/Fe:{exe}"])
        return (exe if r.returncode == 0 else None), r.stdout + r.stderr
    # single-step twins
    cmd = flags[0] + [f"/Fo{objdir}\\"] + srcs[0] + [f"/Fe:{exe}"]
    r = WD.cl_run(cmd)
    return (exe if r.returncode == 0 else None), r.stdout + r.stderr


_BENCH_RE = re.compile(
    r"BENCH entry=(\w+) term=(\S+) status=(\d+) hops=(\d+) ev=(\d+) "
    r"reps=(\d+) total_us=(\d+) avg_us=([\d.]+)")


def run_bench(exe, codebin, base, entry, seed, evb, hopmax, reps, timeout):
    try:
        r = subprocess.run([exe, codebin, f"{base:X}", f"{entry:X}", str(seed),
                            str(evb), str(hopmax), str(reps)],
                           capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None
    m = _BENCH_RE.search(r.stdout)
    if not m:
        return None
    return {"term": m.group(2), "status": int(m.group(3)),
            "hops": int(m.group(4)), "ev": int(m.group(5)),
            "avg_us": float(m.group(8))}


def process(famname, fam, plc, args, results):
    stem = plc["stem"]
    if args.only and stem not in args.only:
        return
    wb_c = os.path.join(WB, f"{stem}_wb.c")
    fast_c = os.path.join(FAST, f"{stem}_fast.c")
    if not os.path.exists(wb_c):
        return
    diag_c = G.resolve_diag_path(stem, plc["diag"]) if plc.get("diag") else None
    if not diag_c or not os.path.exists(diag_c):
        return
    _p, register, diag_h = G.read_header_identity(diag_c)
    compile_bench.diag_c = diag_c
    compile_bench.diag_h_dir = os.path.dirname(diag_h)
    compile_bench.diag_h_name = os.path.basename(diag_h)

    try:
        elf_path, base = GW.placement_input(famname, fam, plc, args.roots)
    except FileNotFoundError:
        return
    code, _b, entry = G.parse_spu_elf(elf_path)
    metrics = D.derive_metrics(fast_c, base, len(code), register)

    outdir = os.path.join(WORK, stem)
    os.makedirs(outdir, exist_ok=True)
    codebin = os.path.join(outdir, "code.bin")
    with open(codebin, "wb") as f:
        f.write(code)

    exes = {}
    for twin in ("diag", "fast", "wb"):
        exe, err = compile_bench(stem, twin, register, outdir)
        if not exe:
            print(f"[{famname}] {stem}: bench compile fail ({twin}): "
                  f"{err[-300:]}")
            return
        exes[twin] = exe

    required = [int(v, 0) if isinstance(v, str) else int(v)
                for v in plc.get("conformance_entries", [])]
    entries = D.pick_entries(entry, base, metrics, args.max_entries, required)
    rows = []
    for e in entries:
        row = {"entry": e}
        ok = True
        for twin in ("diag", "fast", "wb"):
            b = run_bench(exes[twin], codebin, base, e, args.seed,
                          args.evbudget, args.hopmax, args.reps, args.timeout)
            if b is None:
                ok = False
                break
            row[twin] = b
        if not ok:
            continue
        # identical architectural work only
        if not (row["diag"]["term"] == row["fast"]["term"] == row["wb"]["term"]
                and row["diag"]["ev"] == row["fast"]["ev"] == row["wb"]["ev"]
                and row["diag"]["term"] in ("EVCUT", "RETURNED", "STOPPED",
                                            "HALTED", "DRAINED")):
            continue
        row["speedup_fast_wb"] = (row["fast"]["avg_us"] / row["wb"]["avg_us"]
                                  if row["wb"]["avg_us"] > 0 else None)
        row["speedup_diag_wb"] = (row["diag"]["avg_us"] / row["wb"]["avg_us"]
                                  if row["wb"]["avg_us"] > 0 else None)
        rows.append(row)
        print(f"  e{e:05X} {row['diag']['term']:8s} "
              f"diag={row['diag']['avg_us']:10.1f}us "
              f"fast={row['fast']['avg_us']:10.1f}us "
              f"wb={row['wb']['avg_us']:10.1f}us  "
              f"fast/wb={row['speedup_fast_wb']:.2f}x "
              f"diag/wb={row['speedup_diag_wb']:.2f}x "
              f"hops d/f/w={row['diag']['hops']}/{row['fast']['hops']}/"
              f"{row['wb']['hops']}")
    ent = {"family": famname, "stem": stem, "windows": rows}
    su = [r["speedup_fast_wb"] for r in rows
          if r.get("speedup_fast_wb") and r["fast"]["avg_us"] >= args.min_us]
    if su:
        ent["geomean_fast_wb"] = round(math.exp(sum(math.log(x) for x in su)
                                                / len(su)), 3)
        ent["n_timed_windows"] = len(su)
    results.append(ent)
    print(f"[{famname}] {stem:26s} windows={len(rows)} "
          f"geomean fast/wb={ent.get('geomean_fast_wb', '-')} "
          f"(>= {args.min_us}us windows: {len(su)})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--families", default=None)
    ap.add_argument("--only", default=None)
    ap.add_argument("--max-entries", type=int, default=14)
    ap.add_argument("--seed", type=int, default=2)
    ap.add_argument("--evbudget", type=int, default=384)
    ap.add_argument("--hopmax", type=int, default=2000000)
    ap.add_argument("--reps", type=int, default=9)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--min-us", type=float, default=50.0,
                    help="geomean only counts windows with fast >= this")
    ap.add_argument("--input-roots", default=None)
    args = ap.parse_args()
    args.only = set(args.only.split(",")) if args.only else None
    fams = set(args.families.split(",")) if args.families else None
    args.roots = (args.input_roots.split(";") if args.input_roots
                  else GW.DEFAULT_INPUT_ROOTS)
    if not D.CL_EXE:
        raise SystemExit("cl not found; run from a vcvars64 shell")
    os.makedirs(WORK, exist_ok=True)
    manifest = json.load(open(GW.MANIFEST))
    results = []
    for famname, fam in manifest["families"].items():
        if famname in GW.SKIP_FAMILIES:
            continue
        if fams and famname not in fams:
            continue
        for plc in fam.get("images", fam.get("placements", [])):
            process(famname, fam, plc, args, results)
            D.write_report(os.path.join(WORK, "report.json"), results)
    print(f"\nreport -> {os.path.join(WORK, 'report.json')}")


if __name__ == "__main__":
    main()
