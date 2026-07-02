# FLAGS.md — registry of `YZ_*` environment flags

Rule (docs/LESSONS.md #13): every flag added to the runtime/runner gets a row here — purpose,
default, category, and (for band-aids) the retirement condition. Update this file in the same
change that adds/retires a flag. Categories:

- **load-bearing** — default behavior depends on it; removing it changes the default boot.
- **kill-switch** — turns OFF a committed fix, for A/B verification. Default: fix ON.
- **lever** — opt-in experiment/force for diagnosis. Default OFF. Never load-bearing.
- **config** — address/size overrides for the memory map. Stable.
- **diag** — logging/tracing/watch probes. Default OFF, must be side-effect-free when unset.
- **RETIRED/refuted** — kept only until deleted; do not re-enable (see archive ledgers).

Last full audit: 2026-06-29 (STATUS archive); inventory refreshed 2026-07-01.

## Load-bearing (the ones that matter)

| Flag | Where | Meaning |
|---|---|---|
| `YZ_NO_FLOWCTL` | yakuza/main.cpp ~1779 | **UN-RETIRED 2026-07-03 (default ON again)** — the 07-02 retirement was validated on one lucky 60s window. The LAYER-1 race persists: GET occasionally wins against the game's deferred patch drain, parks in an un-patched display list, t1 wedges in the libgcm reserve, and the whole CRI/codec init never runs (~1/2 of boots after the SPU_RET+backoff timing shift; codec-launch reproducer 4/4 GOOD with the lever ON). **NEW retirement condition:** the stopper-release applier respects full journal order (data patches + sublists applied before the release), or the GET-enters-unfinished-list root is fixed. |
| `YZ_NO_THR_NUDGE` | yakuza/main.cpp ~951 | Kill-switch for the throttle nudge — lives INSIDE the (now opt-in) yz_flip_advance thread, so it's inert unless `YZ_FLOWCTL=1`. Retires with the band-aid code. |
| `YZ_NO_APPLY_REL` | import_overrides.cpp ~1132 | Kill-switch for the **faithful deferred stopper-release applier (f8d0386)** — the committed LAYER-1 fix. Keep. |
| `YZ_NO_LAUNCH_UNWIND` | spu_channels.c ~946 | Kill-switch for the SPU task launch-unwind (5882fe4). Keep. |
| `YZ_NO_SPUBACKOFF` | spu_dma.h (GETLLAR) | Kill-switch for the **SPU idle-poll host-yield backoff** (2026-07-03): same-line GETLLARs with an unchanged write-generation escalate cpu-pause → scheduler-yield, so 5 spinning SPURS kernels stop saturating the global lock-line lock (measured 5×~97% core + boot-pacing collapse without it). Faithful (polling continues; ladder resets on any observed write). Keep. |
| `YZ_NORESUME` | spu_channels.c ~1026 | Kill-switch for the SPURS yield/resume context-switch path. Keep. |
| `YZ_STARTTASK_HOOK` | spu_channels.c (spu_task_launch_check + prof path) | **RETIRED-to-opt-in 2026-07-02**: re-enables the legacy "LS 0x1CC0 = StartTask" launch hijack for A/B. LS 0x1CC0 is actually the taskset-SYSCALL switch (`bi $r2`, jump table at 0x1CC4); the hijack turned every WAIT_SIGNAL/YIELD of the matched elfs into a bogus instant relaunch and skipped Sony's context save. Default OFF = Sony's case handlers + dispatch run lifted. Delete after quiet sessions. |

## Config (memory-map overrides — stable)

`YZ_TLS_BASE`, `YZ_HEAP_BASE`, `YZ_HEAP_END`, `YZ_LIBSRE_BASE`, `YZ_LIBGCM_BASE`,
`YZ_IMPORT_OPD_BASE`, `YZ_IMPORT_FAKE_BASE`, `YZ_GCM_CTX_ADDR`, `YZ_GCM_CB_OPD_ADDR`,
`YZ_GCM_CTRL_ADDR`, `YZ_GCM_LABELS_ADDR`, `YZ_GCM_CB_FAKE_KEY`, `YZ_GCM_LOCAL_BASE`,
`YZ_GCM_LOCAL_SIZE` (import_overrides.cpp / yakuza_runner.h).

## Levers (opt-in, default OFF — for diagnosis only)

| Flag | Where | Meaning |
|---|---|---|
| `YZ_FORCE_CODEC` | import_overrides.cpp ~1924 | Force the CRI codec task path (blocker-#22 lever). |
| `YZ_RSX_INLINE` | import_overrides.cpp / shims.cpp / sys_timer.c | Run the FIFO consumer inline with the producer (tested: does NOT fix the pacing wall by itself). |
| `YZ_TIGHT` | import_overrides.cpp ~1032 | Tight-poll consumer (tested-negative as a fix). |
| `YZ_AUDIO_FORCE` | libs/audio/cellAudio.c ~730 | Force audio port behavior. |
| `YZ_SKIP_VOICE` | yakuza/shims.cpp ~218 | Skip the CRI intro-voice path — useful as a RECON probe past the movie gate, not a shipping path. |

## Diagnostics (default OFF; side-effect-free when unset)

**Permanent generic probes (2026-07-02 — prefer these over new hardcoded ones):**
`YZ_PEEK=ea1,ea2,...` (main.cpp: change-triggered 4-word dumps of up to 16 hex guest EAs,
VirtualQuery-guarded — moves a memory probe with no rebuild); `YZ_HOOK=addr1,...`
(dispatch.cpp: log args+lr on every INDIRECT call to up to 8 guest code/OPD addresses —
direct `bl` calls are invisible; libsre names in scratch/libsre_lle_map.txt).

Tracing/watches: `YZ_SPU_PROF`, `YZ_SPU_TRACE`, `YZ_SPU_TRACE_IMG`, `YZ_SPU_TRACE_N`
(instruction budget for YZ_SPU_TRACE, default 600000; output is unbuffered so a crashing SPU
keeps its trace tail — added 2026-07-01), `YZ_SPU_TRACE_SPU` (lock the tracer to a specific
SPU id instead of first-seen; `any` disables the SPU filter for PC and rt lines — added
2026-07-02), `YZ_SPU_TRACE_EVARM` (hold trace arming until an event site fires — the 0xA70
taskset-syscall probe or the CRI request-queue GETLLAR in spu_dma.h; added 2026-07-02),
`YZ_QLINE` (spu_context.h: log lifted image-3 stores to the GETLLAR line copy at LS 0x80 —
2026-07-02, REMOVE with the frontier),
`YZ_CTXSAVE_WATCH` (DMA + syscall-entry watch on the task context-save protocol: logs
transfers touching LS [0x2C80,0x3000) and the three save-bail checks at the 0xA70 syscall —
added 2026-07-02, REMOVE when the codec frontier closes), `YZ_CODEC_PUT` (PUT-class DMAs +
line atomics with pc from task images 3/4; dumps the LS line for atomics on the CRI queues;
its request-line GETLLAR releases YZ_SPU_TRACE_EVARM — 2026-07-02, REMOVE with the
frontier), `YZ_SIGCALL`
(dispatch.cpp: log indirect calls into the libsre LLE signal/queue family, addresses in
scratch/libsre_lle_map.txt — 2026-07-02, REMOVE with the frontier), `YZ_IMGLOG`, `YZ_SIGW`,
`YZ_SIGCNT`, `YZ_LRWAKE`, `YZ_LS_DUMP`, `YZ_HALT_LOG`, `YZ_POLTRACE`, `YZ_POLHOP`,
`YZ_DISP_TRACE`, `YZ_TRACE_CODEC`, `YZ_CODEC_WATCH`, `YZ_ELF_WATCH`, `YZ_PUT_WATCH`,
`YZ_TS_WATCH`, `YZ_TASK_TRACE`, `YZ_TASK_RET`, `YZ_CB_TRACE`, `YZ_DRAIN_TRACE`, `YZ_RECPROBE`,
`YZ_PHASE`, `YZ_FIFO_TRACE`, `YZ_TRACE_RSX`, `YZ_TRACE_DEFER`, `YZ_LOG_FIFOSET`,
`YZ_DUMP_BUFDESC`, `YZ_DUMP_SEG`, `YZ_WATCH_LIST`, `YZ_WATCH_DLEA`, `YZ_WATCH_OPLIST`,
`YZ_WATCH_300`, `YZ_WATCH_FENCE`, `YZ_WATCH_EA`, `YZ_WATCH_READ`, `YZ_WATCH_DLIST`,
`YZ_WATCH_FLAG`, `YZ_WATCH_BD`, `YZ_L1SNAP`, `YZ_GCMCTX_BISECT`, `YZ_GUARD`, `YZ_GUARD_FULL`,
`YZ_GUEST_ADDR`, `YZ_ARG`, `YZ_VSYNC_PRECISE`, `YZ_THROTTLE_DIAG` (sys_timer.c — REVERT
before commit per STATUS). (Removed 2026-07-02 with the geometry root fixed: `YZ_GEO_PROBE`,
`YZ_GEO_SKIPNULL`, `YZ_LSWATCH`, `YZ_LSWRITE` — stripped from the code; re-derive fresh
probes if a new LS-provenance question comes up rather than resurrecting these ranges.)

## RETIRED / refuted (do not re-enable; delete when convenient)

| Flag | Verdict |
|---|---|
| `YZ_FLIPADV` | Band-aid retired by the faithful flip lifecycle (6fbc3c6). |
| `YZ_FORCE_START` | Retired bc8efa6 (superseded by the 5882fe4 image-match fix). |
| `YZ_FRC`, `YZ_FRC3` | readyCount force — superseded by the real poll-status path. |
| `YZ_CLEARRUN`, `YZ_CLEARRUN3`, `YZ_FIXRUN`, `YZ_FIXEXIT`, `YZ_FORCE_TASK`, `YZ_POLLFORCE`, `YZ_NOLAUNCH`, `YZ_NO_MGMT`, `YZ_NOHELPRET`, `YZ_CORET_GEN` | SPURS-dispatch-era forces; the dispatch now works via the real path. |
| `YZ_IMM_REL`, `YZ_NO_DEFER`, `YZ_ONESEG`, `YZ_SEGBIG`, `YZ_BIG_SEG` | FIFO/stopper experiment forces; refuted or superseded by f8d0386 (see archive tested-negative ledgers). |
| `YZ_WKLSIG` | Signal-era probe/force; machinery proven healthy. |
