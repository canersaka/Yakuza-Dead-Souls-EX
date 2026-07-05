#!/usr/bin/env python3
"""SPU fuzz reference plug-in -- MEMORY family (quadword load/store).

Group: memory. Mnemonics: lqd, lqx, lqa, lqr, stqd, stqx, stqa, stqr.
SUBTLE family (byte-order / quadword / preferred-slot sensitive).

Clean-room sources on disk:
  * SPU_ISA_v1.2_27Jan2007_pub.pdf  -- per-instruction RTL pseudocode.
  * SPU_Assembly_Language_Spec_1.5.pdf -- Table 2-2 (instructions), the
    immediate forms, and the effective-address construction per mnemonic.

--------------------------------------------------------------------------
WHY THESE REFS LOOK TRIVIAL -- a MEASURED constraint of the harness, not an
oversight.  Read this before trusting a GREEN result on this family.
--------------------------------------------------------------------------
The fuzz harness (tools/test_spu_lift.py) has NO observable local-store (LS)
model on the DATA path:

  * emit_driver() does `memset(ctx, 0, sizeof(*ctx))` per case -- so LS
    (ctx->ls[256KB]) is ALL ZERO -- and it NEVER seeds LS.
  * It seeds only ctx->gpr[r] registers and compares only ctx->gpr[RT]
    (the four result words) afterward. LS bytes are never read back.
  * Each case is ONE lifted instruction (lift_function(insns, addr, addr+4)),
    so a store cannot be paired with a following load to observe the bytes.

Consequences, per the actual lifted C (tools/spu_lifter.py lines 510-525) and
the LS accessors (runtime/spu/spu_context.h spu_ls_read128/write128):

  LOADS  (lqd/lqx/lqa/lqr): `rt = spu_ls_read128(ctx, <addr>)`. With LS all
  zero and the address masked into [0, 0x3FFF0] by (SPU_LS_MASK & ~0xF), the
  loaded quadword is [0,0,0,0] for EVERY in-range address. So the only thing
  a register-observing ref can assert is: the load actually OVERWRITES RT
  (i.e. was NOT mis-lifted to a no-op leaving the RT canary intact). That is a
  real regression check (a no-op lift would leave RT = 0xA5A5A5A5.. != 0 and
  FAIL), but it does NOT exercise byte order, displacement scaling, or the
  address arithmetic -- those need nonzero, address-dependent LS contents the
  harness cannot provide.  ref -> [0,0,0,0].

  STORES (stqd/stqx/stqa/stqr): `spu_ls_write128(ctx, <addr>, rt)` -- the
  store does NOT modify RT. So the only register-observable assertion is that
  RT is left UNCHANGED (the store did not clobber/read-modify RT, and was not
  mis-lifted into a load). To assert that, RT must be SEEDED as an input so
  the ref can return the same value (identity). That requires an RT-reading
  FORM:
     stqx  -> RR   encoding -> use form "rrt"   (seeds RA,RB,RT; ref(a,b,t)=t)
     stqa  -> RI16 encoding -> use form "ri16t" (seeds RT,i16; ref(t,i16)=t)
     stqr  -> RI16 encoding -> use form "ri16t"
  NOTE the canary guard in _fuzz_add(): a store ref must NOT return the
  CANARY value; it returns the SEEDED input t, which is a fuzzed vector, so
  no collision.

  stqd is RI10 with RT as the stored source. The harness has NO "ri10t" form
  (RI10 that seeds RT), and RI10's own forms (ri10) seed only RA and hand RT
  the driver's private CANARY -- which the ref cannot name (the canary guard
  flips it if the ref tries). So stqd's RT-preservation is NOT expressible in
  this harness without editing it (forbidden for this agent). stqd is
  therefore left UNREGISTERED and reported as a coverage gap, not fuzzed as a
  false-green. (lqd, its load twin, IS registered -- loads need no RT seed.)

None of the above is a lifter bug; it is the limit of a register-only
differential harness against memory-touching instructions. Reported MEASURED.
--------------------------------------------------------------------------
"""

from spu_refs_api import register, to_b, from_b, to_h, from_h, sext, M32


# --- ZERO-LS load refs ------------------------------------------------------
# ISA v1.2 load-quadword RTL (lqd p66, lqx p68, lqa p64, lqr p70): compute the
# local-store address, force it to a 16-byte boundary, wrap to LS size, then
# RT <- LocalStore quadword at that address. Our harness zeroes all of LS, so
# the architected result is the all-zero quadword for every in-range address.
# A ref of [0,0,0,0] thus (a) passes when the lifter emits a genuine load and
# (b) FAILS if the load was mis-lifted to a no-op (RT keeps its canary).
def ref_load_zero_ls(*_operands):
    return [0, 0, 0, 0]


# lqd  rt, disp(ra)   RI10 (op8 0x34). Lifted addr = ra._u32[0] + (i10<<4).
register("lqd", "ri10", ref_load_zero_ls, op8=0b00110100, subtle=True)

# lqx  rt, ra, rb     RR   (op11 0x1C4). Lifted addr = ra._u32[0] + rb._u32[0].
register("lqx", "rr", ref_load_zero_ls, op11=0x1C4, subtle=True)

# lqa  rt, s18        RI16 (op9 0x061). Lifted addr = (i16<<2) & 0x3FFFF (abs).
register("lqa", "ri16", ref_load_zero_ls, op9=0b001100001, subtle=True)

# lqr  rt, s18        RI16 (op9 0x067). Lifted addr = (pc + i16*4) & 0x3FFFF.
#   pc == 0 in the harness (case laid out at addr 0), so == lqa's form here.
register("lqr", "ri16", ref_load_zero_ls, op9=0b001100111, subtle=True)


# --- store refs: RT must be preserved (store does not write RT) -------------
# ISA v1.2 store-quadword RTL (stqx p150, stqa p146, stqr p152): compute the
# LS address as for the load, then LocalStore[addr] <- RT. RT is a SOURCE and
# is left unchanged. With RT seeded as an input, the ref returns it verbatim;
# any lift that clobbers RT (e.g. mis-lifted as a load) diverges.
def ref_store_rt_identity_rrt(a, b, t):   # form "rrt": ref(a, b, t)
    return [w & M32 for w in t]

def ref_store_rt_identity_ri16t(t, i16):  # form "ri16t": ref(t, i16)
    return [w & M32 for w in t]


# stqx rc, ra, rb     RR   (op11 0x144). rrt seeds RA,RB,RT; asserts RT held.
register("stqx", "rrt", ref_store_rt_identity_rrt, op11=0x144, subtle=True)

# stqa rc, s18        RI16 (op9 0x041). ri16t seeds RT + i16; asserts RT held.
register("stqa", "ri16t", ref_store_rt_identity_ri16t, op9=0b001000001, subtle=True)

# stqr rc, s18        RI16 (op9 0x047). ri16t seeds RT + i16; asserts RT held.
register("stqr", "ri16t", ref_store_rt_identity_ri16t, op9=0b001000111, subtle=True)

# stqd  -- INTENTIONALLY UNREGISTERED. RI10 store with RT as source; the
# harness has no RT-seeding RI10 form, so RT-preservation is not expressible
# here without editing the harness. See the module docstring. Coverage gap,
# not a false green.
