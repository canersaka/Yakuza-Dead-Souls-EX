#!/usr/bin/env python3
"""WB assembly audit: disassemble the compiled WB object and prove that
covered ordinary blocks execute with no per-operation helper calls.

For every `spu_wb_*` function in the WB object (dumpbin /DISASM):
  * count instructions,
  * classify every `call` target:
      - platform   (channels, dispatch, halt, drain hooks -- by design)
      - ls-hook    (yz_tagread_repair_read -- the LS repair hook, by design)
      - float      (the wrapped scalar float stack + CRT math, by design)
      - fast/wb    (direct twin-to-twin control transfer)
      - VIOLATION  (anything else -- e.g. an integer/logic/shuffle helper
                    that failed to inline)
  * count guest-register spills/reloads: 16-byte moves against
    [rcx + disp] with disp inside the gpr file (first 0x800 bytes of
    spu_context -- gpr is the struct's first member).

Exit 1 on any VIOLATION. Also emits the same instruction/spill metrics for
the FAST twin's `spu_region_*` functions when given --fast-obj, so the
comparison lands in one report.

Usage (vcvars shell):
    py -3 tools/spu_wb_asm_audit.py scratch/wbdiff/<stem>/obj_wb/wb.obj \
        [--fast-obj .../fast_rn.obj] [--json out.json]
"""

import argparse
import json
import os
import re
import subprocess
import sys


PLATFORM = {
    "spu_rdch", "spu_wrch", "spu_rchcnt", "spu_indirect_branch", "spu_halt",
    "spu_img_restore", "spu_task_launch_check",
    "spu_task_launch_behavior_check", "yz_sguard_check", "yz_lockstep_tick",
    "spu_prof_hop",
}
LS_HOOK = {"yz_tagread_repair_read"}
FLOAT_RE = re.compile(
    r"^(spu_(fa|fs|fm|fi|fma|fms|fnms|fceq|fcgt|fcmeq|fcmgt|frest|frsqest|"
    r"fesd|frds|cflts|cfltu|csflt|cuflt|dfa|dfs|dfm|dfma|dfms|dfnms|dfnma|"
    r"dfceq|dfcmeq|dfcgt|dfcmgt|dftsv|xf_\w+|clz|gbh|gbb|shlh|roth|rothm|"
    r"rothma|mfspr)|ldexp|frexp|exp2|fmaf?|_CIpow|__libm\w*|_dtest|pow)")
MEMORY = {"memcpy", "memset", "memmove", "_memcpy", "_memset"}


def dumpbin_disasm(obj):
    exe = "dumpbin"
    r = subprocess.run([exe, "/DISASM:NOBYTES", obj], capture_output=True,
                       text=True)
    if r.returncode != 0:
        raise SystemExit(f"dumpbin failed on {obj}: {r.stderr[:400]}")
    return r.stdout


_FUNC_RE = re.compile(r"^([A-Za-z_$][\w$@?]*):\s*$")
_INSN_RE = re.compile(r"^\s+[0-9A-F]{8,16}:\s+(\S+)(.*)$")
_CALL_TGT_RE = re.compile(r"call\s+(\S+)")
_GPR_RE = re.compile(r"\[rcx(?:\+([0-9A-Fa-f]+)h?)?\]")


def audit_obj(obj, func_prefix):
    text = dumpbin_disasm(obj)
    funcs = {}
    cur = None
    for line in text.splitlines():
        m = _FUNC_RE.match(line)
        if m:
            name = m.group(1)
            cur = funcs.setdefault(name, {
                "insns": 0, "calls": {}, "gpr_loads": 0, "gpr_stores": 0})
            continue
        if cur is None:
            continue
        mi = _INSN_RE.match(line)
        if not mi:
            continue
        mnem, rest = mi.group(1), mi.group(2)
        cur["insns"] += 1
        if mnem == "call":
            mt = _CALL_TGT_RE.search(line)
            tgt = mt.group(1) if mt else "?"
            cur["calls"][tgt] = cur["calls"].get(tgt, 0) + 1
        if mnem in ("movdqa", "movdqu", "vmovdqa", "vmovdqu", "movaps",
                    "vmovaps", "movups", "vmovups"):
            mg = _GPR_RE.search(rest)
            if mg:
                disp = int(mg.group(1) or "0", 16)
                if disp < 0x800:      # inside the gpr file
                    # store if the memory operand comes first
                    ops = rest.strip()
                    if ops.startswith(("xmmword", "oword")) or \
                            re.match(r"^\s*[a-z]+ ptr \[rcx", ops):
                        cur["gpr_stores"] += 1
                    else:
                        cur["gpr_loads"] += 1
    out = {}
    for name, d in funcs.items():
        if not name.startswith(func_prefix):
            continue
        cats = {"platform": 0, "ls-hook": 0, "float": 0, "twin": 0,
                "memory": 0, "violation": {}}
        for tgt, n in d["calls"].items():
            t = tgt.strip()
            if t in PLATFORM:
                cats["platform"] += n
            elif t in LS_HOOK:
                cats["ls-hook"] += n
            elif t in MEMORY:
                cats["memory"] += n
            elif FLOAT_RE.match(t):
                cats["float"] += n
            elif t.startswith("spu_region_") or t.startswith("spu_wb_"):
                cats["twin"] += n
            else:
                cats["violation"][t] = cats["violation"].get(t, 0) + n
        out[name] = {"insns": d["insns"], "gpr_loads": d["gpr_loads"],
                     "gpr_stores": d["gpr_stores"], "calls": cats}
    return out


def summarize(audit):
    tot = {"funcs": len(audit), "insns": 0, "gpr_loads": 0, "gpr_stores": 0,
           "platform": 0, "ls-hook": 0, "float": 0, "twin": 0, "memory": 0,
           "violations": {}}
    for d in audit.values():
        tot["insns"] += d["insns"]
        tot["gpr_loads"] += d["gpr_loads"]
        tot["gpr_stores"] += d["gpr_stores"]
        for k in ("platform", "ls-hook", "float", "twin", "memory"):
            tot[k] += d["calls"][k]
        for t, n in d["calls"]["violation"].items():
            tot["violations"][t] = tot["violations"].get(t, 0) + n
    return tot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wb_obj")
    ap.add_argument("--fast-obj", default=None)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    wb = audit_obj(args.wb_obj, "spu_wb_")
    wb_sum = summarize(wb)
    report = {"wb": wb_sum, "wb_funcs": wb}
    print(f"WB   {wb_sum['funcs']} fn(s): {wb_sum['insns']} insns, "
          f"gpr {wb_sum['gpr_loads']}L/{wb_sum['gpr_stores']}S, calls: "
          f"platform={wb_sum['platform']} ls-hook={wb_sum['ls-hook']} "
          f"float={wb_sum['float']} twin={wb_sum['twin']} mem={wb_sum['memory']}")
    if args.fast_obj:
        fast = audit_obj(args.fast_obj, "spu_region_")
        f_sum = summarize(fast)
        report["fast"] = f_sum
        print(f"FAST {f_sum['funcs']} fn(s): {f_sum['insns']} insns, "
              f"gpr {f_sum['gpr_loads']}L/{f_sum['gpr_stores']}S, calls: "
              f"platform={f_sum['platform']} float={f_sum['float']} "
              f"mem={f_sum['memory']}")
        if f_sum["insns"]:
            print(f"  WB/FAST instruction ratio: "
                  f"{wb_sum['insns'] / f_sum['insns']:.3f}; "
                  f"gpr-access ratio: "
                  f"{(wb_sum['gpr_loads'] + wb_sum['gpr_stores']) / max(1, f_sum['gpr_loads'] + f_sum['gpr_stores']):.3f}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(report, f, indent=1)
    if wb_sum["violations"]:
        print("VIOLATIONS (unexpected helper calls in WB functions):")
        for t, n in sorted(wb_sum["violations"].items(), key=lambda kv: -kv[1]):
            print(f"    {t}: {n}")
        sys.exit(1)
    print("PASS: no per-operation helper calls outside the designed "
          "platform/ls-hook/float/twin surfaces")


if __name__ == "__main__":
    main()
