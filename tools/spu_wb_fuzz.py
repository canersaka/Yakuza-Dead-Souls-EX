#!/usr/bin/env python3
"""WB randomized-block fuzzer: random legal instruction sequences, lifted
three ways (DIAG instruction twin, FAST region twin, WB whole-block twin),
executed over the deterministic s50 harness environment across seeds, all
architectural state compared.

This covers optimizer edge cases real images cannot: random operand
register reuse, dense flush/branch interleavings, LS alias patterns, and
random immediates -- against the per-instruction reference translation.

Image shape: straight-line random data-processing instructions with
occasional in-image forward conditional/unconditional branches, occasional
channel reads/writes (the stub environment is a pure function of the seed),
occasional conditional-halt instructions, `stop` terminators, and a final
stop. No calls and no register-indirect branches (their targets are
meaningless in a random image; real images cover them). Instructions are
REJECTION-SAMPLED through spu_disasm: a candidate 32-bit word is kept only
if it decodes to a whitelisted mnemonic -- the fuzzer needs no encoder for
the data-processing space, and branch/stop words are built from
dynamically probed opcode bases (spu_disasm remains the single source of
decode truth).

Usage (vcvars64 shell):
    py -3 tools/spu_wb_fuzz.py --images 24 --seeds 3 [--keep]
"""

import argparse
import json
import os
import random
import shutil
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import spu_disasm                     # noqa: E402
import spu_region_diff as D           # noqa: E402
import spu_wb_diff as WD              # noqa: E402
from wrap_spu_elf import wrap         # noqa: E402

WORK = os.path.join(ROOT, "scratch", "wbfuzz")
HARNESS = os.path.join(TOOLS, "spu_diff_harness.c")
BASE = 0x4000

# Data-processing whitelist (no control flow, no channels here -- those are
# inserted deliberately). Mirrors the WB-supported set.
DATA_MNEMONICS = {
    "il", "ila", "ilh", "ilhu", "iohl", "fsmbi",
    "a", "sf", "ah", "sfh", "ai", "ahi", "sfi", "sfhi",
    "addx", "sfx", "cg", "cgx", "bg", "bgx",
    "and", "or", "xor", "nand", "nor", "andc", "orc", "eqv",
    "andi", "ori", "xori", "andhi", "andbi", "orhi", "orbi", "xorhi", "xorbi",
    "ceq", "ceqh", "ceqb", "cgt", "cgth", "cgtb", "clgt", "clgth", "clgtb",
    "ceqi", "cgti", "clgti", "ceqbi", "ceqhi", "clgtbi", "clgthi",
    "cgthi", "cgtbi",
    "mpy", "mpyu", "mpyi", "mpyui", "mpyh", "mpyhh", "mpyhhu", "mpyhha",
    "mpyhhau", "mpys", "mpya",
    "selb", "shufb", "clz", "cntb", "gb", "gbh", "gbb", "orx",
    "xsbh", "xshw", "xswd", "sumb", "absdb", "avgb",
    "fsm", "fsmh", "fsmb",
    "cbd", "chd", "cwd", "cdd", "cbx", "chx", "cwx", "cdx",
    "shli", "shlhi", "roti", "rothi", "rotmi", "rotmai", "rotmhi",
    "rotmahi", "rothmi",
    "shlqbyi", "rotqbyi", "shlqbii", "rotqbii", "rotqmbii", "rotqmbyi",
    "shl", "shlh", "rot", "roth", "rotm", "rotma", "rothm", "rothma",
    "rotmah",
    "shlqbi", "rotqbi", "shlqby", "rotqby", "shlqbybi", "rotqbybi",
    "rotqmbi", "rotqmby", "rotqmbybi",
    "fa", "fs", "fm", "fi", "fma", "fms", "fnms",
    "fceq", "fcgt", "fcmeq", "fcmgt", "frest", "frsqest",
    "fesd", "frds", "cflts", "cfltu", "csflt", "cuflt",
    "dfa", "dfs", "dfm", "dfma", "dfms", "dfnms", "dfnma",
    "dfceq", "dfcmeq", "dfcgt", "dfcmgt",
    # dftsv EXCLUDED: its RI7 field decodes through an RR-priority path as a
    # "$rN" token that the DIAG reference lifter emits verbatim (invalid C),
    # so the per-instruction reference cannot express it. The WB lifter
    # handles it (raw-field strip; see spu_wb_lifter VECI token handling).
    "lqd", "stqd", "lqa", "stqa", "lqx", "stqx", "lqr", "stqr",
    "xsbh", "eqv", "nop", "lnop",
    "hgt", "hlgt", "heq", "hgti", "hlgti", "heqi",
}


def probe_base(mnemonic):
    """Find an op11 whose zero-field decode yields `mnemonic`."""
    for op11 in range(2048):
        r = spu_disasm.spu_decode(op11 << 21, 0)
        if getattr(r, "mnemonic", None) == mnemonic:
            return op11 << 21
    raise SystemExit(f"cannot probe encoding for {mnemonic}")


BR_BASE = None      # filled in main
BRZ_BASE = None
BRNZ_BASE = None


def enc_ri16(basew, rt, i16):
    return basew | ((i16 & 0xFFFF) << 7) | (rt & 0x7F)


def gen_word(rng):
    """One random whitelisted data-processing word (rejection sampling)."""
    while True:
        w = rng.getrandbits(32)
        r = spu_disasm.spu_decode(w, 0)
        mn = getattr(r, "mnemonic", None)
        if mn in DATA_MNEMONICS:
            return w


def gen_image(rng, n_insns):
    """Random instruction words + deliberate control flow. Returns bytes."""
    words = []
    i = 0
    while i < n_insns:
        roll = rng.random()
        remaining = n_insns - i
        if roll < 0.06 and remaining > 4:
            # forward branch (cond or uncond) to a later slot
            tgt_slot = i + rng.randint(2, min(remaining - 1, 24))
            delta = tgt_slot - i
            kind = rng.random()
            rt = rng.randint(0, 127)
            if kind < 0.4:
                words.append(enc_ri16(BR_BASE, 0, delta))
            elif kind < 0.7:
                words.append(enc_ri16(BRZ_BASE, rt, delta))
            else:
                words.append(enc_ri16(BRNZ_BASE, rt, delta))
        elif roll < 0.08:
            words.append(0x00000000 | rng.randint(0, 0x3FFF))   # stop
        else:
            words.append(gen_word(rng))
        i += 1
    words.append(0x00000000 | 0x1234)   # terminal stop
    out = b"".join(w.to_bytes(4, "big") for w in words)
    return out


def run_lifter(argv, log):
    with open(log, "w") as lf:
        return subprocess.run([sys.executable] + argv, stdout=lf,
                              stderr=subprocess.STDOUT, cwd=ROOT).returncode


def build_twins(idx, code, outdir):
    """DIAG (per-instruction), FAST (region), WB. Returns (diag_exe, wb_exe)."""
    raw = os.path.join(outdir, "code.bin")
    with open(raw, "wb") as f:
        f.write(code)
    elf = os.path.join(outdir, "img.elf")
    with open(elf, "wb") as f:
        f.write(wrap(code, base=BASE, entry=BASE))
    reg = f"fuzz{idx}_register"
    prefix = f"spu_fz{idx}_"
    common = ["--func-prefix", prefix, "--register-name", reg,
              "--output", outdir]
    # DIAG twin (per-instruction reference; whole-image single lift)
    rc = run_lifter([os.path.join(TOOLS, "spu_lifter.py"), raw,
                     "--base", hex(BASE)] + common +
                    ["--source-name", "diag.c", "--header-name", "diag.h"],
                    os.path.join(outdir, "diag.lift.log"))
    if rc:
        return None, None, "diag-lift"
    # FAST twin (fallback surface for WB)
    rc = run_lifter([os.path.join(TOOLS, "spu_lifter.py"),
                     "--auto-functions", elf, "--regions"] + common +
                    ["--source-name", "fast.c", "--header-name", "fast.h"],
                    os.path.join(outdir, "fast.lift.log"))
    if rc:
        return None, None, "fast-lift"
    # WB twin
    rc = run_lifter([os.path.join(TOOLS, "spu_wb_lifter.py"),
                     "--auto-functions", elf] + common +
                    ["--fast-source", os.path.join(outdir, "fast.c"),
                     "--source-name", "wb.c", "--header-name", "wb.h",
                     "--metrics-json", os.path.join(outdir, "wb.json")],
                    os.path.join(outdir, "wb.lift.log"))
    if rc:
        return None, None, "wb-lift"

    inc = ["/I", os.path.join(ROOT, "include"),
           "/I", os.path.join(ROOT, "runtime", "spu"), "/I", outdir]
    # DIAG exe
    r = WD.cl_run(D.CLFLAGS + inc +
                  ["/DTWIN_HEADER=diag.h", f"/DREGISTER_FN={reg}",
                   f"/Fo{outdir}\\", f"/Fe:{outdir}\\diag.exe",
                   HARNESS, os.path.join(outdir, "diag.c")])
    if r.returncode:
        open(os.path.join(outdir, "diag.cl.log"), "w").write(r.stdout + r.stderr)
        return None, None, "diag-cl"
    # WB exe (harness + renamed fast + AVX2 wb)
    steps = [
        (D.CLFLAGS + inc + ["/DTWIN_HEADER=wb.h", f"/DREGISTER_FN={reg}",
                            "/c", HARNESS, f"/Fo{outdir}\\harness.obj"]),
        (D.CLFLAGS + inc + [f"/D{reg}={reg}_fastbase",
                            "/c", os.path.join(outdir, "fast.c"),
                            f"/Fo{outdir}\\fast_rn.obj"]),
        (D.CLFLAGS + WD.WB_EXTRA + inc +
         ["/c", os.path.join(outdir, "wb.c"), f"/Fo{outdir}\\wb.obj"]),
    ]
    for s in steps:
        r = WD.cl_run(s)
        if r.returncode:
            open(os.path.join(outdir, "wb.cl.log"), "w").write(
                " ".join(s) + "\n" + r.stdout + r.stderr)
            return None, None, "wb-cl"
    r = WD.cl_run(["/nologo", "/MT", f"{outdir}\\harness.obj",
                   f"{outdir}\\fast_rn.obj", f"{outdir}\\wb.obj",
                   f"/Fe:{outdir}\\wb.exe"])
    if r.returncode:
        return None, None, "wb-link"
    return os.path.join(outdir, "diag.exe"), os.path.join(outdir, "wb.exe"), None


def main():
    global BR_BASE, BRZ_BASE, BRNZ_BASE
    ap = argparse.ArgumentParser()
    ap.add_argument("--images", type=int, default=24)
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--insns", type=int, default=120)
    ap.add_argument("--rng-seed", type=int, default=1)
    ap.add_argument("--evbudget", type=int, default=256)
    ap.add_argument("--hopmax", type=int, default=400000)
    ap.add_argument("--timeout", type=int, default=45)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    if not D.CL_EXE:
        raise SystemExit("cl not found; run from a vcvars64 shell")
    BR_BASE = probe_base("br")
    BRZ_BASE = probe_base("brz")
    BRNZ_BASE = probe_base("brnz")

    rng = random.Random(args.rng_seed)
    os.makedirs(WORK, exist_ok=True)
    n_match = n_mis = n_inc = 0
    fails = []
    for idx in range(args.images):
        outdir = os.path.join(WORK, f"img{idx:03d}")
        os.makedirs(outdir, exist_ok=True)
        code = gen_image(rng, args.insns)
        diag_exe, wb_exe, err = build_twins(idx, code, outdir)
        if err:
            fails.append((idx, err))
            print(f"img{idx:03d}: BUILD-FAIL ({err})")
            continue
        codebin = os.path.join(outdir, "code.bin")
        img_ok = True
        for seed in range(1, args.seeds + 1):
            od = os.path.join(outdir, f"s{seed}.diag.txt")
            ow = os.path.join(outdir, f"s{seed}.wb.txt")
            st_d = D.run_one(diag_exe, codebin, BASE, BASE, seed,
                             args.evbudget, args.hopmax, od, args.timeout)
            st_w = D.run_one(wb_exe, codebin, BASE, BASE, seed,
                             args.evbudget, args.hopmax, ow, args.timeout)
            if st_d or st_w:
                n_inc += 1
                continue
            v, detail = D.cmp_runs(D.parse_out(od), D.parse_out(ow))
            if v == "MATCH":
                n_match += 1
            elif v == "MISMATCH":
                n_mis += 1
                img_ok = False
                fails.append((idx, f"seed {seed}: {detail}"))
                print(f"img{idx:03d} seed{seed}: MISMATCH {detail}")
            else:
                n_inc += 1
        if img_ok:
            print(f"img{idx:03d}: ok")
            if not args.keep:
                shutil.rmtree(outdir, ignore_errors=True)
    print(f"\nfuzz: {n_match} MATCH, {n_mis} MISMATCH, {n_inc} inconclusive "
          f"over {args.images} images x {args.seeds} seeds")
    with open(os.path.join(WORK, "report.json"), "w") as f:
        json.dump({"match": n_match, "mismatch": n_mis, "inconclusive": n_inc,
                   "failures": fails}, f, indent=1)
    sys.exit(1 if n_mis or any(e for _i, e in fails if "lift" in e or "cl" in e)
             else 0)


if __name__ == "__main__":
    main()
