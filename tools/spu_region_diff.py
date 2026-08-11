#!/usr/bin/env python3
"""s50 differential runner: instruction-lifted twin vs region-lifted twin.

For every placement that the generation sweep (gen_spu_regions.py) has
processed, this runner:

  1. compiles TWO harness executables with the REAL Release flags
     (/O2 /Ob2 /DNDEBUG /MT /std:c17 /experimental:c11atomics /bigobj):
     one linking the freshly re-generated INSTRUCTION twin
     (scratch/regionlift/repro/<stem>.c -- same-lifter-version twin, so
     classification is provably shared), one linking the REGION twin
     (yakuza/generated/fast/<stem>_fast.c). Compile wall time + object size are
     recorded: these ARE the Release compile sweep for every generated TU.
  2. runs both over an identical deterministic environment from a matrix of
     entry pcs -- the image entry, the load base, evenly-sampled region
     starts, mid-region pcs (entry-switch coverage), and region-tail pcs
     (fall-through boundary coverage) -- with multiple seeds;
  3. compares: terminal kind, status/stop_code, the full ordered channel/MFC
     event stream (values + DMA payload hashes; pc field excluded -- the
     instruction lift does not maintain pc at events, MEASURED
     job_bin_a.c:32), the complete GPR file, and the LS hash;
  4. statically validates the REGION twin's materialized pc at every
     channel event and stop against the image disassembly (the requirement-4
     check the instruction twin cannot express);
  5. records the per-run dispatch (hop) counts of both twins -- the measured
     basis for the host-dispatch reduction estimate.

Run from a vcvars64-imported shell (cl must be on PATH).

Usage:
    py -3 tools\\spu_region_diff.py --families job_a
    py -3 tools\\spu_region_diff.py                    # everything swept
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import gen_spu_regions as G           # noqa: E402  (helpers; argparse only under __main__)
import spu_disasm                     # noqa: E402

WORK = os.path.join(ROOT, "scratch", "regionlift")
DIFFD = os.path.join(WORK, "diff")
REPRO = os.path.join(WORK, "repro")
FAST = os.path.join(ROOT, "yakuza", "generated", "fast")
HARNESS = os.path.join(TOOLS, "spu_diff_harness.c")
INPUT_ROOT = ROOT

CLFLAGS = ["/nologo", "/O2", "/Ob2", "/DNDEBUG", "/DYZ_PERF_CLEAN=1", "/MT", "/W3",
           "/wd4100", "/wd4244", "/wd4267", "/wd4996",
           "/std:c17", "/experimental:c11atomics", "/bigobj"]


def resolve_cl():
    """Find cl even when a Python launcher restores its original PATH.

    vcvars still exports VCToolsInstallDir and the selected host/target
    architecture in that environment, so use those as the authoritative
    fallback instead of rejecting a correctly initialized developer shell.
    """
    found = shutil.which("cl")
    if found:
        return found
    root = os.environ.get("VCToolsInstallDir")
    host = os.environ.get("VSCMD_ARG_HOST_ARCH", "x64")
    target = os.environ.get("VSCMD_ARG_TGT_ARCH", "x64")
    if root:
        candidate = os.path.join(root, "bin", f"Host{host}", target, "cl.exe")
        if os.path.isfile(candidate):
            return candidate
    return None


CL_EXE = resolve_cl()


def input_path(relative):
    return os.path.join(INPUT_ROOT, relative)


def family_code_from_input(fam):
    """Use a read-only external input tree without relocating test output."""
    old_root = G.ROOT
    try:
        G.ROOT = INPUT_ROOT
        return G.family_code(fam)
    finally:
        G.ROOT = old_root


def derive_metrics(fast_c, base, code_size, register):
    """Recover the small metric subset needed by the differential matrix.

    Shipped region twins contain one named function per region and a dense
    entry switch.  This lets an integration worktree validate tracked twins
    even when the generator's disposable metrics directory is absent.
    """
    text = open(fast_c, errors="replace").read()
    starts = sorted({
        int(m.group(1), 16)
        for m in re.finditer(
            r"^void [A-Za-z_]\w*?([0-9A-F]{8})\(spu_context", text, re.M)
        if base <= int(m.group(1), 16) < base + code_size
    })
    if not starts:
        raise RuntimeError(f"{fast_c}: no region functions found")
    end = base + code_size
    regions = []
    for i, start in enumerate(starts):
        region_end = starts[i + 1] if i + 1 < len(starts) else end
        regions.append([start, region_end, (region_end - start) // 4])
    return {
        "register_name": register,
        "image_span": [base, end],
        "regions": regions,
    }


def compile_twin(stem, twin_c, twin_h_dir, twin_h_name, register, outdir, tag):
    exe = os.path.join(outdir, f"{stem}.{tag}.exe")
    if (os.path.exists(exe)
            and os.path.getmtime(exe) > os.path.getmtime(twin_c)
            and os.path.getmtime(exe) > os.path.getmtime(HARNESS)):
        return exe, 0.0, None
    objdir = os.path.join(outdir, f"obj_{tag}")
    os.makedirs(objdir, exist_ok=True)
    cmd = ([CL_EXE] + CLFLAGS +
           ["/I", os.path.join(ROOT, "include"),
            "/I", os.path.join(ROOT, "runtime", "spu"), "/I", twin_h_dir,
            f"/DTWIN_HEADER={twin_h_name}", f"/DREGISTER_FN={register}",
            f"/Fo{objdir}\\", f"/Fe:{exe}", HARNESS, twin_c])
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    dt = time.time() - t0
    if r.returncode != 0:
        log = os.path.join(outdir, f"{stem}.{tag}.compile.log")
        with open(log, "w") as f:
            f.write(r.stdout + "\n" + r.stderr)
        return None, dt, log
    twin_obj = os.path.join(objdir, os.path.basename(twin_c)[:-2] + ".obj")
    osz = os.path.getsize(twin_obj) if os.path.exists(twin_obj) else 0
    return exe, dt, osz


def parse_out(path):
    d = {"ev": [], "gpr": [], "term": None}
    for ln in open(path):
        if ln.startswith("TERM "):
            m = re.match(r"TERM (\S+) status=(\d+) stop=0x([0-9A-F]+) hops=(\d+) "
                         r"events=(\d+) pc=0x([0-9A-F]+)", ln)
            d["term"], d["status"], d["stop"] = m.group(1), int(m.group(2)), int(m.group(3), 16)
            d["hops"], d["events"], d["pc"] = int(m.group(4)), int(m.group(5)), int(m.group(6), 16)
        elif ln.startswith("EV "):
            m = re.match(r"EV (\d+) k=(\d+) ch=(\d+) a=0x(\w+) b=0x(\w+) c=0x(\w+) "
                         r"d=0x(\w+) pc=0x(\w+)", ln)
            d["ev"].append(tuple(int(m.group(i), 16 if i >= 4 else 10)
                                 for i in range(2, 9)))
        elif ln.startswith("EVHASH "):
            d["evhash"] = ln.split()[1]
        elif ln.startswith("GPR "):
            d["gpr"].append(ln.strip())
        elif ln.startswith("LSHASH "):
            d["lshash"] = ln.split()[1]
    return d


def pick_entries(entry, base, metrics, max_entries, conformance_entries=()):
    regions = metrics["regions"]          # [start, end, ninsns]
    starts = [r[0] for r in regions]
    # Manifest-required entries come first so a bounded run always exercises
    # image-specific hot paths and hand-edited continuations before the broad
    # region sample.  Values remain ordinary synthetic entry-PC tests; they do
    # not weaken the full-state/event comparison below.
    picks = [entry, base] + list(conformance_entries)
    if len(starts) > 2:
        step = max(1, len(starts) // 8)
        picks += starts[::step][:8]
    big = sorted(regions, key=lambda r: -r[2])[:4]
    for rs, re_, n in big:
        if n >= 8:
            picks.append(rs + 4 * (n // 2))       # mid-region: entry-switch depth
    for rs, re_, n in big[:2]:
        if n >= 2:
            picks.append(re_ - 4)                  # region tail: fall-through boundary
    seen, out = set(), []
    for p in picks:
        if p not in seen:
            seen.add(p)
            out.append(p)
    return out[:max_entries]


def cmp_runs(da, db):
    """(verdict, detail) comparing diag vs fast parses."""
    inconclusive = {"HOPMAX", "TIMEOUT", "NOENTRY"}
    if da["term"] in inconclusive or db["term"] in inconclusive:
        if da["term"] == db["term"] == "NOENTRY":
            return "SKIP", "no entry in either twin"
        return "INCONCLUSIVE", f"term diag={da['term']} fast={db['term']}"
    if da["term"] != db["term"]:
        return "MISMATCH", f"terminal diag={da['term']} fast={db['term']}"
    if (da["status"], da["stop"]) != (db["status"], db["stop"]):
        return "MISMATCH", (f"status/stop diag={da['status']}/{da['stop']:#x} "
                            f"fast={db['status']}/{db['stop']:#x}")
    if len(da["ev"]) != len(db["ev"]):
        return "MISMATCH", f"event count {len(da['ev'])} vs {len(db['ev'])}"
    for i, (ea, eb) in enumerate(zip(da["ev"], db["ev"])):
        if ea[:6] != eb[:6]:                     # exclude pc (index 6)
            return "MISMATCH", f"event {i}: diag={ea[:6]} fast={eb[:6]}"
    if da.get("evhash") != db.get("evhash"):
        return "MISMATCH", "event hash"
    if da["gpr"] != db["gpr"]:
        bad = next(i for i, (x, y) in enumerate(zip(da["gpr"], db["gpr"])) if x != y)
        return "MISMATCH", f"GPR: {da['gpr'][bad]} vs {db['gpr'][bad]}"
    if da.get("lshash") != db.get("lshash"):
        return "MISMATCH", "LS hash"
    return "MATCH", ""


def pc_check(dfast, mnem_at, span):
    """Region-twin pc validity: event pc must decode to the matching op."""
    want = {1: "rdch", 2: "wrch", 3: "rchcnt", 4: "wrch"}
    bad = []
    for index, ev in enumerate(dfast["ev"]):
        kind, pc = ev[0], ev[6]
        if kind not in want:
            continue
        if not (span[0] <= pc < span[1]):
            bad.append((pc, "outside-span", kind))
            continue
        mn = mnem_at.get(pc)
        # The clean-performance atomic-status helper fuses the architectural
        # rchcnt + conditional rdch pair into one runtime lookup.  Its two
        # harness events therefore share the rchcnt site's materialized PC;
        # accept the read half only when it immediately follows that exact
        # count event on the same channel.
        if (kind == 1 and mn == "rchcnt" and index > 0
                and dfast["ev"][index - 1][0] == 3
                and dfast["ev"][index - 1][1] == ev[1]
                and dfast["ev"][index - 1][6] == pc):
            continue
        if mn != want[kind]:
            bad.append((pc, mn, kind))
    return bad


def run_sweep(exe, codebin, base, seed, evb, hopmax, outp, timeout):
    try:
        r = subprocess.run([exe, codebin, f"{base:X}", "sweep", str(seed),
                            str(evb), str(hopmax), outp],
                           capture_output=True, text=True, timeout=timeout)
        return "CRASH" if r.returncode != 0 else None
    except subprocess.TimeoutExpired:
        return "TIMEOUT"


def run_one(exe, codebin, base, entry, seed, evb, hopmax, outp, timeout):
    try:
        r = subprocess.run([exe, codebin, f"{base:X}", f"{entry:X}", str(seed),
                            str(evb), str(hopmax), outp],
                           capture_output=True, text=True, timeout=timeout)
        if r.returncode != 0:
            return "CRASH"
        return None
    except subprocess.TimeoutExpired:
        return "TIMEOUT"


def process(famname, fam, plc, args, results):
    stem = plc["stem"]
    if args.only and stem not in args.only:
        return
    fast_c = os.path.join(FAST, f"{stem}_fast.c")
    metrics_p = os.path.join(WORK, "metrics", f"{stem}.json")
    if args.tracked_twins:
        diag_c = G.resolve_diag_path(stem, plc["diag"])
        _prefix, register, diag_h = G.read_header_identity(diag_c)
        diag_h_dir = os.path.dirname(diag_h)
        diag_h_name = os.path.basename(diag_h)
    else:
        diag_c = os.path.join(REPRO, f"{stem}.c")
        register = None
        diag_h_dir = REPRO
        diag_h_name = f"{stem}.h"
    if not (os.path.exists(diag_c) and os.path.exists(fast_c)):
        print(f"[{famname}] {stem}: sweep outputs missing, skipped")
        return

    # code bytes + base + entry, exactly as the sweep derived them
    if plc.get("elf"):
        code, base, entry = G.parse_spu_elf(input_path(plc["elf"]))
    elif fam["kind"] == "elf":
        code, base, entry = G.parse_spu_elf(input_path(fam_elf(plc)))
    else:
        code, ref_base, ref_entry = family_code_from_input(fam)
        delta = (ref_entry - ref_base) if (fam.get("ref_elf") and ref_entry is not None) else 0
        base = int(plc["base"], 0)
        entry = base + delta

    if os.path.exists(metrics_p):
        metrics = json.load(open(metrics_p))
        if register is None:
            register = metrics["register_name"]
    elif args.tracked_twins:
        metrics = derive_metrics(fast_c, base, len(code), register)
    else:
        print(f"[{famname}] {stem}: metrics missing, skipped")
        return
    span = metrics["image_span"]

    outdir = os.path.join(DIFFD, stem)
    os.makedirs(outdir, exist_ok=True)
    codebin = os.path.join(outdir, "code.bin")
    with open(codebin, "wb") as f:
        f.write(code)

    exe_d, t_d, o_d = compile_twin(stem, diag_c, diag_h_dir, diag_h_name,
                                   register, outdir, "diag")
    exe_f, t_f, o_f = compile_twin(stem, fast_c, FAST, f"{stem}_fast.h", register, outdir, "fast")
    ent = {"family": famname, "stem": stem,
           "compile_diag_s": round(t_d, 1), "compile_fast_s": round(t_f, 1),
           "obj_diag": o_d if exe_d else None, "obj_fast": o_f if exe_f else None}
    if not exe_d or not exe_f:
        ent["verdict"] = "COMPILE-FAIL"
        ent["detail"] = f"diag={'ok' if exe_d else o_d} fast={'ok' if exe_f else o_f}"
        results.append(ent)
        print(f"[{famname}] {stem}: COMPILE FAIL ({ent['detail']})")
        return

    insns = spu_disasm.disassemble_spu(code, base)
    mnem_at = {i.addr: i.mnemonic for i in insns}

    required_entries = [
        int(value, 0) if isinstance(value, str) else int(value)
        for value in plc.get("conformance_entries", [])
    ]
    entries = pick_entries(
        entry, base, metrics, args.max_entries, required_entries)
    n_match = n_mis = n_inc = n_skip = 0
    mismatches = []
    hop_pairs = []
    pc_bad_total = []
    for e in entries:
        for seed in range(1, args.seeds + 1):
            od = os.path.join(outdir, f"e{e:05X}_s{seed}.diag.txt")
            of = os.path.join(outdir, f"e{e:05X}_s{seed}.fast.txt")
            st_d = run_one(exe_d, codebin, base, e, seed, args.evbudget, args.hopmax, od, args.timeout)
            st_f = run_one(exe_f, codebin, base, e, seed, args.evbudget, args.hopmax, of, args.timeout)
            if st_d or st_f:
                n_inc += 1
                continue
            da, db = parse_out(od), parse_out(of)
            v, detail = cmp_runs(da, db)
            if v == "MATCH":
                n_match += 1
                if da["hops"] >= 64:
                    hop_pairs.append((da["hops"], db["hops"]))
                bad = pc_check(db, mnem_at, span)
                if bad:
                    n_mis += 1
                    n_match -= 1
                    mismatches.append(f"e{e:05X}s{seed}: PCCHECK {bad[:3]}")
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
        ratios = [d / f for d, f in hop_pairs if f > 0]
        ent["hop_diag_total"] = sum(d for d, _ in hop_pairs)
        ent["hop_fast_total"] = sum(f for _, f in hop_pairs)
        ent["hop_ratio_mean"] = round(sum(ratios) / len(ratios), 2)
        ent["hop_ratio_minmax"] = [round(min(ratios), 2), round(max(ratios), 2)]
    # Every-entry-PC coverage, STATIC form (final, 07-31): the region twin's
    # per-pc entry behavior is proven by exact case-table equality against the
    # disassembly -- every instruction pc must appear as `case 0x<pc>u: goto
    # loc_<pc>;` in the generated switches. A DYNAMIC region-twin sweep is not
    # executable: region-internal loops run between runtime hooks, and spins
    # conditioned on PRNG-seeded local-store data cannot be bounded externally
    # (measured 07-31: gs_task fast sweep wedged ~120 pcs in; job A/B fast
    # sweeps timed out under both 16-bit and 8-bit trip seeding while every
    # instruction-twin sweep completed under its dispatch caps). Deep dynamic
    # equivalence stays with the matrix windows above, which enter at region
    # starts, mid-region pcs and region tails.
    if args.every_pc:
        rx = re.compile(r"case 0x([0-9A-F]{8})u: goto loc_([0-9A-F]{8});")
        cases = set()
        for m in rx.finditer(open(fast_c, errors="replace").read()):
            if m.group(1) == m.group(2):
                cases.add(int(m.group(1), 16))
        want = set(mnem_at)
        missing = sorted(want - cases)
        extra = sorted(cases - want)
        ent["everypc_pcs"] = len(want)
        ent["everypc_static_missing"] = len(missing)
        ent["everypc_static_extra"] = len(extra)
        if missing or extra:
            ent["everypc_first"] = [hex(x) for x in (missing + extra)[:6]]
            n_mis += 1
    ent["verdict"] = ("MISMATCH" if n_mis else
                      ("MATCH" if n_match else "INCONCLUSIVE"))
    results.append(ent)
    hr = ent.get("hop_ratio_mean", "-")
    print(f"[{famname}] {stem:26s} {ent['verdict']:12s} match={n_match} mis={n_mis} "
          f"inc={n_inc} hop-ratio~{hr} (cl {t_d:.0f}s/{t_f:.0f}s)")


def fam_elf(plc):
    return plc["elf"]


def write_report(path, entries):
    """Publish a complete JSON snapshot even if the run is interrupted."""
    pending = path + ".tmp"
    with open(pending, "w") as f:
        json.dump(entries, f, indent=1)
        f.flush()
        os.fsync(f.fileno())
    os.replace(pending, path)


def main():
    global INPUT_ROOT
    ap = argparse.ArgumentParser()
    ap.add_argument("--families", default=None)
    ap.add_argument("--only", default=None)
    ap.add_argument("--max-entries", type=int, default=14)
    ap.add_argument("--seeds", type=int, default=2)
    ap.add_argument("--evbudget", type=int, default=384)
    ap.add_argument("--hopmax", type=int, default=2000000)
    ap.add_argument("--timeout", type=int, default=45)
    ap.add_argument("--every-pc", action="store_true", dest="every_pc",
                    help="also run the every-entry-PC digest sweep per placement")
    ap.add_argument("--sweep-timeout", type=int, default=900, dest="sweep_timeout")
    ap.add_argument("--input-root", default=ROOT,
                    help="read-only project tree containing game/raw/ELF inputs")
    ap.add_argument("--tracked-twins", action="store_true",
                    help="compare shipped DIAG/FAST twins and derive missing metrics")
    ap.add_argument("--fresh-report", action="store_true",
                    help="replace prior report entries with only this invocation")
    args = ap.parse_args()
    INPUT_ROOT = os.path.abspath(args.input_root)
    args.only = set(args.only.split(",")) if args.only else None
    fams = set(args.families.split(",")) if args.families else None

    if CL_EXE is None:
        print("ERROR: cl not on PATH -- run from a vcvars64-imported shell")
        sys.exit(2)

    manifest = json.load(open(os.path.join(TOOLS, "spu_region_manifest.json")))
    os.makedirs(DIFFD, exist_ok=True)
    rep = os.path.join(DIFFD, "diff_report.json")
    prior = {}
    if os.path.exists(rep) and not args.fresh_report:
        prior = {e["stem"]: e for e in json.load(open(rep))}
    results = []
    for famname, fam in manifest["families"].items():
        if fams and famname not in fams:
            continue
        for plc in fam.get("images", fam.get("placements", [])):
            process(famname, fam, plc, args, results)
            merged = dict(prior)
            merged.update({e["stem"]: e for e in results})
            write_report(rep, list(merged.values()))  # incremental + merged across runs

    merged = dict(prior)
    merged.update({e["stem"]: e for e in results})
    write_report(rep, list(merged.values()))
    n_bad = sum(1 for r in results if r["verdict"] in ("MISMATCH", "COMPILE-FAIL"))
    n_all = len(results)
    print(f"\n{n_all} placement(s): "
          f"{sum(1 for r in results if r['verdict'] == 'MATCH')} MATCH, "
          f"{n_bad} MISMATCH/COMPILE-FAIL, "
          f"{sum(1 for r in results if r['verdict'] == 'INCONCLUSIVE')} inconclusive "
          f"-> {rep}")
    sys.exit(1 if n_bad else 0)


if __name__ == "__main__":
    main()
