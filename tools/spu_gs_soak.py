#!/usr/bin/env python3
"""gs_task WJ soak: deep differential windows at realistic data widths.

The matrix runs bound every window to a 384-event budget and mask initial
GPR preferred words to 16 bits, which keeps gs_task's deep journal/FIFO
paths shallow. This soak compiles the gs_task DIAG and WJ twins once, then
sweeps a much wider window set -- more entries, more seeds, a 4096-event
budget -- across preferred-word width tiers via the harness's
SPU_DIFF_PREFMASK override (m16 = the documented default as control,
m18/m22 = realistic loop-trip and address widths). Windows whose reference
cannot finish inside the budgets are counted UNBOUNDED, everything else is
verdicted architecturally.

Usage (vcvars64 shell):
    py -3 tools/spu_gs_soak.py [--stem gs_task] [--seeds 3]
        [--max-entries 48] [--evbudget 4096] [--hopmax 60000000]
        [--timeout 180] [--masks m16,m18,m22]
Writes scratch/gssoak/<stem>/soak_report.json.
"""

import argparse
import json
import os
import sys
import time
import types

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import gen_spu_regions as G          # noqa: E402
import spu_region_diff as D          # noqa: E402
import gen_spu_wb as GW              # noqa: E402
import spu_wb_diff as WD             # noqa: E402

MASKS = {"m16": None, "m18": "0x3FFFF", "m20": "0xFFFFF", "m22": "0x3FFFFF"}


def find_placement(manifest, stem):
    for famname, fam in manifest["families"].items():
        for plc in fam.get("images", fam.get("placements", [])):
            if plc["stem"] == stem:
                return famname, fam, plc
    raise SystemExit(f"stem {stem} not in manifest")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stem", default="gs_task")
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--max-entries", type=int, default=48)
    ap.add_argument("--evbudget", type=int, default=4096)
    ap.add_argument("--hopmax", type=int, default=60000000)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--masks", default="m16,m18,m22")
    ap.add_argument("--input-roots", default=None)
    args = ap.parse_args()
    if not D.CL_EXE:
        raise SystemExit("cl not found; run from a vcvars64 shell")
    roots = (args.input_roots.split(";") if args.input_roots
             else GW.DEFAULT_INPUT_ROOTS)
    tags = args.masks.split(",")
    for t in tags:
        if t not in MASKS:
            raise SystemExit(f"unknown mask tier {t}")

    # WJ lane globals for compile_wb_twin
    WD.WB = os.path.join(ROOT, "yakuza", "generated", "wj")
    WD.SUFFIX = "_wj"
    WD.SYM_PREFIX = "spu_wj"

    manifest = json.load(open(GW.MANIFEST))
    famname, fam, plc = find_placement(manifest, args.stem)
    stem = plc["stem"]
    outdir = os.path.join(ROOT, "scratch", "gssoak", stem)
    os.makedirs(outdir, exist_ok=True)

    diag_c = G.resolve_diag_path(stem, plc["diag"])
    _prefix, register, diag_h = G.read_header_identity(diag_c)
    elf_path, base = GW.placement_input(famname, fam, plc, roots)
    code, base2, entry = G.parse_spu_elf(elf_path)
    assert base2 == base
    fast_c = os.path.join(ROOT, "yakuza", "generated", "fast",
                          f"{stem}_fast.c")
    metrics = D.derive_metrics(fast_c, base, len(code), register)
    codebin = os.path.join(outdir, "code.bin")
    with open(codebin, "wb") as f:
        f.write(code)

    print(f"[{stem}] compiling twins into {outdir} ...", flush=True)
    exe_d, t_d, o_d = D.compile_twin(stem, diag_c, os.path.dirname(diag_h),
                                     os.path.basename(diag_h), register,
                                     outdir, "diag")
    exe_w, t_w, o_w = WD.compile_wb_twin(stem, register, outdir)
    if not exe_d or not exe_w:
        raise SystemExit(f"compile failed: diag={exe_d or o_d} "
                         f"wj={exe_w or o_w}")
    print(f"[{stem}] twins ready (diag {t_d:.0f}s, wj {t_w:.0f}s)", flush=True)

    required = [int(v, 0) if isinstance(v, str) else int(v)
                for v in plc.get("conformance_entries", [])]
    entries = D.pick_entries(entry, base, metrics, args.max_entries, required)
    print(f"[{stem}] {len(entries)} entries x {args.seeds} seeds x "
          f"{len(tags)} mask tier(s)", flush=True)

    report = {"stem": stem, "evbudget": args.evbudget, "hopmax": args.hopmax,
              "timeout": args.timeout, "tiers": {}}
    t_start = time.time()
    for tag in tags:
        if MASKS[tag]:
            os.environ["SPU_DIFF_PREFMASK"] = MASKS[tag]
        else:
            os.environ.pop("SPU_DIFF_PREFMASK", None)
        n_match = n_mis = n_unb = 0
        mismatches = []
        for e in entries:
            for seed in range(1, args.seeds + 1):
                od = os.path.join(outdir, f"e{e:05X}_s{seed}_{tag}.diag.txt")
                ow = os.path.join(outdir, f"e{e:05X}_s{seed}_{tag}.wj.txt")
                st_d = D.run_one(exe_d, codebin, base, e, seed, args.evbudget,
                                 args.hopmax, od, args.timeout)
                st_w = D.run_one(exe_w, codebin, base, e, seed, args.evbudget,
                                 args.hopmax, ow, args.timeout)
                da = D.parse_out(od) if not st_d else None
                dw = D.parse_out(ow) if not st_w else None
                if (da is None or dw is None
                        or da.get("term") in ("HOPMAX", "TIMEOUT")
                        or dw.get("term") in ("HOPMAX", "TIMEOUT")):
                    n_unb += 1
                    continue
                v, detail = D.cmp_runs(da, dw)
                if v == "MATCH":
                    n_match += 1
                elif v == "MISMATCH":
                    n_mis += 1
                    mismatches.append(f"e{e:05X}s{seed}: {detail}")
                    print(f"[{tag}] e{e:05X} s{seed}: MISMATCH {detail}",
                          flush=True)
                else:
                    n_unb += 1
        report["tiers"][tag] = {
            "mask": MASKS[tag] or "default(0xFFFF)",
            "match": n_match, "mismatch": n_mis, "unbounded": n_unb,
            "mismatch_detail": mismatches[:12],
        }
        print(f"[{tag}] done: {n_match} MATCH, {n_mis} MISMATCH, "
              f"{n_unb} unbounded ({time.time() - t_start:.0f}s elapsed)",
              flush=True)

    rep = os.path.join(outdir, "soak_report.json")
    with open(rep, "w") as f:
        json.dump(report, f, indent=1)
    any_mis = any(t["mismatch"] for t in report["tiers"].values())
    print(f"\nsoak report -> {rep}")
    sys.exit(1 if any_mis else 0)


if __name__ == "__main__":
    main()
