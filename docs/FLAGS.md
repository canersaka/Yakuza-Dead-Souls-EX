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
| `YZ_JOB_WILDCARD_OK` | spu_channels.c (spu_lookup) | **Kill-switch for the jobchain-family wildcard REFUSAL (2026-07-08, guard DEFAULT ON).** ROOT (MEASURED, scratch/idboot.err + 3 prior boots): the jobchain loads its job binaries into descriptor-assigned LS slots (round-1 loads jobB/0x01275A00 at LS 0x4C00 â€” head bytes 43 49 4E verified resident) but the job lifts were fixed-base, so `spu_lookup`'s image-0 wildcard silently served **gs_task's** `spu_func_00004C00` at the job site â€” the notify job never ran, the IWL event flag never set, zero spup17 (the s19 wall, DONT_RECHASE #23/#24). Fix = each job binary lifted at BOTH slot bases + refuse cross-image wildcard for images 13-15 at job-span addresses (>=0x4880). `=1` restores the old silent substitution for A/B. âš  **A registry-only GENERALIZATION of this guard (refuse any contested address, no game constants) was tried and REFUTED BY MEASUREMENT the same day (scratch/genboot.err): dormant task images (spuimg_06 = img 9) register spans overlapping the RESIDENT kernel's (0x290), so the serviceâ†’kernel yield got falsely refused â€” a context-free registry cannot distinguish "kernel resident, img 9 dormant" from "jobB resident, gs_task dormant". A sound general guard needs RESIDENCY tracking (which binary actually landed where, per context â€” the DMA recorder generalized) or a clean image-0-is-only-the-kernel taxonomy (ours conflates kernel+gs_task under image 0 by design). Do not re-try the registry-only version.** |
| `YZ_KERN_WILDCARD_OK` | spu_channels.c (spu_lookup_apply_job_guard + the foreign-resident adopter) | **Kill-switch for the KERNEL-context wildcard/adoption refusal (s24 4881ef0, extended s25 to the foreign-resident adopter; guard DEFAULT ON).** A context tagged image 16 (SPURS kernel) whose computed branch misses every kernel registration is a wild branch by definition (legitimate kernel exits switch images before entering foreign code); serving it from the image-0 wildcard — or, the s25 close, the foreign-resident adopter — executed gs_task's code in kernel era and mis-attributed the tid-0x2004 death for weeks (ledger #34/#49/#51). `=1` restores the old silent substitution for A/B. |
| `YZ_NO_UCMD_RETRY` | import_overrides.cpp (yz_rsx_method 0xEB00 + yz_ucmd_retry_pending) | **Kill-switch for LOSSLESS user-interrupt delivery (s25, DEFAULT ON — the round-25 stall root, ledger #52).** MEASURED (scratch/s25ride.err): the game coalesces its ucmd cause counter (1..21 then 25, SDK-documented rapid-user-command coalescing) and the single coalesced send hit a momentarily full RSX event queue — sys_event_port_send returned 0x8001000A EBUSY and the fire-and-forget path lost it; the wid4 pool never published rounds 22-25 and the stream parked forever on SEMAPHORE_ACQUIRE want=25. Fix mirrors lv1's single pending-cause register: latch the undelivered cause, retry at the top of the consumer loop until the queue drains (userCmdParam already carries the latest arg). `=1` restores fire-and-forget for A/B. |
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
`YZ_JRNL_WATCH`
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
