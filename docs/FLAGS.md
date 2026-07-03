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
| `YZ_FLOWCTL` | yakuza/main.cpp ~1788 | **RETIRED AGAIN 2026-07-02 (default OFF; this flag opts back IN for A/B).** The race the lever covered is root-caused: OUR deferred-release applier raced Sony's real journal consumer (see `YZ_APPLY_REL`). With both off, 12/12 boots show zero type-1 wedges (state-classified, not lucky-window: 2 slow-but-healthy under compile load; 1 instance of the separate pre-existing late audio race). Evidence scratch/{bad1,cfgA*,cfgB*,val*}.err. Delete with `YZ_APPLY_REL` after quiet sessions. |
| `YZ_APPLY_REL` | import_overrides.cpp ~1216 | **RETIRED 2026-07-02 (default OFF; opts the old deferred-release applier back IN for A/B).** The applier (f8d0386) was correct scaffolding while the real consumer couldn't run; post il/SPU_RET/backoff, gs_task does the whole journal job itself (measured [gs-put]: patch PUTs pc 0xB60C, FENCED release PUTs pc 0x5F00; GET never met an unpatched stopper in 12 applier-off boots). Leaving it on RACES Sony's consumer — releases land without the preceding patches: 3/3 applier-on boots wedged t1 at ~+6 s, 0/12 off. Default = faithful memwatch spin at stoppers. Delete with `YZ_FLOWCTL` after quiet sessions. |
| `YZ_NO_THR_NUDGE` | yakuza/main.cpp ~951 | Kill-switch for the throttle nudge — lives INSIDE the (now opt-in) yz_flip_advance thread, so it's inert unless `YZ_FLOWCTL=1`. Retires with the band-aid code. |
| `YZ_NO_APPLY_REL` | (gone) | **Flag retired 2026-07-02** — the applier it disabled is now default-OFF; the polarity inverted into `YZ_APPLY_REL` (see above). |
| `YZ_JRNL` | import_overrides.cpp (yz_jrnl_retire_through) | OPT-IN EXPERIMENT (2026-07-02): journal **retirement sweep** — when GET applies a deferred release, zero the journal entry tags behind it (the game's GPU-progress ledger; the EDGE consumer's contract per the RPCS3 oracle). UNVALIDATED and now MOOT (2026-07-02 late): reachable only via the retired `YZ_APPLY_REL` path, and the real consumer (gs_task) both applies and retires the journal itself — delete with the applier. Five sibling designs tested + refuted 2026-07-02: eager apply (GET escapes into unbuilt lists), eager release (GET outruns producer), consume-once pending set, zero-all with/without lag-by-one (producer freezes at ~24 entries — it re-reads its own entries). |
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
`YZ_DUMP_AT=<seconds>` (main.cpp: fire yz_dump_all_threads ONCE at +N s regardless of
watchdog state — reads a healthy-but-parked boot at a chosen instant; pair with YZ_L1SNAP
for the invasive sub-dumps; don't use dump-armed runs for pass/fail rates);
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
frontier), `YZ_OVL`
(spu_dma.h: the entry-7 gate probe — [ovl] logs code-sized GETs into LS ≥0x10000 per image
(the image-5 runtime overlay load's source EA + size; image-5 sources also dumped to
scratch\ovl_&lt;ea&gt;_&lt;lsa&gt;.bin, first 16) and [job-rd] logs GET/GETLLAR reads of the published
shader-stream job block [0x40197100,0x40197400) to name the consumer, and [job-bin] logs
image-13 (job module) code-sized GETs past its own end = runtime-loaded JOB BINARIES
(source EA + LS base, the next lift target) — added 2026-07-03,
REMOVE when the jobchain frontier closes. Session-7 additions under the same flag:
[job-io] = every DMA issued by jobchain images 13-15 (pc discriminates module vs job code),
[job-cmd] = command-stream/descriptor fetches with the fetched u64 (change-triggered per
ea, incl. GETLLAR — shows every DISTINCT command the module decodes), [job-cas] = jobchain
header PUTLLC commits with pc + the +0x20..0x2F mask bytes (change-triggered, grab latch
+0x29 masked out); the always-on [dma-null] EA-0 atomic diag now also dumps gpr2-5,
gpr80-82/126/127, the r3 object quads and the taskInfo quads — all REMOVE with the
jobchain/pxd-dispatch frontier), `YZ_TS_PEEK`
(spu_dma.h GETLLAR: change-triggered snapshot of the pxd taskset bitset line 0x40199D00 —
[ts-peek] prints word0 of running/ready/pending_ready/enabled/signalled/waiting with the
reader's img+pc. The wid-0 policy fork discriminator: img 2 sees pend=0x80000000 yet never
launches ⇒ the policy's SELECT_TASK lift; bitsets all-zero while the PPU create-CAS commits
⇒ lost write/visibility — added 2026-07-03 s8, REMOVE with the pxd-dispatch frontier),
`YZ_JRNL_WATCH`
(spu_dma.h: the LAYER-1 consumer discriminator — logs every DMA/atomic touching the gcm
journal HEAD lines 0x41F00080/0x42100080 (with a 32-byte line dump = entry-0 tag+ea) and
every PUT-class into the journal arena [0x41F00000,0x42110000); first 80 hits full, then
every 4096th; also [jrnl-cur] = the consumer's walking-cursor GETLLARs caught by LSA
0x37780 at any EA, and an event-arm release for YZ_SPU_TRACE_EVARM at the journal-head
GETLLAR — 2026-07-02, REMOVE when the producer-side journal frontier closes), `YZ_COND_TRACE`
(sys_cond.c + sys_mutex.c: logs WAIT-enter/exit pairs and any SIGNAL that blocks acquiring
the mutex CS on low-id conds, plus recursive-trylock re-entries — the boot-stall hunt's
sync-layer x-ray; 2026-07-02, retire with the stall frontier), `YZ_GSPUT`
(spu_dma.h: logs every put-class DMA issued under SPU image 0 with pc+ea+size — the probe
that proved gs_task's back half applies journal patches (plain PUTs, pc 0xB60C) and issues
FENCED stopper-release PUTs (pc 0x5F00); 2026-07-02, retire with `YZ_JRNL_WATCH`), `YZ_SIGCALL`
(dispatch.cpp: log indirect calls into the libsre LLE signal/queue family, addresses in
scratch/libsre_lle_map.txt — 2026-07-02, REMOVE with the frontier), `YZ_IMGLOG`, `YZ_SIGW`,
`YZ_SIGCNT`, `YZ_LRWAKE`, `YZ_LS_DUMP`, `YZ_HALT_LOG`, `YZ_POLTRACE`, `YZ_POLHOP`,
`YZ_DISP_TRACE`, `YZ_TRACE_CODEC`, `YZ_CODEC_WATCH`, `YZ_ELF_WATCH`, `YZ_PUT_WATCH`,
`YZ_TS_WATCH`, `YZ_TASK_TRACE`, `YZ_TASK_RET`, `YZ_CB_TRACE`, `YZ_DRAIN_TRACE`, `YZ_RECPROBE`,
`YZ_PHASE`, `YZ_FIFO_TRACE`, `YZ_TRACE_RSX`, `YZ_TRACE_DEFER`, `YZ_LOG_FIFOSET`,
`YZ_DUMP_BUFDESC`, `YZ_DUMP_SEG`, `YZ_WATCH_LIST`, `YZ_WATCH_DLEA`, `YZ_WATCH_OPLIST`,
`YZ_WATCH_300`, `YZ_WATCH_FENCE`, `YZ_WATCH_EA`, `YZ_WATCH_READ`, `YZ_WATCH_DLIST`,
`YZ_WATCH_FLAG`, `YZ_WATCH_BD`, `YZ_L1SNAP` (**gates ALL invasive watchdog dumps** — the
all-threads snapshot's serial thread-suspend + stack walks take 60+ s and froze the guest
+30s→+90s when made always-on 2026-07-02, invalidating four validation loops; even the
t1-only host-stack/spin dumps suspend t1 in the PortStart window and flipped a ~3/4 baseline
to 0/3 — a diagnostic must never perturb the system it measures), `YZ_GCMCTX_BISECT`,
`YZ_GUARD`, `YZ_GUARD_FULL`,
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
