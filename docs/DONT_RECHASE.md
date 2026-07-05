# DONT_RECHASE.md — canonical refuted-claims / tested-negative ledger

**Grep THIS file before proposing any root cause, fix, or instrumentation approach.**
It exists because re-proposing a refuted theory costs a session, and it has happened
repeatedly. Rules:

- One entry per refuted theory / dead end. Each cites its evidence. Entries reflect the
  MEASUREMENT AT THAT DATE — if the system changed since (relift, new fix), say so when
  overturning, and mark the entry `OVERTURNED <date>` rather than deleting it.
- Add an entry the moment a theory is refuted BY MEASUREMENT (not by code-reading).
- Full receipts and the older tested-negative ledgers live verbatim in
  `docs/archive/STATUS_ARCHIVE_2026-06.md` and `docs/archive/STATUS_ARCHIVE_2026-07.md` —
  grep those too before working any OLD frontier.

## The current frontier's dead ends (frame-stall livelock, sessions 10–13)

| # | Refuted claim / dead end | Verdict + evidence | Date |
|---|---|---|---|
| 1 | "The boot wall is the CRI intro-movie gate — wire the FFmpeg decoder into `mwPly` to unblock boot" | The gate is UPSTREAM of the movie player: a CRI producer stall right after `scenario.bin` (cri_dlg gets no work item ~#52+). mwPly wiring is real work for LATER (post-stall), not the unblock. | 07-04/05 |
| 2 | "The boot wall is voice-audio decode readiness (adxm +294/+298/+29C fields)" | RPCS3 proceeds past `adv_voice_talk.cvm` WITHOUT decoding it (opens it, reads the index, moves to `all_csb.par` 1.7 s after `scenario.bin` — RPCS3.log 0:30:47→0:30:49). The adxm fields were an inherited pt47 guess, never a measured gate. | 07-05 |
| 3 | "The intro voice is plain ADX — decode it / get a bigger FFmpeg" | Container holds ADX **and** HCA; the AHX lead is NOT standard MPEG-L2 past frame 1 (measured desync, scratch/ahx_decode_test.c). Codec choice is irrelevant to the boot stall either way (see #2). | 07-04/05 |
| 4 | "pxd taskset 0x40199D00 is never created — that's the root" | In faithful RPCS3 pxd IS created, run, reaped once ~24.5 s guest-time (instrumented-RPCS3 guest-memory watch). We stall long before 24.5 s ⇒ the non-creation we measured is a downstream TIMING artifact. Sessions 5–12 detour. | 07-05 |
| 5 | "Force wid0/wid1 workload-selectable to fire the port-17 heartbeat" | Forced SELECT=1 fired 372× and wid0 STILL never dispatched (no image load, no doorbell) — scratch/_wid0force*.err. Workload-level forcing can't reach the task level; and per #4 the whole wid0 chain is downstream anyway. | 07-05 |
| 6 | "Instrument RPCS3's HLE cellSpurs to oracle task creation" | Dead code: RPCS3 LLE-loads the real `libsre.sprx` for BLUS30826 (RPCS3.log `sys_prx: Loaded module`); its HLE `cellSpursCreateTask*` measured 0 calls in 3:21. Use guest-PPU tracing at the libsre load addresses instead. | 07-05 |
| 7 | "t1 is hard-wedged forever in cellSpursEventFlagWait 0x4019C680 — that's the root" | It's a CHAIN/livelock, not one wait: forcing the flag advances t1 exactly one step to the next wait (scratch/_force1/2.err); steady state = many threads cycling sync syscalls. The evflag is a downstream symptom of the producer stall. | 07-04/05 |
| 8 | "cri_dlg's dispatcher is broken / its work-flag never gets set" | Refuted twice over: the flag was armed+cleared ~50 cycles (write-watch), and cri_dlg ran 51 full work-loop iterations control-flow-IDENTICAL to RPCS3 (trace-diff, 11,110 PCs, zero divergence — re-verified 07-05). Consumer EXONERATED. | 07-05 |
| 9 | "Our sys_cond loses wakeups at the stall (lost-wakeup bug)" | Refuted: t11's `sc 107 (cond_wait id=4)` returns 0x0 (woken) over and over at the stall — it wakes, finds no work, re-waits (scratch/t1spin.err). The wake path works; the PRODUCER stops producing. | 07-05 |
| 10 | "The frame count stalls at a ~30–32 ceiling" (as a metric) | The `[live-draw] frame N` log was HARD-CAPPED at 32 (rsx_live_draw.c) — every log-derived "ceiling ≈ 32" was the cap, not a measurement. Instrument fixed 07-05 (logs every 32nd frame past 32). Re-measure before citing any ceiling. The boot IS stuck (no `all_csb.par`, no `.sfd` ever opened; t1 spins) — but the "32" number was an artifact. | 07-05 |
| 11 | "mwPly was probed and is never called" (as a PROVEN negative) | 0 probe hits, but no probe-liveness banner exists in the logs — can't distinguish "never called" from "probe not armed". Treat as PLAUSIBLE, not proven. (Moot for the boot per #1, but don't cite it as measured.) | 07-05 |
| 12 | "A decode, lift, ABI, or SPU-math fix will unblock the producer stall" | s15 fixed fsqrt (1258 sites), 16 HLE ABI signatures, the SPU fma/fesd/frds and fi/frest/frsqest math, the PPU overflow and vupk lift gaps, and F18, then did a full relift and rebuild. The golden boot milestone vector came back IDENTICAL to baseline (exited_early=no, reached_audio=YES, vblank_alive, deepest=sys_lwmutex_trylock). None of it moves the stall, which is behavioral in tid=1, not a translation bug. Go straight to the tid=1 vs RPCS3 producer trace-diff. | 07-05 |

## Older classics (one-liners; receipts in the archives)

- **crnand/crnor disasm swap symptoms** — the acitm0039 freeze, "CriFsLoader pool exhaustion",
  0x8001000A, the s8 WRAP: ALL symptoms of the swapped opcode table (fixed edfdc7f). Never
  re-open the acitm hunt.
- **The 0x1CC0 "StartTask hook"** — 0x1CC0 is the taskset-SYSCALL switch, not StartTask;
  hooking it hijacked Sony's dispatch (retired c4d6a54).
- **"RPCS3 stubs/HLEs X" claims** — usually FALSE for this game (libsre, libgcm_sys, libfs,
  libsysmodule are LLE-loaded). Check the `Loaded module:` log lines first.
- **Flow-control lever / deferred-release applier** (YZ_FLOWCTL / YZ_APPLY_REL) — both retired
  default-OFF after the applier was measured RACING Sony's own consumer; don't turn them back on
  to "fix" FIFO stalls.
- **il double-sign-extension / cntlzw(0) / bi-$r0 bare-return / brhz halfword classes** — fixed;
  if a NEW symptom smells like these, check the conformance suite first (`py -3
  tools\test_ppu_lift.py`), don't re-derive from scratch.
- **Forcing stuck flags/bits generally** (YZ_FRC, YZ_CLEARRUN, YZ_FORCE_START, YZ_EVFLAG_FORCE…)
  — every force was eventually refuted or retired; a stuck bit is a SYMPTOM (LESSONS #3).
