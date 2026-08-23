#!/usr/bin/env python3
"""WB differential runner: per-instruction DIAG twin vs the WB whole-block
twin (WB TU + renamed FAST TU linked together, exactly as production links
them).

Methodology mirrors tools/spu_region_diff.py (the s50 runner), whose
MEASURED finding stands for WB too: dynamic every-pc sweeps of
region-shaped twins are not executable (in-region loops on PRNG-seeded
data cannot be bounded externally), so deep dynamic equivalence uses the
matrix windows -- image entry, load base, manifest conformance entries,
sampled region starts, mid-region pcs, and region tails, each over
multiple seeds, comparing terminal kind, status/stop, the complete
ordered channel/MFC event stream, the full GPR file, and the LS hash.
Every-pc coverage is proven STATICALLY: the WB registration table must
map every instruction pc of the image -- compiled block leaders to the WB
function of the containing region, everything else to exactly the symbol
the FAST twin's own table maps that pc to.

Run from a vcvars64-imported shell (cl must be on PATH), or any shell
where VCToolsInstallDir is exported.

Usage:
    py -3 tools/spu_wb_diff.py --families job_a
    py -3 tools/spu_wb_diff.py                    # everything generated
"""

import argparse
import json
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
import spu_disasm                    # noqa: E402
from spu_wb_lifter import parse_fast_table   # noqa: E402

WB = os.path.join(ROOT, "yakuza", "generated", "wb")
FAST = os.path.join(ROOT, "yakuza", "generated", "fast")
WORK = os.path.join(ROOT, "scratch", "wbdiff")
HARNESS = os.path.join(TOOLS, "spu_diff_harness.c")

WB_EXTRA = ["/arch:AVX2", "/DYZ_SPU_SIMD_LS128=1", "/DYZ_SPU_SIMD_SHUFB=1"]


def cl_run(args_list, cwd=ROOT):
    r = subprocess.run([D.CL_EXE] + args_list, capture_output=True,
                       text=True, cwd=cwd)
    return r


def compile_wb_twin(stem, register, outdir):
    """Three-object link: diff harness (baseline, WB twin header), the FAST
    TU with its register symbol renamed (production shape), and the WB TU
    compiled /arch:AVX2. Returns (exe, seconds, err_log_or_obj_size)."""
    exe = os.path.join(outdir, f"{stem}.wb.exe")
    wb_c = os.path.join(WB, f"{stem}_wb.c")
    fast_c = os.path.join(FAST, f"{stem}_fast.c")
    if (os.path.exists(exe)
            and os.path.getmtime(exe) > os.path.getmtime(wb_c)
            and os.path.getmtime(exe) > os.path.getmtime(HARNESS)):
        return exe, 0.0, None
    objdir = os.path.join(outdir, "obj_wb")
    os.makedirs(objdir, exist_ok=True)
    t0 = time.time()
    inc = ["/I", os.path.join(ROOT, "include"),
           "/I", os.path.join(ROOT, "runtime", "spu")]
    steps = [
        (D.CLFLAGS + inc + ["/I", WB,
                            f"/DTWIN_HEADER={stem}_wb.h",
                            f"/DREGISTER_FN={register}",
                            "/c", HARNESS, f"/Fo{objdir}\\harness.obj"]),
        (D.CLFLAGS + inc + ["/I", FAST,
                            f"/D{register}={register}_fastbase",
                            "/c", fast_c, f"/Fo{objdir}\\fast_rn.obj"]),
        (D.CLFLAGS + WB_EXTRA + inc + ["/I", WB,
                                       "/c", wb_c, f"/Fo{objdir}\\wb.obj"]),
    ]
    for s in steps:
        r = cl_run(s)
        if r.returncode != 0:
            log = os.path.join(outdir, f"{stem}.wb.compile.log")
            with open(log, "w") as f:
                f.write(" ".join(s) + "\n" + r.stdout + "\n" + r.stderr)
            return None, time.time() - t0, log
    r = cl_run(["/nologo", "/MT", f"{objdir}\\harness.obj",
                f"{objdir}\\fast_rn.obj", f"{objdir}\\wb.obj",
                f"/Fe:{exe}"])
    dt = time.time() - t0
    if r.returncode != 0:
        log = os.path.join(outdir, f"{stem}.wb.link.log")
        with open(log, "w") as f:
            f.write(r.stdout + "\n" + r.stderr)
        return None, dt, log
    osz = os.path.getsize(os.path.join(objdir, "wb.obj"))
    return exe, dt, osz


def static_table_check(stem, insn_pcs, fast_c):
    """Prove the WB registration table covers every instruction pc and
    routes non-WB pcs to exactly the FAST twin's own mapping."""
    wb_c = os.path.join(WB, f"{stem}_wb.c")
    text = open(wb_c, errors="replace").read()
    table = {}
    tbl_re = re.compile(r"^\s*\{ 0x([0-9A-F]{8})u, (\w+) \},", re.M)
    for m in tbl_re.finditer(text):
        table[int(m.group(1), 16)] = m.group(2)
    fast_table, _reg = parse_fast_table(fast_c)
    missing = sorted(set(insn_pcs) - set(table))
    wrong_fast = []
    n_wb = 0
    for pc, sym in table.items():
        if sym.startswith("spu_wb"):
            n_wb += 1
        else:
            if fast_table.get(pc) != sym:
                wrong_fast.append(pc)
    # WB leader cases must appear in the WB function entry switches
    case_re = re.compile(r"case 0x([0-9A-F]{8})u: goto b_([0-9A-F]{8});")
    cases = {int(m.group(1), 16) for m in case_re.finditer(text)
             if m.group(1) == m.group(2)}
    wb_pcs = {pc for pc, sym in table.items() if sym.startswith("spu_wb")}
    uncased = sorted(wb_pcs - cases)
    return {
        "pcs": len(insn_pcs),
        "registered": len(table),
        "wb_leader_pcs": n_wb,
        "missing": len(missing),
        "wrong_fast_route": len(wrong_fast),
        "wb_uncased": len(uncased),
        "first_bad": [hex(x) for x in (missing + wrong_fast + uncased)[:6]],
        "ok": not (missing or wrong_fast or uncased),
    }


def process(famname, fam, plc, args, results):
    stem = plc["stem"]
    if args.only and stem not in args.only:
        return
    wb_c = os.path.join(WB, f"{stem}_wb.c")
    fast_c = os.path.join(FAST, f"{stem}_fast.c")
    if not os.path.exists(wb_c):
        return
    diag_c = G.resolve_diag_path(stem, plc["diag"]) if plc.get("diag") else None
    if diag_c and not os.path.exists(diag_c):
        diag_c = None
    if diag_c:
        _prefix, register, diag_h = G.read_header_identity(diag_c)
        diag_h_dir = os.path.dirname(diag_h)
        diag_h_name = os.path.basename(diag_h)
    else:
        print(f"[{famname}] {stem}: no DIAG twin, skipped")
        return

    try:
        elf_path, base = GW.placement_input(famname, fam, plc, args.roots)
    except FileNotFoundError as e:
        print(f"[{famname}] {stem}: input missing ({e}), skipped")
        return
    code, base2, entry = G.parse_spu_elf(elf_path)
    assert base2 == base
    metrics = D.derive_metrics(fast_c, base, len(code), register)
    span = metrics["image_span"]

    outdir = os.path.join(WORK, stem)
    os.makedirs(outdir, exist_ok=True)
    codebin = os.path.join(outdir, "code.bin")
    with open(codebin, "wb") as f:
        f.write(code)

    exe_d, t_d, o_d = D.compile_twin(stem, diag_c, diag_h_dir, diag_h_name,
                                     register, outdir, "diag")
    exe_w, t_w, o_w = compile_wb_twin(stem, register, outdir)
    ent = {"family": famname, "stem": stem,
           "compile_diag_s": round(t_d, 1), "compile_wb_s": round(t_w, 1),
           "obj_wb": o_w if exe_w else None}
    if not exe_d or not exe_w:
        ent["verdict"] = "COMPILE-FAIL"
        ent["detail"] = f"diag={'ok' if exe_d else o_d} wb={'ok' if exe_w else o_w}"
        results.append(ent)
        print(f"[{famname}] {stem}: COMPILE FAIL ({ent['detail']})")
        return

    insns = spu_disasm.disassemble_spu(code, base)
    mnem_at = {i.addr: i.mnemonic for i in insns}

    # static every-pc registration proof
    st = static_table_check(stem, list(mnem_at), fast_c)
    ent["static_table"] = st

    required = [int(v, 0) if isinstance(v, str) else int(v)
                for v in plc.get("conformance_entries", [])]
    entries = D.pick_entries(entry, base, metrics, args.max_entries, required)
    n_match = n_mis = n_inc = n_skip = 0
    mismatches = []
    hop_pairs = []
    for e in entries:
        for seed in range(1, args.seeds + 1):
            od = os.path.join(outdir, f"e{e:05X}_s{seed}.diag.txt")
            ow = os.path.join(outdir, f"e{e:05X}_s{seed}.wb.txt")
            st_d = D.run_one(exe_d, codebin, base, e, seed, args.evbudget,
                             args.hopmax, od, args.timeout)
            st_w = D.run_one(exe_w, codebin, base, e, seed, args.evbudget,
                             args.hopmax, ow, args.timeout)
            if st_d or st_w:
                n_inc += 1
                continue
            da, dw = D.parse_out(od), D.parse_out(ow)
            v, detail = D.cmp_runs(da, dw)
            if v == "MATCH":
                n_match += 1
                if da["hops"] >= 64:
                    hop_pairs.append((da["hops"], dw["hops"]))
            elif v == "MISMATCH":
                n_mis += 1
                mismatches.append(f"e{e:05X}s{seed}: {detail}")
            elif v == "SKIP":
                n_skip += 1
            else:
                n_inc += 1
    ent.update(entries=len(entries), match=n_match, mismatch=n_mis,
               inconclusive=n_inc, skipped=n_skip,
               mismatch_detail=mismatches[:6])
    if hop_pairs:
        ratios = [d / w for d, w in hop_pairs if w > 0]
        if ratios:
            ent["hop_ratio_mean"] = round(sum(ratios) / len(ratios), 2)
    n_static_bad = 0 if st["ok"] else 1
    ent["verdict"] = ("MISMATCH" if (n_mis or n_static_bad) else
                      ("MATCH" if n_match else "INCONCLUSIVE"))
    results.append(ent)
    print(f"[{famname}] {stem:26s} {ent['verdict']:12s} match={n_match} "
          f"mis={n_mis} inc={n_inc} static={'ok' if st['ok'] else st['first_bad']} "
          f"(cl {t_d:.0f}s/{t_w:.0f}s)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--families", default=None)
    ap.add_argument("--only", default=None)
    ap.add_argument("--max-entries", type=int, default=14)
    ap.add_argument("--seeds", type=int, default=2)
    ap.add_argument("--evbudget", type=int, default=384)
    ap.add_argument("--hopmax", type=int, default=2000000)
    ap.add_argument("--timeout", type=int, default=45)
    ap.add_argument("--input-roots", default=None)
    args = ap.parse_args()
    args.only = set(args.only.split(",")) if args.only else None
    fams = set(args.families.split(",")) if args.families else None
    args.roots = (args.input_roots.split(";") if args.input_roots
                  else GW.DEFAULT_INPUT_ROOTS)

    if not D.CL_EXE:
        raise SystemExit("cl not found; run from a vcvars64-initialized shell")
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

    n_mis = sum(1 for r in results if r.get("verdict") == "MISMATCH")
    n_cf = sum(1 for r in results if r.get("verdict") == "COMPILE-FAIL")
    print(f"\n{len(results)} placement(s); {n_mis} MISMATCH, {n_cf} COMPILE-FAIL; "
          f"report -> {os.path.join(WORK, 'report.json')}")
    sys.exit(1 if (n_mis or n_cf) else 0)


if __name__ == "__main__":
    main()
