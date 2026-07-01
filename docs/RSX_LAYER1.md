# LAYER 1 — faithful RSX FIFO / flow-control + flip lifecycle: timeline + what's left

The graphics wall. **LAYER 1 = a clean-room, faithful RSX command-processor (FIFO GET/PUT,
segment stoppers, reports/labels/fences, the flip lifecycle)** that drives an animating,
structurally-correct **untextured** window with no deadlocks. (LAYER 2 = NV4097→D3D12 method
dispatch + NV40 shader translation, after Layer 1 renders.)

The RSX is hardware — there's no firmware to lift — so it must be **modeled, not LLE'd**
(reimplemented clean-room, NOT copied from RPCS3/GPL). See [[rsx-must-be-modeled-not-lled]].
Tags: **[V]** verified/measured, **[I]** inferred. Companion to `STATUS.md` (live state) and the
blocker ledger in `SUMMARY.md` (#19–#21). Last updated 2026-06-19 (pt20).

---

## TL;DR — where LAYER 1 stands (2026-06-24 pt28 — 🎉 FAITHFUL FLIP PATH; YZ_FLIPADV band-aid RETIRED)

**The flip fence is now FAITHFUL and flow-control is DEFAULT-ON — continuous flips with NO forced fence.**
A 3-agent design pass (RPCS3 `run_FIFO` + the `.rrc` capture + the proven-lever) converged: the flip METHOD
path was already faithful (DRIVER_QUEUE `0xE944` queues the buffer, SEMAPHORE_RELEASE arms pending, the vblank
tick presents + sets flip-done + clears the label `@0x10200010` — all mirroring RPCS3's LLE
`queue_flip`/`flip_command`). The ONLY missing effect was advancing the flip-completion counter `@0x40C00000`
that the throttle `func_00EAC46C` polls. **FIX (2 parts, both default-ON, no env band-aid):**
- **ROOT 2 (faithful fence):** `import_overrides.cpp` `yz_rsx_vblank_tick` FLIP-COMPLETE block now does
  `vm_write32(0x40C00000, +1)` once per RETIRED flip (after present + label-clear) — the fence advances from
  REAL vblank completion, not a forced nudge. (Old `YZ_FLIPADV` fence-force now gated OFF behind `YZ_THR_NUDGE`.)
- **ROOT 1 (stopper flow-control):** `yz_flip_advance`'s wrap-aware GET-advance (advance GET to the next
  committed flip cmd when t1 is reserve-wedged, never backward) is now **default-ON** (was `YZ_FLIPADV`-gated).
  Disable with `YZ_NO_FLOWCTL`.

**RESULT [V]:** default boot (no env) → **8–10 real flips, buffers ALTERNATE (real double-buffered animation),
reaches the full CRI stack (8 cri threads)**, ZERO throttle-nudges. Matches/beats the old band-aid's 5–9.
CONTROL `YZ_NO_FLOWCTL=1` → deadlock at 2 (load-bearing confirmed). Flips stop at the **CRI movie gate**
(Problem 2 = the SOFDEC SPU decoder not producing frames) — faithful flips REACH the movie; continuity THROUGH
it needs Problem 2. Reliability: 3/4 runs reach the CRI stack (1/4 early-boot coin-flip stall — separate).
**NEXT:** (a) delete the now-dead band-aids (`yz_resync_loop`, the force knobs, the deferred-release / faithskip
toggles); (b) Problem 2 (SPU content) for continuous flips through the movie itself.

## TL;DR — where LAYER 1 stands (UPDATED 2026-06-20 pt27 — flip path now DETERMINISTIC to CRI)

**pt27:** `YZ_FLIPADV` was reproducible only in the prior interactive shell (failed 9/9 here; foreground/env/build
theories all busted). Root: the lever idled when the ring drained (`get==put`) instead of driving t1 through the
gcm flip throttle `func_00EAC46C`, whose completion counter **IS the flip fence @0x40C00000** (`while *(0x40C00000)+2
<= target`). FIX (main.cpp): in the `get==put` branch, nudge the fence +1 to release one frame, VERIFY t1 produced it
(PUT advances), latch off when PUT stays frozen 1s (real upstream stall). RESULT: **3/3 runs reach the full CRI stack
(7–9 flips, 23 threads)** + default boot unchanged. The pt26 win below is the same mechanism, now reliable; next gate
is the CRI middleware (STATUS.md pt27).

## TL;DR — where LAYER 1 stood (pt26 — 🎉 FRAME-3 WALL BROKEN)

**The frame-3 wall is BROKEN: 5 distinct frames now render + flip (was 2–3).** It did NOT require
applying the op-list drain (pt25b) — t1's OWN apply runs as soon as its reserve is freed. The fix
(env `YZ_FLIPADV`, main.cpp `yz_flip_advance`, OFF by default): a monitor that (1) frees t1's reserve
when the consumer parks on a stale display-list CALL (`GET=PUT`), and (2) FORWARD-scans the committed
`[GET,PUT)` for each frame's flip command (`0x0004E944`) and advances GET to it so the consumer
dispatches the flip (fence++). pt24's resync OVERSHOT each flip; the scan processes it. Result: fence
2→6, 5 flips, clean stop. The "frame-4 throttle (ROOT 2)" was an artifact of a truncated scan; the
throttle counter is the fence `0x40C00000`. **NEW FRONTIER (non-RSX):** after ~5 frames t1 STOPS
producing — it's in a `sys_timer_usleep` poll loop (`func_00E5DB94` from r31=0xD0100730) waiting on a
NON-RSX condition (GET-circulate / throttle-force / reserve-free all proven inert; no media-decode
calls). Next: find that poll's condition (the upstream producer). Full detail: STATUS.md pt26.

## TL;DR — where LAYER 1 stood (pt25b, SUPERSEDED by pt26 above — the drain was NOT needed)

**The LAYER-1 root is the gcm OP-LIST = a deferred PATCH JOURNAL the game applies only on ring-wrap.**
Each per-frame segment is built with **placeholder words + a journal of deferred patches** (op-list @
`S=*(game_toc-0x7410)`: tags 0x04 transform-const / 0x08-0x09 data / 0x0D DMA / 0x10 sublist / 0x7F
stopper-release). Our consumer **can't parse the un-drained segment** (placeholders misparse → derail
into the io 0x1104D00 shader region). The circle: consumer needs the drained segment ↔ drain needs
ring-wrap ↔ wrap needs flips ↔ flips need the consumer at the in-stream flip cmd. **Every prior framing
(stopper standoff / pacing / faithful consumer) was a symptom; this is the cause.** Proven pt25:
segment 3 IS a complete frame (2 draws 0x1808 + DRIVER_QUEUE 0xE944 — t1 BUILT it); `YZ_FAITHSKIP`
frees Root 1 (reserve) → t1 reaches Root 2 (flip throttle `func_00EAC46C`); we're NOT on the movie
(0 draws, 393 setup methods byte-match RPCS3). RPCS3 ground truth: released stopper = `0x20300004`
(jump fwd 1), patched before its GET arrives. **NEXT: apply the op-list drain** — invoke the game's
own drainer (`func_00E9B7C0` reserve/recycle + the not-yet-located data-patch applier) or replicate it,
or force the throttle now that Root 1 is cleared (full detail: STATUS.md pt25b). Tooling: `YZ_FIFO_TRACE`,
`tools/rrc_methods.py`, `tools/cmp_fifo.py`, `YZ_FAITHSKIP`, `YZ_DUMP_SEG`.

## TL;DR — where LAYER 1 stood (pt20, superseded by pt25b above)

The window opens and 2 frames render+flip; the wall is a **producer/consumer PACING interlock**:
the game's render thread defers finalizing the frame-3 display list (`io 0x1104D00`) behind a
GET-advance, while the RSX consumer parks on that unfinalized list — so GET never advances, the
flip fence stays frozen at 2, and the render thread spins. We built a faithful, coherence-safe
consumer + the flip lifecycle and cracked the `@0x300000` stopper wall; the remaining gap is
**how RPCS3's RSX keeps GET advancing through deferred stoppers** (it uses the same plain reads we
do). A multi-session detour suspected gs_task (the SPU geometry task) was the missing producer —
**that's now resolved (gs_task is healthy but downstream/starved, pt19–20), so the gate is
squarely back here.**

---

## Timeline — what was done

### 2026-06-13 — render loop reached
- Blockers #9–#19 cleared (gcm bring-up, LLE libgcm + sys_rsx, FIFO consumer, endianness, fs).
  **#19d** fixed (a `sys_process_get_paramsfo` memset stack-smash) → gcm init COMPLETES, a real
  **1280×720 window opens, 2 frames render + flip.** [V]

### 2026-06-13 → 06-14b — flip lifecycle (#20)
- Implemented the gcm flip lifecycle vs the in-repo RPCS3 oracle (nv406e.cpp + sys_rsx 0xFEC +
  RSXThread.cpp): SEMAPHORE_ACQUIRE, DRIVER_QUEUE, DEVICE ctx, label-clear, **vblank pacing**.
  Flips queue/present; GET advances ~2 MB. **#20 DEAD.** [V]

### 2026-06-14b–e — the `@0x300000` stopper wall (#21 part 1)
- Found the deadlock: the game's gcm cmd buffer uses **jump-to-self STOPPERS** at every commit and
  releases the previous one; a **cross-fragment release** (commit spanned a gcm ring recycle) is
  **DEFERRED into the game's gcm op-list and never drained** (the game wedges on the flip fence
  that needs the very frame whose stopper is deferred). See [[gcm-stopper-patch-mechanism]].
- **Built a clean-room faithful FIFO consumer** (Layer-1: ring-aware GET/PUT, NV4097 NON_METHOD
  validation `0xa0030003`) — isolates the wall to the GAME's gcm stopper-release, NOT the RSX. [V]
- **06-14e-FIX: `@0x300000` wall CRACKED ACCURATELY (#21 part 1 DEAD).** When GET parks on a
  stopper whose release is queued in the op-list (`tag=0x7F` entry, e.g. ea 0x40700000 = io
  0x300000), apply the game's OWN committed release (write 0/NOP, mirroring func_00E9BC9C@0xE9BE60)
  and advance. RESULT: 4 deferred-releases fire, **fence 2→3, frame 3 executes, 3 flips**, no
  crash. (import_overrides.cpp `yz_gcm_stopper_release_deferred`.) [V]

### 2026-06-14f–h — corrected root cause + base hardening
- **06-14f/g:** a brief "HLE the gcm layer" detour was a WRONG TURN (RPCS3 uses LLE libgcm); the
  real frontier is the **RSX CONTROL PLANE**, not the gcm HLE. [V]
- **06-14h:** double-checked the base sound; built authoritative thread tooling; fixed a
  **sys_event ABI bug** (runtime/syscalls/sys_event.c — independently upstreamable). [V]

### 2026-06-15 — root-caused to the RSX-PACING wall
- Via **RPCS3 live-debug ground truth**: the remaining deadlock is a **serial producer/consumer
  pacing** problem. The producer (render thread t1) defers finalizing the display list behind a
  GET-advance; the consumer parks waiting for finalization → circular. **The lift is VERIFIED
  faithful** (not a translation bug); the deadlock was located to the exact instruction. [V]
- **Killed:** distractor theories + an unsound consumer-side band-aid (NOP-release that let the
  producer lap GET → corruption). [V]
- **Built the COHERENCE-SAFE consumer** (import_overrides.cpp `yz_rsx_consumer`): advances GET past
  a stopper WITHOUT zeroing it (holds it; zeroes only when GET leaves the segment → no
  producer-laps-GET corruption) + faithful NV4097 validation (waits at non-command words instead of
  drifting). This is the default path. [V]

### 2026-06-16 — banked + RPCS3 install added; config insight
- User added the full working RPCS3 (`rpcs3-v0.0.41-...`): a real **RSX capture (.rrc)**, decrypted
  firmware PRX, the working `config_BLUS30826.yml`. See [[rpcs3-full-install-resource]].
- **Key config insight:** RPCS3 renders this game with **FIFO Fetch = Fast + reservation access =
  false = PLAIN reads, NO reservation** — *exactly our consumer's approach*. ⇒ reservation/coherence
  is a DEAD END; **our architecture is right; the gap is the FIFO PROCESSING LOGIC + PPU↔RSX
  TIMING** — how RPCS3 keeps GET advancing through deferred stoppers with the same plain reads. [V]

### 2026-06-16b → 06-19 (pt5–pt19) — the gs_task detour
- **Hypothesis (06-16b):** the render deadlock is a SYMPTOM of a **stubbed SPU geometry task
  (gs_task)** that never fills the display list — not an RSX-coherence bug. Pivoted to SPU work.
- Drove gs_task from "never launches" → launches → **halt FIXED** (pt19: `tasksetMgmtAddr=0x2700`).
  See [[spurs-taskset-dispatch-reference]] / `docs/SPURS_TASKSET.md`.
- **OUTCOME (pt20): the detour resolved the SPU side but it's DOWNSTREAM.** gs_task is healthy but
  STARVED — its job queue (0x40197180) is empty because the **producer (render thread) is stuck in
  the LAYER-1 flip wait before it posts any job**. So the gate is back to LAYER 1, now with the SPU
  variable eliminated. [V]

### 2026-06-19 (pt20) — producer investigation (this lands us back at LAYER 1)
- **[V]** Steady state: **only t7 (`_gcm_intr_thread`) is active**, looping
  `sys_event_queue_receive` (sc 130) and servicing RSX **vblank interrupts normally**. Every other
  thread is silent → spinning in **guest code** (polling memory, no syscalls). Flip fence
  `@0x40C00000 = 2` (frozen); RSX parks on stale `0xA2000500`; 0 PPU writes to the display list or
  gs_task's queue. ⇒ the render thread spins on the flip-counter poll; the display list never
  finalizes; the flip never advances. **This is the LAYER-1 producer/consumer interlock.**

### 2026-06-19 (pt20 deep-dive) — every lever tested; the wall is GET-circular pacing
- **[V] Producer model CORRECTED:** the watch (`YZ_WATCH_300`) proves **guest tid=1 (t1) is ACTIVELY
  writing segment `0x300000`** (repeatedly, from a memcpy at guest `lr=0x016229D0`) — NOT wedged. It's
  a **ring-recycle lapping race**: GET held at the `0x300000` stopper → t1 lapped the ring and
  recycled `0x300000` for a future frame → GET stuck reading the half-rewritten segment (the lost
  frame's data at `io 0x300040` is GONE, overwritten).
- **[V] RPCS3 FIFO rule (RSXFIFO.cpp):** GET **never reads past PUT**, and at a jump-to-self stopper
  it **spins waiting for the producer to patch it** (it would `throw "Unexpected command"` on a word
  like `0xA2000500` — i.e. in RPCS3 GET never reaches stale data; the producer stays ahead).
- **[V] Every consumer-side lever dead-ends** (all env-gated, default boot unaffected):
  deferred-release → derails into stale `io 0x1104D00`; `YZ_NO_DEFER` → waits at the stopper forever
  (t1 defers the release into its op-list); `YZ_SKIP_STALE` → skips one stale CALL, hits more stale
  data; `YZ_RESYNC` (GET→PUT) → catches up then stalls.
- **[V] DECISIVE: the flip-throttle is NOT the gate.** `YZ_FORCE_REF` forced the game's flip counter
  (`[[0x0164FE78]] = 0x40C00000`) to 281 and t1 **still didn't flow** (PUT frozen at `0x300DB0`, GET
  parked, only 2 flips). ⇒ t1 is blocked on the **GET-advance / gcm ring-reserve**, a true
  producer↔consumer circle — NOT the flip counter. (Confirms pt14–16: no band-aid breaks this.)
- **⇒ The fix is the faithful consumer rebuild, full stop** — and the RSX-capture replay is the
  highest-leverage way to ground it (only the user can run it).

### 2026-06-19 — RPCS3 gcm ring reference (Agent 1, citation-backed) + sharpened diagnosis
**The faithful producer↔consumer handshake (RPCS3 cellGcmSys.cpp / RSXFIFO.cpp):**
- **[V] Ring = fixed 32 KB FRAGMENTS, not 1 MB segments.** `g_defaultCommandBufferFragmentCount =
  cmdSize / 32KB` (cellGcmSys.cpp:447-452). First 4 KB of frag0 reserved; last 4 B of each frag = the
  forward-JUMP slot. PUT/GET are io offsets. The producer wraps by cycling fragments (a forward JUMP
  at the old frag's cursor → next frag start); after the last frag it wraps to frag 0.
- **[V] No jump-to-self stopper in cellGcmCallback.** The fragment boundary = flush PUT + write a
  **forward JUMP** to the next fragment (cellGcmSys.cpp:1481-1490). The jump-to-self at io 0x300000 is
  a SEPARATE, game-level "not ready" marker the game patches when it finalizes the list.
- **[V] THE LAP-PREVENTION = a producer-side spin** (cellGcmSys.cpp:1492-1505): before recycling a
  fragment, the producer `busy_wait`s until `isInCommandBufferExcept(ctrl.get, frag.begin, frag.end)`
  — i.e. **until our consumer's GET register has LEFT that fragment.** It reads `ctrl.get`.
- **[V] Consumer (run_FIFO):** forward JUMP → `set_get(offs)` writes the new GET to the register;
  jump-to-self (offs==get) → memwatch-spin in place (don't advance GET) until the game patches the
  word; never reads past PUT (RSXFIFO.cpp:92-96, 209-221, 691-715).

**⇒ SHARPENED DIAGNOSIS (we LLE this producer, so the lap = OUR consumer breaks the handshake):**
Our `deferred-release`/`resync`/`skip-stale` band-aids advance the **GET register** to positions the
consumer hasn't actually processed → Sony's producer sees "GET left the fragment" → recycles it → LAP.
The **faithful** consumer (`YZ_NO_DEFER`: keep GET at the stopper, memwatch-spin) is CORRECT — Sony's
producer then correctly blocks (no lap). The residual deadlock there is **t1 never patching the
io 0x300000 stopper** (it never finalizes the io 0x1104D00 display list; pt9: 0 PPU writes) — a
producer-finalization problem, NOT a consumer bug.

**⇒ REBUILD PLAN (concrete):**
1. Make the consumer FAITHFUL by default: strip the band-aids; process `[GET,PUT)` one cmd at a time;
   forward JUMP → set GET; jump-to-self → memwatch-spin; CALL → follow 1 level; RET → return; never
   past PUT; write GET back at the io offset Sony's `isInCommandBufferExcept` reads. (≈ RPCS3 run_FIFO.)
2. Verify GET-reporting EA matches Sony's `ctrl.get` (the gcm control block) so the producer's spin
   sees our real GET. This removes the lap class entirely.
3. THEN attack the residual: why t1 never finalizes io 0x1104D00 / patches the stopper. (Needs the
   RPCS3 RSX-debug dynamic-timing observation, or the gs_task↔render-thread coupling.)

---

## What's left

1. **NEXT (immediate): pin the producer's exact wait.** A steady-state per-thread guest-PC dump to
   find WHICH thread spins on WHAT (the `*(0x40C00000)` flip-counter poll / the GET-advance it
   waits on). That names the precise handshake to satisfy.
2. **Replay the RPCS3 RSX capture** (`captures/BLUS30826_..._capture.rrc.gz`) in RPCS3's RSX Capture
   viewer → the ground-truth working FIFO structure (how segment stoppers / display-list CALLs are
   laid out in a frame that DOES finalize). Compare to our consumer's parse trace.
3. **Study RPCS3's RSX thread loop** (read-only oracle: `Emu/RSX/RSXThread.cpp` `on_task`/`run_FIFO`
   + `RSXFIFO.cpp`) for the one thing that matters: when its GET reaches a jump-to-self stopper whose
   release is DEFERRED, **what makes it advance** (NOT reservation — that's off; so it's a
   FIFO-processing / timing detail). User can also RSX-debug RPCS3 watching GET cross io 0x300000.
4. **Rebuild our consumer to match RPCS3's RSX execution** (clean-room, don't copy GPL) — match the
   working reference rather than RE'ing the game's gcm internals. EXIT: animating,
   structurally-correct **untextured** window, NO deadlocks.
5. **Then LAYER 2:** NV4097 methods → D3D12 (surface setup, CLEAR, draws, texture upload), then
   **NV40 shader ISA → HLSL/SPIR-V** (clean-room reimplement of RPCS3's decompiler). Staged:
   clear-color → vertex-color geometry → textured → translated shaders.

## In tree (keepers, builds clean)

- **Coherence-safe FIFO consumer** (import_overrides.cpp `yz_rsx_consumer`) — default path, plain
  reads, holds-stopper-until-GET-leaves, NV4097 NON_METHOD validation.
- **Deferred stopper-release** (import_overrides.cpp `yz_gcm_stopper_release_deferred`) — applies the
  game's own queued release when GET reaches it (cracked `@0x300000`).
- **Flip lifecycle + vblank pacing**; **producer defer trace** (main.cpp `yz_rsx_state_trace`,
  `YZ_TRACE_RSX`); **sys_event ABI fix** (upstreamable).
- Gated diagnostic knobs (off by default): `YZ_TRACE_RSX`, `YZ_WATCH_DLIST` (`[ppu-w]` / `[dlist-w]`),
  plus the dead-end experiment toggles (strip before a gameplay commit).

## Oracle resources

- RSX capture: `rpcs3-v0.0.41-.../captures/BLUS30826_..._capture.rrc.gz` (the working command stream).
- RPCS3 RSX source (read-only): `Emu/RSX/RSXFIFO.cpp`, `Emu/RSX/RSXThread.cpp`,
  `Emu/Cell/Modules/cellGcmSys.cpp`, `Emu/RSX/Core/` + `gcm_enums.h`. Spec: envytools/rnndb (MIT),
  Mesa nvfx (MIT). Working config: `config/custom_configs/config_BLUS30826.yml`. Run log: `RPCS3.log`.

## 2026-06-19 deep-dive: verified mechanics + ruled-out fixes (READ before retrying)

**Reliably verified (read the code / measured):**
- RPCS3 `run_FIFO` (Emu/RSX/RSXFIFO.cpp:691-731): on a jump-to-self stopper it `set_get(offs==get)`
  and **spins, never advancing** (waits for the producer to patch it). CALL = ONE nesting level only;
  CALL/JUMP set GET to the target (same as us). So our faithful path matches RPCS3's stopper handling.
- **t1's binding wait is libgcm's reserve `func_02103AAC`** (disassembled from
  `recomp_prx/libgcm_sys_image.bin`, base 0x02100000): a `sys_timer_usleep(30)` loop waiting until the
  ring GET register `*0x10000044` clears the region the producer wants to write. Danger-region width =
  `bufdesc[+30]<<2 = 0x40000<<2 = 1 MB` → the 8 MB io buffer is carved into **1 MB segments**
  (libgcm default, not our choice). `bufdesc = *(libgcm_toc[0x02114000] - 0x7FD8) = 0x0210C3FC`.
- **t1 builds the display lists itself, CPU-side** (write-watch on ea 0x41500100: tid=1, real method
  data via libgcm path `func_00F9AEC8 -> func_00FBE68C -> func_02103570`).
- **t1 polls the flip fence 0x40C00000** (read-watch, tid=1) but is **NOT gated on it** (forcing the
  fence to 220 changed nothing).
- At the stall t1 is **alive in game logic** (sc 90 lwmutex / _sys_memcpy / sc 82/94), not spinning
  in libgcm — it just never does the next gcm flush that would release the io 0x300000 stopper.

**Ruled out this session (do NOT retry without new evidence):**
- Deferred consumer → GET derails into the unbuilt display list io 0x1104D00 (out of ring range).
- Faithful consumer (YZ_NO_DEFER) → GET correctly parks on io 0x300000 but the stopper is never released.
- Force immediate release `S[0x1C]=0` (YZ_IMM_REL), both consumer modes → no effect (t1 never reaches
  the release call; it's blocked/looping before it).
- Fence-kick to 220 (YZ_FENCE_KICK), both modes → no effect (t1 not fence-gated).
- Runtime pin of segment-end `bufdesc+4` → breaks libgcm before frame 1 (libgcm needs that field for
  its own wrap accounting; can't override mid-use).

**Tooling caveat:** the recompiled-PPU guest back-chain is UNRELIABLE (mis-symbolizes string data, e.g.
"func_01114E4C +0x440" = the `/Project/OgreZ_oe/.../main.cpp` string table; host-rip "func_00EAC46C +0xB67"
is past a 30-instruction function). Trust only: register values, the trampoline-ring `caller[]` chain,
write/read-watch (which run in the accessor's context), and disassembled code — NOT the back-chain.

**Open root (not cracked):** t1 (alive, game logic) and GET (stuck on the unreleased io 0x300000 stopper)
are coupled at a level above the consumer mechanics. Freshest unexplored angles: (a) single-segment AT
INIT — find the libgcm init that sets `bufdesc[+30]=0x40000` (1 MB) and override to the full buffer
(structural; the runtime poke can't); (b) replay RPCS3's actual GET/PUT trace from the .rrc to see what
its GET does that ours doesn't; (c) reconsider running the consumer in-line with libgcm vs as a racing thread.

## 2026-06-19 pt22: ROOT established + decision (faithful consumer, NOT HLE)

After the four-agent audit (libgcm-lift FAITHFUL, runtime/consumer CLEAN, PPU/SPU lifter bugs fixed
but not the cause) and reliable RIP profiling, the LAYER-1 deadlock ROOT is settled:

**It is a producer/consumer PACING mismatch, not a single bug.** Our RSX consumer is an async software
thread; the real RSX is a continuous hardware pipeline. t1 deadlocks in libgcm's reserve `func_02103AAC`
(host-RIP profiler CONFIRMED: ~20/24 samples in its usleep, ctr=0x02103AAC) waiting for GET to clear the
ring region it wants to write -- but t1 recycled a 1 MB segment and **wrapped PUT past the stalled GET**
(GET frozen on the io 0x300000 stopper whose release is queued in the op-list but never applied: t1 is in
the reserve, which runs before the drain). Real HW never laps GET. `io 0x1104D00` stays empty because t1
(reserve-blocked) never builds it.

**RPCS3 LLEs gcm for BLUS30826 -- definitively (Loaded module libgcm_sys.sprx; cellGcmSys exports at real
loaded addrs 0x19axxxxx; no HLE config).** So both RPCS3 and we run the SAME libgcm; the ONLY difference
is the RSX consumer (RPCS3's continuous RSXThread vs our async thread that gets lapped). HLE gcm is the
WRONG path -- it does what RPCS3 deliberately avoids for this title, and the game's gcm is split
(inline flush func_00E9BC9C in the game + libgcm.prx reserve), so HLE-ing one half risks state mismatch.

**DECISION: build a FAITHFUL RSX consumer that paces like RPCS3's RSXThread/RSXFIFO** so the producer
never laps GET. Oracle (read-only, GPL -- reimplement): Emu/RSX/RSXThread.cpp, Emu/RSX/RSXFIFO.cpp.
Don't retry: HLE gcm; force-immediate-release (S[0x1C]); fence-kick; runtime segment/ctx->end pin
(breaks libgcm); single-segment via bufdesc[+0x30] (dynamic, not a static knob).

**Reliable tooling discovered (the guest back-chain mis-symbolizes -- do not trust it):** host-RIP
profiler (suspend + GetThreadContext + yz_func_from_host, multi-sample) reliably names a PPU thread's
spin; read/write-watch fire in the accessor's context (reliable caller chain); the trampoline-ring caller[].

## 2026-06-20 pt23: pacing A/B — the consumer rebuild is FALSIFIED as the fix; pivot to PRODUCER-side

pt22 decided to rebuild the consumer faithful + continuous so the producer "never laps GET." Tested
that hypothesis directly before the rewrite, via a cheap env-gated knob.

- **`YZ_TIGHT`** (import_overrides.cpp `rsx_idle()`, OFF by default): replaces the consumer's idle
  `Sleep(1)`s — a ~1-15 ms Windows quantum at every park/idle — with a continuous, FAIR spin
  (YieldProcessor + SwitchToThread). This makes the consumer drain like a hardware pipeline.
- **A/B vs the faithful consumer (`YZ_NO_DEFER=1`):** [V]
  - Baseline: t1 19/24 host-RIP samples in the reserve usleep wait + 4 in `func_02103AAC`; fence=2;
    2 flips; 18 deferred-release entries ("GET's stopper IS in the list").
  - Tight: t1 23/24 in the SAME reserve usleep wait; fence=2; 2 flips; **IDENTICAL** 18-entry defer
    list, same stopper EAs. (scratch/pace_baseline.txt, scratch/pace_tight.txt.)
  - ⇒ A continuously-draining consumer reaches the EXACT same wall. **Consumer pacing is NOT the gate.**
- **Also verified [V]:** GET is written to `0x10000044` (`RSX_DMA_CONTROL 0x10000000 + 0x44`) — exactly
  the EA libgcm's reserve reads. The "GET-reporting EA mismatch" theory is ruled out too.

**Why pacing can't help (supersedes pt22's lap framing):** the faithful consumer CORRECTLY parks GET at
the `io 0x300000` jump-to-self stopper and can never advance past it (that's what a stopper is). GET
freezes there regardless of consumer speed; t1 then wraps PUT around the frozen GET (the "lap"). The
lap is real but its CAUSE is the **unpatched stopper, not consumer slowness** — a true producer-side
circular wait: t1 queued the release into its op-list (18 entries) but never drains it; it's wedged in
libgcm's reserve `func_02103AAC` FIRST, waiting for GET to clear a recycled 1 MB segment, and GET can't
advance because it's on the stopper t1 hasn't patched.

**⇒ The fix is PRODUCER-side.** Highest-leverage untested lever = **single-segment AT INIT** (angle (a)):
intercept the libgcm init that sets `bufdesc[+0x30]/[+30]=0x40000` (1 MB) so the 8 MB io buffer is ONE
segment → t1's reserve recycle-wait only triggers at the ring's very end → t1 never wedges mid-frame →
drains its op-list → patches the `0x300000` stopper → GET advances. The codebase already proved
single-segment kills this deadlock class (the HLE `yz_gcm_fifo_callback`); the LLE version must change
it AT INIT (runtime poke breaks libgcm's wrap accounting — proven). Disambiguator (needs USER + RPCS3):
replay the `.rrc` / RSX-debug to see if RPCS3's GET ever parks at `0x300000` or the stopper is patched
first. **Don't retry (twice-confirmed): consumer rebuild for pacing; runtime poke of bufdesc.**

## 2026-06-20 pt24: 🎉 WALL BREACHED — frame 3 RENDERS + FLIPS (fence 2→3). Mechanism proven; LAYER 1 = a small chain of handshakes (~2 roots), NOT per-frame.

**THE WIN [V]:** `YZ_RESYNC_LOOP=1` → 3 flips, fence=3 (scratch/resyncloop2.txt). First progress past frame 2 in ~10 sessions.

**MECHANISM — proven by autonomous multi-angle crack (see STATUS.md pt24 for the full angle log):**
- **t1 (PPU render thread) builds the per-frame display lists itself, via memcpy** — page-watch on the
  WORKING list io 0x1100100 caught `accessor guest tid=1` + 64-bit stores (scratch/prodwatch.txt).
  pt9's "0 PPU writes" was a vm_write32-only blind spot. (gs_task ruled out for the movie, pt23.)
- **ROOT 1 — the stopper standoff (THE recurring one):** the consumer (GET) WAITS at the game's
  jump-to-self stopper, but waiting HOLDS the ring segment t1 needs to flush the next chunk → t1 can't
  flush → can't patch the stopper → GET waits. Circular. Real HW: the GPU runs continuously and stays
  *behind* the producer, so GET reaches each stopper AFTER the game patched it (never waits). Our
  software consumer catches up and parks, holding t1 hostage. **PROVEN: force GET past the stopper
  (resync GET=PUT) → t1 builds the list (`-> now 0x500A2`, real methods) → reaches the flip submit →
  fence advances** (scratch/resyncwatch.txt). One mechanism, recurs every frame → fix it ONCE (correctly)
  and all frames' stoppers clear. The "every frame needs a poke" appearance is the crude resync only
  breaking one instance + fighting the ring-wrap.
- **ROOT 2 — the flip lifecycle (frame 4, observed not cracked):** at fence=3 the ring is fully drained
  (GET=PUT=0x40030C) and t1 `usleep`-waits in the flip path (ctr=`_cellGcmSetFlipCommand` func_02108AE0;
  20/24 host-RIP samples in a wait). A DIFFERENT mechanism from root 1 (not the reserve). scratch/frame4.txt.

**⇒ "Faithful consumer" reframed:** the consumer we have is faithful at DECODING but NOT at stopper/ring/
flip timing — in faithful mode it deadlocks (parks at the stopper), in default it band-aids (deferred-
release zooms GET into unbuilt lists). The real fix is faithful STOPPER/RING handling, not method decode.

**NEXT SESSION — FIRST ACTIONS (the ~2-root fix, not per-frame):**
1. **ROOT 1 done right:** make the consumer not hold t1 hostage at stoppers — generalize the resync lever
   (advance GET past the stopper / out of t1's reserve segment when t1 is reserve-blocked) **wrap-aware**
   (the crude GET=PUT fights the wrap: GET pulled backward when PUT wraps). The lever is proven; do it
   correctly + integrate into yz_rsx_consumer (or a clean monitor) so it covers ALL frames.
2. **ROOT 2:** crack the frame-4 flip wait — what flip-label/fence/buffer condition `_cellGcmSetFlipCommand`
   / the flip-throttle func_00EAC46C waits on at fence=3 (likely the flip label@0x10200010 clear / the
   vblank→DRIVER_QUEUE→fence lifecycle). Drive it → frame 4 → continuous animation.
3. **REPRO THE WIN first:** `$env:YZ_RESYNC_LOOP="1"; .\yakuza\build\yakuza_recomp.exe game\EBOOT.elf 2> log.txt`
   → grep "FLIP COMPLETE" (=3) + "fence] @0x40C00000" (→3).

**Env knobs in tree (OFF by default, default boot VERIFIED unchanged; strip before gameplay commit):**
`YZ_RESYNC_LOOP` (the working lever, main.cpp yz_resync_loop + frame4 dump), `YZ_GETFOLLOW` (aggressive
pin), `YZ_RESYNC_PROBE`, `YZ_SEGBIG` (single-segment poke, import_overrides yz_segsize_mon — insufficient
alone), `YZ_CALLWAIT`/`YZ_CALLSKIP`, `YZ_WATCH_DLEA=<ea>` (page-watch any display-list EA → names the
producer), `YZ_TIGHT`, `YZ_DUMP_BUFDESC`. Audit tool: `py -3 yakuza\gen_imports.py --audit`.
