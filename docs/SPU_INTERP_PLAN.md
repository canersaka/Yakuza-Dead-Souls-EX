# SPU interpreter — plan of record (2026-07-01 reassessment, item 3a)

## Why (one paragraph)

Silent SPU mislifts + wrong-image dispatch were the project's dominant historical cost
(RRR operand order, cbd/chd byte order, brhz halfword, shufb positions, the
spu_link splat, the image-0 wildcard). An in-process interpreter kills the class: it executes
any image correctly on first contact (Sofdec = EBOOT SPU image #9 is still ahead), lockstep-
diffs against the lifted code without the instrumented-RPCS3 cycle, and instantly classifies
anomalies as lift-bug vs runtime-bug. RPCS3's interpreter CANNOT be embedded (GPLv2 vs MIT +
clean-room); it stays the external semantic oracle via `tools/tracediff.py`.

## What exists (current state)

- `tools/gen_spu_interp.py` — GENERATES the decoder from `tools/spu_disasm.py` by probing all
  2048 op11 slots of the real Python decode path. The debugged tables + priority order stay
  the single source of truth; regenerate after any disasm change.
- `runtime/spu/spu_interp_gen.h` — generated: 196-op enum, full op11→(op,format) map, field
  extractor (`spu_decode`). DO NOT hand-edit.
- `runtime/spu/spu_interp.c` — compiled into the runtime library:
  - `spu_interp_trace(ls, pc, n)` / `spu_interp_format(...)` — in-process disassembler,
    usable TODAY from crash/halt paths (e.g. the `unknown branch pc=...` diagnostic in
    spu_channels.c).
  - `spu_interp_step(ls, regs, &pc)` — the execution frame; semantic switch intentionally
    empty (returns `SPU_INTERP_UNIMPL`). NOT wired into dispatch.
  - `spu_interp_selftest()` — decode vectors cross-checked against the Python decoder
    (already caught one bad vector at authoring time: 0x33800280 = lqr, not brsl).

## Implementation order

1. **Tranche 1 — the gs_task working set.** Run `tools/tracediff.py` op-frequency over
   `scratch/spu_trace.txt` to rank ops by actual use; implement loads/stores (lqd/stqd/
   lqa/stqa/lqr/stqr/lqx/stqx), integer ALU (a/ai/ah/ahi/sf/sfi/and/or/xor/nand/nor + imm
   forms), il/ilh/ilhu/ila/iohl, and ALL branches. Register layout: SPU word 0 in `_u32[0]`
   (the spu_link proof). Authority: CBEA; RPCS3 read-only cross-check.
2. **Lockstep harness.** A driver that runs interpreter + lifted function on a snapshot of
   the same LS/registers and compares registers after each instruction — the in-process
   tracediff. Divergence = lift bug (or interpreter bug: settle against CBEA).
3. **Tranche 2** — shifts/rotates (word, quadword-by-bits/bytes), shufb/selb/fsm* family,
   cbd/chd/cwd/cdd, gb/gbh/gbb, extends/converts. These are the historical bug ops: verify
   each against the tracediff RPCS3 trace once, then the interpreter becomes the cheaper
   oracle.
4. **Tranche 3** — float (fa/fs/fm/fma/fnms/frest/frsqest/fi, cflts family with the 173/155
   scale exponents), double precision, then channels: route `SPU_INTERP_CHANNEL` returns
   into the existing spu_channels.c machinery (rdch/wrch/rchcnt already have host
   implementations).
5. **Wire the fallback**: on `spu_lookup` miss / unknown branch, if `YZ_SPU_INTERP=1`,
   enter `spu_interp_step` at the miss pc instead of halting (register the flag in
   docs/FLAGS.md when wired). Acceptance: gs_task runs fully interpreted to the same
   geometry PUTs as the lifted path.

## Rules

- Every op verified before the next tranche (batch per CBEA section, tracediff after each).
- The interpreter must share NO semantics code with the lifter emission (independence is
  what makes it an oracle for emission bugs); decode is shared BY DESIGN (generated).
- Slow is fine. Correct is the whole point.
