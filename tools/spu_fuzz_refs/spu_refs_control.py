#!/usr/bin/env python3
"""SPU fuzz reference plug-in -- control family (nop, lnop, mfspr).

Clean-room refs transcribed from SPU_ISA_v1.2_27Jan2007_pub.pdf Section 10
"Control Instructions" (pages 236-246) RTL/prose:
    nop    -- No Operation (Execute)             (ISA p241, op11 0x201, RR)
    lnop   -- No Operation (Load)                (ISA p240, op11 0x001, RR)
    mfspr  -- Move From Special-Purpose Register (ISA p244, op11 0x00C, RR)

DELIBERATELY SKIPPED (pure control-flow / no register-level "effect" to
fuzz through this RR-data-op harness -- the static suite's own docstring
already excludes this class, "Deliberately excluded: ... stop/branch/link
control flow (F18/F19)"):
    stop, stopd  -- ISA p238/239: "Execution of the program in the SPU
                    stops" -- sets ctx->status and RETURNS (see
                    tools/spu_lifter.py ~line 346/373); there is no RT to
                    compare, only a control-flow/halt effect.
    sync, dsync  -- ISA p242/243: memory-ordering barriers with NO
                    architectural register effect at all (lifter emits
                    "/* sync */;", a bare no-op statement -- there is
                    nothing for a register-comparing harness to check).
    mtspr        -- ISA p245: writes RT into SPR(SA); mtspr's own operand
                    IS the source, so an rr1-style ref would need to
                    observe an SPR bank the fuzzer/runtime doesn't expose
                    (the lifter emits "/* mtspr: SPR write ignored */;" --
                    genuinely nothing to compare against RT/register state
                    from this harness's rr1/rr/... contract).

mfspr rt, sa (ISA p244 RTL):
    if defined(SPR(SA)) then RT <- SPR(SA) else RT <- 0
No SPU special-purpose register is architecturally defined by this ISA
revision (the note under mtspr/mfspr says "If SPR SA is not defined, zeros
are supplied"), and the runtime (runtime/spu/spu_helpers.h spu_mfspr) does
not model any SPR bank -- so the correct, spec-faithful result for EVERY
SA is the all-zero quadword. This IS the full ISA contract, not a stand-in:
an implementation with no defined SPRs must return 0 per p244.
"""

from spu_refs_api import register


# --- nop / lnop (ISA p240-241) ----------------------------------------------
# "This instruction has no effect on the execution of the program." Both are
# no-ops in the lifter ("/* nop */;" -- see tools/spu_lifter.py). Neither
# reads RA/RB (the ISA bit diagram marks those fields "///", reserved) nor
# writes RT (nop's own page: "RT is a false target ... False targets are not
# written"), so the only faithful expectation is that RT comes back UNCHANGED.
# We (ab)use the "rrt" form purely to get RT wired as a real input the driver
# seeds and the ref can echo -- RA/RB are drawn but architecturally inert, so
# the ref legitimately ignores them.
def ref_nop(a, b, t):
    return list(t)


register("nop", "rrt", ref_nop, op11=0x201)


def ref_lnop(a, b, t):
    return list(t)


register("lnop", "rrt", ref_lnop, op11=0x001)


# --- mfspr (ISA p244) --------------------------------------------------------
# RT <- SPR(SA) if defined, else RT <- 0. No SPR is defined -> always 0.
def ref_mfspr(a):
    return [0, 0, 0, 0]


register("mfspr", "rr1", ref_mfspr, op11=0x00C)
