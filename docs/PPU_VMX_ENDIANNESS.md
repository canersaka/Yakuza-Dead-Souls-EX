# PPU VMX endianness — execution-ready design (the rendering-blocker fix)

**Status:** NOT yet implemented. Verified by 3 review agents. Latent until 3D rendering
is reached (the movie path at fence 33 does pure load/store copies, not vector
arithmetic, so it doesn't expose this). Do this as a *focused, testable* change when
rendering is the active frontier — it is a large hand-touched edit that cannot be
validated until VMX arithmetic actually executes, so it must NOT be done blind.

## The bug

`tools/ppu_lifter.py`:
- `lvx`/`stvx` (lines ~1326/1333) do a raw 16-byte `memcpy` between guest memory and
  `ctx->vr[N]`. Guest memory is big-endian, so `ctx->vr[N]` holds **raw BE bytes**.
- Every typed VMX handler (vaddfp, vadduwm, vcmpequw, vmaxfp, …) reads
  `((float*)&ctx->vr[N])[i]` / `((uint32_t*)…)[i]` — **host little-endian** lanes.
- Result: each lane value is byte-reversed. Pure load→store copies round-trip fine
  (bytes unchanged), but any op that *interprets* a lane (all arithmetic, compares,
  min/max, conversions) operates on garbage. `runtime/ppu/ppu_ops.h` `PPU_LVX` asserts
  yet a third convention and is dead code (the lifter inlines its own memcpy).

## Two correct options

### Option A — reverse 16 bytes at lvx/stvx (RECOMMENDED; fewer edits)
Make `lvx` store the vector fully byte-reversed (and `stvx` reverse back). Then
`ctx->vr` matches RPCS3's `v128` storage (`se_storage<v128>::swap` = full 16-byte
reverse). KEY INSIGHT the agents under-weighted: **element-wise ops are order-agnostic**.
After a consistent full-reverse, every lane holds a host-native value and BE element `k`
lives at host index `N-1-k`. A loop `for(i) d[i] = a[i] OP b[i]` over a,b,d that are all
in the same reversed layout produces the correct result UNCHANGED — the per-lane op
doesn't care that "lane i" means "element N-1-i", only that it's consistent. So the bulk
(all float arithmetic, integer add/sub/and/or/xor, compare→mask, min/max, abs) needs
**no handler change**. Only order/position-sensitive ops need re-derivation (see list).

Cost: change `lvx`/`stvx` (+ the sub-vector loads `lvebx/lvehx/lvewx/lvsl/lvsr/lvlx/lvrx`
and `stvebx/...`) + the ~15–20 position-sensitive handlers + author the 55 missing
handlers in reversed layout.

### Option B — keep raw-BE storage, add lane accessors in every numeric handler
Leave lvx/stvx and the byte-granular ops (vperm/lvsl) untouched; add
`vr_w/vr_setw/vr_f/vr_h/vr_d` (per-lane `ps3_bswap`, element order preserved) and route
**every** numeric handler through them. Cost: ~80 existing handlers edited + 55 new
(all via accessors). Byte ops untouched. More total edits than A but each is mechanical
and the convention is "element k at index k" (less mental remapping).

**Recommendation: Option A** — fewer touched handlers because element-wise ops come free.
Whichever is chosen, the 55 new handlers must be authored in the SAME convention, and the
dead `PPU_LVX/PPU_STVX/PPU_V*` macros in `runtime/ppu/ppu_ops.h` deleted.

## Position/order-sensitive handlers that need explicit attention (Option A)
vperm, vsldoi, vspltw/vsplth/vspltb (+ vspltisw/h/b), vmrghw/vmrglw/vmrghh/vmrglh/
vmrghb/vmrglb, vpkuhum/vpkuwum/vpk*us/vpk*ss (pack), vupkhsb/vupklsb/vupkhsh/vupklsh
(unpack), vmuleub/vmulesb/vmulesh/vmuloub/vmulosb/vmulosh (even/odd multiply),
vsl/vsr/vslo/vsro (whole-vector shift), vsumsws/vsum2sws/vsum4* (sum-across), lvsl/lvsr,
lvebx/lvehx/lvewx + stvebx/stvehx/stvewx.

## The 55 currently-missing handlers (emit `/* TODO */` no-op today)
Compare (6): vcmpgtsb/sh/sw, vcmpgtub/uh/uw.
Pack (9): vpkuhum, vpkuwum, vpkuhus, vpkuwus, vpkshus, vpkswus, vpkswss (+ verify vpkshss).
Unpack (3): vupkhsb, vupklsb, vupklsh.
MAC/sum (9+): vmhaddshs, vmhraddshs, vmladduhm, vmsumubm, vmsumuhm, vmsumuhs, vmsumshs;
odd/even mul vmuloub, vmulosb, vmulesb, vmulesh, vmuleub.
Add/sub-saturate (6): vaddsws, vaddubs, vsubsbs, vsubshs, vsubuhs, vsubuws.
Average (5): vavguh, vavguw, vavgsb, vavgsh, vavgsw.
Shift/rotate byte/half (6): vslh, vsrb, vsrh, vsrab, vsrah, vrlh.
Sum-across (4): vsumsws, vsum4ubs, vsum4sbs, vsum4shs.

## Test plan (must run before trusting)
1. Relift PPU (slow, full).  2. Build.  3. Reach a frame that runs VMX arithmetic and
diff a known vector op against RPCS3 (the oracle stores fully-reversed v128 — compare a
captured vaddfp/vmaddfp result). 4. Confirm pure copies still round-trip.
