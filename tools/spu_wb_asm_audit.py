#!/usr/bin/env python3
"""WB assembly audit: disassemble the compiled WB translation unit and prove
that covered ordinary blocks execute with no per-operation helper calls.

The TU is recompiled here WITHOUT /bigobj (dumpbin drops function-name
labels from extended-COFF disassembly; the audit compile is otherwise
flag-identical to the differential build). For every `spu_wb_*` function:

  * instruction count,
  * every `call` target classified:
      platform  (channels, dispatch, halt, drain hooks -- by design)
      ls-hook   (yz_tagread_repair_read -- the LS repair hook, by design)
      float     (the wrapped scalar float stack + CRT math, by design)
      twin      (direct FAST/WB control transfers)
      memory    (memcpy/memset intrinsic outlining)
      VIOLATION (anything else -- e.g. an integer/logic/shuffle helper
                 that failed to inline)
  * guest-register reloads/spills: 16-byte moves against [reg+disp] with
    disp < 0x800 (the gpr file is spu_context's first member), stack
    bases excluded.

Exit 1 on any VIOLATION. --fast-source additionally audits the FAST twin
(`spu_region_*` functions, baseline flags) for the comparison row.

Usage (vcvars64 shell):
    py -3 tools/spu_wb_asm_audit.py --stem job_bin_a_e400 [--json out.json]
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import spu_region_diff as D      # noqa: E402

WB = os.path.join(ROOT, "yakuza", "generated", "wb")
FAST = os.path.join(ROOT, "yakuza", "generated", "fast")

PLATFORM = {
    "spu_rdch", "spu_wrch", "spu_rchcnt", "spu_indirect_branch", "spu_halt",
    "spu_img_restore", "spu_task_launch_check",
    "spu_task_launch_behavior_check", "yz_sguard_check", "yz_lockstep_tick",
    "spu_prof_hop", "spu_arch_fence", "spu_return_needs_dispatch",
}
LS_HOOK = {"yz_tagread_repair_read", "wbk_ls_read", "wbk_ls_write",
           "spu_ls_read128", "spu_ls_write128"}   # the canonical LS surface
# Scalar-wrapped WB kernels (the WBK_WRAP list in spu_wb_simd.h): outlining
# these is fine -- they ARE the proven scalar paths. A native kernel showing
# up as a call is a violation (it failed to inline).
WRAPPED_WBK = re.compile(
    r"^wbk_(fa|fs|fm|fi|fma|fms|fnms|fceq|fcgt|fcmeq|fcmgt|frest|frsqest|"
    r"fesd|frds|cflts|cfltu|csflt|cuflt|dfa|dfs|dfm|dfma|dfms|dfnms|dfnma|"
    r"dfceq|dfcmeq|dfcgt|dfcmgt|dftsv|clz|gbh|gbb|shlh|roth|rothm|rothma|"
    r"mfspr)$")
# register-indirect calls: the SPU_DRAIN loop's `_tf(ctx)` dispatch (the
# same construct the FAST twins compile to)
_INDIRECT_RE = re.compile(r"^(r\w{1,3}|qword)$")   # reg or `call qword ptr [...]`
FLOAT_RE = re.compile(
    r"^(spu_(fa|fs|fm|fi|fma|fms|fnms|fceq|fcgt|fcmeq|fcmgt|frest|frsqest|"
    r"fesd|frds|cflts|cfltu|csflt|cuflt|dfa|dfs|dfm|dfma|dfms|dfnms|dfnma|"
    r"dfceq|dfcmeq|dfcgt|dfcmgt|dftsv|xf_\w+|clz\w*|gbh|gbb|shlh|roth|rothm|"
    r"rothma|mfspr)\b|ldexp|frexp|exp2|fmaf?$|pow|_CI\w+|__libm\w*|_dtest)")
MEMORY = {"memcpy", "memset", "memmove",
          "__chkstk"}   # MSVC stack probe for >4KB frames -- compiler runtime

_FLAGS_NOBIGOBJ = [f for f in D.CLFLAGS if f != "/bigobj"]


def compile_for_audit(src, out_obj, extra):
    inc = ["/I", os.path.join(ROOT, "include"),
           "/I", os.path.join(ROOT, "runtime", "spu"),
           "/I", WB, "/I", FAST,
           "/I", os.path.join(ROOT, "yakuza", "generated", "wj")]
    cmd = [D.CL_EXE] + _FLAGS_NOBIGOBJ + extra + inc + \
        ["/c", src, f"/Fo{out_obj}"]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        raise SystemExit(f"audit compile failed:\n{r.stdout}\n{r.stderr}")


_FUNC_RE = re.compile(r"^([A-Za-z_][\w@?]*):\s*$")     # '$'-labels excluded
_INSN_RE = re.compile(r"^\s+[0-9A-F]{16}:\s+(\S+)\s*(.*)$")
_CALL_TGT_RE = re.compile(r"call\s+(\S+)")
_MEM16_RE = re.compile(r"(xmmword|ymmword|oword) ptr \[(r\w+)(?:\+([0-9A-Fa-f]+)h?)?\]")


def audit_obj(obj, func_prefix):
    r = subprocess.run(["dumpbin", "/DISASM:NOBYTES", obj],
                       capture_output=True, text=True)
    if "File Type" not in r.stdout:
        raise SystemExit(f"dumpbin failed on {obj}: {r.stderr[:300]}")
    funcs = {}
    cur = None
    for line in r.stdout.splitlines():
        m = _FUNC_RE.match(line)
        if m:
            cur = funcs.setdefault(m.group(1), {
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
            tgt = (mt.group(1) if mt else "?").strip()
            cur["calls"][tgt] = cur["calls"].get(tgt, 0) + 1
        if mnem in ("movdqa", "movdqu", "vmovdqa", "vmovdqu", "movaps",
                    "vmovaps", "movups", "vmovups"):
            mm = _MEM16_RE.search(rest)
            if mm and mm.group(2) not in ("rsp", "rbp", "rip"):
                disp = int(mm.group(3) or "0", 16)
                if disp < 0x800:
                    if rest.strip().startswith(mm.group(1)):
                        cur["gpr_stores"] += 1
                    else:
                        cur["gpr_loads"] += 1
    out = {}
    for name, d in funcs.items():
        if not name.startswith(func_prefix):
            continue
        cats = {"platform": 0, "ls-hook": 0, "float": 0, "twin": 0,
                "memory": 0, "dispatch-indirect": 0, "violation": {}}
        for tgt, n in d["calls"].items():
            if tgt in PLATFORM:
                cats["platform"] += n
            elif tgt in LS_HOOK:
                cats["ls-hook"] += n
            elif tgt in MEMORY:
                cats["memory"] += n
            elif FLOAT_RE.match(tgt) or WRAPPED_WBK.match(tgt):
                cats["float"] += n
            elif tgt.startswith(("spu_region_", "spu_wb_", "spu_wj_", "spu_func_")):
                cats["twin"] += n
            elif _INDIRECT_RE.match(tgt):
                cats["dispatch-indirect"] += n
            else:
                cats["violation"][tgt] = cats["violation"].get(tgt, 0) + n
        out[name] = {"insns": d["insns"], "gpr_loads": d["gpr_loads"],
                     "gpr_stores": d["gpr_stores"], "calls": cats}
    return out


def summarize(audit):
    tot = {"funcs": len(audit), "insns": 0, "gpr_loads": 0, "gpr_stores": 0,
           "platform": 0, "ls-hook": 0, "float": 0, "twin": 0, "memory": 0,
           "dispatch-indirect": 0, "violations": {}}
    for d in audit.values():
        tot["insns"] += d["insns"]
        tot["gpr_loads"] += d["gpr_loads"]
        tot["gpr_stores"] += d["gpr_stores"]
        for k in ("platform", "ls-hook", "float", "twin", "memory",
                  "dispatch-indirect"):
            tot[k] += d["calls"][k]
        for t, n in d["calls"]["violation"].items():
            tot["violations"][t] = tot["violations"].get(t, 0) + n
    return tot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stem", required=True)
    ap.add_argument("--lane", choices=("wb", "wj"), default="wb")
    ap.add_argument("--no-fast", action="store_true")
    ap.add_argument("--json", default=None)
    ap.add_argument("--workdir", default=None)
    args = ap.parse_args()
    if not D.CL_EXE:
        raise SystemExit("cl not found; run from a vcvars64 shell")
    lane_dir = WB if args.lane == "wb" else os.path.join(
        ROOT, "yakuza", "generated", "wj")
    lane_sfx = "_wb" if args.lane == "wb" else "_wj"
    lane_pfx = "spu_wb_" if args.lane == "wb" else "spu_wj_"
    wd = args.workdir or os.path.join(ROOT, "scratch",
                                      "wbaudit" if args.lane == "wb" else "wjaudit",
                                      args.stem)
    os.makedirs(wd, exist_ok=True)

    wb_obj = os.path.join(wd, "wb_audit.obj")
    compile_for_audit(os.path.join(lane_dir, f"{args.stem}{lane_sfx}.c"), wb_obj,
                      ["/arch:AVX2", "/DYZ_SPU_SIMD_LS128=1",
                       "/DYZ_SPU_SIMD_SHUFB=1"])
    wb = audit_obj(wb_obj, lane_pfx)
    wb_sum = summarize(wb)
    report = {"stem": args.stem, "wb": wb_sum, "wb_funcs": wb}
    print(f"WB   {wb_sum['funcs']} fn(s): {wb_sum['insns']} insns, "
          f"gpr {wb_sum['gpr_loads']}L/{wb_sum['gpr_stores']}S, calls: "
          f"platform={wb_sum['platform']} ls-hook={wb_sum['ls-hook']} "
          f"float={wb_sum['float']} twin={wb_sum['twin']} mem={wb_sum['memory']}")
    if not args.no_fast:
        fast_obj = os.path.join(wd, "fast_audit.obj")
        compile_for_audit(os.path.join(FAST, f"{args.stem}_fast.c"), fast_obj, [])
        fast = audit_obj(fast_obj, "spu_region_")
        f_sum = summarize(fast)
        report["fast"] = f_sum
        print(f"FAST {f_sum['funcs']} fn(s): {f_sum['insns']} insns, "
              f"gpr {f_sum['gpr_loads']}L/{f_sum['gpr_stores']}S, calls: "
              f"platform={f_sum['platform']} float={f_sum['float']} "
              f"mem={f_sum['memory']}")
        if f_sum["insns"]:
            gwb = wb_sum["gpr_loads"] + wb_sum["gpr_stores"]
            gfa = f_sum["gpr_loads"] + f_sum["gpr_stores"]
            print(f"  WB/FAST host-instruction ratio: "
                  f"{wb_sum['insns'] / f_sum['insns']:.3f}; "
                  f"gpr-access ratio: {gwb / max(1, gfa):.3f}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(report, f, indent=1)
    if wb_sum["violations"]:
        print("VIOLATIONS (unexpected calls in WB functions):")
        for t, n in sorted(wb_sum["violations"].items(), key=lambda kv: -kv[1]):
            print(f"    {t}: {n}")
        sys.exit(1)
    print("PASS: no per-operation helper calls outside the designed "
          "platform/ls-hook/float/twin surfaces")


if __name__ == "__main__":
    main()
