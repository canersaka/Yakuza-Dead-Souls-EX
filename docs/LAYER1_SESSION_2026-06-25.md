# LAYER 1 — session record 2026-06-25 (RSX FIFO deadlock: deep measurement + exhaustive rule-out)

This session did NOT fix the LAYER-1 deadlock, but it **measured the mechanism precisely** and
**ruled out a large hypothesis space with evidence**. The problem reduced to a single runtime
question that can only be answered by observing RPCS3 live (the build, see §6). Read this before
re-attempting LAYER 1.

## 1. The PROVEN deadlock mechanism (measured, not inferred)
Pure-faithful config = `YZ_NO_FLOWCTL=1 YZ_NO_DEFER=1` (all GET-forcing band-aids OFF).

- The producer (render thread t1) **laps the entire ring**, then deadlocks when it comes back to
  recycle the segment GET is frozen in. Steady state (`scratch/l1snap.err`, held 45 s):
  `GET=0x300000 (seg3, on a jump-to-self stopper 0x20300000), PUT=0x200004, reserve recycling
  seg3 (reserve_lo=0x300000 reserve_hi=0x3FFFFC), GET INSIDE the reserve window`.
- t1 is in libgcm reserve `func_02103AAC` (sc 141 `sys_timer_usleep`) waiting for GET to leave
  seg3; GET can't leave because it's parked on a stopper that was **never released**.
- WHY the stopper was never released (the root): the release was **DEFERRED** (the game's gcm
  flush `func_00E9BC9C`/`func_00E9BE4C` defers when `S[0x24] != ctx_end`, i.e. the producer
  crossed a 1 MB segment boundary between placing and releasing the stopper — which it does
  because it laps the ring). **And the game has NO code to apply a deferred release** — grep
  confirmed no tag-0x7F dispatcher exists; `func_00E9B7C0` only PLACES stoppers, it is not an
  applier. So the game is NOT designed to defer — on hardware/RPCS3 it releases immediately, and
  our cross-segment deferral is the divergence, caused by the producer lapping the consumer.
- Even the band-aided runs flip **0 draws** — but the `.rrc` ground truth (62 draws) is a 3-D
  frame while our boot is at the **movie** (≈no 3-D draws), so "0 draws" is partly frame-type
  confounded, NOT pure proof of a render bug. The deadlock is the real, frame-independent issue.

## 2. Fixes TRIED and why each FAILED (do not re-propose without new evidence)
- `YZ_APPLY_REL` (write the game's recorded release word over the stopper): freed the reserve
  once (PUT 0x200004→0x300DB0) but GET followed a CALL into a shader buffer (io 0x1104D00) and
  re-wedged — premature, an artifact.
- `YZ_GETBOUND` (don't follow a jump whose target > PUT): the guard's `put>=get` precondition is
  dead when GET>PUT (the wrap case), so it never fired. The non-wrap-aware compare was wrong.
- `YZ_HOLDSEG` (don't follow a jump into a segment whose entry is still an un-released stopper):
  fired, but **shifted the deadlock back one segment** — holding GET back drags the producer's
  recycle target back too (they stay phase-locked one segment apart).
- `YZ_IMM_REL` (clamp the defer latch S[0x1C]=0): 2 flips, identical deadlock (the earlier "10
  flips" was the `yz_flip_advance` band-aid, not IMM_REL). Clamping only stops FUTURE deferrals;
  the already-placed stopper stays frozen.
- `YZ_PROD_THROTTLE=5000` (busy-wait at each reserve to slow the producer): no change. Wrong
  lever — throttling the reserve doesn't prevent the deferral that freezes GET.
- HLE-gcm pivot: REFUTED. `RPCS3.log:60394` shows `cellGcmSys` exported at LLE addr `0x19b017c`
  (`_cellGcmInitBody`), and the HLE `cellGcmInit`/`isHLE` path is never entered — **RPCS3 LLE-runs
  the same libgcm we do.** (The `0x10f3xxx` import-stub addr is the documented weak signal — do
  not mistake it for HLE again; see [[rpcs3-lle-set-blus30826]].)

## 3. RPCS3 source findings (answering "how does it stay coupled")
- The coupling is **emergent thread timing, not logic.** RPCS3's `run_FIFO`/`FIFO_control::read`
  decode + memwatch-spin-on-jump-to-self are the SAME as ours.
- RPCS3 DOES pace: a precise drift-corrected **60 Hz vblank thread** (`RSXThread.cpp:1022`,
  `post_event_time = start + count*period/rate`) and a **`driver_wakeup_delay`** knob
  (`fifo_wake_delay()`) specifically for the NOP+jump-to-self stopper pattern — but that knob is
  **0** in `config_BLUS30826.yml`, so it's not the coupling for this game.
- Our vblank is loose: `main.cpp` `yz_vblank_thread` was `Sleep(16)` (Windows-jittery ~32-62 Hz).
  Added `YZ_VSYNC_PRECISE` (drift-corrected) — a genuine faithfulness improvement, kept.
- INTERPRETER-vs-RECOMPILER: the in-repo `config_BLUS30826.yml` has the PPU/SPU **Interpreter** —
  that was a USER debug choice (for breakpoints), NOT how RPCS3 normally runs the game. RPCS3
  renders it with the fast LLVM recompiler. So "RPCS3's slow producer keeps it coupled" is WRONG.
  Leading remaining suspicion: our consumer dispatches to a null/GDI backend (instant) while
  RPCS3's RSX does real Vulkan rendering (per-command latency), so ours reaches stoppers BEFORE
  they're released and parks. UNVERIFIED — needs the live trace.

## 4. Code audits vs the official specs (refs/) — cheap correctness fixes worth landing
All three audits found NO bug causing the deadlock, but did find:
- **SPU (`scratch/audit_spu.md`): 3 missing disassembler entries** — `orbi`(op8 0x06), `xorbi`(0x46),
  `xorhi`(0x45) absent from `tools/spu_disasm.py` `RI10_TABLE`; the lifter+runtime already handle
  them, so it's a **3-line add**. Silently dropped today.
- **PPU (`scratch/audit_ppu.md`): VMX scramble fully repaired (clean).** One dead-code mis-decode
  to delete (`ppu_disasm.py:679-682` maps XO 827→`lhaux`, should be `sradi`; never executes).
  ~14 unhandled-but-safe VMX op-4 (vsl/vsr/vslo/vsro, vpkpx, vrfin/ip/iz, etc.) fall through.
- **RSX (`scratch/audit_rsx.md`): REF register init gap** — we zero-init REF at context_allocate;
  RPCS3/the gcm doc set it to `0xFFFFFFFF` (cellGcmFinish polls it). Not the deadlock, but will
  bite a REF-based wait later. Also a `yz_rsx_sem_addr` default-to-0 fallthrough that could drop a
  release for an unknown DMA handle (worth hardening).

## 5. New diagnostic knobs added this session (all env-gated, OFF by default, default boot unchanged)
- `YZ_TRACE_DEFER` — gcm defer-decision + op-list trace (`yz_defer_trace_mon`).
- `YZ_L1SNAP` — reserve-window snapshot at the 30 s watchdog (`yz_dump_layer1_snapshot`, main.cpp).
- `YZ_PHASE` — full producer+consumer timeline (GET/PUT/cursor/flip) + per-reserve log.
- `YZ_PROD_THROTTLE=<us>`, `YZ_VSYNC_PRECISE`, `YZ_APPLY_REL`, `YZ_GETBOUND`, `YZ_HOLDSEG`,
  pkg-0x001 FIFO-set logging (`YZ_LOG_FIFOSET`).
- Raw captures: `scratch/{defer_faithful,l1snap,getbound_fifo,throttle5k,immrel_verify,...}.{err,txt}`.

## 6. THE decisive next step — the RPCS3 GET/PUT trace (only way to answer the open question)
The one thing no static analysis can give: does RPCS3's GET ever **park on the io 0x300000
stopper**, or is it always pre-released? That splits "consumer reaches stoppers too early" vs
something else.
- BUILD STATUS (in progress, user's machine): fresh recursive clone at
  `C:\Users\csaka\Downloads\rpcs3clone\rpcs3`; LLVM built; **patch applied at
  `Emu/RSX/RSXFIFO.cpp:713`** =
  `rsx_log.error("rsx jump: get=0x%x put=0x%x offs=0x%x cmd=0x%x", fifo_ctrl->get_pos(), (ctrl->put & ~3), offs, cmd);`.
  **BLOCKED on Qt:** the Qt 6.11.1 `msvc2022_64` kit installed libs but NOT the tools
  (`bin\qmake.exe`/`moc.exe` missing) — suspected antivirus quarantining the dev `.exe`s. Fix:
  add `C:\Qt` AV exclusion + reinstall MSVC 2022 64-bit (or `aqtinstall` once a version resolves).
- CAPTURE PLAN: run BLUS30826 ~15-20 s with **Recompiler (LLVM)** PPU+SPU decoders (not the
  interpreter), Stop, send `<build>/log/RPCS3.log`. Grep `rsx jump`; check whether GET sits at
  `0x300000` and for how long relative to PUT.

## 7. Reference material gathered (refs/) — for future correctness work, NOT the FIFO deadlock
The FIFO/gcm reserve+op-list+stopper protocol is **Sony SDK-internal — in NO public doc** (verified
against the actual psdevwiki RSXFIFOCommands page + Nucleus gpu.md). Reverse-engineering from the
lifted binary + RPCS3 is correct and necessary. Gathered into `refs/`: Cell BE / SPU ISA / SPU ABI /
SPU Assembly / PowerPC ISA / AltiVec PEM PDFs, `RSXFIFO.txt` (psdevwiki, confirms our decode),
`nucleus_gpu.md`, SPU channel docs. Open-source gcm references to consult if needed: PSL1GHT,
libps3rsx (both simplify the op-list away, like RPCS3's not-used HLE path).
