# CPU differential trace-diff — the general lift-bug finder (PPU + SPU)

**Problem this solves.** The project's "Verification reality" (SUMMARY.md): there is *no automated
correctness oracle until the game runs*. A wrong lift compiles and silently computes the wrong value;
you only find out when a downstream consumer crashes — the VMX/AltiVec scramble (~26 PPU ops decoded to
the *wrong operation*, caught only by hand-review), the gs_task null-pointer wall, the `5882fe4`
dispatch bug: all the same class. Every one cost a boot-wall to discover, one at a time.

**The fix.** RPCS3 runs the *same* guest code at the *same* addresses (real LLE PRXs for cellSpurs/
libgcm/etc.). Trace both engines instruction-by-instruction, align by PC, report the **first
divergence** — the exact instruction our lift first gets wrong. One run surfaces *every* miscompute up
to that point, no crash required. This is the "RPCS3 trace-diff" SUMMARY.md names as the real oracle.

The diff engine — `tools/tracediff.py` — is built and self-tested (`py -3 tools/tracediff.py
--self-test`, 7/7, PPU + SPU shapes). It is **engine-agnostic**: it diffs any trace in the format
below, so one tool covers SPU images (128-bit regs, two hex words) and PPU threads (64-bit GPRs, one
word; FPR/CR/LR by name). What remains is wiring the two trace *emitters* per engine.

---

## Trace format (both engines emit this)

```
<PC hex>                     one line per retired instruction (the PC about to execute)
  <reg> <word...>            optional: a register written this instruction, post-state.
                             <reg> is a name (r5, f3, cr, lr, v10, ...);
                             <word...> is 1..4 big-endian hex words (SPU reg = 2, PPU GPR = 1).
# ...                        comment/header lines, ignored by the differ
```

Both emitters MUST use the same value encoding (big-endian, MSB word first). Calibrate once: at the
target's entry PC the argument registers are known and must match; if *every* register differs from
the first instruction, byte-swap/reorder one emitter until the entry state agrees, then the diff is
meaningful.

---

## SPU (built infra; ready to wire)

Our side already has `spu_lifter --trace` (emits `spu_trace_pc`/`spu_trace_rt`, runtime/spu/
spu_channels.c). Two small steps:

1. Generalize the `YZ_SPU_TRACE` gate (currently hardcoded to policy image 2) to a chosen image:
   ```c
   // spu_channels.c, spu_trace_pc's YZ_SPU_TRACE block (~line 1358), replace `if (ctx->image_id != 2) return;`
   { static int timg = -1; if (timg < 0) { const char* e = getenv("YZ_SPU_TRACE_IMG");
         timg = e ? (int)strtol(e, 0, 0) : 2; }
     if (ctx->image_id != timg) return; }
   ```
2. Relift the target SPU module **with `--trace`**, rebuild, run faithful:
   ```powershell
   $env:YZ_NO_FLOWCTL=1; $env:YZ_SPU_TRACE=1; $env:YZ_SPU_TRACE_IMG=0   # 0 = gs_task
   .\yakuza\build\yakuza_recomp.exe game\EBOOT.elf                       # -> scratch/spu_trace.txt
   ```

RPCS3 side: interpreter (precise) mode; instrument the per-instruction step to snapshot the reg file
and emit changed regs in the format above, for the target SPU thread
(`SPUNAME 'PS3_Release(Retail)/gs_task.elf'`).

---

## PPU (the big surface — 32k functions, never systematically verified)

The PPU is the bulk of the lifted code and the largest silent-bug surface: "PPU is correct/complete"
only means *no bug has surfaced on the path the boot has exercised so far*. This is the high-leverage
target. It uses the identical method; the wiring is a bit more involved.

**Our side — `ppu_lifter` has NO `--trace` mode yet (confirmed); add one, mirroring `spu_lifter`:**
- Wrap each emitted instruction with `ppu_trace_pc(ctx, <PC>)` before and, for rt-writing ops,
  `ppu_trace_rt(ctx, <reg>)` after (reuse `spu_lifter`'s `_NO_RT_WRITE` idea; PPU writes go to
  GPR rD/rA, FPR frD, CR fields, LR/CTR/XER).
- Add `ppu_trace_pc/rt` to the runtime, emitting `<PC>` and `  r<n> %016llX` (GPR), `  f<n> ...`,
  `  lr/ctr/cr/xer ...`. Gate to ONE thread (env `YZ_PPU_TRACE` + a target tid, e.g. t1) with a
  budget, exactly like `YZ_SPU_TRACE` — PPU is multi-threaded, so tracing one thread avoids interleave.
- This is a PPU relift (the expensive ~263 MB build) — do it once for a trace-enabled build kept aside.

**RPCS3 side:** PPU interpreter mode; instrument the step to emit changed regs for the target thread.

**Alignment / the PPU-specific tuning cost:** trace ONE PPU thread from a sync point
(`--align-pc <entry>`). The real work is filtering **HLE-boundary divergences**: our HLE imports vs
RPCS3's LLE PRX calls are *expected* to differ (different code, same effect). Pause tracing across an
import/syscall and resume on return, or annotate those PCs so the differ skips them — otherwise every
`bl` into an HLE stub looks like a "bug". Once filtered, a compute divergence inside game/PRX code is a
genuine lift bug (the VMX-scramble class), and a control-flow split flags a mis-computed condition.

---

## Diff

```powershell
py -3 tools/tracediff.py scratch/spu_trace.txt scratch/rpcs3_spu_trace.txt --align-pc 3050 --context 12
py -3 tools/tracediff.py scratch/ppu_trace.txt scratch/rpcs3_ppu_trace.txt --align-pc 100004A0
```

Output = the **first divergence**:
- **compute** (same PC, a register value differs) → the lift of *that PC* is wrong (or its input was);
  disassemble it and compare to the lifted C. Direct hit for the null-pointer / VMX-scramble class.
- **control-flow** (PCs split) → a branch went differently; the mis-computed condition is a few
  instructions earlier in the OURS window the tool prints.

Exit code: 0 agree, 1 divergence (CI-friendly), 2 usage/parse error.

---

## Getting *many* bugs per pass (vs one-at-a-time)

- **Fix-and-rerun** (default): first-divergence, fix, rerun. Cheap because each run is deterministic
  (no boot wall) — already far faster than one-bug-per-boot-wall.
- **`--continue` candidate list** (extension): report every divergence in one pass, deduped by PC.
  Caveat: after the first miscompute, downstream state is corrupted, so later hits may be *cascades*.
- **Oracle-resync co-sim** (strong version): after each divergence, reset our reg/mem state to
  RPCS3's and continue, so every reported bug is independent. More plumbing (feed RPCS3's trace as
  ground-truth checkpoints); this is what makes "scan across everything" literal.

---

## Validation on gs_task (2026-07-01) — RAN END-TO-END; 3 real gotchas found

The harness was wired and run on all sides: our `--trace` build produced scratch/ours_gstask_trace.txt
(456k instrs); an instrumented RPCS3 (SPUInterpreter.cpp, interpreter mode) produced
scratch/rpcs3_gstask_trace.txt (3M instrs). `gpr3` at gs_task entry matched byte-for-byte (calibration
OK, no encoding swap). `tools/tracediff.py --align-pc 3050` aligned + diffed them. Three gotchas surfaced
(all now handled/documented — reusable for the PPU diff and future SPU images):

1. **Bounded reference tracer ⇒ calls OUT of the window look like divergences.** The RPCS3 tracer was
   bounded to LS [0x3000,0x8000); a normal `brsl` to 0xB3D8 (>0x8000) was executed but not logged, so the
   diff saw ours=0xB3D8 / ref=0x55EC. FIX: `--pc-range 3000:8000` filters BOTH sides to the same window.
   (Better: trace the full gs_task LS range on the reference side so nothing is dropped.)
2. **Our `--trace` mislabels the dest register for RRR-format ops.** `spu_lifter --trace` emits
   `spu_trace_rt(insn & 0x7F)`, but 4-operand ops (shufb/selb/mpya/fma/fms/fnms) put the dest at
   `(insn>>21)&0x7F`. So register VALUES compare cleanly for normal ops but are mislabeled for RRR ops ->
   register-diff is noisy. FIX (small, needed for compute-diffs): in spu_lifter, use the RRR dest field
   for those mnemonics, then relift with `--trace`. (Control-flow / `--pc-only` diffs are unaffected.)
3. **Invocation/input alignment.** gs_task runs per-dispatch; aligning at the *first* 0x3050 in each trace
   can pair different invocations with different LS input (seen as ref=zeros vs ours=real-data at matching
   PCs). Verify the full entry state (gpr3/gpr4/gpr80/gpr81 + the LS input), or align at a known-matching
   dispatch, before trusting a compute-diff.

RESULT: within [0x3000,0x8000) both engines ran IDENTICAL control flow for ~1310 instructions, then split
at 0x65EC — with register data diverging earlier. Consistent with nq_agent1's "our build takes extra
produce passes" (reprocessing a drained SRC state -> different data at the same PCs). Pinning the single
mis-lifted op needs gotcha #2 fixed (so the register-diff is trustworthy) + gotcha #3 (aligned inputs).

## Hardening (2026-07-01/02) — the pass that found the il-decode root

Three harness fixes landed; together they took the diff from "27 artifact findings" to the ONE
real bug (the `il` negative-immediate double sign-extension, spu_disasm.py — see STATUS.md):

1. **Crash-safe tracer.** `YZ_SPU_TRACE` output is now UNBUFFERED and the budget is env
   `YZ_SPU_TRACE_N` (default 600k) with a `# trace budget exhausted` end-marker. Before: a
   crashing SPU halted with the trace TAIL (the approach into the fault — the evidence) stuck
   in the stdio buffer, and a silent budget cap made "ours loops 1024× vs 9236×" a truncation
   artifact rather than a fact. Never compare event counts across traces without checking both
   end conditions.
2. **Stale-ref guard (fixes gotcha #1 properly).** `--pc-range` no longer silently drops
   out-of-window events: their register writes ride along as *shadow* writes that INVALIDATE
   the other side's reconstructed state (both engines ran that code; the windowed one just
   couldn't record it). Kills the 0x51C4 false-compute class; register mode is trustworthy
   again. Self-tests cover it (13/13).
3. **Watch the DMA class you actually need.** The LS-content "garbage" at 0x37780 was GETLLAR
   (cmd 0xD0) staging of the EDGE job chain — invisible to an MFC_GET_CMD-only probe. The
   YZ_LSWATCH probe now matches any into-LS command (`cmd & 0x40`).

Also note (word-decomposition trap): in OUR trace `rN <hi64>:<lo64>` prints `_u64[0]:_u64[1]`
on a little-endian host, so SPU word0 (preferred) = the LOW 32 bits of the FIRST number.
Reading `r5=4:35` as {w0=0,w1=4,...} inverted a session's conclusion (STATUS step 9).

## Scope / status

- **Built + self-tested:** `tools/tracediff.py` (engine-agnostic differ).
- **Staged (build-blocked, not design-blocked):** the SPU `YZ_SPU_TRACE_IMG` patch + `--trace` relift;
  the RPCS3 SPU/PPU interpreter emitters; the new `ppu_lifter --trace` mode. Deliberately not applied
  while a yakuza build is in flight.
- **Cannot catch** (honest bounds): bugs RPCS3 *also* has; timing/concurrency RPCS3 doesn't reproduce;
  code the run doesn't execute; and HLE-boundary divergences are noise to filter, not bugs.
- **Complementary cheap nets** (no run needed): a helper-conformance fuzz vs a reference model (the
  `cwd`/byte-order class — would have cleared `shufb` in minutes), a decode-diff vs a second decoder
  (the VMX-scramble class, statically, image-wide), and a "one LS address owned by >1 image" lint
  (the `5882fe4` class).
```
