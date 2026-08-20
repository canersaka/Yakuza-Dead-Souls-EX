#!/usr/bin/env python3
"""s50 mass-region-lift batch driver.

Orchestration ONLY: every lift is a subprocess invocation of the single
region-lifter implementation (tools/spu_lifter.py --regions, the s41
architecture validated on gs_task). For every placement in
tools/spu_region_manifest.json this driver:

  1. materializes the input artifact (shipped elf; or family raw bytes
     re-wrapped at the placement base via wrap_spu_elf.wrap; or an EBOOT
     slice for the orphanage family) -- job placements of one family are
     PROVEN byte-identical code by construction (same raw slice);
  2. REPRO GATE: re-runs the per-instruction (DIAG) lift with the same
     inputs and byte-diffs it against the shipped instruction twin. A
     placement whose twin cannot be byte-reproduced is flagged: the region
     twin generated from those inputs would not be provably classification-
     consistent (bi-$r0 set, boundaries) with the shipped twin. Two command
     shapes are tried: S1 --auto-functions <elf> --seed-all (the
     gen_spu_images.py shape) and S2 <raw> --base B --seed-all;
  3. runs the REGION lift (--regions --regions-strict --metrics-json) with
     the SAME input shape that reproduced, emitting <stem>_fast.c/.h into
     yakuza/generated/fast/. Hand-edited entries emit a comparison oracle in
     scratch/regionlift/oracle instead of overwriting their accepted twin;
  4. aggregates a machine-readable report.

Symbol prefixes / register names are extracted from the shipped twin
HEADERS (ground truth), not hardcoded here.

Usage:
    py -3 tools\\gen_spu_regions.py                # everything
    py -3 tools\\gen_spu_regions.py --families job_a,sony
    py -3 tools\\gen_spu_regions.py --only job_bin_a_e400
    py -3 tools\\gen_spu_regions.py --skip-repro   # region gen only
"""

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

from wrap_spu_elf import wrap  # noqa: E402

MANIFEST = os.path.join(TOOLS, "spu_region_manifest.json")
OUT_FAST = os.path.join(ROOT, "yakuza", "generated", "fast")
TRACKED_DIAG = os.path.join(ROOT, "yakuza", "generated", "diag")
WORK = os.path.join(ROOT, "scratch", "regionlift")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def read_header_identity(diag_c_path):
    """Extract (func_prefix, register_name) from the shipped twin's header."""
    h_path = diag_c_path[:-2] + ".h"
    text = open(h_path, "r", errors="replace").read()
    m_reg = re.search(r"^void ([A-Za-z_]\w*)\(void\);", text, re.M)
    reg = None
    for m in re.finditer(r"^void ([A-Za-z_]\w*)\(void\);", text, re.M):
        if "register" in m.group(1):
            reg = m.group(1)
            break
    # Prefix = identifier minus its trailing 8-hex address. NOT required to
    # end in '_': two shipped placements (job_bin_b_6c00, job_bin_d_15800)
    # were lifted with bare prefixes like 'spu_jobB6C'. The split is unique:
    # only one position leaves exactly 8 hex digits directly before '('.
    m_pre = re.search(r"^void ([A-Za-z_]\w*?)[0-9A-F]{8}\(spu_context", text, re.M)
    if not reg or not m_pre:
        raise SystemExit(f"cannot extract prefix/register from {h_path}"
                         f" (reg={reg}, prefix={'ok' if m_pre else None})")
    return m_pre.group(1), reg, h_path


def resolve_diag_path(stem, declared_path):
    """Return the active instruction twin after integration relocations.

    The integration branch tracks the systematic instruction lifts under
    yakuza/generated/diag while the manifest retains their provenance paths.
    Exact placements added later still live at their declared paths.
    """
    integrated = os.path.join(TRACKED_DIAG, f"{stem}.c")
    if os.path.exists(integrated):
        return integrated
    return os.path.join(ROOT, declared_path)


def parse_spu_elf(path):
    """Minimal BE32 SPU-elf reader -> (code_bytes, base, entry)."""
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[5] == 2, f"{path}: not a BE ELF"
    entry = struct.unpack_from(">I", d, 24)[0]
    phoff = struct.unpack_from(">I", d, 28)[0]
    phnum = struct.unpack_from(">H", d, 44)[0]
    for i in range(phnum):
        t, off, va, _pa, fsz, _msz, _fl, _al = struct.unpack_from(">8I", d, phoff + i * 32)
        if t == 1:  # PT_LOAD
            return d[off:off + fsz], va, entry
    raise SystemExit(f"{path}: no PT_LOAD")


def eboot_slice(eboot_path, ea, size):
    from elf_parser import ELFFile, vaddr_to_offset
    image = ELFFile(eboot_path)
    image.load()
    off = vaddr_to_offset(image.program_headers, ea)
    if off is None:
        raise SystemExit(f"EA 0x{ea:X} not inside an EBOOT PT_LOAD segment")
    return image.raw_data[off:off + size]


def family_code(fam):
    """The family's raw code bytes + (ref_base, ref_entry) for entry-delta."""
    if fam.get("raw"):
        code = open(os.path.join(ROOT, fam["raw"]), "rb").read()
    elif fam.get("ref_elf"):
        code, _va, _e = parse_spu_elf(os.path.join(ROOT, fam["ref_elf"]))
    else:
        ea = int(fam["eboot_ea"], 0)
        size = int(fam["eboot_size"], 0)
        code = eboot_slice(os.path.join(ROOT, fam["eboot"]), ea, size)
    if fam.get("ref_elf"):
        _c, ref_base, ref_entry = parse_spu_elf(os.path.join(ROOT, fam["ref_elf"]))
    else:
        ref_base = ref_entry = None
    return code, ref_base, ref_entry


def run_lift(argv, log_path):
    with open(log_path, "w") as lf:
        r = subprocess.run([sys.executable, os.path.join(TOOLS, "spu_lifter.py")] + argv,
                           stdout=lf, stderr=subprocess.STDOUT, cwd=ROOT)
    return r.returncode


def file_diff(a, b):
    """(identical, n_differing_bytes_or_size_note)."""
    da = open(a, "rb").read()
    db = open(b, "rb").read()
    if da == db:
        return True, 0
    if len(da) != len(db):
        return False, f"size {len(da)} vs {len(db)}"
    return False, sum(1 for x, y in zip(da, db) if x != y)


def process_placement(famname, fam, plc, args, report):
    stem = plc["stem"]
    if args.only and stem not in args.only:
        return
    if args.resume and stem in args.prior and args.prior[stem].get("region_rc") == 0:
        report.append(args.prior[stem])
        print(f"[{famname}] {stem:26s} cached (resume)")
        return
    if plc.get("diag"):
        diag_c = resolve_diag_path(stem, plc["diag"])
        prefix, register, h_path = read_header_identity(diag_c)
    else:
        # New placement with NO shipped instruction twin (e.g. the 07-30
        # observed orphanage 0x3BC00): identity comes from the manifest and
        # the repro lane's freshly generated instruction lift IS the twin.
        diag_c = None
        prefix, register = plc["prefix"], plc["register"]
        h_path = None
    base = int(plc["base"], 0) if "base" in plc else None

    # ---- input artifact -------------------------------------------------
    if plc.get("elf"):
        elf_path = os.path.join(ROOT, plc["elf"])
        code, elf_base, elf_entry = parse_spu_elf(elf_path)
        raw_path = None
    elif fam["kind"] == "elf":
        raise SystemExit(f"{stem}: elf-family placement without an elf")
    else:
        code, ref_base, ref_entry = family_code(fam)
        delta = (ref_entry - ref_base) if (fam.get("ref_elf") and ref_entry is not None) else 0
        entry = base + delta
        elf_bytes = wrap(code, base=base, entry=entry)
        os.makedirs(os.path.join(WORK, "inputs"), exist_ok=True)
        elf_path = os.path.join(WORK, "inputs", f"{stem}.elf")
        with open(elf_path, "wb") as f:
            f.write(elf_bytes)
        elf_base, elf_entry = base, entry
        raw_path = os.path.join(WORK, "inputs", f"{stem}_raw.bin")
        with open(raw_path, "wb") as f:
            f.write(code)

    ent = {
        "family": famname, "stem": stem, "base": hex(elf_base),
        "entry": hex(elf_entry), "prefix": prefix, "register": register,
        "canonical_ea": fam.get("eboot_ea"),
        "code_sha256": hashlib.sha256(code).hexdigest(),
        "code_bytes": len(code),
        "diag": plc.get("diag"), "image_id": plc.get("image_id"),
        "entry_evidence": plc.get("entry_evidence",
            "elf e_entry" if plc.get("elf")
            else ("reference elf entry delta rebased to placement"
                  if fam.get("ref_elf") else "base (validated by byte repro of the shipped twin)")),
        "overlaps": [],
    }

    # ---- repro gate -----------------------------------------------------
    repro_dir = os.path.join(WORK, "repro")
    os.makedirs(repro_dir, exist_ok=True)
    shape = None
    if diag_c is None:
        # No shipped twin: generate the instruction twin fresh (it becomes
        # the correctness twin for the differential matrix) and record the
        # provenance class loudly instead of a byte-diff verdict.
        s1 = ["--auto-functions", elf_path, "--seed-all",
              "--func-prefix", prefix, "--register-name", register,
              "--output", repro_dir,
              "--source-name", f"{stem}.c", "--header-name", f"{stem}.h"]
        rc = run_lift(s1, os.path.join(WORK, "liftlogs", f"{stem}.repro1.log"))
        shape = "S1-autofn-elf"
        ent["repro"] = "NEW-PLACEMENT-NO-SHIPPED-TWIN" if rc == 0 else f"TWIN-GEN-FAILED rc={rc}"
    elif not args.skip_repro:
        s1 = ["--auto-functions", elf_path, "--seed-all",
              "--func-prefix", prefix, "--register-name", register,
              "--output", repro_dir,
              "--source-name", f"{stem}.c", "--header-name", f"{stem}.h"]
        rc = run_lift(s1, os.path.join(WORK, "liftlogs", f"{stem}.repro1.log"))
        ident_c = ident_h = False
        if rc == 0:
            ident_c, dc = file_diff(os.path.join(repro_dir, f"{stem}.c"), diag_c)
            ident_h, dh = file_diff(os.path.join(repro_dir, f"{stem}.h"), h_path)
        if rc == 0 and ident_c and ident_h:
            shape = "S1-autofn-elf"
            ent["repro"] = "IDENTICAL"
        else:
            ent["repro_s1"] = f"rc={rc} c_diff={0 if ident_c else dc} h_diff={0 if ident_h else dh}"
            if raw_path or fam["kind"] == "job":
                if raw_path is None:
                    raw_path = os.path.join(WORK, "inputs", f"{stem}_raw.bin")
                    with open(raw_path, "wb") as f:
                        f.write(code)
                s2 = [raw_path, "--base", hex(elf_base), "--seed-all",
                      "--func-prefix", prefix, "--register-name", register,
                      "--output", repro_dir,
                      "--source-name", f"{stem}.c", "--header-name", f"{stem}.h"]
                rc2 = run_lift(s2, os.path.join(WORK, "liftlogs", f"{stem}.repro2.log"))
                ident_c2 = ident_h2 = False
                if rc2 == 0:
                    ident_c2, dc2 = file_diff(os.path.join(repro_dir, f"{stem}.c"), diag_c)
                    ident_h2, dh2 = file_diff(os.path.join(repro_dir, f"{stem}.h"), h_path)
                if rc2 == 0 and ident_c2 and ident_h2:
                    shape = "S2-raw-base"
                    ent["repro"] = "IDENTICAL"
                else:
                    ent["repro_s2"] = f"rc={rc2} c_diff={0 if ident_c2 else dc2} h_diff={0 if ident_h2 else dh2}"
                    shape = "S1-autofn-elf"
                    ent["repro"] = "DIFFERS"
            else:
                shape = "S1-autofn-elf"
                ent["repro"] = "DIFFERS-HANDEDIT" if plc.get("expect_handedit") else "DIFFERS"
        if plc.get("expect_handedit") and ent["repro"] != "IDENTICAL":
            ent["repro"] = "DIFFERS-HANDEDIT-EXPECTED"
    else:
        shape = "S1-autofn-elf"
        ent["repro"] = "SKIPPED"

    # ---- region lift ----------------------------------------------------
    region_out = (os.path.join(WORK, "oracle")
                  if plc.get("expect_handedit") else OUT_FAST)
    os.makedirs(region_out, exist_ok=True)
    os.makedirs(os.path.join(WORK, "metrics"), exist_ok=True)
    metrics_path = os.path.join(WORK, "metrics", f"{stem}.json")
    common = ["--regions", "--regions-strict",
              "--region-cap", str(args.region_cap),
              "--func-prefix", prefix, "--register-name", register,
              "--metrics-json", metrics_path,
              "--output", region_out,
              "--source-name", f"{stem}_fast.c", "--header-name", f"{stem}_fast.h"]
    if shape == "S2-raw-base":
        argv = [raw_path, "--base", hex(elf_base)] + common
    else:
        argv = ["--auto-functions", elf_path] + common
    rc = run_lift(argv, os.path.join(WORK, "liftlogs", f"{stem}.region.log"))
    ent["region_rc"] = rc
    ent["region_output"] = os.path.relpath(region_out, ROOT)
    ent["shape"] = shape
    if rc == 0 and os.path.exists(metrics_path):
        m = json.load(open(metrics_path))
        ent["n_regions"] = m["n_regions"]
        ent["n_insns"] = m["n_insns"]
        ent["source_bytes"] = m["source_bytes"]
        ent["trampoline_sites"] = m["trampoline_sites"]
        ent["cross_region_call_sites"] = m["cross_region_call_sites"]
        ent["fallback_internal"] = len(m["fallback_internal"])
        ent["fallback_external"] = len(m["fallback_external"])
        ent["unsupported"] = sum(m["unsupported"].values())
        ent["unresolved_calls"] = len(m["unresolved_calls"])
    report.append(ent)
    flag = "" if rc == 0 else "  <-- REGION LIFT FAILED"
    print(f"[{famname}] {stem:26s} repro={ent['repro']:26s} shape={shape:14s} "
          f"regions={ent.get('n_regions','-'):>4} rc={rc}{flag}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--families", default=None,
                    help="comma-separated family filter (default: all)")
    ap.add_argument("--only", default=None,
                    help="comma-separated placement stems to process")
    ap.add_argument("--skip-repro", action="store_true")
    ap.add_argument("--resume", action="store_true",
                    help="skip placements already recorded successful in report.json")
    ap.add_argument("--region-cap", type=int, default=256)
    args = ap.parse_args()
    args.only = set(args.only.split(",")) if args.only else None
    fams = set(args.families.split(",")) if args.families else None

    os.makedirs(os.path.join(WORK, "liftlogs"), exist_ok=True)
    rep_path = os.path.join(WORK, "report.json")
    args.prior = {}
    if os.path.exists(rep_path):
        args.prior = {e["stem"]: e for e in json.load(open(rep_path))}
    manifest = json.load(open(MANIFEST))

    # ---- placement identity validation (07-30 contract) -----------------
    # A symbol namespace (func prefix) may be SHARED only by placements whose
    # LS spans are pairwise disjoint (the shipped spu_func_ trio: kernel2,
    # sysservice, gs_task); overlapping spans MUST live in distinct
    # namespaces or their per-pc symbols would collide and a PC-keyed lookup
    # could serve the wrong placement. Register names are plain link symbols
    # and must be globally unique. Every overlapping span pair (across
    # namespaces) is reported: that list is exactly the ambiguity set the
    # runtime residency layer must disambiguate before mapping PC to code.
    seen_reg, by_pfx, spans = {}, {}, []
    fam_code_cache = {}
    for famname, fam in manifest["families"].items():
        if fams and famname not in fams:
            continue
        for plc in fam.get("images", fam.get("placements", [])):
            if args.only and plc["stem"] not in args.only:
                continue
            if plc.get("diag"):
                try:
                    pfx, reg, _h = read_header_identity(
                        resolve_diag_path(plc["stem"], plc["diag"]))
                except SystemExit:
                    continue
            else:
                pfx, reg = plc["prefix"], plc["register"]
            if reg in seen_reg:
                print(f"VALIDATION FAIL: register {reg!r} shared by "
                      f"{seen_reg[reg]} and {plc['stem']}")
                sys.exit(3)
            seen_reg[reg] = plc["stem"]
            if plc.get("elf"):
                _c, b, _e = parse_spu_elf(os.path.join(ROOT, plc["elf"]))
                ln = len(_c)
            elif fam["kind"] == "job":
                if famname not in fam_code_cache:
                    fam_code_cache[famname] = len(family_code(fam)[0])
                ln = fam_code_cache[famname]
                b = int(plc["base"], 0)
            else:
                continue
            spans.append((plc["stem"], pfx, b, b + ln))
            by_pfx.setdefault(pfx, []).append((plc["stem"], b, b + ln))
    for pfx, group in by_pfx.items():
        for i in range(len(group)):
            for j in range(i + 1, len(group)):
                a, c = group[i], group[j]
                if a[1] < c[2] and c[1] < a[2]:
                    print(f"VALIDATION FAIL: namespace {pfx!r} shared by "
                          f"OVERLAPPING placements {a[0]} and {c[0]}")
                    sys.exit(3)
    overlaps = []
    for i in range(len(spans)):
        for j in range(i + 1, len(spans)):
            a, c = spans[i], spans[j]
            if a[1] != c[1] and a[2] < c[3] and c[2] < a[3]:
                overlaps.append((a[0], c[0]))
    print(f"placement validation: {len(by_pfx)} namespaces over {len(spans)} "
          f"spans, shared-namespace groups disjoint, {len(overlaps)} "
          f"overlapping span pair(s) across distinct namespaces")
    with open(os.path.join(WORK, "overlap_pairs.json"), "w") as f:
        json.dump(overlaps, f, indent=1)

    report = []
    for famname, fam in manifest["families"].items():
        if fams and famname not in fams:
            continue
        for plc in fam.get("images", fam.get("placements", [])):
            process_placement(famname, fam, plc, args, report)
            merged = dict(args.prior)
            merged.update({e["stem"]: e for e in report})
            with open(rep_path, "w") as f:      # incremental, crash-safe, merged
                json.dump(list(merged.values()), f, indent=1)

    merged = dict(args.prior)
    merged.update({e["stem"]: e for e in report})
    with open(rep_path, "w") as f:
        json.dump(list(merged.values()), f, indent=1)
    n_bad = sum(1 for e in report if e["region_rc"] != 0)
    n_norepro = sum(1 for e in report if e.get("repro", "").startswith("DIFFERS")
                    and "EXPECTED" not in e.get("repro", ""))
    print(f"\n{len(report)} placement(s); {n_bad} region-lift failure(s); "
          f"{n_norepro} unexplained repro mismatch(es); report -> {rep_path}")
    sys.exit(1 if n_bad else 0)


if __name__ == "__main__":
    main()
