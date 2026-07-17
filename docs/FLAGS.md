# FLAGS.md â€” registry of `YZ_*` environment flags

Rule: every flag added to the runtime/runner gets a row here â€” purpose,
default, category, and (for band-aids) the retirement condition. Update this file in the same
change that adds/retires a flag. Categories:

- **load-bearing** â€” default behavior depends on it; removing it changes the default boot.
- **kill-switch** â€” turns OFF a committed fix, for A/B verification. Default: fix ON.
- **lever** â€” opt-in experiment/force for diagnosis. Default OFF. Never load-bearing.
- **config** â€” address/size overrides for the memory map. Stable.
- **diag** â€” logging/tracing/watch probes. Default OFF, must be side-effect-free when unset.
- **RETIRED/refuted** â€” kept only until deleted; do not re-enable (see archive ledgers).

Last full audit: 2026-06-29 (STATUS archive); inventory refreshed 2026-07-01.

## Load-bearing (the ones that matter)

| Flag | Where | Meaning |
|---|---|---|
| `YZ_FLOWCTL` | yakuza/main.cpp ~1788 | **RETIRED AGAIN 2026-07-02 (default OFF; this flag opts back IN for A/B).** The race the lever covered is root-caused: OUR deferred-release applier raced Sony's real journal consumer (see `YZ_APPLY_REL`). With both off, 12/12 boots show zero type-1 wedges (state-classified, not lucky-window: 2 slow-but-healthy under compile load; 1 instance of the separate pre-existing late audio race). Evidence scratch/{bad1,cfgA*,cfgB*,val*}.err. Delete with `YZ_APPLY_REL` after a quiet stretch of boots. |
| `YZ_APPLY_REL` | import_overrides.cpp ~1216 | **RETIRED 2026-07-02 (default OFF; opts the old deferred-release applier back IN for A/B).** The applier (f8d0386) was correct scaffolding while the real consumer couldn't run; post il/SPU_RET/backoff, gs_task does the whole journal job itself (measured [gs-put]: patch PUTs pc 0xB60C, FENCED release PUTs pc 0x5F00; GET never met an unpatched stopper in 12 applier-off boots). Leaving it on RACES Sony's consumer â€” releases land without the preceding patches: 3/3 applier-on boots wedged t1 at ~+6 s, 0/12 off. Default = faithful memwatch spin at stoppers. Delete with `YZ_FLOWCTL` after a quiet stretch of boots. |
| `YZ_NO_THR_NUDGE` | yakuza/main.cpp ~951 | Kill-switch for the throttle nudge â€” lives INSIDE the (now opt-in) yz_flip_advance thread, so it's inert unless `YZ_FLOWCTL=1`. Retires with the band-aid code. |
| `YZ_NO_APPLY_REL` | (gone) | **Flag retired 2026-07-02** â€” the applier it disabled is now default-OFF; the polarity inverted into `YZ_APPLY_REL` (see above). |
| `YZ_JRNL` | import_overrides.cpp (yz_jrnl_retire_through) | OPT-IN EXPERIMENT (2026-07-02): journal **retirement sweep** â€” when GET applies a deferred release, zero the journal entry tags behind it (the game's GPU-progress ledger; the EDGE consumer's contract per the RPCS3 oracle). UNVALIDATED and now MOOT (2026-07-02 late): reachable only via the retired `YZ_APPLY_REL` path, and the real consumer (gs_task) both applies and retires the journal itself â€” delete with the applier. Five sibling designs tested + refuted 2026-07-02: eager apply (GET escapes into unbuilt lists), eager release (GET outruns producer), consume-once pending set, zero-all with/without lag-by-one (producer freezes at ~24 entries â€” it re-reads its own entries). |
| `YZ_NO_LAUNCH_UNWIND` | spu_channels.c ~946 | Kill-switch for the SPU task launch-unwind (5882fe4). Keep. |
| `YZ_NO_DMAGUARD` | runtime/spu/spu_dma.h (mfc_submit atomic leg, after the [dma-null] diag) | **Kill-switch for the s31 §13 NULL-PAGE ATOMIC GUARD (fix DEFAULT ON, ledger #74).** The recurring fatal "t1 crash" (s17 family, 5+ sightings, killed s31roll3/cure3) was an SPU host thread's lock-line atomic against guest EA 0 -- the unguarded 128-byte copies AV'd the process; "tid=1" was the threads.cpp s_cur_tid TLS fallback (tramp_idx=0 = the tell) and the [crash-t1] dump was the HEALTHY main thread. The guard completes the op benignly (GETLLAR reads zeros/no reservation, PUTLLC fails -- the faithful no-valid-reservation result, PUTLLUC dropped) with a loud [dma-guard] line; the [dma-null] issuer dump still fires first, so the null-EA producer (the yield-resume register-loss class) keeps its signal. On the console EA 0 is mapped kernel memory -- an SPU atomic there never halts the machine; the fatal AV was our uncommitted-page artifact. `=1` restores the fatal proceed for A/B. Retire with the null-EA producer fix. |
| `YZ_NO_TASKRELOAD` | spu_channels.c (yz_task_segment_guard, called at the natural-launch adoption point) | **Kill-switch for the s31 §12 TASK-IMAGE RO-SEGMENT GUARD (fix DEFAULT ON, ledger #72/#34).** The real SPURS resume re-DMAs the task ELF's read-only segments on EVERY resume (RPCS3 cellSpursSpu.cpp:1815-1830, LoadElf skipWriteableSegments=true) because rotation legitimately deploys other workloads over the task region; our resumes measurably did not (s31roll2.err: gs_task's static vtable at LS 0xBA88 -- TEXT/RODATA -- all zeros at dispatch = the #34 death's second flavor; pre-rotation the same class fired via the image-0 wildcard on SPUs that never held the image, ledger #24). The guard parses the task's guest ELF at taskInfo.elf and memcmp-verifies + redeploys each non-writable PT_LOAD at every branch into the task region (race-free: task not yet executing; idempotent; [task-reload] log, first 16 + every 64th). Writable/BSS state deliberately untouched -- that is the ctxsave/ls_pattern leg; if the roll1 BSS-husk flavor persists with this guard live, the save/restore leg is the named residual. `=1` disables for A/B. Retire when the #34 race is closed end-to-end. |
| `YZ_NO_CTXRESTORE` | spu_channels.c (yz_task_ctx_restore, at the natural-launch adoption after the RO guard) | **Kill-switch for the s32 FINAL RESUME-CONTRACT LEG (fix DEFAULT ON — closes the #34 family end-to-end with the RO guard + module guard).** Boot 9 named the wiper ([ls-wipe]: pool-task img-4 work fetches, 0x1000-byte all-zero GETs into LS 0xB880 from unwritten work regions 0x41Bxxxxx — LEGAL foreign-era writes overlaying the parked consumer's state; the census, scratch/s32_flavor_census.md, proved all death flavors = different unresurrected zeroed spans meeting their first reader). The fix completes Sony's dispatch resume (RPCS3 cellSpursSpu.cpp:1823-1846): restore the 0x380 context header (LS 0x2C80) + the ls_pattern windows (LS 0x40000-blocks*0x800 ..) from the GUEST ctxsave at the adoption boundary. Safe vs the iteration-4 rollback class: source = guest ctxsave (populated by Sony's own saves since the seam fix — always current for a parked task), gated resume-only + foreign-era-or-RO-stale, never from virgin (loud [ctx-restore] WARN instead). Pre-committed failure shape: [jrnl-cur] cursor regression right after a [ctx-restore] = rollback, revisit the gate. `=1` disables for A/B. |
| `YZ_CTXSHADOW` (+`YZ_NO_CTXSHADOW` honored as off) | spu_channels.c (yz_ctx_shadow_save/restore; save at the image-2 exit-unwind + the legacy seam, restore after yz_task_segment_guard) | **s32 TASK WRITABLE-CONTEXT SHADOW — DEMOTED TO OPT-IN (default OFF) the same day it was built.** The contract decode (scratch/s32_pollswitch_contract.md) (decoded from the lifted firmware binaries + the RPCS3 oracle) established every legitimate kernel crossing happens AFTER Sony's own save path ran (three-door rule; TaskPoll switch-free), so the guest machinery is self-sufficient when the seam stops lying; the shadow's forced restores measurably CAUSED a rollback death (s32ctxsh4: snapshot ~79k polls stale restored over live state). Kept as an evidence tool/contingency for the [exit-unsaved] probe's findings. Original rationale below. MEASURED s32ctxw1.err: gs_task's guest ctxsave register block keeps its task-START hash through every healthy resume (Sony's SaveTaskContext leg never runs on our side — the coret seam bypasses the policy's post-poll save), and the one pre-death write delivered byte-SHIFTED content (the death forensics' off-by-N object/vtable bytes). The shadow snapshots ground-truth LS (savedContext block 0x2C80-0x3000 + task region 0x3000+allocLsBlocks*0x800) at the switch-away doors — AND (iter 3, the decisive leg) writes it through to the GUEST ctxsave area in Sony's own layout (regblock at +0, LS blocks position-keyed at +0x400), because the guest RESTORE leg measurably RUNS on our side (s32ctxsh2.err:3625: policy pc=0x25CC GETs from ctxsave, hash = the virgin-zero value → restored zeros over live LS → savedContextLr=0 → the branch-to-LS-0 death) while the guest SAVE leg never does. Host-side restore at re-adoption stays as the second belt (fires when a foreign owner used the region, or RO-staleness proves foreign presence; fresh starts just take ownership — Sony's isWaiting=0 leg loads fresh). Content-equivalent to RPCS3 cellSpursSpu.cpp:1683-1864; guest ctxsave stays untouched (deviation: nothing PPU-side reads it). [ctxsh] SAVE/RESTORE log, first 16 + every 64th. Pre-committed residual: job workloads (img 13-15) DMA into the task region with no adoption event (invisible to owner tracking) — a death following [job-launch] with no foreign ADOPTION between is the next leg. `=1` disables for A/B. Retire when #34 is closed end-to-end (with YZ_NO_TASKRELOAD). |
| `YZ_CORET_LEGACY` | spu_channels.c (spu_indirect_branch: the legacy 0x838/0x231C special-case + the s31 exit-unwind block) | **Kill-switch, two eras. s32 UPDATE (contract correction): LS 0x2308's `bisl 0x838` (link 0x231C) is cellSpursModuleExit's stub, NOT a poll — the real pollStatus (0x2320) never enters the kernel (TaskPoll spec-guaranteed switch-free). The entire 0x231C special-case (the gpr3=0 fake "poll resume" + the wcl==2 real-kernel branch, both of which modeled a coroutine seam that does not exist on silicon) now runs ONLY under this flag =1; default routes every 0x838 entry through the general exit-unwind (total workload death, fresh 0xA00 re-entry — the decoded contract). Also gates (as before) the pre-s31 nested-exit behavior.** Original s31 rationale: **Kill-switch for the REAL module-exit -> kernel transition (s31, ledger #71 -- fix DEFAULT ON).** cellSpursModuleExit's one-way jump to kernel 0x838 used to run the kernel's select/contention/dispatch chain NESTED in the exited workload's dead host frames -- the seam that permanently killed wid2 (the gcm journal consumer's taskset) at its first contended workload switch, at the CRI bring-up (85/85 boots, one `[yz-coret] wcl=2` each, then zero wid2 dispatches ever; scratch/s31_consumer_death.md). The fix unwinds the host stack to the driver (same context-replacement mechanism as the task launch-unwind / acfccf6 class) and re-dispatches the lifted kernel at 0x838 on a fresh top-level stack; the POLL yield (bisl link 0x231C) keeps the clean-coroutine return, which the oracle proves synchronous (RPCS3 cellSpursSpu.cpp:97-119). `=1` restores the pre-s31 behavior for A/B (BOTH legs: the fake synchronous poll AND nested exits). s31 iteration 2 (after scratch/s31cure1.err measured the fake leaving wklCurrentContention[2] stuck at max with SPU4 spinning the policy select-verify loop): the wcl==2 POLL yield now ALSO runs the real lifted kernel 0x838 via the same depth-0 unwind -- the kernel-mode select's claim-subtract/commit/dispatch is the only mechanism that performs the switch the policy's verify loop waits for; the fake is retained only for image 13/coret_gen (measured zero hits) and under this flag. Retirement: after the journal consumer survives the CRI transition across a quiet stretch of validated boots (`[jrnl-cur]` continuing past the CRI window, park-rel applies ~0), fold the unwind in as unconditional and delete the flag. |
| `YZ_JOB_WILDCARD_OK` | spu_channels.c (spu_lookup) | **Kill-switch for the jobchain-family wildcard REFUSAL (2026-07-08, guard DEFAULT ON).** ROOT (MEASURED, scratch/idboot.err + 3 prior boots): the jobchain loads its job binaries into descriptor-assigned LS slots (round-1 loads jobB/0x01275A00 at LS 0x4C00 â€” head bytes 43 49 4E verified resident) but the job lifts were fixed-base, so `spu_lookup`'s image-0 wildcard silently served **gs_task's** `spu_func_00004C00` at the job site â€” the notify job never ran, the IWL event flag never set, zero spup17 (the s19 wall, DONT_RECHASE #23/#24). Fix = each job binary lifted at BOTH slot bases + refuse cross-image wildcard for images 13-15 at job-span addresses (>=0x4880). `=1` restores the old silent substitution for A/B. âš  **A registry-only GENERALIZATION of this guard (refuse any contested address, no game constants) was tried and REFUTED BY MEASUREMENT the same day (scratch/genboot.err): dormant task images (spuimg_06 = img 9) register spans overlapping the RESIDENT kernel's (0x290), so the serviceâ†’kernel yield got falsely refused â€” a context-free registry cannot distinguish "kernel resident, img 9 dormant" from "jobB resident, gs_task dormant". A sound general guard needs RESIDENCY tracking (which binary actually landed where, per context â€” the DMA recorder generalized) or a clean image-0-is-only-the-kernel taxonomy (ours conflates kernel+gs_task under image 0 by design). Do not re-try the registry-only version.** |
| `YZ_KERN_WILDCARD_OK` | spu_channels.c (spu_lookup_apply_job_guard + the foreign-resident adopter) | **Kill-switch for the KERNEL-context wildcard/adoption refusal (s24 4881ef0, extended s25 to the foreign-resident adopter; guard DEFAULT ON).** A context tagged image 16 (SPURS kernel) whose computed branch misses every kernel registration is a wild branch by definition (legitimate kernel exits switch images before entering foreign code); serving it from the image-0 wildcard — or, the s25 close, the foreign-resident adopter — executed gs_task's code in kernel era and mis-attributed the tid-0x2004 death for weeks (ledger #34/#49/#51). `=1` restores the old silent substitution for A/B. |
| `YZ_NO_EV_RETRY` | import_overrides.cpp (yz_rsx_ev_send) | **Kill-switch for the GENERALIZED lossless RSX-event latch (s25, post notification-surface audit — DEFAULT ON).** Any failed sys_event_port_send to the RSX port (user-cmd 0x80, queue event 0x20<<head, future bits) ORs its cause bits into one pending mask retried at the consumer top; covers audit risks #1/#2 with the ride-validated ucmd mechanism. `YZ_NO_UCMD_RETRY` is honored as an alias for off. |
| `YZ_NO_THROW_RETRY` | spu_channels.c (yz_throw_latch_add/yz_throw_retry_flush) + import_overrides.cpp (vblank flush) | **Kill-switch for the SPU throw_event loss latch (s25, audit risk #3 — DEFAULT ON).** WrOutIntrMbox codes 64-127 are fire-and-forget by protocol: a full destination queue loses the event with no guest signal (the CRI doorbell wall's mechanism class). Failed throws latch into a 16-entry ring and redeliver from the vblank tick (~16 ms); loud [throw-lat] log per latch so firings are visible; latch overflow drops (the faithful behavior). `=1` restores the pure drop. |
| `YZ_NO_UCMD_RETRY` | import_overrides.cpp (yz_rsx_method 0xEB00 + yz_ucmd_retry_pending) | **Kill-switch for LOSSLESS user-interrupt delivery (s25, DEFAULT ON — the round-25 stall root, ledger #52).** MEASURED (scratch/s25ride.err): the game coalesces its ucmd cause counter (1..21 then 25, a documented rapid-user-command coalescing behavior) and the single coalesced send hit a momentarily full RSX event queue — sys_event_port_send returned 0x8001000A EBUSY and the fire-and-forget path lost it; the wid4 pool never published rounds 22-25 and the stream parked forever on SEMAPHORE_ACQUIRE want=25. Fix mirrors lv1's single pending-cause register: latch the undelivered cause, retry at the top of the consumer loop until the queue drains (userCmdParam already carries the latest arg). `=1` restores fire-and-forget for A/B. |
| `YZ_NO_IMGSTACK` | spu_channels.c (spu_img_restore + adopt-on-serve in spu_indirect_branch) | **Kill-switch for the s25 adopt-on-serve + restore-on-host-return image model (ledger #51's designed fix; DEFAULT ON).** Lifted brsl/bisl brackets save/restore ctx->image_id across nested host calls (the host C stack of bracket locals = the shadow image stack); the dispatcher adopts the image whose registration serves each lookup; pc==0xA00 re-adopts the DMA-recorded resident module (ctx->module_img_a00, spu_dma.h). `=1` reverts to the old sticky-image behavior AND disables adopt-on-serve with it (only safe together — adoption without brackets is the ledger-#51 mislabel trap). Keep permanently for regression bisection. |
| `YZ_NO_SPUBACKOFF` | spu_dma.h (GETLLAR) | Kill-switch for the **SPU idle-poll host-yield backoff** (2026-07-03): same-line GETLLARs with an unchanged write-generation escalate cpu-pause â†’ scheduler-yield, so 5 spinning SPURS kernels stop saturating the global lock-line lock (measured 5Ã—~97% core + boot-pacing collapse without it). Faithful (polling continues; ladder resets on any observed write). Keep. Note: setting this (disable backoff) ALSO breaks the SPURS dispatch livelock (SPU polls hot, catches the transient workload signal) â€” it was the discovery lever for `YZ_NO_LRWAKE`'s fix. |
| `YZ_NO_LRWAKE` | spu_dma.h (GETLLAR mgmt line) | **Kill-switch for the SPURS dispatch LOST-WAKEUP FIX (2026-07-05, DEFAULT ON).** ROOT (MEASURED): the backed-off SPURS kernel misses the PPU-side wklSignal/readyCount workload-submit (that write doesn't raise the kernel's SPU_EVENT_LR â€” it bypasses the coherence write-generation path), so the transient signal lands in a poll gap and the workload that the main thread's `cellSpursEventFlagWait` on 0x4019C680 bit0 depends on is never dispatched â†’ boot livelocks (the long-standing dispatch gate). The fix delivers the missed HW edge: on a GETLLAR of the mgmt line 0x40197C80 that still carries a pending wklSignal1 (+0x70 != 0), raise LR so the kernel re-enters selection + dispatches. Faithful (re-derives a dropped cross-processor wakeup, not a game-logic force; self-limits once dispatched). `=1` disables for A/B. MEASURED: default boot now progresses into rendering (~520-560 set_render_target, stable, no crash) where before it livelocked. |
| `YZ_RSX_DRAW` | libs/video/rsx_live_draw.c (`rsx_live_draw_enabled`) | **Kill-switch for the Track B live NV4097 draw path (2026-07-03, Track B stage 5 B2).** Default ON; `=0` makes the live-draw engine inert so the runtime keeps its existing (null/clear-color) present. The engine is the validated capture-replay D3D12 pipeline (rsx_dispatch + NV40 VP/FP decompilers + PSO/sampler/mip engine) driven by the live FIFO consumer. **WIRED 2026-07-04** into yakuza/import_overrides.cpp: `rsx_live_draw_method(method,arg)` fed at the top of `yz_rsx_method` (full stream), engine self-presents on the 0xE944 flip; `rsx_live_draw_init` runs on the window thread bound to the null backend's HWND (`rsx_null_backend_get_hwnd`) and suppresses the null GDI present on success (`rsx_null_backend_suppress_present`). Boot-verified: `[rsx] live-draw engine up (D3D12)`, 12 flips presented, zero crashes. |
| `YZ_FP_CTRL_AUTO` | libs/video/rsx_live_draw.c (get_pso) | **Kill-switch for feeding the real SHADER_CONTROL word to the live-path FP decompiler (s25; DEFAULT = real ctrl word).** The s24 replay-harness root cause applied to the live path: NV40 selects the fragment output register via SHADER_CONTROL bit 0x40 (clear ⇒ h0/fp16, set ⇒ r0/fp32); the old AUTO heuristic returned stale fp32 scratch for h0-writing materials (flat/black colors). The live path now passes `rsx_dsp_shader_control()` and folds bit 0x40 into the PSO cache key, mirroring replay_main.c. `=1` restores the AUTO heuristic for A/B. |
| `YZ_RSX_DUMP` | libs/video/rsx_live_draw.c (`rsx_live_draw_present`) | **DIAG (default OFF): dump the live-draw framebuffer.** When set, writes the presented color surface of the first 8 frames to `scratch/ld_frame_NN.ppm` (self-contained D3D12 readback). The per-frame `[live-draw] frame N presented: draws=X clears=Y` stderr line is always on for the first 32 frames (cheap; verifies real geometry vs clear-only). Remove when the render path is stable. |
| `YZ_NORESUME` | spu_channels.c ~1026 | Kill-switch for the SPURS yield/resume context-switch path. Keep. |
| `YZ_STARTTASK_HOOK` | spu_channels.c (spu_task_launch_check + prof path) | **RETIRED-to-opt-in 2026-07-02**: re-enables the legacy "LS 0x1CC0 = StartTask" launch hijack for A/B. LS 0x1CC0 is actually the taskset-SYSCALL switch (`bi $r2`, jump table at 0x1CC4); the hijack turned every WAIT_SIGNAL/YIELD of the matched elfs into a bogus instant relaunch and skipped Sony's context save. Default OFF = Sony's case handlers + dispatch run lifted. Delete after a quiet stretch of boots. |

## Config (memory-map overrides â€” stable)

`YZ_TLS_BASE`, `YZ_HEAP_BASE`, `YZ_HEAP_END`, `YZ_LIBSRE_BASE`, `YZ_LIBGCM_BASE`,
`YZ_IMPORT_OPD_BASE`, `YZ_IMPORT_FAKE_BASE`, `YZ_GCM_CTX_ADDR`, `YZ_GCM_CB_OPD_ADDR`,
`YZ_GCM_CTRL_ADDR`, `YZ_GCM_LABELS_ADDR`, `YZ_GCM_CB_FAKE_KEY`, `YZ_GCM_LOCAL_BASE`,
`YZ_GCM_LOCAL_SIZE` (import_overrides.cpp / yakuza_runner.h).

## Levers (opt-in, default OFF â€” for diagnosis only)

| Flag | Where | Meaning |
|---|---|---|
| `YZ_FORCE_CODEC` | import_overrides.cpp ~1924 | Force the CRI codec task path (blocker-#22 lever). |
| `YZ_RSX_INLINE` | import_overrides.cpp / shims.cpp / sys_timer.c | Run the FIFO consumer inline with the producer (tested: does NOT fix the pacing wall by itself). |
| `YZ_RSX_FENCE_SYNC` | import_overrides.cpp (yz_rsx_method NV406E_SET_REFERENCE) + libs/video/rsx_live_draw.c (`rsx_live_draw_flush`) | **RSX fence-timing fidelity A/B (2026-07-06, opt-in, default OFF).** On NV406E_SET_REFERENCE, flush + wait the D3D12 backend (a real GPU fence, `ld_flush`) BEFORE writing REF, mirroring RPCS3 `nv406e::set_reference`'s `sync()`. Rationale (MEASURED via the PPU trace-diff at func_00EBBFB4, ours vs RPCS3 armed at 0xEB3238): our async consumer writes REF instantly and races ahead of real GPU time, so the game's REF poll (usleep-30 loop) skips the wait RPCS3 performs; this paces the consumer to actual GPU completion. A/B verdict (2026-07-06): INERT on the boot stall (the CRI-phase re-lock persists with it on); kept opt-in as a fidelity lever for later pacing work. |
| `YZ_SURMIXER_SYNC` | libs/audio/cellAudio.c (`cellAudioSetNotifyEventQueue`) | **CRI surmixer notify-queue sync A/B (2026-07-06, opt-in, default OFF).** Mirrors RPCS3 `AudioSetNotifyEventQueue` (cellAudio.cpp:1671-1719 + merged fix #18857): when the surmixer registers its notify queue (key `c_mxr000`=0x8000CAFE02460300 or 0) and a `_cellsurMixerMain` thread exists, BLOCK (poll up to 100x50ms) until that thread has created the `c_mxr000` lv2 event queue, then correct key==0 -> c_mxr000. RPCS3 issue #18852 (cellSurMixer NULL-deref/hang on SEGA AM2/CRI titles) hinges on this. MEASURED CONTEXT: our boot reaches the CRI audio phase at ~200s (mixer tid=10 + cri_dlg/CriSr t9-t25), the c_mxr000 queue IS created and SetNotifyEventQueue IS called (via YZ_AUDIO_FORCE), but ours registers with no wait. A/B verdict (2026-07-06): FIRES (`surmixer-sync: c_mxr000 queue found after wait`) but INERT on the stall â€” the wall is the CRI SPU job chain a layer below; kept opt-in. NOTE requires >=250s boots to reach the CRI phase. |
| `YZ_TIGHT` | import_overrides.cpp ~1032 | Tight-poll consumer (tested-negative as a fix). |
| `YZ_AUDIO_FORCE` | libs/audio/cellAudio.c ~730 | **OBSOLETE s23 (2026-07-09, DONT_RECHASE #36): drop from the baseline.** pt30-era force (port running at open + hardcoded mixer-queue registration); s23boot4 measured the game calling cellAudioPortStart + SetNotifyEventQueue ITSELF, wall unchanged without it. |
| `YZ_SKIP_VOICE` | yakuza/shims.cpp ~218 | Skip the CRI intro-voice path â€” useful as a RECON probe past the movie gate, not a shipping path. |
| `YZ_MOVIE_TEST` | yakuza/import_overrides.cpp (yz_window_thread) | `=<path.sfd>`: standalone proof of the host movie path â€” decode the .sfd with FFmpeg (libs/codec/movie_ffmpeg.c) and present it straight to the D3D12 window via `rsx_live_draw_present_rgba` (movie mode gates the guest's draws off). Verified 2026-07-04: plays hd_sega_logo_us1012.sfd end-to-end, 100 frames, no crash. NOT the game hook â€” just proves decodeâ†’present in-process. Needs YZ_RSX_DRAW on. |
| `YZ_ADX_RELEASE_TEST` | libs/filesystem/cellFs.c (`yz_adx_release_test_tick`) | **2026-07-04, decisive control-flow experiment (default OFF), independent of `YZ_ADX_HLE`.** Fires the SAME two calls a real ADX decode batch would (`yz_adx_hle_advance_adxm` + `yz_adx_hle_release_spurs_waiter`, i.e. advance ADXM `0x01613368+0x294/+298/+29C` to a fabricated monotonic "N blocks decoded" value, then call the guest's real `cellSpursEventFlagSet` on `0x63D61720` with all bits set) UNCONDITIONALLY on every `.cvm`/voice-stream `cellFsRead`, with NO real decode (silent/zero PCM â€” tests the control-flow release only, not audio). Purpose: settle whether the SPURS-release lever is even the right one BEFORE building an AHX/MPEG-Layer-II decoder (YZ_ADX_HLE was measured inert on the real stream â€” it's AHX not ADX). REMOVE once the SPURS-release question is settled either way. |
| `YZ_ADX_HLE` | libs/filesystem/cellFs.c (`yz_adx_hle_on_read` + helpers) | **2026-07-04, opt-in experiment (default OFF). MEASURED INERT on the real stream â€” see below.** Clean-room host ADX decode (libs/codec/adx_decode.{c,h}, written from the public ADX spec only) of the CRI intro-voice stream, intended to bypass the LLE `cri_audio` SPU codec (measured dead: launches, never advances the ADXM progress fields). Hooks `cellFsRead` on `adv_voice_talk.cvm`/`*.cvm` paths: mirrors read bytes into a host shadow buffer by file offset, scans for a valid ADX header, decodes every complete block, advances the ADXM progress object `0x01613368+0x294/+298/+29C`, and calls the guest's real `cellSpursEventFlagSet` (libsre 0x02016010) on the measured SPURS poll object `0x63D61720` with an all-bits-set release (UNPROVEN which bits t1's `cellSpursEventFlagWait` call actually waits on). **BOOT-TESTED 2026-07-04 (YZ_AUDIO_FORCE=1 YZ_ADX_HLE=1, scratch/adx_hle_boot2.{out,err}): zero `[adx-hle]` log lines â€” the decoder never fires.** Root cause (parsed the container's real ISO9660 directory, scratch investigation not yet a committed tool): `adv_voice_talk.cvm` holds `*.AHX` files, not `*.ADX` â€” AHX is a DIFFERENT CRI codec (ADX-shaped header + MPEG-1 Layer II payload, confirmed via the MPEG sync word right after the copyright tag), which `adx_open()` correctly rejects (encoding_type != 2/3). The SPURS-release/ADXM-advance machinery is therefore UNEXERCISED against the live boot. REMOVE or extend to AHX (MPEG Layer II) decode when the CRI-voice frontier is next picked up. |
| `YZ_FIFO_RECOVER_RET` | yakuza/import_overrides.cpp (`yz_rsx_fifo_step`, CALL and RETURN branches, ~line 1577-1638 helpers + the two call sites) | **2026-07-10, opt-in behavior change (default OFF), NOT yet A/B'd against a live boot.** Per scratch/s29_terminal_park_re.md (Q4): RPCS3 (RSXFIFO.cpp) treats a nested CALL (a second CALL before the pending one RETURNs) and a RETURN with no pending CALL as FIFO_ERROR and calls `recover_fifo()` -- checkpoint/retry, escalating to a fatal abort after 20 recoveries inside a 2s window. Our port used to silently clobber the one-level `g_fifo_ret` return slot on a nested CALL, and idle completely silently forever (after one one-ever warning) on a RETURN-without-CALL -- the MEASURED s28m10 terminal park at GET=0x011001EC and the s28m4 park at GET=0x0000098C. Loud detection logging (`[rsx] CALL inside subroutine ...`) is now UNCONDITIONAL (zero behavior change) at both sites. With the flag SET, both states additionally get the recovery analog: no valid rewind target exists in either case (GET never advanced into the bad word), so "restore to checkpoint" is a same-position retry, rate-limited to ~1 attempt/50ms (substituting our poll loop's SwitchToThread cadence for RPCS3's blocking 2ms sleep), logged `[fifo-rec-ret]`, escalating to one latched `[fifo-rec-ret] FATAL` print + a permanent, loud park after 20 strikes in a rolling 2s window (does not tear down the process, unlike RPCS3's hard exception -- the FIFO consumer just parks with a clear diagnostic instead of silently). Armed banner `[fifo-rec-ret] ARMED (YZ_FIFO_RECOVER_RET)` prints once, on first use of either recovery path. Default OFF preserves the exact pre-existing default-boot behavior (now logged instead of silent). Retirement: fold into the default path once a live A/B boot confirms the recovery doesn't regress a healthy boot and the s28-class terminal park is either avoided or at least loudly diagnosed instead of silently pinned. |
| `YZ_BOUNDARY_DRAIN` (+ `YZ_BDRAIN_DWELL_MS`, `YZ_BDRAIN_CAP`) | yakuza/import_overrides.cpp (park-time fire at the FIFO-consumer [stop-jrnl] site) + runtime/spu/spu_channels.c (`yz_bdrain_fire_ea`) | **s42 boundary drain, iteration C — PARK-TIME FIRE (default OFF). Design: this card's park-time redesign, per the iteration-B postmortem scratch/s42_drainB_postmortem.md.** A faithfulness-restoring MITIGATION for the release-engine wall: post-transition our engine stops applying journaled RSX-FIFO stopper releases, so the GPU strands ("parks") on a jump-to-self stopper forever. When the host FIFO consumer has been parked on the SAME stopper EA past `YZ_BDRAIN_DWELL_MS` (default 3000ms), it fires the drain's guarded release write for THAT stopper only: `0x20000000 \| ((io_off+4) & 0x1FFFFFFC)` written big-endian to the guest RSX-FIFO word (mirrors the SPU's own MFC PUT), gated by a read-guard that writes only while the word still holds its JTS self-jump `0x20000000 \| (io_off & 0x1FFFFFFC)` and an RSX-arena window check (`0x40400000..0x40C00000`) — idempotent (a re-fire finds the release word and skips) and content-preserving. Re-fire is allowed per NEW distinct park (the EA changes), capped at `YZ_BDRAIN_CAP` (default 64) writes/boot. **NO host LS writes:** the iteration-B neutralize half and cross-thread LS mutation are RETIRED; the engine's own machinery is left untouched. Witnesses: `[bdrain] ARMED` banner (env config echoed), `[bdrain] FIRE:`/`[bdrain] SUMMARY:` per fire, and `[bdrain] NOTE: past-wall territory` once per fire. Kill = unset `YZ_BOUNDARY_DRAIN` (zero footprint). **HONEST FRAMING:** this is a BRIDGE — the retired park-release lever's fire-point married to the drain's guarded faithful write; WHY our engine stops applying releases post-storm remains OPEN (durable road = block-lift gs_task). Iteration B v2 measured that crossing the wall exposes a downstream guest fault (a `0x23C786AD` read on tid=18/func_022001B4), so expect NEW crash flavors past the wall — that is progress, not a drain failure (unless the faulting access is the drain's own write). **Retirement:** delete when the tag-2 producer is suppressed OR gs_task is block-lifted. **HISTORY (iteration B, superseded):** the original design fired at the gs_task 0x352C consumer-reinit sweep and walked the mgr+0xE0 release table (neutralize + fire-all-pending). Postmortem verdict: the fire mechanism was PROVEN (v2's fired release advanced GET past all four fired stoppers) but the 0x352C SITE is unreachable once the wedge parks the consumer at the 0x4C24 stopper upstream of reinit (only 1 of 5 matrix boots ever fired). Iteration B code (`yz_bdrain_maybe`, the ximg-storm arm, the gs_task.c 0x352C hook) is retained commented/disabled; `yz_bdrain_fire_ea` reuses its guarded write path verbatim. |

## Diagnostics (default OFF; side-effect-free when unset)

**Permanent generic probes (2026-07-02 â€” prefer these over new hardcoded ones):**
`YZ_DUMP_AT=<seconds>` (main.cpp: fire yz_dump_all_threads ONCE at +N s regardless of
watchdog state â€” reads a healthy-but-parked boot at a chosen instant; pair with YZ_L1SNAP
for the invasive sub-dumps; don't use dump-armed runs for pass/fail rates);
`YZ_PEEK=ea1,ea2,...` (main.cpp: change-triggered 4-word dumps of up to 16 hex guest EAs,
VirtualQuery-guarded â€” moves a memory probe with no rebuild); `YZ_HOOK=addr1,...`
(dispatch.cpp: log args+lr on every INDIRECT call to up to 8 guest code/OPD addresses â€”
direct `bl` calls are invisible; libsre names in scratch/libsre_lle_map.txt. **Updated
2026-07-09 s23:** now prints an armed banner listing every hooked address at first dispatch
(probe-liveness rule); note the OPD-vs-code-address trap â€” a guest function has TWO
addresses (its code entry and its OPD/function-descriptor slot) and indirect callers may
dispatch through either, so hook BOTH or a live target reads as dead. s23boot12 measured
`0x00DDDA6C` receiving ZERO indirect calls by either address while the round driver ran,
i.e. it is reached some other way (direct `bl` or a different dispatch table entry), not
proof the function is unused.);
`YZ_TASKARG` (spu_channels.c, 2026-07-03: log every SPURS task launch â€” the pc-0x3050
entry branch â€” with gpr3/gpr4 args, cap 400; the lean replacement for YZ_SPU_PROF when
only launch args are needed â€” PROF's per-branch overhead crawls the whole boot).

`YZ_INTRMBOX_LOG` (spu_channels.c, `SPU_WrOutIntrMbox` handler, 2026-07-04, diag â€” pure
diagnosis for the t1 event-flag wedge): logs EVERY SPU class-2 doorbell write
UNCONDITIONALLY, before any routing decision â€” raw value, decoded code/port(spup), the
`spu_group_spup_queue` resolution (0 = no binding, i.e. would-be-dropped), and which path
the existing routing logic takes (send_event/throw_event routed, dropped, eflag_set_bit,
unrouted/buffered). Cap 200. Answers whether the SPU ever targets port 17 / queue 2 (t1's
SPURS event-flag queue). Side-effect-free when unset. REMOVE once the t1-wedge frontier is
resolved.

`YZ_LV2_LOG` (yakuza/shims.cpp, the LV2 syscall dispatch wrapper ~line 770, SPU-SPEED
workpackage item 2, diag): gates the two always-on `[LV2 ...]` stderr clauses that log
EVERY matching syscall, not just its first occurrence -- `spu_range` (syscall numbers
82..200, the whole SPURS management-syscall family; hot during SPU/SPURS activity) and
`intr` (every syscall from thread t7, the gcm interrupt thread). Default OFF: only the
cheap one-per-syscall-number `first` print fires (unchanged, always on). `=1` restores
the full original always-on behavior for both clauses, byte-for-byte, for A/B or deep
syscall tracing. `yz_wait_enter`/`yz_wait_exit` (the stall-dump bookkeeping) are NOT
gated -- they run unconditionally regardless of this flag, same as before. Permanent
(prefer this over re-adding unconditional syscall prints); keep.

**PPU differential trace group (shims.cpp `ppu_trace_pc`, emitted before EVERY instruction by
the `--trace` lifter build only â€” inert in a normal build):**
`YZ_PPU_TRACE=1` (enable) + `YZ_PPU_TRACE_TID=<n>` (gate to one guest tid, default 1) +
`YZ_PPU_TRACE_ARM=0xADDR` (start logging on the first hit of this PC, default = from start) +
`YZ_PPU_TRACE_N=<count>` (budget, default 3M) â†’ one hex PC/line to scratch/ppu_trace.txt (the
format tools/tracediff.py + the RPCS3 emitter both use; driver
tools/ppu_diverge.ps1).
`YZ_ARM_PC=0xADDR` (shims.cpp `ppu_trace_pc`, added 2026-07-05, diag) â€” on the --trace build
ONLY: when ANY thread executes the target PC, prints `[arm-pc] #N pc=... tid=... r3=... r5=...
pos=... len=... callers:<guest back-chain>` (r3/r5 = the GPRs at that PC; pos/len =
vm_read64(r5+0x10 / +0x18), meaningful only when r5 points at the 00EEFC2C-style request record).
Independent of YZ_PPU_TRACE (runs first). Cap 120 hits. Identified the producer of
cri_dlg's work flag (tid=1); the generic "who runs this PC + with what args" probe. Retire once
the producer stall is rooted.

`YZ_VMGUARD` / `YZ_VMGUARD_SURVIVE` (yakuza/shims.cpp, `yz_vmguard_check`, 2026-07-04):
diagnostic for the intermittent 0xC0000005 on the mixer/CRI thread startup
(~32 rendered frames in): a lifted guest fn `vm_read*`s a wild pointer
(~535 MB, outside every committed guest region) and `ea(addr)` (shims.cpp,
no bounds check) hits uncommitted VM. The crash handler can't name the caller
(trampoline hop: `cia=0 lr=0`). The committed-region set is WIDER than
`vm.h`'s 4 static regions (main mem/stack/SPU-LS/RSX): sys_memory.c
[0x40000000,0x50000000), cellAudio.c [0x50000000,0x50400000), sys_vm.c
[0x60000000,0x70000000) (approximated as the whole reserved window, not just
the live bump pointer), and the GCM local/RSX video memory VirtualAlloc at
[0xC0000000,0xC0000000+0x0F900000) (`YZ_GCM_LOCAL_BASE/SIZE`) -- the first
pass omitted these and false-positived 20/20 boots on two legitimate boot-time
touches (a dlmalloc chunk-walk into a sys_vm_memory_map'd heap, and a GCM-local
zero-init loop) before any frame rendered. `YZ_VMGUARD` alone logs
`[vmguard] READ/WRITE wild addr=0x%08X w=%u tid=%u guest-callers: <addrs>`
(host-stack walk -> `yz_guest_addr_from_host`, the `yz_watch_bd`/`yz_mem_guard`
pattern; volume-capped at 20) without changing behavior -- the natural AV
still fires. `YZ_VMGUARD_SURVIVE` additionally makes the guarded read return 0
/ guarded write a no-op instead of dereferencing, so the boot survives the
transient race and can reach deeper instrumentation (e.g. `YZ_EVFLAG_WATCH`'s
t1 wedge). Diagnostic-only -- NOT a shipping fix; the real fix
is whatever hands the lifted code this address (lift/race bug, caller TBD by
this flag). REMOVE (or promote to a real fix) once the wild-read root is
named and fixed.

`YZ_EVFLAG_WATCH` (yakuza/dispatch.cpp, `ps3_indirect_call`, 2026-07-04): logs
the REAL args t1 passes to `func_02015F74` (cellSpursEventFlagWait: object ea=r3,
mode=r4, mask=r5, plus the object's live 64-bit flag-bits word at ea+0 and its
mode/type byte at ea+0xE, read at entry -- BEFORE the wait) for tid==1 only, and
every `func_02016010` (cellSpursEventFlagSet: object ea=r3, set-bits=r4) call
for ALL tids. Both are reached exclusively via indirect (bctr) dispatch from
game code (verified: zero direct-`bl` references in recomp/*.c), so hooking
`ps3_indirect_call`'s `target` match is sufficient -- no tramp_guard hop needed
(unlike the mwPly probe). Volume-capped (first 10 + 1-in-97). Settles whether
the previously-poked object 0x63D61720 (libs/filesystem/cellFs.c's
`ADX_SPURS_EVENTFLAG_EA`) is even the one t1 waits on, and whether any producer
ever calls Set on it. REMOVE with the t1-wedge frontier.

`YZ_EVFLAG_LIFECYCLE` (yakuza/dispatch.cpp, `ps3_indirect_call`, 2026-07-04, diagnosis
task): logs the CREATE/ATTACH/DETACH side of the SPURS event-flag lifecycle --
`func_02015758` (`_cellSpursEventFlagInitialize`: object ea=r3, taskset/spurs ptr=r4,
direction=r5; also reads `*(taskset+0x74)` = the owning wid (the CellSpursTaskset
wid field), or reports `taskset==NULL` = the IWL case),
`func_020158C4` (`cellSpursEventFlagAttachLv2EventQueue`: object ea=r3, its type byte
at ea+0xE, and the raw word at ea+0x74), `func_02015AA4`
(`cellSpursEventFlagDetachLv2EventQueue`: object ea=r3). Companion to
`YZ_EVFLAG_WATCH` (Wait/Set only) -- names WHO created/owns a given event-flag object.
REMOVE with the t1-wedge frontier.

`YZ_FORCE_WID0` / `YZ_FORCE_WID1` (runtime/spu/spu_dma.h, `MFC_GETLLAR_CMD` handler on
the SPURS mgmt line 0x40197C80, 2026-07-04 forcing experiment --
DIAGNOSTIC ONLY, default OFF, never the shipping fix): mirrors `YZ_FRC`'s
mechanism (direct mutation of the GETLLAR'd 128-byte mgmt line so the forced value
survives the kernel's own PUTLLC CAS) but continuously (every GETLLAR, not one-shot)
forces wid0's (resp. wid1's) `wklReadyCount1` byte to 1 and sets its `wklSignal1` bit
(bit0/0x8000 for wid0, bit1/0x4000 for wid1) so the SELECT gate
(`run && prio>0 && maxContention>realContention && (signal||readyCount>realContention)`,
spu_dma.h ~1088/1121) is forced true for the whole boot. Purpose: confirm which of
wid0/wid1 is the port-17 heartbeat-doorbell producer that t1's `cellSpursEventFlagWait`
on 0x4019C680 bit 0x1 depends on. Logs `[force-wid0]`/
`[force-wid1]` (first 8) under `YZ_WID01`/`YZ_SPU_PROF`. REMOVE once the producer is
confirmed and the faithful fix (the real readyCount/signal producer) lands.

`YZ_T1SPIN` (yakuza/shims.cpp): logs t1's caller + working regs when it signals
cond-4 at the CRI movie gate (the clean-binary spin loop) â€” used 2026-07-04 to show t1 is
deep in the CRI player's server loop with an empty work queue (lr=0, trampoline-dispatched;
so no single poke â€” the movie needs real decode output to advance). REMOVE with the CRI frontier.

`YZ_MWPLY_PROBE` (yakuza/dispatch.cpp, `ps3_indirect_call` + `yz_tramp_guard`, 2026-07-04):
dynamic-ABI probe for the mwPly (CRI Sofdec) player leaf calls
(func_00F4D0A8=mwPlyIsNextFrmReady, func_00F4DA90=mwPlyGetFrm,
func_00F48E48=mwPlyGetAudioPcmData_PS3 â€” see scratch/MWPLY_RESOLVE.md). Two hook sites are
needed for full coverage: `ps3_indirect_call` gets a full entry+exit+out-param-dump
pass-through wrap for the genuinely-indirect (bctr) path; `yz_tramp_guard` gets an
entry-args-only log for the same-chunk direct-tail-branch path (func_00F48E48 is reached
that way too â€” `g_trampoline_fn = func_00F48E48; return;` inside another lifted function
â€” which never reaches ps3_indirect_call; DRAIN_TRAMPOLINE captures the callee before any
hook runs and calls it unconditionally, so a post-call wrapper can't be spliced in there
without editing generated code). Volume-bounded (first 40 calls of each
target, then 1-in-500). MEASURED 2026-07-04 (2 boots, ~100s each, reaching the healthy
32-frame live-draw render state): ZERO hits on either hook site â€” the mwPly cluster is not
dispatched at all yet at this point in boot. REMOVE with the CRI/mwPly frontier.

`YZ_MWPLY_FORCEREADY` (yakuza/dispatch.cpp, the `ps3_indirect_call` mwPly wrap): diagnostic
force, default OFF â€” when the poll gate func_00F4D0A8 IS dispatched, skip the real call and
return 1 ("ready") to see whether the player advances into calling the getters. Only fires
inside the mwPly wrap, so it's a no-op until that wrap is ever reached (measured 2026-07-04:
never reached in a 100s boot â€” see YZ_MWPLY_PROBE). REMOVE with the CRI/mwPly frontier.

`YZ_T1_UNBLOCK` (runtime/syscalls/sys_semaphore.c `sys_semaphore_wait`, runtime/syscalls/sys_event.c
`sys_event_queue_receive`, 2026-07-04, DIAGNOSTIC ONLY): companion to `YZ_EVFLAG_FORCE` â€” scopes
how deep t1's CHAIN of SPURS waits goes by making t1's (tid==1 only) blocking waits in this stuck
phase return CELL_OK immediately instead of parking: `sys_semaphore_wait` (syscall 92) returns
CELL_OK without decrementing/consuming when the semaphore value is <=0 (logs `[t1-unblock] sem_wait
id=%u forced`); `sys_event_queue_receive` (syscall 130) returns CELL_OK with a zeroed event
(gpr4-7 and the out-buffer all 0) when the queue is empty instead of blocking (logs `[t1-unblock]
eq_recv q=%u forced`). Logging capped at the first 100 hits per syscall; the forcing itself is
unbounded. Heavy hammer â€” deliberately breaks the real wait semantics (a real producer's payload
is never delivered) to let t1 barrel through every wait in the chain and reveal whether the boot
wall is finite (t1 reaches new state) or bottomless (endless waits, no progress). Diagnostic-only
Diagnostic-only, NOT a shipping fix. REMOVE with the t1-wedge frontier.

`YZ_WATCH_WR=hexEA[,hexEA...]` (yakuza/main.cpp, `yz_watch_wr_init`/`yz_watch_wr_veh`, 2026-07-05):
env-driven MULTI-address write-watch (up to 16 EAs, `0x` optional), built so "who
writes this EA" no longer costs a code edit + rebuild (the compile-time `yz_watch_bd tg[]`
array in shims.cpp does; that mechanism is left as-is, this is a parallel array-based
extension of the SAME page-guard/VEH protocol `yz_watch_arm`/`yz_watch_veh` already use, kept
separate because `yz_watch_arm`'s `g_watch_guest` is a single-slot design already claimed by
`YZ_WATCH_EA`/`YZ_WATCH_FLAG`). Zero code runs when unset (single `getenv` + return at the top
of `yz_watch_wr_init`, called unconditionally right after `vm_init()` in `main()`).
Arm-time liveness banner per EA: `[watch-wr] armed ea=0x... page=0x...`. If
a watched page falls in the known-hot SPURS-mgmt class (`0x40197xxx`), prints a loud
`WARNING` (a page-guard traps the WHOLE 4 KB page and single-stepping every
access on a hot lock line can badly slow or wedge the run) but still arms it. On a write into
any watched `[ea,ea+4)`: `[watch-wr] tid=N ea=... old=... new=... bt: 0xPC1<-0xPC2<-...` (host
backtrace resolved to guest addresses via the existing `yz_guest_addr_from_host` walker, same
as `yz_watch_bd`) plus a trampoline-ring line (reliable across memcpy/data-lr hops the raw
host back-chain mis-symbolizes). Writes that land on a watched PAGE but not a watched DWORD
are counted silently and reported once per 4096 (`[watch-wr] N same-page-other-dword writes
so far`). Diagnostic-only, permanent kit â€” retirement: superseded-by-nothing.
Built to answer who arms cri_dlg's work flag `0x01661474`.

Tracing/watches: `YZ_SPU_PROF`, `YZ_SPU_TRACE`, `YZ_SPU_TRACE_IMG`, `YZ_SPU_TRACE_N`
(instruction budget for YZ_SPU_TRACE, default 600000; output is unbuffered so a crashing SPU
keeps its trace tail â€” added 2026-07-01), `YZ_SPU_TRACE_SPU` (lock the tracer to a specific
SPU id instead of first-seen; `any` disables the SPU filter for PC and rt lines â€” added
2026-07-02), `YZ_SPU_TRACE_EVARM` (hold trace arming until an event site fires â€” the 0xA70
taskset-syscall probe or the CRI request-queue GETLLAR in spu_dma.h; added 2026-07-02),
`YZ_QLINE` (spu_context.h: log lifted image-3 stores to the GETLLAR line copy at LS 0x80 â€”
2026-07-02, REMOVE with the frontier),
`YZ_CTXSAVE_WATCH` (DMA + syscall-entry watch on the task context-save protocol: logs
transfers touching LS [0x2C80,0x3000) and the three save-bail checks at the 0xA70 syscall â€”
added 2026-07-02, REMOVE when the codec frontier closes), `YZ_CODEC_PUT` (PUT-class DMAs +
line atomics with pc from task images 3/4; dumps the LS line for atomics on the CRI queues;
its request-line GETLLAR releases YZ_SPU_TRACE_EVARM â€” 2026-07-02, REMOVE with the
frontier), `YZ_OVL`
(spu_dma.h: the entry-7 gate probe â€” [ovl] logs code-sized GETs into LS â‰¥0x10000 per image
(the image-5 runtime overlay load's source EA + size; image-5 sources also dumped to
scratch\ovl_&lt;ea&gt;_&lt;lsa&gt;.bin, first 16) and [job-rd] logs GET/GETLLAR reads of the published
shader-stream job block [0x40197100,0x40197400) to name the consumer, and [job-bin] logs
image-13 (job module) code-sized GETs past its own end = runtime-loaded JOB BINARIES
(source EA + LS base, the next lift target) â€” added 2026-07-03,
REMOVE when the jobchain frontier closes. Later additions under the same flag:
[job-io] = every DMA issued by jobchain images 13-15 (pc discriminates module vs job code),
[job-cmd] = command-stream/descriptor fetches with the fetched u64 (change-triggered per
ea, incl. GETLLAR â€” shows every DISTINCT command the module decodes), [job-cas] = jobchain
header PUTLLC commits with pc + the +0x20..0x2F mask bytes (change-triggered, grab latch
+0x29 masked out); the always-on [dma-null] EA-0 atomic diag now also dumps gpr2-5,
gpr80-82/126/127, the r3 object quads and the taskInfo quads â€” all REMOVE with the
jobchain/pxd-dispatch frontier), `YZ_TS_PEEK`
(spu_dma.h GETLLAR: change-triggered snapshot of the pxd taskset bitset line 0x40199D00 â€”
[ts-peek] prints word0 of running/ready/pending_ready/enabled/signalled/waiting with the
reader's img+pc. The wid-0 policy fork discriminator: img 2 sees pend=0x80000000 yet never
launches â‡’ the policy's SELECT_TASK lift; bitsets all-zero while the PPU create-CAS commits
â‡’ lost write/visibility â€” added 2026-07-03, REMOVE with the pxd-dispatch frontier),
`YZ_W2LIFE`
(spu_channels.c yz_w2life_dump + per-SPU hop census; callers import_overrides.cpp park-rel apply +
main.cpp stall watchdog + the coret/exit-unwind sites, s31 ledger #71: SPURS wid accounting
(readyCount1/current/pending/max contention, wklState1, wklSignal1, sysSrvMsgUpdateWorkload,
wklInfo1[2] per-SPU priorities) + the gs_task taskset bitsets + per-SPU host-liveness
(hops/lastpc/lastimg). Armed banner, hard 64-dump cap. Discriminates the H-wedge vs H-accounting
fork of scratch/s31_consumer_death.md §6 -- retire when the wid2 consumer survives the CRI
transition in validated boots), `YZ_W2CTOR`
(recomp_prx/gs_task.c via scratch/patch_w2ctor_probe.py, s31 iter-3 ledger #72: gs_task
work-item CONSTRUCTION probe -- [w2ctor] at ctor 0x76E0 entry prints item/payload ptr + 16
payload bytes (the type word), and the dispatcher 0x3C78 prints a loud HUSK DISPATCH line
when an object's vtable word is zero (the #34 death state, caught pre-crash with the payload
in hand). Armed banner; re-apply the patch after any gs_task relift; retire with the #34
race), `YZ_ARMGATE`
(spu_context.h spu_ls_read128/write128 + spu_dma.h GET path, s38 scratch/s38_findings.md:
the gs_task RELEASE state machine apply_entry (LS 0xB088) emits a stopper release
(0xB170->0x5EB8->0x5F00) for a CODE==0 descriptor ONLY if the arm-gate witness
LS[0xBD70]==key(r11) at gate 0xB0C4/0xB0CC (recomp_prx/gs_task.c:41037-41062); LS[0xBD70]
is READ at the gate but has NO literal-address writer in the whole image. Logs [armgate]
on every gate read (CODE=r3, key=r11, witness value, descriptor quad, MATCH/nomatch),
[armgate-wr] on any lifted store to 0xBD70, [armgate-dma] on any GET landing on it. Armed
banner, capped. One boot names whether the witness is stuck/never-written/written-but-stale
and whether it ever equals the stuck stopper's key. Retire when the consumer releases the
stuck stopper natively), `YZ_BD70W`
(spu_context.h spu_ls_write128, s40: UNCAPPED change-only watch on the gs_task singleton-slot
cache LS 0xBD70 (image 0) — the s39 stg2 probe measured stage-2 (0xB088) firing a release
ONLY when LS[0xBD70] equals the item's own slot address, items retrying as no-ops until the
cache rotates to them. Logs [bd70-wr] pc+old→new on every real change (1/16 decimation past
20000) + [bd70-hb] current value + write count every 4096 writes, so a frozen/skipping
rotation AT THE WEDGE is measurable (the YZ_ARMGATE watch caps at 400 = blind by then,
LESSONS #6d). Pair with YZ_STG2 (the re-added gs_task.c 0xB088 entry probe, change-only +
hb, lost on relift). Retire with the journal-consumer wedge), `YZ_ZSLOT`
(s40 foreign-wiper kit, one flag arms three watches: [z-slot st] spu_context.h
spu_ls_write128 = any LIFTED all-zero-quad store into the gs_task item-slot region LS
[0xBF00,0xC400) img0 with writer pc; [z-slot dma] spu_dma.h = ANY GET-class landing
overlapping that region, any size, with zero-run report (the always-on [ls-wipe] needs
>=64B and had a 0xBE00 window blind spot exactly there — s40 widened it to 0xC400);
[z-slot restore-REWIND] spu_channels.c yz_task_ctx_restore = logs when a ctx-restore's
incoming snapshot DIFFERS from live LS at bd70 (0xBD70) or the slot span — the mode-B
rewind discriminator (verifyA). Retire with the journal-consumer wedge), `YZ_HUSKLOG`
(recomp_prx/gs_task.c dispatch 0x3CA4, s40, LOST ON RELIFT: logs [husk] DISPATCH when the
work-item dispatcher is about to branch through a zeroed sub-object (vtbl or method = 0)
— the mode-A death signature. + `YZ_HUSK_SKIP` (default OFF, interventional only): return
via the dispatcher's own null-leg idiom instead of dying, to A/B whether the boot crosses
the wedge when the applier survives. Retire with the journal-consumer wedge), `YZ_NO_CTXSAVE`
(spu_channels.c, s40 — KILL SWITCH for the resident-record task-context SAVE, the fix for the
journal-consumer wedge root: the guest ctxsave was restored-from but never saved-to (frozen
era → mode-B rotation rewind; virgin/foreign content → mode-A husk death). Default = save ON
([ctxsv] ARMED banner + SAVE lines): the region of the previous resident task is written back
to its guest ctxsave at the next task-launch or workload-exit seam (content provably intact
until then), one save per residency era; the restore's same-era skip now keys on the per-SPU
RESIDENT record instead of the broken owner slot. Set YZ_NO_CTXSAVE=1 to get the old
frozen-ctxsave behavior for A/B. Design scratch/s40_ctxsave_fix_design.md + the adversarial
review s40_ctxsave_design_review.md), `YZ_JRNL_WATCH`
(spu_dma.h: the LAYER-1 consumer discriminator â€” logs every DMA/atomic touching the gcm
journal HEAD lines 0x41F00080/0x42100080 (with a 32-byte line dump = entry-0 tag+ea) and
every PUT-class into the journal arena [0x41F00000,0x42110000); first 80 hits full, then
every 4096th; also [jrnl-cur] = the consumer's walking-cursor GETLLARs caught by LSA
0x37780 at any EA, and an event-arm release for YZ_SPU_TRACE_EVARM at the journal-head
GETLLAR â€” 2026-07-02, REMOVE when the producer-side journal frontier closes), `YZ_COND_TRACE`
(sys_cond.c + sys_mutex.c: logs WAIT-enter/exit pairs and any SIGNAL that blocks acquiring
the mutex CS on low-id conds, plus recursive-trylock re-entries â€” the boot-stall hunt's
sync-layer x-ray; 2026-07-02, retire with the stall frontier), `YZ_GSPUT`
(spu_dma.h: logs every put-class DMA issued under SPU image 0 with pc+ea+size â€” the probe
that proved gs_task's back half applies journal patches (plain PUTs, pc 0xB60C) and issues
FENCED stopper-release PUTs (pc 0x5F00); 2026-07-02, retire with `YZ_JRNL_WATCH`), `YZ_SIGCALL`
(dispatch.cpp: log indirect calls into the libsre LLE signal/queue family, addresses in
scratch/libsre_lle_map.txt â€” 2026-07-02, REMOVE with the frontier), `YZ_IMGLOG`, `YZ_SIGW`,
`YZ_SIGCNT`, `YZ_LRWAKE`, `YZ_LS_DUMP`, `YZ_WID01`
(spu_dma.h ~1071-1112, 2026-07-04: extends the existing wid2/wid3 SELECT-gate probe
under `[spu-ls]` to wid0/wid1 -- new `[spu-ls01]` line, same formula (run/prio/maxContention/
realContention/signal/readyCount/SELECT) at offsets 0/1 instead of 2/3. Auto-on under
`YZ_SPU_PROF` too. Added to find which of wid0/wid1 never satisfies SELECT for the port-17
heartbeat doorbell t1 waits on. Retire once the heartbeat-wid root closes.),
`YZ_WID0_REQ`
(spu_channels.c spu_task_launch_check, 2026-07-04: on every policy [image 2] re-entry
to LS 0xA70 (the taskset-syscall handler entry), if the resident taskset pointer (LS 0x27BC)
is wid0/pxd's 0x40199D00 or the captured wid2/gs_task taskset EA, logs `[wid0-req]`: the
request code (gpr3, signed+hex), and the 6 tempAreaTaskset bitset words (running/ready/
pending_ready/enabled/signalled/waiting @ LS 0x2700/10/20/30/40/50) the policy just loaded.
Side-by-side wid0-vs-wid2 request-path compare â€” does wid0 ever issue SELECT_TASK(5)? Retire
with the wid0-dispatch frontier.),
`YZ_HALT_LOG`, `YZ_POLTRACE`, `YZ_POLHOP`,
`YZ_DISP_TRACE`, `YZ_TRACE_CODEC`, `YZ_CODEC_WATCH`, `YZ_ELF_WATCH`, `YZ_PUT_WATCH`,
`YZ_TS_WATCH`, `YZ_TASK_TRACE`, `YZ_TASK_RET`, `YZ_CB_TRACE`, `YZ_DRAIN_TRACE`, `YZ_RECPROBE`,
`YZ_PHASE`, `YZ_FIFO_TRACE`, `YZ_TRACE_RSX`, `YZ_TRACE_DEFER`, `YZ_LOG_FIFOSET`,
`YZ_DUMP_BUFDESC`, `YZ_DUMP_SEG`, `YZ_WATCH_LIST`, `YZ_WATCH_DLEA`, `YZ_WATCH_OPLIST`,
`YZ_WATCH_300`, `YZ_WATCH_FENCE`, `YZ_WATCH_EA`, `YZ_WATCH_READ`, `YZ_WATCH_DLIST`,
`YZ_WATCH_FLAG`, `YZ_WATCH_BD`, `YZ_L1SNAP` (**gates ALL invasive watchdog dumps** â€” the
all-threads snapshot's serial thread-suspend + stack walks take 60+ s and froze the guest
+30sâ†’+90s when made always-on 2026-07-02, invalidating four validation loops; even the
t1-only host-stack/spin dumps suspend t1 in the PortStart window and flipped a ~3/4 baseline
to 0/3 â€” a diagnostic must never perturb the system it measures), `YZ_GCMCTX_BISECT`,
`YZ_GUARD`, `YZ_GUARD_FULL`,
`YZ_GUEST_ADDR`, `YZ_ARG`, `YZ_VSYNC_PRECISE`, `YZ_THROTTLE_DIAG` (sys_timer.c â€” REVERT
before commit per STATUS). (Removed 2026-07-02 with the geometry root fixed: `YZ_GEO_PROBE`,
`YZ_GEO_SKIPNULL`, `YZ_LSWATCH`, `YZ_LSWRITE` â€” stripped from the code; re-derive fresh
probes if a new LS-provenance question comes up rather than resurrecting these ranges.)

## RETIRED / refuted (do not re-enable; delete when convenient)

| Flag | Verdict |
|---|---|
| `YZ_FLIPADV` | Band-aid retired by the faithful flip lifecycle (6fbc3c6). |
| `YZ_FORCE_START` | Retired bc8efa6 (superseded by the 5882fe4 image-match fix). |
| `YZ_FRC`, `YZ_FRC3` | readyCount force â€” superseded by the real poll-status path. |
| `YZ_CLEARRUN`, `YZ_CLEARRUN3`, `YZ_FIXRUN`, `YZ_FIXEXIT`, `YZ_FORCE_TASK`, `YZ_POLLFORCE`, `YZ_NOLAUNCH`, `YZ_NO_MGMT`, `YZ_NOHELPRET`, `YZ_CORET_GEN` | SPURS-dispatch-era forces; the dispatch now works via the real path. |
| `YZ_IMM_REL`, `YZ_NO_DEFER`, `YZ_ONESEG`, `YZ_SEGBIG`, `YZ_BIG_SEG` | FIFO/stopper experiment forces; refuted or superseded by f8d0386 (see archive tested-negative ledgers). |
| `YZ_WKLSIG` | Signal-era probe/force; machinery proven healthy. |

## Diagnostics backfill (2026-07-05)

`YZ_B1_BLEND` / `YZ_B1_CULL` / `YZ_B1_DEPTH` / `YZ_B1_SAMP` (libs/video/tests/replay_main.c
`b1_read_env`): the Track B replay tool's B1 state-group kill switches â€” set any one to `"0"`
to disable that decoded-state group (blend, cull, depth, sampler) in the PSO build, all
default ON. Used to bisect Track B render regressions by state group. `YZ_B1_TEXLOG`
(same file, the texture-decode path): when set (any value), prints one `[texlog]` line per
decoded texture (offset/format/dimensions/mips/filter min-mag/fallback-or-ok) â€” a diagnostic
dump, not a kill switch.

`YZ_CRIDLG` (yakuza/shims.cpp, the `num == 107` cond_wait hook, 2026-07-05): logs cri_dlg's
(tid 11/12, the CRI asset-load dispatcher) control-block state at each `cellCond`-style wait â€”
ctrl address, the work-flag at `+4`, the callback/arg at `+0x40/+0x44`, run state at `+0x50`,
cond byte at `+0x14` (first 12 hits, then every 250th). Forks whether the stall is a producer
bug (work never queued) or a callback bug (work queued but never dispatched); see the comment
citing RPCS3's scenario.bin â†’ player_pos.bin â†’ all_csb.par â†’ movie load sequence.

`YZ_EVFLAG_BT` (yakuza/dispatch.cpp, inside the `YZ_EVFLAG_WATCH` wait-log block, 2026-07-04):
companion to `YZ_EVFLAG_WATCH` â€” walks the guest PPC64 back-chain (r1 â†’ saved caller sp/LR)
to name the real game function(s) that called into `cellSpursEventFlagWait`, since the
immediate `ctx->lr` at the hook site is 0 (fired inside the trampoline hop, not the real
caller). Fires only for `ea==0x4019C680` (the measured t1-wedge object), capped at the first
5 hits.

`YZ_FORCE_RC` (yakuza/import_overrides.cpp, the vblank coherence-test block, ~line 2068): every
vblank, bumps the SPURS taskset's `wklReadyCount1[wid]` to 1 directly inside the SPU lock-line
(so it survives the kernel's own PUTLLC), when a taskset pointer is known. Tests whether the
missing bootstrap step is the coherent readyCount bump that `CreateTask2` should have done â€”
confirms/refutes by whether the kernel then schedules the wid and runs gs_task.

`YZ_FS_TRACE` (libs/filesystem/cellFs.c, the read/Lseek/Fstat path, 2026-07-03): logs EVERY
`cellFsRead`/`Lseek`/`Fstat` call (path, offset, resulting position/size), not just the
stream-container heuristic (`.cvm`/`.sfd`/`/stream/`/`/movie/` paths) the code already applies
by default â€” the pxd spurious-completion probe, added because some boundary streams read an
archive fd whose path misses the built-in stream-path filters.

`YZ_MGMT_CAS` (yakuza/shims.cpp, the SPU-mgmt-line `stwcx` path, 2026-07-03): logs every PPU
`stwcx` (CAS) onto the SPURS mgmt line `0x40197C80` with old/new/ok â€” the wid-0 fork
discriminator: if readyCount commits land here but wid 0 never dispatches, the bug is in the
kernel select's wid-0 path; if no commit ever targets readyCount[0], the signal path bails
before its CAS. The wid-0 wklSignal bit at `+0x70` (`0x40197CF0`) is logged uncapped (rare);
everything else is per-word capped. Extended 2026-07-06: the 8-byte `stdcx` path counts mgmt-line
CAS too (`[mgmt-cas8]`, first 40 + every 997th) â€” the 4-byte-only probe undercounted any mixer
heartbeat committed via 8-byte CAS.

`YZ_WIDSIG_ALL` (yakuza/shims.cpp, the SPU-mgmt-line `stwcx` path, 2026-07-06): UNCAPPED census of
every PPU commit that RAISES a wid signal bit on the wklSignal1 word (`0x40197CF0`), per-wid running
counts (`[widsig]` lines) â€” built to measure the CRI-phase re-arm frequency of wid1 past
`YZ_MGMT_CAS`'s per-word cap. This measured the current frontier fact: the guest producer raises
wid1's bit exactly ONCE per boot (vs continuous re-kicks in RPCS3). Purely observational.

`YZ_JGUARD` (yakuza/shims.cpp `ppu_res_stwcx`/`ppu_res_stdcx` + runtime/spu/spu_dma.h PUTLLC,
2026-07-08, uncapped, armed banners): census of every CAS commit to the CRI jobchain's
CellSpursJobGuard line 0x4019C700 (ncount0 at +0, ncount1 at +4), all three commit paths (PPU 4B,
PPU 8B, SPU PUTLLC). Counts JobGuardNotify at the store itself, so call path does not matter.
Verdict obtained 2026-07-08: the PPU producer notifies ONCE (threshold ncount1=1), the SPU chain
auto-resets and waits correctly; the wall = the producer never notifying again. Retire with the
producer frontier.

`YZ_JOBTRACE` (runtime/spu/spu_channels.c `spu_indirect_branch`, 2026-07-08, capped 400, armed
banner): computed-branch trail of the jobchain JOB binaries (images 14/15) â€” pc, link, r3 per
dispatch. Verdict obtained same day: round-1 jobB enters 0x4C00 with the correct job ABI
(r3=0x4940), returns to the module, module polls kernel (0x290) and exits (0x838). Retire with
the jobchain frontier.

`YZ_FLAGCAS` (runtime/spu/spu_dma.h PUTLLC path, 2026-07-08, uncapped, armed banner): every SPU
PUTLLC ATTEMPT on the IWL event-flag line 0x4019C680 (the flag t1 waits on, bit 0x1), logged
BEFORE the success test with a COMMIT/FAIL verdict + oldâ†’new bits. Verdict obtained same day:
ZERO attempts the whole boot â€” round-1 jobB never reaches its flag-set stretch (RPCS3's
jobB@slot0 PUTLLCs it at LS pc 0x520C) â‡’ early-exit on staged input data. Retire with the
jobchain frontier.

`YZ_JOBDESC` (runtime/spu/spu_dma.h GET path, 2026-07-08, capped 96, armed banner): payload hex
dump (first 64 B) of every SPU GET from the CRI jobchain object area [0x40190000,0x401A0000),
size â‰¤ 0x400 â€” the header/command/JOB-DESCRIPTOR bytes the module stages before calling a job.
Twin probe added to rpcs3clone (same format) for a byte diff; guest EAs are identical. Retire
with the jobchain frontier.

`YZ_WIDSIG_BT` (yakuza/shims.cpp, inside the `YZ_WIDSIG_ALL` block, 2026-07-08): on a wid1 (bit
0x4000) raise, walk the guest PPC64 back-chain via the crash handler's `yz_dump_guest_state`
(first 4 raises only) to NAME the producer call chain that kicks the CRI jobchain. Built because
the raw lr captured near the raise turned out to be a DATA pointer (the 0x01622200 device object),
not the kicker. Requires `YZ_WIDSIG_ALL=1`. Retire with the producer frontier.

`YZ_NO_UCMD` (yakuza/import_overrides.cpp `yz_rsx_method` case 0xEB00/0xEB04, 2026-07-08 s22,
kill-switch, default OFF = mechanism ON): disables the RSX USER-INTERRUPT dispatch — the s22
root fix for the phase-2 bootstrap deadlock. The consumer, on GCM_SET_USER_COMMAND (method
0xEB00/0xEB04), writes driverInfo.userCmdParam (+0x12CC) and posts SYS_RSX_EVENT_USER_CMD
(0x80) to the gcm intr thread, which calls the game's registered user handler func_00E7DB10 =
the EBOOT's only _cellSpursSendSignal path (the wid4 pool pump). Prints `[ucmd]` first 40 +
every 256th. Faithful mechanism (RPCS3 rsx_methods.cpp:68, sys_rsx.cpp:931), not a force —
the switch exists only for A/B regression isolation; retire it after a quiet stretch.

`YZ_NO_FLIPHEAD` (yakuza/import_overrides.cpp `yz_rsx_method` 0xE920/0xE924, 2026-07-08 s23,
kill-switch, default OFF = mechanism ON, armed banner `[fliphead] armed` on the consumer's first
method): disables the GCM_FLIP_HEAD IMMEDIATE-flip dispatch. The consumer routes method
0xE920+head*4 to our existing `yz_sys_rsx_context_attribute` pkg-0x102 display-flip case (the
cellGcmSetFlipImmediate path RPCS3 binds via rsx_methods.cpp:1729 `gcm::driver_flip` →
sys_rsx.cpp:574-627: arg bit31 = grab-queued-buffer, else display-buffer offset). Closes the
top-ranked gap of the s22 RSX method-coverage audit (scratch/rsx_method_coverage_audit.md, same
silently-dropped-method class as 0xEB00, blocker #20 3rd instance). Prints `[fliphead]` first 16
+ every 1024th — the banner + zero hits is the MEASURED answer to the audit's open "is 0xE920
live during boot?" question. Faithful mechanism, not a force; retire after a quiet stretch.
**s23 A/B verdict (DONT_RECHASE #35): the method is EARLY-PHASE ONLY (16..1023 hits/boot,
head=1 arg=0x8000010F); the audio wall is identical with it on/off. The fix stays as driver
contract, not as a boot unblocker.**

`YZ_NO_SEMARM` (yakuza/import_overrides.cpp SEMAPHORE_RELEASE handler, 2026-07-09 s23,
kill-switch, default OFF = heuristic stays ON, armed banner `[semarm] armed` on first flip-label
release): disables the sema-release flip-ARM COMPENSATING HEURISTIC (a nonzero release to the
flip label label+0x10 arms g_rsx_flip_pending). RPCS3's nv406e::semaphore_release does NOT arm
flips — the real trigger is the driver methods; while the heuristic stays on, any non-flip label
write manufactures a phantom flip (uncommanded present + 0x40C00000 throttle bump + FLIP event).
Retirement path: implement the FAITHFUL arm in the QUEUE handler (0xE940/pkg-0x103, mirroring
RPCS3's on_frame_end) first, A/B this switch, then flip the default. NOT yet A/B'd.

`YZ_WID4` (runtime/spu/spu_dma.h GETLLAR path, 2026-07-08 s22, armed banner `[spu-ls4] armed`):
uncapped low-rate (every 500k mgmt GETLLARs) steady-state sampler of wid4's SELECT gate —
run/prio/maxc/cont/rc/sig/SELECT, offsets mirroring `[spu-ls01-slow]` at index 4. wid4 = the pxd
GPU-frame pool (image 4, elf 0x01284200) = the 0xFE0 decode-sync publisher (DONT_RECHASE #29);
t1 raises its signal at the transition but image 4 never DMAs. The `sig` field discriminates
"kernel never consumes the raise" (stuck 1) from "consumed but not dispatched" (drops to 0).
Companion: an unconditional `[wid4gate]` AT-RAISE field dump inside the `YZ_WIDSIG_ALL` block
(yakuza/shims.cpp) reading rc/cont/pend/maxc/state/status/enabled/prio at the raise commit, plus
a first-3-raises guest backtrace riding `YZ_WIDSIG_BT` (`wid4-bt`). Retire with the wid4 frontier.

`YZ_F484_PROBE` (yakuza/shims.cpp, `ppu_trace_pc`, 2026-07-05, needs a `--trace` relift): at
`func_00F00484` entry on the main thread, prints the CRI request object's work-flag fields
(`P`, `V`, `P->[0x4]`, `P->[0x48]`, `P->[0x14]`) so the work-flag state is a DIRECT value
measurement instead of a PC inference. Verdict already obtained (the flag toggles correctly, the
notifier is exonerated); retire once the CRI producer root is fixed.

`YZ_NO_CONSUMER` (yakuza/import_overrides.cpp, ~line 1802, marked "TEMP: isolate consumer vs
libgcm"): kill switch for the RSX FIFO consumer thread â€” when set, the async consumer thread
(`yz_rsx_consumer`) is never created (only takes effect when `YZ_RSX_INLINE` isn't already
running the FIFO inline). Used to isolate whether a symptom comes from the consumer thread or
from libgcm's own producer-side behavior.

`YZ_T1SAMPLE` (yakuza/main.cpp, `yz_t1_sample_thread`, 2026-07-04): starts a low-rate (2 s),
non-suspending sampler thread that reads t1's last indirect-call/trampoline-hop target+LR
(`g_yz_t1_last_target`/`_lr`/`_sample_seq`, written by dispatch.cpp on t1's own call path) and
prints `[t1sample] seq=... (moved|UNCHANGED since last read) target=... lr=...` resolved to
`func_XXXXXXXX+off` where possible. Built to pin WHERE t1 silently spins after it stops issuing
lv2 syscalls (so the lv2-wait recorder can no longer report its location), without suspending
the thread or walking its host stack (both measured to corrupt this kind of measurement).

`YZ_FLIPTRACE` (yakuza/import_overrides.cpp `yz_ft_*`, 2026-07-08 s21): **diag, default OFF.**
Uncapped, sequence-stamped (`[ft] #seq t=ms tid=`) event log of the flip-label lifecycle for the
frame-~640 lost-flip wall (STATUS 1a): label/dev-credit ACQ pass + stall-episode entry, REL
(the 0xFFFFFFFF arm) with qhead+pending state, ARM (pending transition), QUEUE (0xE940 method),
FIFOSET (pkg001 GET/PUT sets = the game's replay loop signature), SYSFLIP/SYSQUEUE/RESETSTATUS/
FEC (sys_rsx syscall path), VBL-RETIRE/VBL-CLEAR (vblank completion + label clear), the HLE
SetFlipCommand overrides, plus a label value-transition WATCHER thread (~ms grain) that catches
writers outside every instrumented path (a plain guest store would otherwise be invisible).
Prints an ARMED banner + watcher banner (probe-liveness rule). All to STDERR.

`YZ_VBL_DIV` (yakuza/main.cpp `yz_vblank_thread`, 2026-07-08 s21): **lever, default 1 (off).**
Integer divider on the host vblank rate (62.5/N Hz; also scales YZ_VSYNC_PRECISE's period) --
the H4 discriminator for the flip-label stall: a ~5-7 FPS boot sees 8-12 real-time vblanks per
frame instead of the ~2 a 30fps title expects; N=4-6 restores the expected ratio. **A/B RUN
2026-07-09 (ftboot1 vs ftboot2): VERDICT = the flip stop point is IDENTICAL (~frame 704) at
full and 1/5 vblank rate -- the logo phase is frame-counted content ending naturally; H4 (and
the whole flip-wall framing) refuted, DONT_RECHASE #28.** Kept as a general-purpose pacing
lever for future timing A/Bs. Slows every vblank-derived guest clock; diagnostic only,
never load-bearing. Prints an ARMED banner when N>1.

`YZ_PARK_REL` (yakuza/import_overrides.cpp, stopper handler, 2026-07-09 s21): **lever, validated,
candidate default-ON.** Narrow deadlock-only variant of the retired YZ_APPLY_REL: applies the
game's OWN journaled tag-0x7F stopper release only after the consumer has parked on the SAME
self-jump stopper for 3 s with PUT ahead. Fixes the movie-boundary deferral deadlock (a commit
crossing a segment recycle defers the release; the drain lives behind t1's flip throttle which
waits on flips behind the very stopper -- scratch/stopper_drain_re.md). The June race partner
(gs_task double-apply during geometry) cannot exist in the parked state. MEASURED: 16 applies
per boot across 4 boots, zero entry-mismatches, zero June-style early wedges (park-rel fires
only past the 3 s threshold; normal logo-phase deferrals resolve through it cleanly too).
Promote to default-ON after a quiet multi-boot stretch; kill-switch stays the env polarity.

`YZ_JOBSTREAM_WATCH` (yakuza/shims.cpp vm_write32/64, 2026-07-09 s21): **diag, default OFF.**
Logs every PPU store to the CRI jobchain command stream (0x4019CA80-CB40) and the movie
decode-sync label family (0x10200FE0-FF0) with value + lifted-caller guest address
(yz_guest_addr_from_host). Two compares behind a cached flag on the hot write path when armed.
Decoded the full per-round producer choreography (DONT_RECHASE #29). ARMED banner on first use.

`YZ_JOBPEEK` (yakuza/import_overrides.cpp vblank tick, 2026-07-09 s21): **diag, default OFF.**
Change-triggered (FNV-hash) hexdump of the jobchain command stream 0x4019CA80-CB40 + chain
header 0x4019C880-8C0, once per vblank tick. The producer-side twin of the SPU-side [job-cmd]
probe; with [job-cmd-re] (same-value refetch counter, spu_dma.h, always on with YZ_OVL) it
discriminates "producer never wrote" from "module never fetched". ARMED banner on first use.

`YZ_NO_LLFAST` (runtime/spu/spu_dma.h GETLLAR, 2026-07-09 s21): **kill-switch for the LOCK-FREE
GETLLAR IDLE POLL (default ON, whitelisted).** The profiled dominant boot cost (5 SPURS kernels
x ~a core spinning through the process-wide lock-line lock, scratch/asset_window_profile.md):
when a context re-GETLLARs the SAME line and its write generation is unchanged, the cached
reservation copy is served lock-free. Companion coherence hardening (kept even with the switch
off): PUTLLC commits now bump the generation + kill peer reservations (CBEA reservation-lost),
and plain SPU PUTs into reserved lines notify. WHITELISTED to the SPURS mgmt line 0x40197C80
only: boot 14 measured a general cached-serve losing t1's flywheel start (zero audio rounds) --
host-side bulk writers (HLE _sys_memset/_sys_memcpy, file-read memcpys) write guest memory
without bumping the generation, so general serving is unsound until those paths sweep the
coherence bitmap (the queued follow-up). MEASURED with the whitelist: logo ~4-6 fps -> 8.5 fps,
t_PortStart 116-180 s -> ~96-102 s, flywheel + park-rel behavior unchanged (boots 14-16 A/B).

## s23 conformance-sweep flags (2026-07-09)

`YZ_NO_SPUDEC` (runtime/syscalls/lv2_register.c `group_start`, 2026-07-09 s23, kill-switch,
default OFF = decrementer starts): disables the SPU decrementer start at thread creation -- the
s23 conformance fix for a frozen decrementer (every `rdch SPU_RdDec` read 0 forever, every SPU
thread since creation). Prints an armed `[spudec]` banner. Faithful mechanism, not a force; the
switch exists only for A/B regression isolation.

`YZ_NO_TIMER1SHOT` (runtime/syscalls/sys_timer.c `sys_timer_start`, 2026-07-09 s23, kill-switch,
default OFF = one-shot timers work per spec): restores the old `period==0` -> EINVAL behavior.
Prints an armed `[timer1shot]` banner. s23boot12 verdict: this title never calls
`sys_timer_start` during the boot window -- the fix is spec-correctness, not a boot unblocker
(DONT_RECHASE #37).

`YZ_NO_MEMACCT` (runtime/syscalls/sys_memory.c, 2026-07-09 s23, kill-switch, default OFF =
accounting matches real hardware): disables user-memory accounting (ELF footprint + `sys_vm`
psize charges). Prints an armed `[sys_memory]` banner. Default (flag unset) = accounting stays
on and `get_user_memory_size` matches real hardware (RPCS3.log oracle).

`YZ_NO_TIMEANCHOR` (runtime/syscalls/sys_timer.c + yakuza/import_overrides.cpp, 2026-07-09 s23,
kill-switch, default OFF = anchored clocks): restores the old epoch behavior (`current_time`
computed from the QPC origin, `system_time` leaking host uptime). Prints an armed `[yz_time]`
banner. Default (flag unset) = wall-clock-anchored `current_time` + process-start-anchored
`system_time`.

`YZ_NO_QEV` (yakuza/import_overrides.cpp `sys_rsx` 0x103 case, reached via the 0xE940 method
bridge, 2026-07-09 s23, kill-switch, default OFF = mechanism ON): disables display-queue event
delivery. Prints an armed `[qev]` banner. s23boot12 verdict: the game never registers the queue
handler bit (handlers 0x04/0x86, qbit 0x40), so delivery is benign-idle every boot; kept for
faithfulness, not because it is load-bearing.

`YZ_CNTGATE` (yakuza/import_overrides.cpp vblank tick, 2026-07-09 s23, diag): the audio
round-driver count-gate probe -- resolves G via `game_toc`, prints the count EA once, then
prints on every change of the count / round-counter / guard fields.

## s24 frontier flags (2026-07-09 late — the main-loop wedge session)

`YZ_PARK_REL` **UPDATED s24: now two-tier.** The 3 s deadlock-only deferred-release apply
(s21) gains a FAST tier: fires at 250 ms parked IF t1 has made ZERO dispatch hops since the
park began (`g_yz_t1_sample_seq` witness — the journal drain runs on t1, so a hop-frozen t1
provably isn't coming) AND the release exists in the game's tag-0x7F journal (unchanged
precondition — we only ever deliver the game's own queued write). The 3 s tier remains as the
unconditional fallback. Rationale: the movie-boundary backlog is 16 consecutive stoppers
(MEASURED scratch/s24pr1.err); at 3 s each the lever spent ~48 s/boot politely confirming
already-witnessed deadlocks. Apply log now records tier + parked-ms + t1seq + fence word.
`YZ_PARKREL_FAST_MS=<n>` tunes the fast threshold; `0` disables the fast tier (3 s only —
the A/B control). STATUS: candidate default-ON pending the s24fast/s24slow/s24off matrix.
**UPDATED s25: fast tier now UNCONDITIONAL at fast_ms (witness dropped).** The measured
chain that killed the witness: (a) rides s25ride4-6 ground at 3-5 s/flip — t1 parks by
SPINNING in the gcm progress-throttle usleep loop (hops climb, so the seq-frozen witness
never fires) while making no flush call (stopper_drain_re.md Q1); (b) ride7 with a widened
spin witness (hop-target sample) still fired at 469-640 ms because t1's orbit is large in
the asset phase; (c) the loading-screen steady state re-parks EVERY frame at the
segment-recycle stopper (io 0x200000, 0x258-byte frame batches) because a LATCHED release
is only ever applied by the SPU journal consumer (which we lack) or this lever — fast_ms
is therefore the frame-rate governor (fps <= 1000/fast_ms; RPCS3's consumer releases at
sub-ms). Safety was always the preconditions (GET parked ON the stopper + the game's own
journal entry + PUT committed past it) — the 3 s tier fired on exactly those. Baseline
diag setting now YZ_PARKREL_FAST_MS=16 (~60 Hz consumer-substitute). The ungated
`g_yz_t1_last_tf` hop-target feed (dispatch.cpp) stays for diagnostics.

`YZ_DEFERWATCH` (yakuza/main.cpp, s24, diag, default OFF): 1 ms journal-head advance watcher —
dumps every new op-list entry (tag+ea) plus the defer-gate inputs (S1C/S20/S24, bufdesc
end/cur, PUT/GET) on each advance. ⚠ print volume PERTURBS the boot heavily (s25ride3:
iteration 1 at 3 min vs 10 at 2 min clean — LESSONS #6c); diagnostic boots only.

`YZ_FE0_WATCH` (runtime/spu/spu_dma.h, s22, diag, default OFF): logs every SPU PUT that
covers the 0x10200FE0 decode label ([fe0], armed banner) — the wid4-pool publish witness.

`YZ_JOBPUT` (runtime/spu/spu_dma.h, s24, diag, default OFF): job-image (13-15) DMA-PUT census
at all three MFC execution paths (list, plain, atomic) — the first-site-only census produced
a false zero that hid the flag CAS.

`YZ_LOOKAHEAD` (yakuza/main.cpp, s24, lever, default OFF — ledger #50 REFUTED eager use):
the 0x7F journal lookahead drain; first boot reproduced the June torn-content wedge.
Explicit opt-in only; do not enable without the patch-class entries being applied first.

`YZ_JRNL_HLE` (yakuza/edge_journal_hle.cpp + import_overrides.cpp, 2026-07-14/15,
experimental, default OFF): ordered, fail-closed EDGE journal wedge takeover (the Mac
07-14 export + two Windows adaptations). Runs only when GET is parked at a committed
self-jump and the producer head is stable; validates the complete span through the
matching release, applies known patch entries before tag-0x7F, and retires a tag only
after its operation. Windows adaptations vs the Mac design: (a) releases write the
faithful gcm jump-forward word (ledger #83 oracle; same value as the lever), not zero;
(b) the takeover cursor bootstraps from the live consumer poll cursor's episode
minimum (g_yz_jrnl_cur_ea), not arena base, so consumed-but-unzeroed entries are never
re-applied. Enabling it disables the legacy release-only `YZ_APPLY_REL` and
`YZ_PARK_REL` paths. Current decoder supports tag `0x10` memcpy, tag `0x7F` release, and
(s39, evidence-triangulated) the `[0x11 header][0x0A payload]` unit as retire-only (no
memory operation — scratch/s39_stage2_probe.md et al.); tags `0x04/08/09/0D` stop safely
and emit all eight entry words. PERMANENT CEILING (s39 decode): `0x08`/`0x09` are
render-attribute decoders writing through SPU-side pointer state — NOT host-replayable
from entry bytes, so spans containing them stay fail-closed by design; the faithful LLE
consumer is the only path through those. This is a safe decoding and A/B scaffold, not
yet a claim that the 800-frame wall is closed. Full contract and test instructions:
`docs/EDGE_JOURNAL_HLE.md`.

`YZ_JRNL_HLE_STABLE_MS` (same path, default `16`, range clamped to `0..5000`): producer-head
stability debounce before the HLE attempts an ordered transaction. This detects a frozen,
complete journal; it is not cycle timing and does not try to emulate SPURS scheduling latency.

`YZ_T1POLL` (yakuza/shims.cpp, s21, diag, default OFF): t1 poll-site sampler companion to
YZ_T1SAMPLE (names the syscall/poll site t1 spins in).

`YZ_W4TS` (runtime/spu/spu_dma.h SPU side + shims.cpp PPU side, s22, diag, default OFF):
uncapped CAS watch on the wid4 pool taskset bitset line 0x42450E00 ([w4ts-spu]/[w4ts-ppu],
running/ready/signalled/waiting transitions with writer attribution) — the s25 thrash
capture's instrument.

`YZ_UPDCB` (yakuza/dispatch.cpp `ps3_indirect_call`, 2026-07-09 s24, diag, default OFF):
logs every bctrl t1 issues with lr inside the master update handler func_00D1E838
[0xD1E838,0xD1FC38). ⚠ MEASURED USELESS AS BUILT: our lifter never materializes ctx->lr at
bctrl (same class as the bl no-lr — s19's "lr=0 on bctr chains"), so the lr-range filter
matches nothing (armed banner + 0 hits, scratch/s24cb1.err). Kept as the anchor for a
flag-scoped rewrite (set a marker in the chain probe at D1E838 entry instead); do not trust
its zero.

**Chain-entry probes (NOT a flag — a scripted recomp patch):** `py -3
scratch/patch_chain_probes.py` injects `yz_chain_probe(ctx, addr)` at the entry of the
main-loop chain functions (entry 00DDDA6C / main loop 000D0CD8 / master update 00D1E838 /
round driver 00A9F8AC / job writer 00E5F094) in the recomp chunks; `--revert` removes them.
The ONLY instrument that sees direct-`bl` entries (saved-LR walks are impossible in our
runtime: `bl` never sets ctx->lr; tramp guard sees only tail-branch hops; YZ_HOOK only
bctrl). Census prints on probe hits (5 s tick) AND from the watchdog every 60 s
(`watchdog-<n>m` tags, LESSONS #6d). Re-apply after any relift; runtime handler
`yz_chain_probe` lives in yakuza/main.cpp and is always compiled in (inert without the
patch).

`YZ_CTXWATCH` (runtime/spu/spu_channels.c + spu_dma.h, 2026-07-10 s26, diag, default OFF):
taskset context save/restore watch — registers each task's ctxsave EA at launch (both the
legacy spu_task_launch path AND spu_indirect_branch's natural-launch path; the pool tasks
take only the latter), logs per-cycle START/RESUME with a FNV hash of the main-memory
register block, and the spu_dma.h side logs every DMA SAVE/LOAD touching a registered
block. Parser: scratch/parse_ctxw.py. s26 verdict: the ctx round-trip is byte-healthy —
exonerated the save/restore path for ledger #57. Per-image print budgets (gs_task's idle
poll ate a flat cap — LESSONS #21).

`YZ_W4REC` (runtime/spu/spu_dma.h, s26, diag, default OFF): dumps every image-4 64-byte
work-record GET (EA + full record bytes, single-write print). Decoded the wid4 record
economy: five 0x40-stride slots at 0x424528A0-0x424529A0, word0=publish value, word1=target
EA guard (0 ⇒ legal no-op).

`YZ_W4REC_POLL` (yakuza/import_overrides.cpp vblank tick, s26, diag, default OFF):
non-faulting poll of the five record slots (word0/word1 on change) — replaced the page-guard
write-watch, which was BOTH invasive (shares the ctx-save page) and silently defeated by the
vm's aliased mappings. Caught the half-staged fetch that cracked ledger #57/#58.

`YZ_SLOTSTORE` (runtime/ppu/ppu_memory.h + spu_channels.c logger, s26, diag, default OFF):
compare-on-store in vm_write32/vm_write64 for the record-slot range — names any LIFTED PPU
store writer with tid + guest fn (via _ReturnAddress). s26 measured ZERO lifted stores to
the slots (the stagers are DMA/HLE-side) — kept as the store-path discriminator.

`YZ_UCMD_ON_FLIP` (yakuza/import_overrides.cpp, s26, kill-switch, default OFF): restores the
pre-s26 flip-event over-broadcast (delivering ALL handler-mask bits incl. USER_CMD 0x80 on
flip completion) — THE ledger #58 mode-B root. Only for A/B archaeology; never enable in a
real boot.

`RSX_NO_PASS_DEPTH_CLEAR` (libs/video/tests/replay_main.c, s26, kill-switch, default OFF):
restores the replay harness's old discard-depth-clears + once-per-frame heuristic (the
black-character class root, ledger #56/#58-era render fix). Harness-only; the live path was
already correct.

`YZ_W4MGMT` (runtime/spu/spu_dma.h, s27, diag, default OFF): img-4 atomics on the SPURS
mgmt line 0x40197C80 — measured the pool's readyCount pinned at 01 constant (select-gating
exonerated for the round-tail). ⚠ its print budget throttled across the wedge once
(fresh-eyes caught it) — treat capped tails per LESSONS #21.

`YZ_DLIST_SPU` (runtime/spu/spu_dma.h, s28, diag, default OFF): SPU PUT watch on the
display-list head EA 0x41504C00-4E00 — with YZ_WATCH_DLIST (PPU side) proved NOBODY writes
the list in early-stalled boots (ledger #63; downstream of the #64 lever misfire).

`YZ_NO_RECWAIT` (runtime/spu/spu_dma.h, s26, kill-switch, default OFF): disables the
half-staged work-record absorber (bounded wait-and-recopy on guard-armed/value-empty
fetches). Absorber measured inert when unneeded.

`YZ_NO_FIFO_RECOVER` — RETIRED s33 (2026-07-11). The s28 "recover_fifo analog" it gated
mis-modeled the oracle (RPCS3 re-reads the SAME GET on an illegal word; it never advances)
and measurably stranded GET (s32resur1: 22 skips through the unwritten io-0x8000xx segment
then a garbage-jump teleport; s28m10's 650 s RETURN park). The faithful re-read is now the
DEFAULT; see YZ_FIFO_SKIP4.

`YZ_FIFO_SKIP4` (yakuza/import_overrides.cpp, s33, A/B archaeology, default OFF): restores
the retired s28 skip-4 illegal-word recovery. Only for reproducing the stranded-GET class.

`YZ_FIFO_HB` (yakuza/import_overrides.cpp, s33, diag, default OFF): uncapped 5 s FIFO
heartbeat — GET, PUT, the word at GET, the CALL-return slot. Answers "where is the FIFO"
at any boot depth (every prior terminal park was invisible behind count-capped prints).

`YZ_FIFO_FLOWLOG` (yakuza/import_overrides.cpp, s33, diag, default OFF): log every
successful JUMP/CALL/RETURN flow transfer (get -> target). The s33 audit's discriminator
for how GET arrived anywhere; success paths were previously silent.

`YZ_MUTEX_LEGACY` (runtime/syscalls/sys_mutex.c, s33 conformance fleet, kill-switch,
default OFF): reverts BOTH s33 mutex guards (trylock EDEADLK on a NOT_RECURSIVE
self-relock; destroy EBUSY on an owned mutex) to the old non-conformant behavior.

`YZ_SPU_ET_LEGACY` (runtime/syscalls/lv2_register.c, s33 conformance fleet, kill-switch,
default OFF): collapses sys_spu_thread_group_connect_event's three per-source slots
(RUN/EXCEPTION/SYSTEM_MODULE) back into the old single slot and disables per-et
EINVAL/EBUSY.

`YZ_SPU_EXC_EVT` (runtime/syscalls/lv2_register.c, s33, opt-in, default OFF): deliver
guest-visible SPU-exception EVENTS to a connected exception queue. The host-side
[spu-exc] fault log is always on; delivery is gated because every current SPU fault is
an emulation artifact the resume machinery heals — real hardware never faults here, and
first delivery (golden 2026-07-11) showed Sony's default SPURS handler waking and
dumping diagnostics for our internal hiccups. Flip the default when the death classes
are extinct.

`YZ_AUDIO_LEGACY_PACE` (libs/audio/cellAudio.c, s33 conformance fleet, kill-switch,
default OFF): reverts guest block progression/notifies to WASAPI-occupancy pacing
instead of the fixed 5.333 ms block heartbeat.

`YZ_T1_HB` (yakuza/import_overrides.cpp vblank tick, s28, diag, default OFF): 2 s t1
host-liveness heartbeat — host CPU-time deltas + sampled RIP (brief suspend) + the
in-flight lv2 syscall/r3 (g_yz_t1_sc, set in lv2_syscall_dispatch). Cracked ledger #64.

`YZ_FASTLEVER_EARLY` (yakuza/import_overrides.cpp, s28, kill-switch, default OFF):
restores the pre-#64 behavior (park-rel fast tier active before the update loop starts).
Only for A/B archaeology — the early fast tier is THE measured early-stall trigger.

`YZ_GATE_PROBE` (scratch/patch_wkl4_gates.py + patch_gate_probe.py injections into
recomp_prx lifts, s26-s27, diag, default OFF): the wkl4 barrier/gate operand probes
([arr-pred]/[leg-*]/[cont-*]/[g31FC]/[g3678]) and the gs_task consume-gate probes.
Re-apply the scripts after any SPU relift; --revert removes.

`RSX_DEPTH_RT` (libs/video/tests/replay_main.c + replay_chr_main.c, s27; **s32: default
ON, set =0 to disable**): depth-target-as-texture snapshots in the replay harness. The
s27 composite regression (ledger #62) that kept this OFF was root-caused in s32: the
snapshot copied the 1280x720 canvas out of TALLER zetas (1280x768 / 1280x1024 shadow
maps) and the SRV was sampled at the declared texture scale — spatially garbled
projective reads plus zeroed rows past 720. Fixed by zeta-dims snapshots +
declared-size bind windowing (scratch/s32_character_fixes.md §5); s32 A/B: guard
patches unchanged, frame moves toward the hardware reference (character bbox
mean|d-ref| 14.1→10.8). RSX_NO_PASS_DEPTH_CLEAR / RSX_FP_FORCE_STAGE* /
RSX_VP_CLIP_DUMP / RSX_LOG_ZETA are harness diagnostics from the same investigation
(s26_fp_bisect.md).

`RSX_SURF_CROP` (libs/video/tests/replay_main.c, s29, opt-in, default OFF — KNOWN
REGRESSIVE, debug only): CPU-round-trip crop of a color surface sampled at a smaller
game-declared size than the physical canvas (the x=820 band mechanism,
scratch/s29_x820_band.md). The crop's flush ordering loses the target's own paint
(flush-only isolation repro in the same report) — do not re-default until that is fixed;
the ship fix direction is logical-size surface allocation, not this. **s31 UPDATE: that
ship fix is now IMPLEMENTED default-on (logical-size surface_get, no flag — see
scratch/s31_render_fixes.md, x=820 band FIXED + A/B'd); the crop path stays default-OFF
debug and was only made dimension-consistent.** Same-session harness
diagnostics: `RSX_DEPTH_DUMP_PRE`/`RSX_DEPTH_DUMP_POST` (`idx:path[,...]` raw float32
depth readbacks at exact draw indices, s29_draw803_occluder.md), `RSX_FP_FORCE_STAGE5`
(draw-803 footprint force-white probe), `RSX_FP_FORCE_STAGE6` (DIVSQ divisor/result
probes, s29_blue_remnants.md), `RSX_D3D_DEBUG` (D3D12 validation layer).

`RSX_NO_ZETA_TRACK` (libs/video/tests/replay_main.c, s31, kill-switch, default OFF =
tracking ON): restores the replay harness's old SINGLE shared depth buffer. The s31 fix
models each zeta target (location, offset) as its OWN depth resource (lazy, inline
far-clear on create, content persists across pass revisits, game depth clears applied to
the CURRENT zeta target) — the MEASURED root of the s29-5a player-character occluder
(cross-pass depth contamination; A/B + kill-switch byte-identity receipts in
scratch/s31_render_fixes.md). Setting the flag reproduces the pre-fix baseline
byte-for-byte on cap_user3d.rxs (verified 22/22 surface dumps).
| `YZ_FS_LAT` | diag | OFF | s29 (2026-07-10): cellFs latency-model/discriminator knob for the ledger-#67 staging race. `<usec>` = QPC busy-wait floor per data call (Open/Read/Fstat/Lseek/Close; Read waits POST-fread); `-1` = stderr lock-touch mode (rendezvous discriminator); `-3` = STDOUT lock-touch (s30 fresh-eyes: -1 tested the wrong stream). All four s29 modes measured NOT the FS_TRACE-flip mechanism (STATUS ⚡2) - kept as the probe kit. Armed banner `[fs-lat]`. |
| `YZ_FS_TRACE` | diag | OFF | extended s30: `=2` DARK mode — identical trace work (per-read ftell + same formatting) but written to a private 4KB-buffered scratch/fs_darktrace.log, ZERO stdout — splits the #67 flip-flag into local-work vs stdout-output atoms. `=1` (or any non-numeric value) = the legacy stdout trace. Banner `[fs-trace] ARMED: DARK mode 2`. |
| `YZ_SEM_TRACE` | diag | OFF | s30: bounded (4000-line) stderr trace of sys_semaphore wait/trywait/post on sem ids ≤ 8, with tid/value/caller-lr — built for the staging-race handoff hunt (measured: t1 stops posting the FS request sem 1 at the death; sem layer exonerated). Two ALWAYS-ON capped tripwires ride the post path regardless of the flag: `[sem-post] EBUSY refused` (the shadow-counter max check can spuriously refuse for small-max sems) and `[sem-post] RELEASE-FAILED` (handle/shadow desync = a silently lost wake). Banner `[sem-trace] ARMED`. |
| `YZ_STAGE_DECIDE` | diag | OFF | s30: 5 s stderr peek of the CRI FS DRIVER POOL (base vm[0x135CDFC], 40×0x4D0 slots at +0x868; per-live-slot open/status/mode/tot/cons/take/seq/phase/name) + staging header (w474/stagedSeq/exit/pathOps/P2) — the decision-input camera for the staging-race death (spec scratch/s30_staging_decision.md §6; caught the two wedged status=1/phase=0 slots). Reads only, no page watches. Banner `[stagedec] ARMED`. The `[cellFs] FIRST-READ tms=` stderr markers (once per open) are always-on companions. |

## s35 preload/render diagnostic flags (2026-07-12)

`YZ_FIFO_SKIP_STALE` (yakuza/import_overrides.cpp, RSX FIFO non-command handler, s35): **diag
band-aid, default OFF.** When GET parks on an illegal header (cmd&3==3) for >500 ms with PUT
ahead, scans forward for the next valid command/jump and resumes GET there (else GET<-PUT),
to skip an unfinalised Edge command-buffer hole. MEASURED insufficient on its own (the skip
landed on hole-data that looked like a jump -> off-ring -> drained to PUT, dropping the flip;
s35skip). Kept as a diagnostic; the render-FIFO desync is a PARALLEL track to the preload wall.

`YZ_FIFO_HOLE_DUMP` (yakuza/import_overrides.cpp, same handler, s35): **diag, default OFF.**
On first hit of a non-command park, hexdumps [get, get+0x120) so the unfinalised-hole layout
can be read (used to characterise the round-8 io=0x602E8 hole and the round-43 io=0x800028
out-of-ring 0xFFFFFFFF park). Read-only, one dump per distinct park.

`YZ_HOLD_BOOT` (yakuza/main.cpp yz_hold_boot, injected at func_001AAB20/func_001AB63C entry via
scratch/patch_chain_probes.py, s30 — first registered here s35): **diag/tactical, default OFF.**
Swallows the boot-pump exit-request while the CRI preload is demonstrably in flight (any live
driver-pool slot status==2 or the staging seq advanced within 120 s), capped at 20000 holds.
MEASURED INERT this era: func_001AAB20 is never called on the current binary (the boot pump
STALLS on the render-FIFO, it doesn't reach the intro-timeline exit) -> the shim never fires.
Retained; superseded as the preload fix by the s35 reframe (the wall is the CRI completion-
delivery, not the boot-pump exit). ALWAYS logs an [holdboot] ARMED banner + per-call lines.

### YZ_THREAD_PRIO (s36, 2026-07-12)
Apply the guest lv2 thread priority to the host thread (SetThreadPriority) in the PPU thread
create path (yakuza/threads.cpp, yz_lv2_prio_to_win). The game assigns lv2 priorities
(0=highest..3071=lowest; _gcm_intr=1, cri_dlg=800, cri_adxm_idle=1500) and lv2 schedules
strictly by them; our one-host-thread-per-guest-thread model previously DROPPED the priority
(all threads NORMAL), turning HW-calibrated producer/consumer handshakes (the scenario.bin CRI-FS
read-completion, s36) into timing races. Coarse 5-band map to Windows THREAD_PRIORITY levels
preserving the game's relative ordering. DEFAULT OFF (clean A/B until validated); [thr-prio] ARMED
banner. Faithful direction (honors the game's own priorities).

### YZ_DSTATUS (s36, 2026-07-12)
Enable the [dstatus] (driver status-setter func_00EEECDC) + [readdone] (func_00EEF88C) diagnostic
riders in yz_chain_probe. Their fprintf is on the HOT driver-status path and PERTURBS timing (flips
the s36 completion race, LESSONS #6b), so they are DEFAULT OFF; enable only for the fork-A/B/C
status-transition census. The light [req]/[cinvoke]/[radv]/[oadv] completion probes stay always-on.

### YZ_FORCE_WAKE (s36, 2026-07-12) — REFUTED, kept for archaeology
Watchdog thread that pokes any CRI consumer thread (t2/t3/t11/t12) parked >500ms on sem_wait/cond_wait,
via yz_force_sem_post / yz_force_cond_signal (runtime/syscalls/sys_semaphore.c + sys_cond.c). Diagnostic
test of whether re-driving the stalled CRI completion dispatch walks the preload. RESULT: NEGATIVE — the
poked consumer wakes to no enqueued work and re-parks; the preload does not advance (the work is never
enqueued; the root is t1's master loop wedging, not the wakeup). Default OFF.

### YZ_USLEEP_LR (s36, 2026-07-12)
Log the caller LR (dedup per distinct lr) of each t1 usleep(250) site (runtime/syscalls/sys_timer.c), to
name t1's stuck poll loop at the preload stall. NB the guest LR came back ZERO (ctr=0x010DD688) — use
g_yz_last_targets / YZ_T1SAMPLE instead. Default OFF.

### YZ_ARMFIRE (s37, 2026-07-12) — the completion arrival-before-arm race probe
In yz_chain_probe (yakuza/main.cpp), gates two handlers on the existing chain-probe targets: FIRE at
func_00EEF88C (the driver read-done handler; for scenario's slot D=0x01655848 logs M=*(D+0x440), arg=*(D+0x444),
armed=*(M+0x4), mseq=*(M+0x48), deduped on armed-state change) and ARM at func_00F00580 (the client mailbox
arm; logs M/arg/record). Confirmed the no-preemption race: the mailbox fires completions both armed=1
(delivered) and armed=0 (dropped) intermittently. Low-perturbation (deduped, few lines); heavy logging here can
flip the race (LESSONS #6b). Default OFF. [armfire] tag.

## s39 SPU channel-stall contract flags (2026-07-15)

The faithful SPU channel-wait implementation (runtime/spu/spu_channels.c `spu_ch_wait`/`spu_ch_wake`;
per-SPU CONDITION_VARIABLE+SRWLOCK appended to spu_context.h). `rdch` on an empty read channel now WAITS
instead of returning a stale/architected-default value; the wake is plumbed at every cross-thread producer.
Blocking is DEFAULT-ON. Scope: reads only — `SPU_RdInMbox`, `SPU_RdSigNotify1/2`, `SPU_RdEventStat`. Writes
(`SPU_WrOutMbox`/`SPU_WrOutIntrMbox`) are deliberately left NON-BLOCKING (this runtime has no cross-thread
out-mbox drainer, so block-while-full would deadlock the SPU host thread — documented at the wrch cases).
The `[ch-block]` witness (first 50 + every 512th blocked attempt) and `[ch-wait]` heartbeat (every cumulative
2000 ms of one stall, uncapped) are ALWAYS-ON (cheap; only emit when a stall actually happens).

| Flag | Where | Class | Default | Meaning |
|---|---|---|---|---|
| `YZ_CH_NONBLOCK` | spu_channels.c (`yz_ch_nonblock`; `spu_rdch`/`spu_rchcnt`/`spu_wrch`) | kill-switch | OFF (blocking ON) | `=1` restores the exact legacy non-blocking channel behavior everywhere: `rdch` reads-and-returns-stale, `rchcnt` unknown-channel default stays `1`, and the unimplemented-channel logs are suppressed. Use for A/B against the faithful path. |
| `YZ_CH_STRICT` | spu_channels.c (`yz_ch_strict`; `spu_rdch`/`spu_wrch` default cases) | diag | OFF | `=1` makes an UNIMPLEMENTED `rdch`/`wrch` (unknown channel id) additionally HALT the SPU via the existing stop mechanism (`spu_halt` → `SPU_STATUS_STOPPED_BY_HALT`, longjmp to the host-thread driver) instead of the default continue-with-0/ignore. Surfaces guest use of channels we don't model. Inert under `YZ_CH_NONBLOCK`. |

Note: the s39 design brief said "three new env flags"; the spec itself names only these two (the witness log and
heartbeat are unconditional, not flag-gated), so two are registered. See scratch/s39_ch_wait.md.

## s40b diagnostics + the ungated-trace fix (2026-07-16)

### YZ_LLECALL_TRACE (s40b, 2026-07-16)
Restores the per-call [lle-call] import trace in yakuza/dispatch.cpp (every import call made by
LLE firmware modules, args + result, two stderr lines per call). This was an UNGATED June-era
"TEMP DEBUG (SPURS bring-up)" that was never stripped: it was 34% of every boot log for ~a month
and materially paced the guest (LESSONS #6c; DONT_RECHASE #95). DEFAULT OFF. Turn on only for
targeted import-call archaeology, never during pacing/fps measurements.

### s40b witness kit (all ride YZ_STG2 / YZ_ZSLOT / YZ_BD70W; gs_task.c probes LOST on relift)
[stg2] v3 (sobj/a04/state + pool count + bd80/90), [slots] 12-slot word0 snapshots, [pool]
descriptor-pool alloc witness (0x5168), [s1]/[s1-bail] alloc-entry + alloc-NULL witnesses
(0xB0F4/0xB110), [credit] poll line-credit episodes (0x664C), [anch] poll-EA logger (0x6380),
[anch-wr]/[anch-dma] writes to the poll-EA home 0xBCAC, [anch-save]/[anch-restore]/[mgr-restore]
host ctx save/restore divergence witnesses (spu_channels.c), [reload-diff] RO-guard heal
classifier (zero vs nonzero runs), [feed-wr] mgr+0x110 feed-list push witness (spu_context.h),
[c5064-src]/[c7AC4-val] table-writer source/value witnesses. All default-OFF via their flags.

### YZ_JRNL_THROTTLE=<K lines> (+ YZ_JRNL_THROTTLE_MS) (s41, 2026-07-16)
The PRODUCER THROTTLE — the mechanism-proven fix candidate for the s40b wall (scratch/
s40b_findings.md sec.19-20): t1's [0x11][0x0A] journal append (single choke point
func_00E7DE88, a ppu_recomp_006.cpp HAND-EDIT lost on relift) waits until the consumer's
live poll cursor (published ctx at gs_task.c 0x6380 -> yz_consumer_cursor(),
spu_channels.c) is within K lines (0x80 B) of the producer head (S+0x00, S=vm[toc-0x7410])
before publishing more. Restores the real-HW invariant that the release engine is EMPTY
at the game's phase-boundary reinit. Default OFF; `=1..4096` sets K (a bare non-numeric
value = K 8). Fail-open everywhere (no consumer / wiped anchor / cursor outside arena /
unparsable arena); per-append timeout escape YZ_JRNL_THROTTLE_MS (default 200) so a
consumer wedge can never deadlock t1; 64 consecutive timeouts DISARM for the boot.
Witnesses: [jthr] ARMED banner, sampled wait stats, TIMEOUT/DISARMED lines.
Expect a slower boot while armed (t1 locksteps to the consumer) — never leave on for
fps measurements. Retirement condition: block-lifted gs_task hot loops making the
consumer fast enough that the backlog never builds.

## s41/s42 SPU flight recorder (v2: 2026-07-16)

Phase 1 (s41): a binary ring-buffer history of every LS store, cross-function branch,
channel op and DMA transfer on the gs_task journal-consumer SPU (g_yz_consumer_ctx,
spu_channels.c), dumped on the wedge signature. Replaces sampled fprintf probes with a
complete ordered history. Design + validation: scratch/s41_fltrec_report.md.

v2 (s42, same day): scratch/s42_order_reconstruction.md's sufficiency verdict named three
gaps for the planned RPCS3 ordering diff and this closes them — records now carry a global
atomic `seq` + `spu_id` (multi-context capture, source identity for every record kind
including FOREIGN_WRITE), and two new record kinds (YZ_FR_XIMG at the cross-image adoption
site, YZ_FR_CALL_RET at spu_img_restore — the closest runtime-visible approximation for
direct-call coverage; the lifter's bare-C-call codegen for statically-resolved calls stays
genuinely invisible without touching the lifter/generated code, out of scope). Format
version bumped (28-byte v2 records; old 16-byte s41 dumps stay decodable — see
tools/fltrec_dump.py's format_version dispatch, meta.json's `format_version` field).

Code: runtime/spu/spu_fltrec.{h,c} (rings + record types), hooks in spu_context.h
(spu_ls_write32/128, SPU_DRAIN), spu_channels.c (spu_wrch/rdch, the ctx save/restore memcpy
sites, spu_img_restore, the "[spu-ximg]" adopt site), spu_dma.h (mfc_submit); decoder
tools/fltrec_dump.py.

### YZ_FLTREC (s41/s42, 2026-07-16)
Arms the recorder. Default OFF (unset/`0` = fully inert: every hook site is a single
`g_yz_fltrec_on == 0` check, one predicted-not-taken branch). `=1` (or any non-`0` value)
allocates the two rings (both now atomic-cursor multi-writer as of v2 — see
YZ_FLTREC_ALLCTX) and begins recording once g_yz_consumer_ctx is published. `[fltrec]
ARMED v2 ...` banner names the ring sizes, record size, and allctx state. Without
YZ_FLTREC_ALLCTX, recording still only happens for the one designated consumer ctx (cheap
pointer compare) — inert on every other SPU thread even when armed, same as phase 1.

### YZ_FLTREC_ALLCTX (s42, 2026-07-16)
Multi-context mode. Default OFF (consumer-ctx-only, phase-1 behavior). `=1` records every
registered lifted-SPU context, not just g_yz_consumer_ctx — the MAIN ring's atomic `seq`
cursor makes the cross-writer arrival order reconstructible (same guarantee the FOREIGN
ring already had). Only read once, at arm time, alongside YZ_FLTREC.

### YZ_FLTREC_MUTE_PCS=<hex[,hex-hex,...]> (s42, 2026-07-16 night; ranges added same night)
Per-pc flood mute for allctx captures. Entries are single pcs ("11B0") or inclusive hex
RANGES ("1170-1200"), comma-separated, max 8. Records emitted at a matching pc are sampled
1/256 per SPU instead of recorded raw. Ranges exist because single-pc muting on an unrolled
hot loop just promotes the neighboring instruction (measured: muting 0x11B0 crowned 0x1178
at equal weight, s42 flood census). Max 16 entries. Companion `YZ_FLTREC_MUTE_SKIP_IMG0=1`
exempts image-0 (gs_task/consumer) contexts from ALL mutes — needed because the kernel/policy
flood bands (0x0A00-0x4700, ~93% of idle-SPU traffic) overlap gs_task's own signal pcs
(sweep 0x352C-0x3568) and the mute is otherwise image-blind. Built for the s42 diff's closing
spec (scratch/s42_ordering_diff.md §5): the two spin floods (channel programming 0x11B0,
reservation retry 0x5AC) were ~95% of allctx ring traffic and shrank the visible window to
~2s. `YZ_FLTREC_MUTE_PCS=11B0,5AC` stretches the window to steady-state scale. Unset =
record everything (default, behavior unchanged). Sampling counters deliberately non-atomic
(approximate 1/256). Loud `[fltrec] MUTE armed` banner. Retire with the recorder.

### YZ_FLTREC_MB=<n> (s41, 2026-07-16)
Sizes the MAIN ring in megabytes. Default 1024 (unset). The foreign ring is a separate
fixed 4 MB, not controlled by this flag (see the report for why). Only read once, at arm
time; changing it after boot has no effect.

### YZ_FLTREC_DUMP_ON_SWEEP (s41, 2026-07-16)
Opt-in second dump trigger (independent of YZ_FLTREC being armed for recording — either
trigger fires yz_fltrec_dump, which itself no-ops if the recorder was never armed): dumps
once on the first write to LS [0xBA00,0xBC00) from guest pcs 0x3520-0x3560 (the reinit
sweep), consumer ctx only. Hooked in spu_context.h's spu_ls_write128. The other trigger
(always on, no flag) is the [stop-jrnl] park detector in yakuza/import_overrides.cpp,
which dumps once when a stopper park first crosses 30s. Max 2 dumps per process either way.

### YZ_NO_RESVSW (s41, 2026-07-16) — kill switch, default OFF (fix ON)
CBEA conformance (SPE_MFC-Tutorial p.27): atomic reservations do not survive a context
switch; our task-switch seams (yz_resident_save / yz_ctx_shadow_save / yz_task_ctx_restore /
yz_ctx_shadow_restore, spu_channels.c) now clear the SPU's reservation, with the always-on
[resv-sw] witness logging every LIVE reservation cleared (sampled). Practical exposure was
the ABA case only (PUTLLC content-validates the full line), so this is conformance + a
measurement, not a claimed wall fix. `=1` restores the legacy keep-across-switch behavior.

### Coherence window 2 (s41, 2026-07-16) — always on, no flag
CBEA 9.12.10 puts no address bound on reservation coherence; window 1 [0x40000000,
0x44000000) left every out-of-window SPU-reserved line (notably the SPURS tasksets at
0x63Dxxxxx, PPU-written by CreateTask2) with UNSERIALIZED plain-memcpy PPU writes — the
proven torn-write clobber class of the 2026-06-16 fix. Window 2 [0x60000000,0x68000000)
gets the same bitmap+generation machinery (spu_channels.c). Reservations outside ALL
windows now print the loud [coh] witness (sampled) instead of failing silent — treat any
such line in a log as a coverage gap to close.

## s42 YZ_SPU_LOCKSTEP (2026-07-16) — interventional diagnostic, default OFF

Reviewed design: scratch/s42_lockstep_design.md (v2, the ★REVIEW DELTAS section is
binding) + scratch/s42_lockstep_review.md (adversarial attack log). Code:
runtime/spu/spu_lockstep.{h,c} (ring registry + one global run token, mutex+condvar);
hooks in spu_context.h (SPU_DRAIN, the ★REVIEW-corrected gate site — NOT
spu_indirect_branch), spu_dma.h (both GETLLAR legs), spu_channels.c (spu_ch_wait
block_begin/end), lv2_register.c (spu_exec_thread_proc register/unregister). Interventional
test of the M8 (ORDERING) finalist for the s41 release-engine wall (ledger #101): forces a
structured round-robin SPU interleave and checks whether the wedge dissolves. Per ★REVIEW
(A), the ONLY positive readout is the wedge DISSOLVING past the frame-~800 content
milestone — any terminal-shape change alone is a NULL result (#93: every probe this arc
moved the shape). Class: diagnostic/interventional, never a shipping behavior change
(LESSONS #13); retirement condition = M8 resolved (wall closed or ordering refuted).

### YZ_SPU_LOCKSTEP (s42, 2026-07-16)
Arms the round-robin token. Default OFF/unset = fully inert (every hook site is a single
`g_yz_lockstep_on == 0` check). `=1` (or any non-`0` value) registers every lifted-SPU host
thread into one ring (registration order) behind a global run token; only the holder
executes lifted code. `[lockstep] ARMED quantum=N` banner at first registration; heartbeat
every 100k token passes; starvation watchdog (token held >5s wall by one member while
another is registered) prints up to 10 loud lines. The decrementer is frozen to
wall-time-while-holding-the-token (★REVIEW C) by advancing `ctx->dec_start_tb` at every
acquire — the RdDec/WrDec formulas in spu_channels.c are UNCHANGED, and this module writes
`dec_start_tb` only while armed, so the default boot is bit-identical.

### YZ_LOCKSTEP_QUANTUM=<n> (s42, 2026-07-16)
Quantum unit, default 65536. Counted per drain-tick (SPU_DRAIN re-entry, ≈ one guest
instruction) AND per GETLLAR (both spu_dma.h legs) — whichever fires first ticks the shared
counter. Only read once, at arm time.
