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
| `YZ_NO_FLOWCTL` | yakuza/main.cpp ~1666 | Kill-switch for the **flow-control band-aid (default ON)** — 3 lossy forces (GET-skip, fence-nudge, SET_REFERENCE re-apply) that carry the default boot past the gs_task geometry wall. **Retirement condition: gs_task finishes its geometry segment** (the LS-0x44 / LS[0xBD70] frontier). Setting this = faithful mode. |
| `YZ_NO_THR_NUDGE` | yakuza/main.cpp ~939 | Kill-switch for the throttle nudge (paired with the flow-control band-aid). |
| `YZ_NO_APPLY_REL` | import_overrides.cpp ~1132 | Kill-switch for the **faithful deferred stopper-release applier (f8d0386)** — the committed LAYER-1 fix. Keep. |
| `YZ_NO_LAUNCH_UNWIND` | spu_channels.c ~946 | Kill-switch for the SPU task launch-unwind (5882fe4). Keep. |
| `YZ_NORESUME` | spu_channels.c ~1026 | Kill-switch for the SPURS yield/resume context-switch path. Keep. |

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

Tracing/watches: `YZ_SPU_PROF`, `YZ_SPU_TRACE`, `YZ_SPU_TRACE_IMG`, `YZ_IMGLOG`, `YZ_SIGW`,
`YZ_SIGCNT`, `YZ_LRWAKE`, `YZ_LS_DUMP`, `YZ_HALT_LOG`, `YZ_POLTRACE`, `YZ_POLHOP`,
`YZ_DISP_TRACE`, `YZ_TRACE_CODEC`, `YZ_CODEC_WATCH`, `YZ_ELF_WATCH`, `YZ_PUT_WATCH`,
`YZ_TS_WATCH`, `YZ_TASK_TRACE`, `YZ_TASK_RET`, `YZ_CB_TRACE`, `YZ_DRAIN_TRACE`, `YZ_RECPROBE`,
`YZ_PHASE`, `YZ_FIFO_TRACE`, `YZ_TRACE_RSX`, `YZ_TRACE_DEFER`, `YZ_LOG_FIFOSET`,
`YZ_DUMP_BUFDESC`, `YZ_DUMP_SEG`, `YZ_WATCH_LIST`, `YZ_WATCH_DLEA`, `YZ_WATCH_OPLIST`,
`YZ_WATCH_300`, `YZ_WATCH_FENCE`, `YZ_WATCH_EA`, `YZ_WATCH_READ`, `YZ_WATCH_DLIST`,
`YZ_WATCH_FLAG`, `YZ_WATCH_BD`, `YZ_L1SNAP`, `YZ_GCMCTX_BISECT`, `YZ_GUARD`, `YZ_GUARD_FULL`,
`YZ_GUEST_ADDR`, `YZ_ARG`, `YZ_VSYNC_PRECISE`, `YZ_THROTTLE_DIAG` (sys_timer.c — REVERT
before commit per STATUS), `YZ_GEO_PROBE` / `YZ_GEO_SKIPNULL` (spu_channels.c — UNCOMMITTED
probes from the 2026-07-01 geometry session; REVERT before commit).

## RETIRED / refuted (do not re-enable; delete when convenient)

| Flag | Verdict |
|---|---|
| `YZ_FLIPADV` | Band-aid retired by the faithful flip lifecycle (6fbc3c6). |
| `YZ_FORCE_START` | Retired bc8efa6 (superseded by the 5882fe4 image-match fix). |
| `YZ_FRC`, `YZ_FRC3` | readyCount force — superseded by the real poll-status path. |
| `YZ_CLEARRUN`, `YZ_CLEARRUN3`, `YZ_FIXRUN`, `YZ_FIXEXIT`, `YZ_FORCE_TASK`, `YZ_POLLFORCE`, `YZ_NOLAUNCH`, `YZ_NO_MGMT`, `YZ_NOHELPRET`, `YZ_CORET_GEN` | SPURS-dispatch-era forces; the dispatch now works via the real path. |
| `YZ_IMM_REL`, `YZ_NO_DEFER`, `YZ_ONESEG`, `YZ_SEGBIG`, `YZ_BIG_SEG` | FIFO/stopper experiment forces; refuted or superseded by f8d0386 (see archive tested-negative ledgers). |
| `YZ_WKLSIG` | Signal-era probe/force; machinery proven healthy. |
