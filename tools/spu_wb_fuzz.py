#!/usr/bin/env python3
"""WB/WJ randomized-image fuzzer: random legal instruction sequences, lifted
as twins (DIAG instruction reference, FAST region twin, and the selected
optimizing lane), executed over the deterministic s50 harness environment
across seeds, all architectural state compared.

This covers optimizer edge cases real images cannot: random operand
register reuse, dense flush/branch interleavings, LS alias patterns, and
random immediates -- against the per-instruction reference translation.

Image shapes:
  flat (default for --lane wb): straight-line random data-processing words
    with occasional in-image forward conditional/unconditional branches,
    occasional `stop`s, and a final stop. No calls, no loops.
  cfg (default for --lane wj): additionally bounded counted loops
    (il/.../ai -1/brnz back-edge -- the carry-dataflow shape), brsl calls
    from the main region into subroutine regions returning via `bi $r0`
    (call-continuation and publish machinery), and random rdch/wrch channel
    operations mid-block (barrier windows). The link register $r0 is
    protected image-wide and each loop's counter within its body via a
    destination-register rejection filter, so calls return and loops
    terminate architecturally in both twins.

Instructions are REJECTION-SAMPLED through spu_disasm: a candidate 32-bit
word is kept only if it decodes to a whitelisted mnemonic -- the fuzzer
needs no encoder for the data-processing space, and control/channel words
are built from dynamically probed opcode bases whose field layout is
asserted against spu_disasm at startup (spu_disasm remains the single
source of decode truth).

Usage (vcvars64 shell):
    py -3 tools/spu_wb_fuzz.py --images 24 --seeds 3 [--keep]
    py -3 tools/spu_wb_fuzz.py --lane wj --images 200 --seeds 3
"""

import argparse
import json
import os
import random
import re
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


BR_BASE = None      # filled in init_encodings
BRZ_BASE = None
BRNZ_BASE = None
BRSL_BASE = None
IL_BASE = None
AI_BASE = None
BI_BASE = None
RDCH_BASE = None
WRCH_BASE = None

_OPR_REG = re.compile(r"\$r(\d+)")


def enc_ri16(basew, rt, i16):
    return basew | ((i16 & 0xFFFF) << 7) | (rt & 0x7F)


def enc_ri10(basew, rt, ra, i10):
    return basew | ((i10 & 0x3FF) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F)


def enc_rr_ra(basew, ra, rt=0):
    return basew | ((ra & 0x7F) << 7) | (rt & 0x7F)


def _check(word, mnemonic, want_regs):
    r = spu_disasm.spu_decode(word, 0)
    got = getattr(r, "mnemonic", None)
    regs = [int(x) for x in _OPR_REG.findall(getattr(r, "operands", "") or "")]
    if got != mnemonic or any(w not in regs for w in want_regs):
        raise SystemExit(f"encoding self-check failed: 0x{word:08X} -> "
                         f"{got} {getattr(r, 'operands', '')} "
                         f"(want {mnemonic} regs {want_regs})")


def init_encodings():
    global BR_BASE, BRZ_BASE, BRNZ_BASE, BRSL_BASE
    global IL_BASE, AI_BASE, BI_BASE, RDCH_BASE, WRCH_BASE
    BR_BASE = probe_base("br")
    BRZ_BASE = probe_base("brz")
    BRNZ_BASE = probe_base("brnz")
    BRSL_BASE = probe_base("brsl")
    IL_BASE = probe_base("il")
    AI_BASE = probe_base("ai") & 0xFF000000   # RI10: op8 only, clear I10 bits
    BI_BASE = probe_base("bi")
    RDCH_BASE = probe_base("rdch")
    WRCH_BASE = probe_base("wrch")
    # field-layout self-checks against spu_disasm (single source of truth)
    _check(enc_ri16(IL_BASE, 33, 5), "il", [33])
    _check(enc_ri10(AI_BASE, 33, 34, -1), "ai", [33, 34])
    _check(enc_ri16(BRSL_BASE, 0, 4), "brsl", [0])
    _check(enc_rr_ra(BI_BASE, 55), "bi", [55])
    _check(enc_rr_ra(RDCH_BASE, 3, 44), "rdch", [44])
    _check(enc_rr_ra(WRCH_BASE, 3, 44), "wrch", [44])


def dest_reg(word):
    """Destination register of a data-processing word, conservatively: the
    first register operand (stores/halts name a source first; treating it
    as a dest only over-rejects)."""
    r = spu_disasm.spu_decode(word, 0)
    ops = getattr(r, "operands", "") or ""
    first = ops.split(",")[0].strip()
    m = _OPR_REG.match(first)
    if m:
        return int(m.group(1))
    if first == "$lr":
        return 0
    if first == "$sp":
        return 1
    return None


def gen_word(rng):
    """One random whitelisted data-processing word (rejection sampling)."""
    while True:
        w = rng.getrandbits(32)
        r = spu_disasm.spu_decode(w, 0)
        mn = getattr(r, "mnemonic", None)
        if mn in DATA_MNEMONICS:
            return w


def gen_data_word(rng, protect):
    """Random data word that does not write any protected register."""
    while True:
        w = gen_word(rng)
        if dest_reg(w) not in protect:
            return w


def gen_image(rng, n_insns):
    """flat shape: random words + forward branches + stops. Returns bytes."""
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


# ---- cfg shape (WJ machinery: loops, calls, channels, barriers) -----------
#
# Token stream per body:
#   ("w", word)                        literal instruction word
#   ("br", base, rt, tgt)              adjustable branch to body slot `tgt`
#   ("brfix", base, rt, tgt)           loop back-edge (never re-targeted)
#   ("call", None)                     brsl placeholder, patched at assembly
#
# Adjustable forward branches that would land strictly inside a later
# loop's body (between il and its brnz) are clamped to the loop's il slot
# at encode time, so every path through a counted loop initializes its
# counter and the trip count stays architecturally bounded in both twins.

def gen_body(rng, n, protect, allow, calls_left):
    toks = []
    loops = []          # (il_slot, brnz_slot) body-relative
    i = 0
    while i < n:
        roll = rng.random()
        rem = n - i
        if allow.get("loop") and roll < 0.10 and rem >= 8:
            cnt = rng.randint(96, 126)
            while cnt in protect:
                cnt = rng.randint(96, 126)
            trips = rng.randint(2, 7)
            body_n = rng.randint(2, min(10, rem - 4))
            il_slot = i
            toks.append(("w", enc_ri16(IL_BASE, cnt, trips)))
            for _ in range(body_n):
                toks.append(("w", gen_data_word(rng, protect | {cnt})))
            toks.append(("w", enc_ri10(AI_BASE, cnt, cnt, -1)))
            brnz_slot = i + body_n + 2
            toks.append(("brfix", BRNZ_BASE, cnt, il_slot + 1))
            loops.append((il_slot, brnz_slot))
            i += body_n + 3
        elif allow.get("call") and calls_left[0] > 0 and roll < 0.14:
            toks.append(("call", None))
            calls_left[0] -= 1
            i += 1
        elif allow.get("branch") and roll < 0.20 and rem > 4:
            tgt = i + rng.randint(2, min(rem - 1, 24))
            kind = rng.random()
            rt = rng.randint(2, 127)
            if kind < 0.4:
                toks.append(("br", BR_BASE, 0, tgt))
            elif kind < 0.7:
                toks.append(("br", BRZ_BASE, rt, tgt))
            else:
                toks.append(("br", BRNZ_BASE, rt, tgt))
            i += 1
        elif allow.get("chan") and roll < 0.24:
            ch = rng.randint(0, 63)
            rt = rng.randint(2, 127)
            while rt in protect:
                rt = rng.randint(2, 127)
            base = RDCH_BASE if rng.random() < 0.6 else WRCH_BASE
            toks.append(("w", enc_rr_ra(base, ch, rt)))
            i += 1
        elif allow.get("stop") and roll < 0.26:
            toks.append(("w", 0x00000000 | rng.randint(0, 0x3FFF)))
            i += 1
        else:
            toks.append(("w", gen_data_word(rng, protect)))
            i += 1
    return toks, loops


def gen_image_cfg(rng, n_insns):
    """cfg shape: main region with loops/branches/channels/stops and brsl
    calls into 1-3 subroutine regions that return via `bi $r0`."""
    protect = {0}       # link register: calls must return, image-wide
    n_subs = rng.randint(1, 3)
    n_calls = rng.randint(n_subs, n_subs + 3)
    calls_left = [n_calls]

    main_allow = {"loop": True, "call": True, "branch": True,
                  "chan": True, "stop": True}
    sub_allow = {"loop": True, "branch": True, "chan": True}
    main_n = max(24, int(n_insns * 0.55))
    main_toks, main_loops = gen_body(rng, main_n, protect, main_allow,
                                     calls_left)
    # place any unplaced calls just before the terminal stop
    while calls_left[0] > 0:
        main_toks.append(("call", None))
        calls_left[0] -= 1
    main_toks.append(("w", 0x00000000 | 0x1234))    # terminal stop

    subs = []
    per_sub = max(10, (n_insns - main_n) // n_subs)
    for _ in range(n_subs):
        st, sl = gen_body(rng, per_sub, protect, sub_allow, [0])
        st.append(("w", enc_rr_ra(BI_BASE, 0)))     # return via link
        subs.append((st, sl))

    # global slot layout: main, then subs in order
    bodies = [(main_toks, main_loops, 0)]
    off = len(main_toks)
    sub_entries = []
    for st, sl in subs:
        sub_entries.append(off)
        bodies.append((st, sl, off))
        off += len(st)

    # round-robin over a shuffled sub order: every sub called at least once
    order = list(range(n_subs))
    rng.shuffle(order)
    call_targets = [sub_entries[order[k % n_subs]] for k in range(n_calls)]

    words = []
    call_no = 0
    for toks, loops, base_off in bodies:
        interiors = [(base_off + a, base_off + b) for a, b in loops]
        for local_slot, tok in enumerate(toks):
            s = base_off + local_slot
            if tok[0] == "w":
                words.append(tok[1])
            elif tok[0] in ("br", "brfix"):
                _k, bw, rt, tgt_local = tok
                tgt = base_off + tgt_local
                if tok[0] == "br":
                    for il, brnz in interiors:
                        if il < tgt <= brnz:
                            tgt = il
                            break
                delta = tgt - s
                assert -0x8000 <= delta <= 0x7FFF
                words.append(enc_ri16(bw, rt, delta))
            else:   # call
                tgt = call_targets[call_no]
                call_no += 1
                delta = tgt - s
                assert -0x8000 <= delta <= 0x7FFF
                words.append(enc_ri16(BRSL_BASE, 0, delta))
    assert call_no == n_calls
    return b"".join(w.to_bytes(4, "big") for w in words)


def run_lifter(argv, log):
    with open(log, "w") as lf:
        return subprocess.run([sys.executable] + argv, stdout=lf,
                              stderr=subprocess.STDOUT, cwd=ROOT).returncode


def build_twins(idx, code, outdir, lane, shape):
    """DIAG (per-instruction), FAST (region), and the optimizing lane twin
    (wb or wj). Returns (diag_exe, lane_exe, err)."""
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
    # DIAG twin (per-instruction reference). flat: whole-image single lift
    # (historical shape). cfg: function detection, so brsl call targets are
    # lifted bodies rather than unregistered indirect stubs.
    if shape == "cfg":
        diag_in = ["--auto-functions", elf]
    else:
        diag_in = [raw, "--base", hex(BASE)]
    rc = run_lifter([os.path.join(TOOLS, "spu_lifter.py")] + diag_in + common +
                    ["--source-name", "diag.c", "--header-name", "diag.h"],
                    os.path.join(outdir, "diag.lift.log"))
    if rc:
        return None, None, "diag-lift"
    # FAST twin (fallback surface for the optimizing lane)
    rc = run_lifter([os.path.join(TOOLS, "spu_lifter.py"),
                     "--auto-functions", elf, "--regions"] + common +
                    ["--source-name", "fast.c", "--header-name", "fast.h"],
                    os.path.join(outdir, "fast.lift.log"))
    if rc:
        return None, None, "fast-lift"
    # optimizing lane twin
    if lane == "wj":
        rc = run_lifter([os.path.join(TOOLS, "spu_wj_lifter.py"),
                         "--auto-functions", elf] + common +
                        ["--fast-source", os.path.join(outdir, "fast.c"),
                         "--source-name", "wj.c", "--header-name", "wj.h",
                         "--metrics-json", os.path.join(outdir, "wj.json")],
                        os.path.join(outdir, "wj.lift.log"))
    else:
        rc = run_lifter([os.path.join(TOOLS, "spu_wb_lifter.py"),
                         "--auto-functions", elf] + common +
                        ["--fast-source", os.path.join(outdir, "fast.c"),
                         "--source-name", "wb.c", "--header-name", "wb.h",
                         "--metrics-json", os.path.join(outdir, "wb.json")],
                        os.path.join(outdir, "wb.lift.log"))
    if rc:
        return None, None, f"{lane}-lift"

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
    # lane exe (harness + renamed fast + AVX2 lane TU), production link shape
    steps = [
        (D.CLFLAGS + inc + [f"/DTWIN_HEADER={lane}.h", f"/DREGISTER_FN={reg}",
                            "/c", HARNESS, f"/Fo{outdir}\\harness.obj"]),
        (D.CLFLAGS + inc + [f"/D{reg}={reg}_fastbase",
                            "/c", os.path.join(outdir, "fast.c"),
                            f"/Fo{outdir}\\fast_rn.obj"]),
        (D.CLFLAGS + WD.WB_EXTRA + inc +
         ["/c", os.path.join(outdir, f"{lane}.c"),
          f"/Fo{outdir}\\{lane}.obj"]),
    ]
    for s in steps:
        r = WD.cl_run(s)
        if r.returncode:
            open(os.path.join(outdir, f"{lane}.cl.log"), "w").write(
                " ".join(s) + "\n" + r.stdout + r.stderr)
            return None, None, f"{lane}-cl"
    r = WD.cl_run(["/nologo", "/MT", f"{outdir}\\harness.obj",
                   f"{outdir}\\fast_rn.obj", f"{outdir}\\{lane}.obj",
                   f"/Fe:{outdir}\\{lane}.exe"])
    if r.returncode:
        return None, None, f"{lane}-link"
    return (os.path.join(outdir, "diag.exe"),
            os.path.join(outdir, f"{lane}.exe"), None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lane", choices=("wb", "wj"), default="wb")
    ap.add_argument("--shape", choices=("flat", "cfg"), default=None,
                    help="image shape (default: cfg for --lane wj, else flat)")
    ap.add_argument("--images", type=int, default=24)
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--insns", type=int, default=120)
    ap.add_argument("--rng-seed", type=int, default=1)
    ap.add_argument("--evbudget", type=int, default=256)
    ap.add_argument("--hopmax", type=int, default=400000)
    ap.add_argument("--timeout", type=int, default=45)
    ap.add_argument("--start", type=int, default=0,
                    help="first image index (parallel slices / resume)")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    if not D.CL_EXE:
        raise SystemExit("cl not found; run from a vcvars64 shell")
    shape = args.shape or ("cfg" if args.lane == "wj" else "flat")
    work = os.path.join(ROOT, "scratch", f"{args.lane}fuzz")
    init_encodings()

    os.makedirs(work, exist_ok=True)
    n_match = n_mis = n_inc = 0
    fails = []
    for idx in range(args.start, args.start + args.images):
        # image content is a pure function of (rng_seed, idx, shape)
        rng = random.Random((args.rng_seed << 20) ^ idx)
        outdir = os.path.join(work, f"img{idx:03d}")
        os.makedirs(outdir, exist_ok=True)
        code = (gen_image_cfg if shape == "cfg" else gen_image)(rng, args.insns)
        diag_exe, lane_exe, err = build_twins(idx, code, outdir, args.lane,
                                              shape)
        if err:
            fails.append((idx, err))
            print(f"img{idx:03d}: BUILD-FAIL ({err})", flush=True)
            continue
        codebin = os.path.join(outdir, "code.bin")
        img_ok = True
        for seed in range(1, args.seeds + 1):
            od = os.path.join(outdir, f"s{seed}.diag.txt")
            ow = os.path.join(outdir, f"s{seed}.{args.lane}.txt")
            st_d = D.run_one(diag_exe, codebin, BASE, BASE, seed,
                             args.evbudget, args.hopmax, od, args.timeout)
            st_w = D.run_one(lane_exe, codebin, BASE, BASE, seed,
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
                print(f"img{idx:03d} seed{seed}: MISMATCH {detail}", flush=True)
            else:
                n_inc += 1
        if img_ok:
            print(f"img{idx:03d}: ok", flush=True)
            if not args.keep:
                shutil.rmtree(outdir, ignore_errors=True)
    print(f"\nfuzz[{args.lane}/{shape}]: {n_match} MATCH, {n_mis} MISMATCH, "
          f"{n_inc} inconclusive over {args.images} images x {args.seeds} seeds")
    rep = os.path.join(work, f"report_{args.start}_{args.images}.json"
                       if args.start else "report.json")
    with open(rep, "w") as f:
        json.dump({"lane": args.lane, "shape": shape, "start": args.start,
                   "match": n_match, "mismatch": n_mis, "inconclusive": n_inc,
                   "failures": fails}, f, indent=1)
    sys.exit(1 if n_mis or any(e for _i, e in fails if "lift" in e or "cl" in e)
             else 0)


if __name__ == "__main__":
    main()
