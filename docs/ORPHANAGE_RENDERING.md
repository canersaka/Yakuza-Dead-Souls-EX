# A010 orphanage rendering investigation

This document records the A010 orphanage investigation through July 28, 2026.
The branch contains both durable fixes and opt-in diagnostics. Diagnostic and
force flags remain off by default and are not part of the validated normal
execution path.

## Outcome

The coastal orphanage environment now reaches the renderer and becomes visible.
A validated run displayed the beach, road, vegetation, fences, utility poles,
and foreground roof geometry. It crossed the former asynchronous geometry
worker stall and continued advancing afterward.

The scene is not yet complete. A later shot can present black while frame and
FIFO counters continue advancing, audio remains incomplete, and the current
runtime runs the scene at approximately 0.8-0.9 FPS.

## Timeline

### July 24

- Restored the live renderer investigation from its checkpoint.
- Added fragment-program conditional execution and condition-code handling.
- Added fragment cubemap declarations and sampling based on texture-unit
  dimensionality.
- Uploaded cubemap faces, preserved the registered flip surface, and presented
  that surface instead of an unrelated buffer.

### July 25-26

- Added bounded A010 asset, animation, camera, stage, command-list, and worker
  diagnostics.
- Added presentation-clock, movie handoff, input automation, audio, and
  synchronization probes used to reach the scene repeatably.
- Registered the observed SPURS task and job images, including the orphanage
  geometry worker.
- Added renderer capture/replay checks for the additional scene state.

### July 27

- Captured a matched reference checkpoint at guest PC `0x00113584`, including
  registers and the object graph reachable from `r24`.
- Reduced the first meaningful state difference to the transform beginning at
  `object+0x30`.
- Traced the transform producer to an `fctiwz`/`fcfid` sequence in the camera
  and placement path.
- Added a deterministic transform replay fixture and regression test.

### July 28

- Corrected `fctiw` and `fctiwz` lifting so the 32-bit integer result is
  sign-extended across the complete 64-bit FPR payload. The previous lift
  retained stale high bits, causing `fcfid` to convert a hybrid value and
  produce invalid transforms.
- Identified the remaining geometry stall as incorrect SPU job-image
  selection. The orphanage descriptor can arrive before higher-level scene
  tracking is armed.
- Made the exact descriptor select worker image 19 independently of diagnostic
  scene state and added a four-case dispatcher regression test.
- Built and validated a release executable with lifted PPU optimization,
  function-entry tracing disabled, and the region-lifted geometry task.
- Captured a 15-second CPU profile of the visible-scene run.

## Durable fixes

### Full-width `fctiw`/`fctiwz` result

The lifted implementation previously replaced only the low 32 bits of the
destination FPR. PowerPC integer-to-floating conversion consumes all 64 bits,
so stale high bits corrupted later transforms. The implementation now writes a
fully sign-extended 64-bit integer.

Coverage includes saturation, rounding, negative results, NaN handling, and
the exact `lfd -> fctiwz -> fcfid -> stfd` sequence used by the affected path.

### Descriptor-driven orphanage worker selection

The SPURS job module can dispatch binary `0x01252680` at local-store entry
`0x04C00` while the current image is 13. That descriptor must select image 19
even if the A010 asset-open observer has not run yet.

The dispatcher now treats descriptor identity as authoritative and leaves
nearby binaries, entries, and modules unchanged.

## Validation evidence

- The deterministic transform replay passes.
- The PPU lifting conformance suite passes 1,629 checks.
- The SPU job-dispatch regression passes all four cases.
- The full release build completes.
- The validation run crossed the old frame-2233 worker stall and continued
  beyond frame 2660.
- The visible coastal shot reached 2,736 draws.
- A later extended run continued from frame 4,950 to 4,955 at approximately
  0.8 FPS; producer and consumer counters remained balanced.

The extended visual run used normal movie HLE only. Scene, camera, palette,
clock, visibility, and model forces were disabled.

## Performance findings

The profile captured six nearly saturated host threads. Five of the hottest
threads each accounted for approximately 11.5-12 percent of samples.

Four shared the same SPU-runtime profile:

- `mfc_submit`
- `spu_rdch`
- `spu_wrch`
- host synchronization

The fifth spent 56.9 percent of its own samples in `spu_prof_addr_of`. This
lookup hashes a native function pointer to recover a guest SPU PC even though
the trampoline already carries `ctx->pc`.

Aggregate module samples were:

- recomp executable: 69.32 percent
- operating-system runtime: 29.69 percent
- graphics driver and D3D12: less than 0.2 percent

Measured GPU engine use was approximately 1.1 percent. The current bottleneck
is CPU-side SPU dispatch, DMA/channel emulation, and synchronization rather
than shader execution.

## Performance follow-up

Recommended order:

1. Compile diagnostic-only hooks out of the normal runtime, pass `ctx->pc`
   directly to task-launch checks, and cache configuration once at startup.
2. Region-lift the hot SPURS policy, system-service, job-chain, and remaining
   workload images instead of crossing a host function boundary for each
   translated instruction.
3. Add direct fast paths for ordinary aligned DMA, restrict reservation
   synchronization to reservation commands, and replace timed polling with
   precise event-driven wakeups.
4. Move to a block compiler that retains SPU registers in native SIMD
   registers, links hot blocks directly, inlines local-store access, and lowers
   recognized polling loops.
5. Consider compute-shader implementations only for complete, proven
   data-parallel geometry jobs. Individual SPU instructions and SPURS control
   flow are poor GPU workloads.

Removing the remaining diagnostics is necessary but cannot by itself reach
real-time speed. Moving from approximately 0.85 FPS to 30 FPS requires roughly
a 35-fold frame-time reduction and therefore an architectural SPU-runtime
improvement.

## Remaining correctness work

- Trace the later black presentation to its first camera, composite, or
  displayed-buffer divergence.
- Restore normal A010 dialogue and movie audio without relying on fast
  acceptance modes.
- Remove experimental force paths once their diagnostics have been replaced by
  proven producer-side fixes.
- Re-run the matched stage checkpoint after each durable fix and retain normal
  forward playback through the visual result.
