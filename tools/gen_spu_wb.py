#!/usr/bin/env python3
"""WB (whole-block SPU backend) batch generation driver.

For every placement in tools/spu_region_manifest.json (minus the
experimental sony family), invokes tools/spu_wb_lifter.py to emit
yakuza/generated/wb/<stem>_wb.c/.h beside the tracked FAST twins, which
remain the fallback surface. Mirrors gen_spu_regions.py's input
materialization; identity (func prefix / register symbol) comes from the
shipped DIAG twin headers (ground truth), the manifest otherwise.

Input artifacts (ELFs, raw slices, EBOOT) are untracked local oracles that
may live in the main checkout or a sibling worktree; --input-roots lists
the search order (read-only).

Usage:
    py -3 tools/gen_spu_wb.py                    # everything
    py -3 tools/gen_spu_wb.py --families job_a
    py -3 tools/gen_spu_wb.py --only job_bin_a_e400
"""

import argparse
import json
import os
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import gen_spu_regions as G      # noqa: E402
from wrap_spu_elf import wrap    # noqa: E402

MANIFEST = os.path.join(TOOLS, "spu_region_manifest.json")
OUT_WB = os.path.join(ROOT, "yakuza", "generated", "wb")
FAST = os.path.join(ROOT, "yakuza", "generated", "fast")
WORK = os.path.join(ROOT, "scratch", "wbgen")

# lane selection (wb = whole-block stage 1, wj = whole-job stage 2)
LANES = {
    "wb": {"tool": "spu_wb_lifter.py", "outdir": "wb", "suffix": "_wb",
           "work": "wbgen"},
    "wj": {"tool": "spu_wj_lifter.py", "outdir": "wj", "suffix": "_wj",
           "work": "wjgen"},
}

DEFAULT_INPUT_ROOTS = [
    ROOT,
]

# Blocks containing these pcs must fall back to the FAST twin: the
# YZ_TAGREAD_REPAIR read hook qualifies by exact ctx->pc values that only the
# DIAG/FAST twins materialize (spu_channels.c yz_tagread_repair_read).
REPAIR_PCS = {"gs_task": [0x65E4, 0x6550, 0x6568]}

SKIP_FAMILIES = {"sony"}   # experimental LLE oracle lane, not a WB target


def resolve_input(relpath, roots):
    for r in roots:
        p = os.path.join(r, relpath)
        if os.path.exists(p):
            return p
    return None


def family_code(fam, roots):
    if fam.get("raw"):
        p = resolve_input(fam["raw"], roots)
        if not p:
            raise FileNotFoundError(fam["raw"])
        code = open(p, "rb").read()
    elif fam.get("ref_elf"):
        p = resolve_input(fam["ref_elf"], roots)
        if not p:
            raise FileNotFoundError(fam["ref_elf"])
        code, _va, _e = G.parse_spu_elf(p)
    else:
        p = resolve_input(fam["eboot"], roots)
        if not p:
            raise FileNotFoundError(fam["eboot"])
        ea = int(fam["eboot_ea"], 0)
        size = int(fam["eboot_size"], 0)
        code = G.eboot_slice(p, ea, size)
    if fam.get("ref_elf"):
        p = resolve_input(fam["ref_elf"], roots)
        _c, ref_base, ref_entry = G.parse_spu_elf(p)
    else:
        ref_base = ref_entry = None
    return code, ref_base, ref_entry


def placement_input(famname, fam, plc, roots):
    """Return (elf_path, base) for the placement, materializing a wrapped
    ELF for raw/EBOOT families exactly like gen_spu_regions."""
    if plc.get("elf"):
        p = resolve_input(plc["elf"], roots)
        if not p:
            raise FileNotFoundError(plc["elf"])
        _c, base, _e = G.parse_spu_elf(p)
        return p, base
    if fam["kind"] == "elf":
        raise SystemExit(f"{plc['stem']}: elf-family placement without an elf")
    code, ref_base, ref_entry = family_code(fam, roots)
    base = int(plc["base"], 0)
    delta = (ref_entry - ref_base) if (fam.get("ref_elf") and ref_entry is not None) else 0
    elf_bytes = wrap(code, base=base, entry=base + delta)
    os.makedirs(os.path.join(WORK, "inputs"), exist_ok=True)
    p = os.path.join(WORK, "inputs", f"{plc['stem']}.elf")
    with open(p, "wb") as f:
        f.write(elf_bytes)
    return p, base


def identity(plc):
    if plc.get("diag"):
        diag_c = G.resolve_diag_path(plc["stem"], plc["diag"])
        if os.path.exists(diag_c):
            prefix, register, _h = G.read_header_identity(diag_c)
            return prefix, register
    return plc["prefix"], plc["register"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--families", default=None)
    ap.add_argument("--only", default=None)
    ap.add_argument("--input-roots", default=None,
                    help="semicolon-separated search roots for untracked inputs")
    ap.add_argument("--region-cap", type=int, default=256)
    ap.add_argument("--lane", choices=sorted(LANES), default="wb")
    args = ap.parse_args()
    lane = LANES[args.lane]
    global OUT_WB, WORK
    OUT_WB = os.path.join(ROOT, "yakuza", "generated", lane["outdir"])
    WORK = os.path.join(ROOT, "scratch", lane["work"])
    fams = set(args.families.split(",")) if args.families else None
    only = set(args.only.split(",")) if args.only else None
    roots = (args.input_roots.split(";") if args.input_roots
             else DEFAULT_INPUT_ROOTS)

    os.makedirs(OUT_WB, exist_ok=True)
    os.makedirs(os.path.join(WORK, "metrics"), exist_ok=True)
    os.makedirs(os.path.join(WORK, "logs"), exist_ok=True)
    manifest = json.load(open(MANIFEST))

    report = []
    n_fail = 0
    for famname, fam in manifest["families"].items():
        if famname in SKIP_FAMILIES:
            continue
        if fams and famname not in fams:
            continue
        for plc in fam.get("images", fam.get("placements", [])):
            stem = plc["stem"]
            if only and stem not in only:
                continue
            fast_c = os.path.join(FAST, f"{stem}_fast.c")
            if not os.path.exists(fast_c):
                print(f"[{famname}] {stem:26s} SKIP (no FAST twin)")
                continue
            try:
                elf_path, base = placement_input(famname, fam, plc, roots)
            except FileNotFoundError as e:
                print(f"[{famname}] {stem:26s} SKIP (input missing: {e})")
                report.append({"stem": stem, "family": famname,
                               "rc": None, "skip": f"input missing: {e}"})
                continue
            prefix, register = identity(plc)
            metrics_p = os.path.join(WORK, "metrics", f"{stem}.json")
            argv = [sys.executable, os.path.join(TOOLS, lane["tool"]),
                    "--auto-functions", elf_path,
                    "--func-prefix", prefix,
                    "--register-name", register,
                    "--fast-source", fast_c,
                    "--output", OUT_WB,
                    "--source-name", f"{stem}{lane['suffix']}.c",
                    "--header-name", f"{stem}{lane['suffix']}.h",
                    "--region-cap", str(args.region_cap),
                    "--metrics-json", metrics_p]
            rp = REPAIR_PCS.get(stem)
            if rp:
                argv += ["--repair-pcs", ",".join(hex(x) for x in rp)]
            log = os.path.join(WORK, "logs", f"{stem}.log")
            with open(log, "w") as lf:
                r = subprocess.run(argv, stdout=lf, stderr=subprocess.STDOUT,
                                   cwd=ROOT)
            ent = {"stem": stem, "family": famname, "rc": r.returncode,
                   "prefix": prefix, "register": register, "base": hex(base)}
            if r.returncode == 0 and os.path.exists(metrics_p):
                m = json.load(open(metrics_p))
                for k in ("n_insns", "n_regions", "n_blocks",
                          "n_blocks_compiled", "n_blocks_fallback",
                          "wb_leader_pcs", "kernel_native", "kernel_wrapped",
                          "const_folded", "cse_hits", "dce_dropped",
                          "ls_load_forwarded", "gpr_loads", "gpr_stores"):
                    ent[k] = m.get(k)
            else:
                n_fail += 1
            report.append(ent)
            flag = "" if r.returncode == 0 else "  <-- WB LIFT FAILED"
            print(f"[{famname}] {stem:26s} rc={r.returncode} "
                  f"blocks={ent.get('n_blocks_compiled','-')}/"
                  f"{ent.get('n_blocks','-')}{flag}")
            with open(os.path.join(WORK, "report.json"), "w") as f:
                json.dump(report, f, indent=1)

    print(f"\n{len(report)} placement(s), {n_fail} failure(s); "
          f"report -> {os.path.join(WORK, 'report.json')}")
    sys.exit(1 if n_fail else 0)


if __name__ == "__main__":
    main()
