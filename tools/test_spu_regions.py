#!/usr/bin/env python3
"""s50 REGION-MODE conformance extension.

Runs the existing SPU conformance suite (tools/test_spu_lift.py -- the 900+
ISA cases with expectations computed from the SPU ISA RTL) and then re-runs
the IDENTICAL cases through the REGION emitter (SPULifter.lift_region, the
s41 --regions path): the same case blob is region-lifted one region per
16-byte case slot, the generated conformance driver is mechanically
retargeted from `spu_case_<pc>(ctx)` calls to `ctx->pc = <pc>;
spu_rcase_<pc>(ctx)` region entries, and both runs' FAIL sets must be
EQUAL -- region emission may introduce no new failure and hide none.

This exercises, for every case: the entry switch (enter-at-exact-pc), the
per-pc labels, the shared _translate() path under pc_to_region, and the
region tail transfer (each slot's fall-through sets pc + trampoline, which
the driver ignores by design -- cases are single-instruction).

Also asserts the derive_region_prefix() contract (the s50 collision rule).

Usage:  py -3 tools\\test_spu_regions.py            (exit 0 = green)
Artifacts land in scratch\\sputest\\ alongside the base suite's.
"""

import os
import re
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import spu_disasm                                    # noqa: E402
from spu_lifter import SPULifter, derive_region_prefix  # noqa: E402

OUTDIR = os.path.join(ROOT, "scratch", "sputest")
VCVARS = (r"C:\Program Files\Microsoft Visual Studio\18\Community"
          r"\VC\Auxiliary\Build\vcvars64.bat")


def check_prefix_contract():
    table = {
        "spu_func_": "spu_region_",          # gs_task byte-compat, load-bearing
        "spu_jobA_": "spu_region_jobA_",
        "spu_jobAE4_": "spu_region_jobAE4_",
        "spu_polfunc_": "spu_region_polfunc_",
        "spu_jobfunc_": "spu_region_jobfunc_",
        "spu_tsexit_": "spu_region_tsexit_",
        "cri_audio_": "spu_region_cri_audio_",
        "wkl4_": "spu_region_wkl4_",
        "spuimg6_": "spu_region_spuimg6_",
    }
    for fp, want in table.items():
        got = derive_region_prefix(fp)
        assert got == want, f"derive_region_prefix({fp!r}) = {got!r}, want {want!r}"
    vals = [derive_region_prefix(k) for k in table]
    assert len(set(vals)) == len(vals), "derived region prefixes must be pairwise distinct"
    print(f"[region-conf] prefix contract OK ({len(table)} cases, pairwise distinct)")


def parse_fails(log_text):
    return sorted(set(re.findall(r"^FAIL (\[[^\]]+\] \S+):", log_text, re.M)))


def run_base_suite():
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "test_spu_lift.py")],
                       capture_output=True, text=True, cwd=ROOT)
    log = os.path.join(OUTDIR, "spu_conformance.log")
    log_text = open(log).read() if os.path.exists(log) else ""
    print(f"[region-conf] base suite rc={r.returncode}")
    tail = [l for l in (r.stdout or "").splitlines() if "passed" in l or "FAIL" in l]
    for l in tail[-4:]:
        print("   " + l)
    return r.returncode, parse_fails(log_text)


def region_lift_blob():
    blob = open(os.path.join(OUTDIR, "spu_conf_blob.bin"), "rb").read()
    insns = spu_disasm.disassemble_spu(blob, 0)
    nslots = len(blob) // 16
    regions = [(i * 16, i * 16 + 16) for i in range(nslots)]
    pc_to_region = {}
    for rs, re_ in regions:
        for insn in insns:
            if rs <= insn.addr < re_:
                pc_to_region[insn.addr] = rs

    lifter = SPULifter(trace=False)
    lifter.header_name = "spu_rcase_recomp.h"
    lifter.register_name = "spu_rconf_register"
    lifter.func_prefix = "spu_case_"          # unused for naming in region mode
    lifter.region_prefix = "spu_rcase_"
    lifter.image_span = (0, len(blob))
    for rs, re_ in regions:
        lifter.lift_region(insns, rs, re_, pc_to_region)
    with open(os.path.join(OUTDIR, "spu_rcase_recomp.h"), "w", newline="\n") as f:
        f.write(lifter.emit_region_header())
    with open(os.path.join(OUTDIR, "spu_rcase_recomp.c"), "w", newline="\n") as f:
        f.write(lifter.emit_region_source(pc_to_region))
    assert not lifter.region_fallback_internal, \
        f"in-image fallbacks in conformance blob: {lifter.region_fallback_internal[:4]}"
    print(f"[region-conf] region-lifted blob: {len(regions)} slot-regions, "
          f"{len(insns)} insns")
    return len(regions)


def transform_driver():
    src = open(os.path.join(OUTDIR, "spu_conf_driver.c")).read()
    src = src.replace('#include "spu_recomp.h"', '#include "spu_rcase_recomp.h"')
    src, n = re.subn(r"spu_case_([0-9A-F]{8})\(ctx\);",
                     r"ctx->pc = 0x\1u; spu_rcase_\1(ctx); g_spu_trampoline_fn = 0;",
                     src)
    assert n > 0, "driver transform matched no case calls"
    with open(os.path.join(OUTDIR, "spu_rconf_driver.c"), "w", newline="\n") as f:
        f.write(src)
    print(f"[region-conf] driver retargeted: {n} case call(s)")


def build_and_run():
    bat = os.path.join(OUTDIR, "spu_rconf_run.bat")
    log = os.path.join(OUTDIR, "spu_rconf.log")
    exe = os.path.join(OUTDIR, "spu_rconf.exe")
    with open(bat, "w") as f:
        f.write("@echo off\n")
        f.write(f'call "{VCVARS}" >nul 2>nul\n')
        f.write(f'cd /d "{ROOT}"\n')
        f.write(f'cl /nologo /O1 /W3 /std:c17 /experimental:c11atomics /bigobj '
                f'/I {OUTDIR} /I runtime\\spu '
                f'/Fo{OUTDIR}\\ /Fe:{exe} '
                f'{OUTDIR}\\spu_rconf_driver.c {OUTDIR}\\spu_rcase_recomp.c '
                f'> "{log}" 2>&1\n')
        f.write("if errorlevel 1 exit /b 2\n")
        f.write(f'"{exe}" >> "{log}" 2>&1\n')
    r = subprocess.run(["cmd", "/c", bat], cwd=ROOT)
    text = open(log).read() if os.path.exists(log) else ""
    return r.returncode, text


def main():
    check_prefix_contract()
    base_rc, base_fails = run_base_suite()
    region_lift_blob()
    transform_driver()
    rc, log_text = build_and_run()
    if rc == 2:
        print("[region-conf] COMPILE FAILED -- scratch\\sputest\\spu_rconf.log")
        for ln in log_text.splitlines():
            if "error" in ln.lower():
                print("   " + ln)
        sys.exit(2)
    region_fails = parse_fails(log_text)
    for ln in log_text.splitlines():
        if "passed" in ln:
            print("[region-conf] " + ln.strip())
    if region_fails != base_fails:
        new = [f for f in region_fails if f not in base_fails]
        gone = [f for f in base_fails if f not in region_fails]
        print(f"[region-conf] FAIL-SET MISMATCH: {len(new)} new, {len(gone)} hidden")
        for f in new[:12]:
            print("   NEW-FAIL " + f)
        for f in gone[:12]:
            print("   HIDDEN   " + f)
        sys.exit(1)
    print(f"[region-conf] PASS: region fail-set identical to instruction fail-set "
          f"({len(base_fails)} standing failure(s) in both)")
    sys.exit(0)


if __name__ == "__main__":
    main()
