#!/usr/bin/env python3
"""s50 mandatory placement-collision regression (the 07-30 0x3B000/0x3BC00
orphanage case).

The diagnosed live failure: PC 0x3BC00 is BOTH offset 0xC00 inside the
orphanage placement based at 0x3B000 AND offset 0 (the prologue) of the
placement based at 0x3BC00; range-based image fallback picked the old
placement and skipped initialization. The static contract this test pins:

  1. both placements' generated twins LINK into one binary (namespace
     isolation: identical numeric PCs cannot collide),
  2. each placement's dispatch table maps 0x3BC00 to a DIFFERENT function,
  3. executed under the 0x3B000 identity, PC 0x3BC00 runs the interior code;
     under the 0x3BC00 identity it runs the prologue; the two runs must
     produce DIFFERENT architectural results,
  4. per identity, the instruction-pair binary and the region-pair binary
     must produce IDENTICAL results (the differential guarantee),
  5. table contents are independent of registration order,
  6. the forensic pc 0x3B218 resolves under the 0x3B000 identity and is
     NOENTRY under the 0x3BC00 identity.

The runtime residency SELECTION (which identity is active at a live launch)
is the main thread's exact-placement fix and is deliberately out of scope
here; this test pins everything below that seam. Run from a vcvars shell.
"""

import json
import os
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import gen_spu_regions as G           # noqa: E402
import spu_region_diff as D           # noqa: E402

WORK = os.path.join(ROOT, "scratch", "regionlift", "collision")
REPRO = os.path.join(ROOT, "scratch", "regionlift", "repro")
FAST = os.path.join(ROOT, "yakuza", "generated", "fast")
HARNESS = os.path.join(TOOLS, "spu_diff_harness.c")

BASE_A, BASE_B = 0x3B000, 0x3BC00
PC_COLLIDE, PC_FORENSIC = 0x3BC00, 0x3B218
REG_A = "spu_recomp_register_jobbin_orphanage_3b000"
REG_B = "spu_recomp_register_jobbin_orphanage_3bc00"

DRIVER = r'''
#define SPU_DIFF_NO_MAIN
#include "spu_diff_harness.c"
#include XSTR_(COLLISION_SECOND_HEADER)
void REG_B(void);

static void (*tabA[SPU_LS_SIZE / 4])(spu_context*);
static void (*tabB[SPU_LS_SIZE / 4])(spu_context*);
static void (*tabA2[SPU_LS_SIZE / 4])(spu_context*);
static void (*tabB2[SPU_LS_SIZE / 4])(spu_context*);
static spu_context g_c;

static int run_ident(const char* code, uint32_t base,
                     void (**tab)(spu_context*), uint32_t pc,
                     uint64_t seed, uint64_t hopmax, const char* outp)
{
    spu_diff_registry_install(tab);
    if (spu_diff_load_code(code, base) != 0) return 1;
    spu_diff_env_reset(seed);
    spu_diff_ctx_init(&g_c);
    const char* term = spu_diff_run(&g_c, pc, hopmax);
    FILE* o = fopen(outp, "w");
    if (!o) return 1;
    spu_diff_dump_single(o, term, &g_c);
    fclose(o);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 6) { fprintf(stderr, "usage: code.bin seed evb hopmax outdir\n"); return 2; }
    const char* code = argv[1];
    uint64_t seed = strtoull(argv[2], 0, 0);
    g_ev_budget = (uint32_t)strtoul(argv[3], 0, 0);
    uint64_t hopmax = strtoull(argv[4], 0, 0);
    const char* od = argv[5];
    char p[512];

    spu_diff_registry_clear(); REGISTER_FN(); spu_diff_registry_snapshot(tabA);
    spu_diff_registry_clear(); REG_B();       spu_diff_registry_snapshot(tabB);
    spu_diff_registry_clear(); REG_B();       spu_diff_registry_snapshot(tabB2);
    spu_diff_registry_clear(); REGISTER_FN(); spu_diff_registry_snapshot(tabA2);
    if (memcmp(tabA, tabA2, sizeof tabA) || memcmp(tabB, tabB2, sizeof tabB)) {
        printf("FAIL ORDER-DEPENDENT-TABLES\n"); return 4;
    }
    if (!tabA[0x3BC00u >> 2] || !tabB[0x3BC00u >> 2]) {
        printf("FAIL MISSING-COLLIDING-PC\n"); return 5;
    }
    if (tabA[0x3BC00u >> 2] == tabB[0x3BC00u >> 2]) {
        printf("FAIL NAMESPACE-COLLAPSE\n"); return 6;
    }
    printf("tables OK: order independent, 0x3BC00 dual-mapped to distinct code\n");

    snprintf(p, sizeof p, "%s\\identA_c.txt", od);
    if (run_ident(code, 0x3B000u, tabA, 0x3BC00u, seed, hopmax, p)) return 7;
    snprintf(p, sizeof p, "%s\\identB_c.txt", od);
    if (run_ident(code, 0x3BC00u, tabB, 0x3BC00u, seed, hopmax, p)) return 7;
    snprintf(p, sizeof p, "%s\\identA_f.txt", od);
    if (run_ident(code, 0x3B000u, tabA, 0x3B218u, seed, hopmax, p)) return 7;
    snprintf(p, sizeof p, "%s\\identB_f.txt", od);
    if (run_ident(code, 0x3BC00u, tabB, 0x3B218u, seed, hopmax, p)) return 7;
    printf("COLLISION-DRIVER-OK\n");
    return 0;
}
'''


def build(tag, twin_dir, hdr_a, hdr_b, src_a, src_b):
    os.makedirs(WORK, exist_ok=True)
    drv = os.path.join(WORK, f"collision_driver_{tag}.c")
    with open(drv, "w", newline="\n") as f:
        f.write(DRIVER)
    exe = os.path.join(WORK, f"collision_{tag}.exe")
    objd = os.path.join(WORK, f"obj_{tag}")
    os.makedirs(objd, exist_ok=True)
    if D.CL_EXE is None:
        raise SystemExit("cl not found; run from a vcvars64-initialized shell")
    cmd = ([D.CL_EXE] + D.CLFLAGS +
           ["/I", os.path.join(ROOT, "include"),
            "/I", os.path.join(ROOT, "runtime", "spu"), "/I", twin_dir,
            "/I", TOOLS,
            f"/DTWIN_HEADER={hdr_a}", f"/DCOLLISION_SECOND_HEADER={hdr_b}",
            f"/DREGISTER_FN={REG_A}", f"/DREG_B={REG_B}",
            f"/Fo{objd}\\", f"/Fe:{exe}", drv, src_a, src_b])
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        log = os.path.join(WORK, f"compile_{tag}.log")
        open(log, "w").write(r.stdout + "\n" + r.stderr)
        print(f"[collision] {tag} COMPILE FAIL -> {log}")
        for ln in (r.stdout + r.stderr).splitlines():
            if "error" in ln.lower():
                print("   " + ln)
        sys.exit(2)
    return exe


def main():
    manifest = json.load(open(os.path.join(TOOLS, "spu_region_manifest.json")))
    fam = manifest["families"]["orphanage"]
    code, _rb, _re = G.family_code(fam)
    os.makedirs(WORK, exist_ok=True)
    codebin = os.path.join(WORK, "orphanage_code.bin")
    with open(codebin, "wb") as f:
        f.write(code)

    exe_c = build("diag", REPRO,
                  "job_bin_orphanage_3b000.h", "job_bin_orphanage_3bc00.h",
                  os.path.join(REPRO, "job_bin_orphanage_3b000.c"),
                  os.path.join(REPRO, "job_bin_orphanage_3bc00.c"))
    exe_r = build("fast", FAST,
                  "job_bin_orphanage_3b000_fast.h", "job_bin_orphanage_3bc00_fast.h",
                  os.path.join(FAST, "job_bin_orphanage_3b000_fast.c"),
                  os.path.join(FAST, "job_bin_orphanage_3bc00_fast.c"))

    for tag, exe in (("diag", exe_c), ("fast", exe_r)):
        od = os.path.join(WORK, tag)
        os.makedirs(od, exist_ok=True)
        # hop budget must cover the instruction twin's cost of the bounded
        # validation loop at the interior entry (~60 dispatches per trip plus
        # nested calls, up to 64k trips from the masked seeds): a small budget
        # cuts the instruction twin mid-loop while the region twin finishes
        # in-region and reaches the heqi guard, which is a budget artifact,
        # not a divergence (diagnosed 07-30).
        r = subprocess.run([exe, codebin, "1", "384", "500000000", od],
                           capture_output=True, text=True, timeout=600)
        print(f"[collision] {tag}: rc={r.returncode} {r.stdout.strip()}")
        if r.returncode != 0:
            sys.exit(1)

    fails = []
    # (4) per identity+pc, diag must MATCH fast (identB_f is the NOENTRY leg,
    # asserted separately below)
    for leg in ("identA_c", "identB_c", "identA_f"):
        a = D.parse_out(os.path.join(WORK, "diag", leg + ".txt"))
        b = D.parse_out(os.path.join(WORK, "fast", leg + ".txt"))
        v, det = D.cmp_runs(a, b)
        print(f"[collision] {leg}: diag-vs-fast {v} {det} "
              f"(term={a['term']}/{b['term']} hops={a['hops']}/{b['hops']})")
        if v not in ("MATCH",):
            fails.append(f"{leg}: {v} {det}")
    # (3) identity A vs identity B at the colliding pc must DIFFER
    a = D.parse_out(os.path.join(WORK, "diag", "identA_c.txt"))
    b = D.parse_out(os.path.join(WORK, "diag", "identB_c.txt"))
    v, _det = D.cmp_runs(a, b)
    if v == "MATCH":
        fails.append("identity A and B produced IDENTICAL results at 0x3BC00 "
                     "(interior vs prologue must differ)")
    else:
        print("[collision] identity separation: interior(0x3B000) vs "
              f"prologue(0x3BC00) at PC 0x3BC00 differ as required ({v})")
    # (6) forensic pc under B must be NOENTRY
    fb = D.parse_out(os.path.join(WORK, "fast", "identB_f.txt"))
    if fb["term"] != "NOENTRY":
        fails.append(f"pc 0x3B218 under identity B: expected NOENTRY, got {fb['term']}")
    else:
        print("[collision] pc 0x3B218 under identity B is NOENTRY as required")

    if fails:
        print("[collision] FAIL:")
        for f_ in fails:
            print("   " + f_)
        sys.exit(1)
    print("[collision] PASS: link isolation, dual mapping, order independence, "
          "identity separation, and diag-vs-fast equivalence all hold")
    sys.exit(0)


if __name__ == "__main__":
    main()
