# FLAGS.md — registry of `YZ_*` environment flags

Rule: every flag added to the runtime/runner gets a row here — purpose,
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
| `YZ_FLOWCTL` | yakuza/main.cpp ~1788 | **RETIRED AGAIN 2026-07-02 (default OFF; this flag opts back IN for A/B).** The race the lever covered is root-caused: OUR deferred-release applier raced Sony's real journal consumer (see `YZ_APPLY_REL`). With both off, 12/12 boots show zero type-1 wedges (state-classified, not lucky-window: 2 slow-but-healthy under compile load; 1 instance of the separate pre-existing late audio race). Evidence scratch/{bad1,cfgA*,cfgB*,val*}.err. Delete with `YZ_APPLY_REL` after a quiet stretch of boots. |
| `YZ_APPLY_REL` | import_overrides.cpp ~1216 | **RETIRED 2026-07-02 (default OFF; opts the old deferred-release applier back IN for A/B).** The applier (f8d0386) was correct scaffolding while the real consumer couldn't run; post il/SPU_RET/backoff, gs_task does the whole journal job itself (measured [gs-put]: patch PUTs pc 0xB60C, FENCED release PUTs pc 0x5F00; GET never met an unpatched stopper in 12 applier-off boots). Leaving it on RACES Sony's consumer — releases land without the preceding patches: 3/3 applier-on boots wedged t1 at ~+6 s, 0/12 off. Default = faithful memwatch spin at stoppers. Delete with `YZ_FLOWCTL` after a quiet stretch of boots. |
| `YZ_NO_THR_NUDGE` | yakuza/main.cpp ~951 | Kill-switch for the throttle nudge — lives INSIDE the (now opt-in) yz_flip_advance thread, so it's inert unless `YZ_FLOWCTL=1`. Retires with the band-aid code. |
| `YZ_NO_APPLY_REL` | (gone) | **Flag retired 2026-07-02** — the applier it disabled is now default-OFF; the polarity inverted into `YZ_APPLY_REL` (see above). |
| `YZ_JRNL` | import_overrides.cpp (yz_jrnl_retire_through) | OPT-IN EXPERIMENT (2026-07-02): journal **retirement sweep** — when GET applies a deferred release, zero the journal entry tags behind it (the game's GPU-progress ledger; the EDGE consumer's contract per the RPCS3 oracle). UNVALIDATED and now MOOT (2026-07-02 late): reachable only via the retired `YZ_APPLY_REL` path, and the real consumer (gs_task) both applies and retires the journal itself — delete with the applier. Five sibling designs tested + refuted 2026-07-02: eager apply (GET escapes into unbuilt lists), eager release (GET outruns producer), consume-once pending set, zero-all with/without lag-by-one (producer freezes at ~24 entries — it re-reads its own entries). |
| `YZ_NO_LAUNCH_UNWIND` | spu_channels.c ~946 | Kill-switch for the SPU task launch-unwind (5882fe4). Keep. |
| `YZ_NO_SPUBACKOFF` | spu_dma.h (GETLLAR) | Kill-switch for the **SPU idle-poll host-yield backoff** (2026-07-03): same-line GETLLARs with an unchanged write-generation escalate cpu-pause → scheduler-yield, so 5 spinning SPURS kernels stop saturating the global lock-line lock (measured 5×~97% core + boot-pacing collapse without it). Faithful (polling continues; ladder resets on any observed write). Keep. |
| `YZ_RSX_DRAW` | libs/video/rsx_live_draw.c (`rsx_live_draw_enabled`) | **Kill-switch for the Track B live NV4097 draw path (2026-07-03, Track B stage 5 B2).** Default ON; `=0` makes the live-draw engine inert so the runtime keeps its existing (null/clear-color) present. The engine is the validated capture-replay D3D12 pipeline (rsx_dispatch + NV40 VP/FP decompilers + PSO/sampler/mip engine) driven by the live FIFO consumer. **WIRED 2026-07-04** into yakuza/import_overrides.cpp: `rsx_live_draw_method(method,arg)` fed at the top of `yz_rsx_method` (full stream), engine self-presents on the 0xE944 flip; `rsx_live_draw_init` runs on the window thread bound to the null backend's HWND (`rsx_null_backend_get_hwnd`) and suppresses the null GDI present on success (`rsx_null_backend_suppress_present`). Boot-verified: `[rsx] live-draw engine up (D3D12)`, 12 flips presented, zero crashes. |
| `YZ_RSX_DUMP` | libs/video/rsx_live_draw.c (`rsx_live_draw_present`) | **DIAG (default OFF): dump the live-draw framebuffer.** When set, writes the presented color surface of the first 8 frames to `scratch/ld_frame_NN.ppm` (self-contained D3D12 readback). The per-frame `[live-draw] frame N presented: draws=X clears=Y` stderr line is always on for the first 32 frames (cheap; verifies real geometry vs clear-only). Remove when the render path is stable. |
| `YZ_NORESUME` | spu_channels.c ~1026 | Kill-switch for the SPURS yield/resume context-switch path. Keep. |
| `YZ_STARTTASK_HOOK` | spu_channels.c (spu_task_launch_check + prof path) | **RETIRED-to-opt-in 2026-07-02**: re-enables the legacy "LS 0x1CC0 = StartTask" launch hijack for A/B. LS 0x1CC0 is actually the taskset-SYSCALL switch (`bi $r2`, jump table at 0x1CC4); the hijack turned every WAIT_SIGNAL/YIELD of the matched elfs into a bogus instant relaunch and skipped Sony's context save. Default OFF = Sony's case handlers + dispatch run lifted. Delete after a quiet stretch of boots. |

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
| `YZ_MOVIE_TEST` | yakuza/import_overrides.cpp (yz_window_thread) | `=<path.sfd>`: standalone proof of the host movie path — decode the .sfd with FFmpeg (libs/codec/movie_ffmpeg.c) and present it straight to the D3D12 window via `rsx_live_draw_present_rgba` (movie mode gates the guest's draws off). Verified 2026-07-04: plays hd_sega_logo_us1012.sfd end-to-end, 100 frames, no crash. NOT the game hook — just proves decode→present in-process. Needs YZ_RSX_DRAW on. |
| `YZ_ADX_RELEASE_TEST` | libs/filesystem/cellFs.c (`yz_adx_release_test_tick`) | **2026-07-04, decisive control-flow experiment (default OFF), independent of `YZ_ADX_HLE`.** Fires the SAME two calls a real ADX decode batch would (`yz_adx_hle_advance_adxm` + `yz_adx_hle_release_spurs_waiter`, i.e. advance ADXM `0x01613368+0x294/+298/+29C` to a fabricated monotonic "N blocks decoded" value, then call the guest's real `cellSpursEventFlagSet` on `0x63D61720` with all bits set) UNCONDITIONALLY on every `.cvm`/voice-stream `cellFsRead`, with NO real decode (silent/zero PCM — tests the control-flow release only, not audio). Purpose: settle whether the SPURS-release lever is even the right one BEFORE building an AHX/MPEG-Layer-II decoder (YZ_ADX_HLE was measured inert on the real stream — it's AHX not ADX). REMOVE once the SPURS-release question is settled either way. |
| `YZ_ADX_HLE` | libs/filesystem/cellFs.c (`yz_adx_hle_on_read` + helpers) | **2026-07-04, opt-in experiment (default OFF). MEASURED INERT on the real stream — see below.** Clean-room host ADX decode (libs/codec/adx_decode.{c,h}, written from the public ADX spec only) of the CRI intro-voice stream, intended to bypass the LLE `cri_audio` SPU codec (measured dead: launches, never advances the ADXM progress fields). Hooks `cellFsRead` on `adv_voice_talk.cvm`/`*.cvm` paths: mirrors read bytes into a host shadow buffer by file offset, scans for a valid ADX header, decodes every complete block, advances the ADXM progress object `0x01613368+0x294/+298/+29C`, and calls the guest's real `cellSpursEventFlagSet` (libsre 0x02016010) on the measured SPURS poll object `0x63D61720` with an all-bits-set release (UNPROVEN which bits t1's `cellSpursEventFlagWait` call actually waits on). **BOOT-TESTED 2026-07-04 (YZ_AUDIO_FORCE=1 YZ_ADX_HLE=1, scratch/adx_hle_boot2.{out,err}): zero `[adx-hle]` log lines — the decoder never fires.** Root cause (parsed the container's real ISO9660 directory, scratch investigation not yet a committed tool): `adv_voice_talk.cvm` holds `*.AHX` files, not `*.ADX` — AHX is a DIFFERENT CRI codec (ADX-shaped header + MPEG-1 Layer II payload, confirmed via the MPEG sync word right after the copyright tag), which `adx_open()` correctly rejects (encoding_type != 2/3). The SPURS-release/ADXM-advance machinery is therefore UNEXERCISED against the live boot. REMOVE or extend to AHX (MPEG Layer II) decode when the CRI-voice frontier is next picked up. |

## Diagnostics (default OFF; side-effect-free when unset)

**Permanent generic probes (2026-07-02 — prefer these over new hardcoded ones):**
`YZ_DUMP_AT=<seconds>` (main.cpp: fire yz_dump_all_threads ONCE at +N s regardless of
watchdog state — reads a healthy-but-parked boot at a chosen instant; pair with YZ_L1SNAP
for the invasive sub-dumps; don't use dump-armed runs for pass/fail rates);
`YZ_PEEK=ea1,ea2,...` (main.cpp: change-triggered 4-word dumps of up to 16 hex guest EAs,
VirtualQuery-guarded — moves a memory probe with no rebuild); `YZ_HOOK=addr1,...`
(dispatch.cpp: log args+lr on every INDIRECT call to up to 8 guest code/OPD addresses —
direct `bl` calls are invisible; libsre names in scratch/libsre_lle_map.txt);
`YZ_TASKARG` (spu_channels.c, 2026-07-03: log every SPURS task launch — the pc-0x3050
entry branch — with gpr3/gpr4 args, cap 400; the lean replacement for YZ_SPU_PROF when
only launch args are needed — PROF's per-branch overhead crawls the whole boot).

`YZ_INTRMBOX_LOG` (spu_channels.c, `SPU_WrOutIntrMbox` handler, 2026-07-04, diag — pure
diagnosis for the t1 event-flag wedge): logs EVERY SPU class-2 doorbell write
UNCONDITIONALLY, before any routing decision — raw value, decoded code/port(spup), the
`spu_group_spup_queue` resolution (0 = no binding, i.e. would-be-dropped), and which path
the existing routing logic takes (send_event/throw_event routed, dropped, eflag_set_bit,
unrouted/buffered). Cap 200. Answers whether the SPU ever targets port 17 / queue 2 (t1's
SPURS event-flag queue). Side-effect-free when unset. REMOVE once the t1-wedge frontier is
resolved.

**PPU differential trace group (shims.cpp `ppu_trace_pc`, emitted before EVERY instruction by
the `--trace` lifter build only — inert in a normal build):**
`YZ_PPU_TRACE=1` (enable) + `YZ_PPU_TRACE_TID=<n>` (gate to one guest tid, default 1) +
`YZ_PPU_TRACE_ARM=0xADDR` (start logging on the first hit of this PC, default = from start) +
`YZ_PPU_TRACE_N=<count>` (budget, default 3M) → one hex PC/line to scratch/ppu_trace.txt (the
format tools/tracediff.py + the RPCS3 emitter both use; docs/TRACEDIFF.md §PPU; driver
tools/ppu_diverge.ps1).
`YZ_ARM_PC=0xADDR` (shims.cpp `ppu_trace_pc`, added 2026-07-05, diag) — on the --trace build
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
direction=r5; also reads `*(taskset+0x74)` = the owning wid per SPURS_TASKSET.md's
`taskset+0x74=wid` field, or reports `taskset==NULL` = the IWL case),
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
cond-4 at the CRI movie gate (the clean-binary spin loop) — used 2026-07-04 to show t1 is
deep in the CRI player's server loop with an empty work queue (lr=0, trampoline-dispatched;
so no single poke — the movie needs real decode output to advance). REMOVE with the CRI frontier.

`YZ_MWPLY_PROBE` (yakuza/dispatch.cpp, `ps3_indirect_call` + `yz_tramp_guard`, 2026-07-04):
dynamic-ABI probe for the mwPly (CRI Sofdec) player leaf calls
(func_00F4D0A8=mwPlyIsNextFrmReady, func_00F4DA90=mwPlyGetFrm,
func_00F48E48=mwPlyGetAudioPcmData_PS3 — see scratch/MWPLY_RESOLVE.md). Two hook sites are
needed for full coverage: `ps3_indirect_call` gets a full entry+exit+out-param-dump
pass-through wrap for the genuinely-indirect (bctr) path; `yz_tramp_guard` gets an
entry-args-only log for the same-chunk direct-tail-branch path (func_00F48E48 is reached
that way too — `g_trampoline_fn = func_00F48E48; return;` inside another lifted function
— which never reaches ps3_indirect_call; DRAIN_TRAMPOLINE captures the callee before any
hook runs and calls it unconditionally, so a post-call wrapper can't be spliced in there
without editing generated code). Volume-bounded (first 40 calls of each
target, then 1-in-500). MEASURED 2026-07-04 (2 boots, ~100s each, reaching the healthy
32-frame live-draw render state): ZERO hits on either hook site — the mwPly cluster is not
dispatched at all yet at this point in boot. REMOVE with the CRI/mwPly frontier.

`YZ_MWPLY_FORCEREADY` (yakuza/dispatch.cpp, the `ps3_indirect_call` mwPly wrap): diagnostic
force, default OFF — when the poll gate func_00F4D0A8 IS dispatched, skip the real call and
return 1 ("ready") to see whether the player advances into calling the getters. Only fires
inside the mwPly wrap, so it's a no-op until that wrap is ever reached (measured 2026-07-04:
never reached in a 100s boot — see YZ_MWPLY_PROBE). REMOVE with the CRI/mwPly frontier.

`YZ_T1_UNBLOCK` (runtime/syscalls/sys_semaphore.c `sys_semaphore_wait`, runtime/syscalls/sys_event.c
`sys_event_queue_receive`, 2026-07-04, DIAGNOSTIC ONLY): companion to `YZ_EVFLAG_FORCE` — scopes
how deep t1's CHAIN of SPURS waits goes by making t1's (tid==1 only) blocking waits in this stuck
phase return CELL_OK immediately instead of parking: `sys_semaphore_wait` (syscall 92) returns
CELL_OK without decrementing/consuming when the semaphore value is <=0 (logs `[t1-unblock] sem_wait
id=%u forced`); `sys_event_queue_receive` (syscall 130) returns CELL_OK with a zeroed event
(gpr4-7 and the out-buffer all 0) when the queue is empty instead of blocking (logs `[t1-unblock]
eq_recv q=%u forced`). Logging capped at the first 100 hits per syscall; the forcing itself is
unbounded. Heavy hammer — deliberately breaks the real wait semantics (a real producer's payload
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
so far`). Diagnostic-only, permanent kit — retirement: superseded-by-nothing.
Built to answer who arms cri_dlg's work flag `0x01661474`.

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
REMOVE when the jobchain frontier closes. Later additions under the same flag:
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
⇒ lost write/visibility — added 2026-07-03, REMOVE with the pxd-dispatch frontier),
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
Side-by-side wid0-vs-wid2 request-path compare — does wid0 ever issue SELECT_TASK(5)? Retire
with the wid0-dispatch frontier.),
`YZ_HALT_LOG`, `YZ_POLTRACE`, `YZ_POLHOP`,
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

## Diagnostics backfill (2026-07-05)

`YZ_B1_BLEND` / `YZ_B1_CULL` / `YZ_B1_DEPTH` / `YZ_B1_SAMP` (libs/video/tests/replay_main.c
`b1_read_env`): the Track B replay tool's B1 state-group kill switches — set any one to `"0"`
to disable that decoded-state group (blend, cull, depth, sampler) in the PSO build, all
default ON. Used to bisect Track B render regressions by state group. `YZ_B1_TEXLOG`
(same file, the texture-decode path): when set (any value), prints one `[texlog]` line per
decoded texture (offset/format/dimensions/mips/filter min-mag/fallback-or-ok) — a diagnostic
dump, not a kill switch.

`YZ_CRIDLG` (yakuza/shims.cpp, the `num == 107` cond_wait hook, 2026-07-05): logs cri_dlg's
(tid 11/12, the CRI asset-load dispatcher) control-block state at each `cellCond`-style wait —
ctrl address, the work-flag at `+4`, the callback/arg at `+0x40/+0x44`, run state at `+0x50`,
cond byte at `+0x14` (first 12 hits, then every 250th). Forks whether the stall is a producer
bug (work never queued) or a callback bug (work queued but never dispatched); see the comment
citing RPCS3's scenario.bin → player_pos.bin → all_csb.par → movie load sequence.

`YZ_EVFLAG_BT` (yakuza/dispatch.cpp, inside the `YZ_EVFLAG_WATCH` wait-log block, 2026-07-04):
companion to `YZ_EVFLAG_WATCH` — walks the guest PPC64 back-chain (r1 → saved caller sp/LR)
to name the real game function(s) that called into `cellSpursEventFlagWait`, since the
immediate `ctx->lr` at the hook site is 0 (fired inside the trampoline hop, not the real
caller). Fires only for `ea==0x4019C680` (the measured t1-wedge object), capped at the first
5 hits.

`YZ_FORCE_RC` (yakuza/import_overrides.cpp, the vblank coherence-test block, ~line 2068): every
vblank, bumps the SPURS taskset's `wklReadyCount1[wid]` to 1 directly inside the SPU lock-line
(so it survives the kernel's own PUTLLC), when a taskset pointer is known. Tests whether the
missing bootstrap step is the coherent readyCount bump that `CreateTask2` should have done —
confirms/refutes by whether the kernel then schedules the wid and runs gs_task.

`YZ_FS_TRACE` (libs/filesystem/cellFs.c, the read/Lseek/Fstat path, 2026-07-03): logs EVERY
`cellFsRead`/`Lseek`/`Fstat` call (path, offset, resulting position/size), not just the
stream-container heuristic (`.cvm`/`.sfd`/`/stream/`/`/movie/` paths) the code already applies
by default — the pxd spurious-completion probe, added because some boundary streams read an
archive fd whose path misses the built-in stream-path filters.

`YZ_MGMT_CAS` (yakuza/shims.cpp, the SPU-mgmt-line `stwcx` path, 2026-07-03): logs every PPU
`stwcx` (CAS) onto the SPURS mgmt line `0x40197C80` with old/new/ok — the wid-0 fork
discriminator: if readyCount commits land here but wid 0 never dispatches, the bug is in the
kernel select's wid-0 path; if no commit ever targets readyCount[0], the signal path bails
before its CAS. The wid-0 wklSignal bit at `+0x70` (`0x40197CF0`) is logged uncapped (rare);
everything else is per-word capped.

`YZ_NO_CONSUMER` (yakuza/import_overrides.cpp, ~line 1802, marked "TEMP: isolate consumer vs
libgcm"): kill switch for the RSX FIFO consumer thread — when set, the async consumer thread
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
