# SPURS Taskset / gs_task dispatch — reference (Yakuza port)

Durable reference so we don't re-derive this. Sourced from RPCS3 source (the readable
twin of the libsre SPU code we run), our lifted `recomp_prx/`, and measured build cycles.
Tags: **[V]** = verified (measured this build, or read directly from source); **[I]** =
inferred (consistent, not yet directly measured).

Last updated 2026-06-19 (pt20).

---

## TL;DR — current state

- **[V] The SPU/SPURS side is healthy.** gs_task launches (LS entry `0x3050`), runs its
  main event loop, and polls its job queue correctly. The long-standing "gs_task halt" wall
  is **fixed** (see Fix below).
- **[V] gs_task is STARVED, not broken.** Its geometry job queue (EA `0x40197180`) is empty;
  the PPU render thread (t1) writes nothing (0 writes to both the job queue and the
  `io 0x1104D00` display list).
- **[V] The frame gate is now the PPU/RSX flip interlock**, not gs_task. t1 is stuck in an
  event-wait loop (sc 82/90/94), flip fence `@0x40C00000 = 2` (frozen), RSX parked on the
  stale `0xA2000500`. Circular: t1 waits for a flip → flip needs the finalised display list
  → the list needs the producer → the producer is t1.
- **[I] gs_task is the 3D-geometry path (menu/gameplay), needed later — likely NOT the
  pre-menu movie's render gate** (the movie is a 2D/video flip path). Confirm against RPCS3.

---

## The halt fix (pt19) — KEEP THIS

**Symptom:** gs_task launched, ran ~21 funcs, then `bi 0` → `[SPU] tid=0x2004 stopped
status=0x4 pc=0x00000`, every run.

**Root cause [V]:** at gs_task LS `0xB6C0` it does a vtable dispatch through the SPURS
taskset context: reads the `0x2FB0` qword, takes word2 = `tasksetMgmtAddr` (LS `0x2FB8`),
then `call *(tasksetMgmtAddr + 0xC4)` = `LS[0x27C4]` = `syscallAddr`. `tasksetMgmtAddr` was
**0** in our run → `base=0` → `*(0xC4)=0` → `bi 0` → halt. The halt dump's `gpr2=0xC4`
(= `0 + 0xC4`) is the giveaway.

RPCS3 sets `ctxt->tasksetMgmtAddr = 0x2700` in `spursTasksetDispatch`'s START path
(`Emu/Cell/Modules/cellSpursSpu.cpp:1787`). Our bandaid launch (coret-skip / helpret /
`0x1CC0` ABI re-establish) skips the policy's START-path context-setup block
(`recomp_prx/policy_module.c` ~`0x1B5C`/`0x1B6C` builds `0x2700`), leaving `0x2FB8 = 0`.

**Fix in tree:** `runtime/spu/spu_channels.c`, at the `0x1CC0` launch — write
`tasksetMgmtAddr = 0x2700` to LS `0x2FB8`. ON by default, off-switch `YZ_NO_MGMT`.
**Validated:** `tasksetMgmtAddr@2FB8 = 0x2700`, zero halts, gs_task dispatches to its
syscall handler (`[spu-ib] -> 0x00A70 (SERVICE)`).

> Principled follow-up (not yet done): make the real `0x838` kernel context save/restore run
> so the policy's own context-setup block executes (writing `0x2700` natively), removing the
> coret/helpret/launch bandaids. The `0x2FB8` write proves exactly what that block must produce.

---

## SpursTasksetContext — LS layout (base `0x2700`)

From `Emu/Cell/Modules/cellSpurs.h:1237` (`struct SpursTasksetContext`, size `0x900`):

| LS addr | field | notes |
|---|---|---|
| `0x2700` | `tempAreaTaskset[0x80]` | GETLLAR target for the taskset bitsets (from main mem) |
| `0x2780` | `tempAreaTaskInfo[0x30]` | the selected task's `TaskInfo` (DMA'd here) |
| `0x27B8` | `taskset` (bptr) | EA of the CellSpursTaskset in main memory |
| `0x27C0` | `kernelMgmtAddr` | |
| `0x27C4` | `syscallAddr` | **gs_task calls this** (`= tasksetMgmtAddr+0xC4`); our run = `0x0A70` |
| `0x27CC` | `spuNum` | |
| `0x27D0` | `dmaTagId` | |
| `0x27D4` | `taskId` | |
| `0x2C80` | `savedContextLr` (v128) | task entry / resume LR (word3); our run = `0x3050` ✓ |
| `0x2C90` | `savedContextSp` | |
| `0x2CA0` | `savedContextR80ToR127[48]` | the SPU register context save area |
| `0x2FA0` | `savedContextFpscr` | |
| `0x2FB0` | `savedWriteTagGroupQueryMask` | |
| `0x2FB4` | `savedSpuWriteEventMask` | |
| **`0x2FB8`** | **`tasksetMgmtAddr`** | **= `0x2700`; the field whose absence caused the halt** |
| `0x2FBC` | `guidAddr` | |
| `0x2FC0` | `x2FC0` | |
| `0x2FD0` | `taskExitCode` | |
| `0x2FD4` | `x2FD4` | `= elfAddr & 5` |

## CellSpursTaskset::TaskInfo — 48 bytes (`cellSpurs.h:1040`)

| off | field | |
|---|---|---|
| `0x00` | `args` (CellSpursTaskArgument, 16B) | task-specific; **not** the elf — differs per task |
| `0x10` | `elf` (bcptr u64) | **task ELF EA**; low 3 bits are flags (`&2` = the escape, `&5` → x2FD4) |
| `0x18` | `context_save_storage_and_alloc_ls_blocks` | `&-0x80` = ctx-save EA, `&0x7F` = alloc LS blocks |
| `0x20` | `ls_pattern` (16B) | which LS blocks are saved/restored on resume |

## CellSpursTaskset — main-memory struct (`cellSpurs.h:1038`)

Atomic task bitsets (4×u32 = 128-bit, one bit per task): `running@0x00`, `ready@0x10`,
`pending_ready@0x20`, `enabled@0x30`, `signalled@0x40`, `waiting@0x50`. `wkl_flag_wait_task@0x72`
(0x80=none, 0x81=flag pending, 0..127=waiter). `last_scheduled_task@0x73`. `wid@0x74`.
`task_info[128]@0x80`. In our run the taskset is at EA ~`0x42100000` (task_info[] from `0x42100080`).

---

## The dispatch flow (`spursTasksetDispatch`, cellSpursSpu.cpp:1740)

1. `SELECT_TASK` → `taskId`, `isWaiting`. Scans `(signalled|ready|pending_ready) & ~running`;
   returns `CELL_SPURS_MAX_TASK` (128, "nothing ready") → `spursTasksetExit` (PM exits to kernel).
2. `memcpy(taskInfo@0x2780, &taskset->task_info[taskId], 48)`; `elfAddr = taskInfo->elf`.
3. **START** (`isWaiting==0`): memset task region; `LoadElf(elf)`; set `savedContextLr=entry`,
   `guidAddr`, **`tasksetMgmtAddr=0x2700`**, `x2FC0=0`, `taskExitCode`, `x2FD4=elfAddr&5`;
   `if (elfAddr & 2) g_escape` (RPCS3 recompiler bailout — our lift runs the real SPU escape);
   `spursTasksetStartTask(taskInfo->args)`.
4. **RESUME** (`isWaiting!=0`): LoadElf (unless full-LS saved); restore context from
   `contextSaveStorage` (memcpy LS `0x2C80` ← ctx-save EA, `0x380` bytes) + LS blocks per `ls_pattern`.

RPCS3 reaches BOTH `0x1C40` (StartTask) and `0xB60` (ResumeTask) at different times
(RPCS3 capture, pt18). The launch is `bi savedContextLr` → `spu.pc = savedContextLr.word3`.

### Taskset request codes (`spursTasksetProcessRequest`, cellSpursSpu.cpp:1304)

| code | request | proceeds when |
|---|---|---|
| -1 | POLL_SIGNAL | `signalled & taskIdMask` set (clears it) |
| 0 | DESTROY_TASK | — |
| 1 | YIELD_TASK | moves running→waiting |
| 2 | WAIT_SIGNAL | `signalled & taskIdMask` set; else blocks (→waiting) |
| **3** | **POLL** | `(signalled\|ready\|pending_ready) & ~running` has a non-masked task. **gs_task's loop uses this.** |
| 4 | WAIT_WKL_FLAG | `wkl_flag_wait_task==0x81` |
| 5 | SELECT_TASK | a runnable task exists |
| 6 | RECV_WKL_FLAG | — |

For the kernel to re-select the taskset workload: `spurs->wklReadyCount1[wid] != 0` **or**
`wklSignal1` bit for `wid` set (cellSpursSpu.cpp:330-360). (Our `YZ_FRC` fakes readyCount.)

---

## gs_task (Edge geometry) — pipeline + how it gets a job

- Entry `0x3050`; code `0x3000-0xBC00`; data PH1 `vaddr 0xBC00 filesz 0x30 memsz 0x330D0`
  (huge `.bss` to `0x3ECD0`, includes LS `0x10000`). taskArgs (launch ABI):
  `r3 = 0x40197180 / 0x40176B10 / 0x000067F0 / 0`, `r4.w3 = spurs 0x40197C80`, sp `0x2c30`.
- Main loop: `POLL` (request 3 via the syscall handler at `0xA70`) + a ch27 (MFC_RdAtomicStat)
  GETLLAR of its **job queue at EA `0x40197180`** (= taskArgs[0]; measured `[gst-llar]` pc=`0x7B14`).
- **How a job is fed [V from RPCS3 source, cellSpurs.cpp:4185-4308]:** the PPU must
  (1) write a geometry batch to the ring at the job-queue EA + bump its producer count,
  (2) set `taskset->signalled` bit for the taskId (`_cellSpursSendSignal`), and
  (3) `cellSpursSendWorkloadSignal` + `cellSpursWakeUp` to kick the SPU. **[V] None of this
  happens in our run — t1 is wedged, so the queue stays empty and gs_task polls forever.**

## Key EAs / addresses

| addr | what |
|---|---|
| `0x40197C80` | CellSpurs management struct (SPURS header; wklReadyCount/wklSignal) |
| `0x40197180` | **gs_task geometry job queue** (taskArgs[0]) — empty in our run |
| `0x42100000`+ | CellSpursTaskset (task_info[] from `0x42100080`; task_info[id] DMA'd from `0x42100100`) |
| `0x40C00000` | flip fence — stuck at `2` |
| `io 0x1104D00` (EA `0x41504D00`) | frame-3 display list — stale `0xA2000500`, never finalised |
| gs_task `0x3050` | entry; `0xB6C0` syscall-vtable dispatch; `0x069B0`/`0x7B14` ch27 job poll |
| policy `0x1C40` | StartTask; `0xB60` ResumeTask; `0x838` kernel exit/yield; `0x290` SelectWorkload |

---

## Diagnostics in tree (env-gated; normal boot unaffected)

- `YZ_SPU_PROF=1`: `[pol-dma]` (all policy DMAs), `[stcmp]` (0x1C40/0xB60 reg diff vs RPCS3),
  `[gst-sc]` (gs_task→0xA70 syscall request: r3=3=POLL), `[gst-llar]` (gs_task GETLLAR EA +
  line contents), `[elf-dec]`/`[pol-gate]` (TaskInfo/elf decode), `[verdict]`/`[bi7]`/`[dtrace]`.
- `YZ_WATCH_DLIST=1`: `[ppu-w]` — PPU writes to the job queue (`0x40197180`) and io dlist.
- `YZ_FRC=1`: fakes wklReadyCount so the taskset is scheduled (force-launch; **note:** launches
  gs_task without a real job — cart before horse).
- Run: `$env:YZ_FRC="1"; $env:YZ_SPU_PROF="1"; .\yakuza\build\yakuza_recomp.exe game\EBOOT.elf`.

## Don't re-chase (ruled out with evidence)

- **gs_task's TaskInfo is malformed** — NO. `[elf-dec]`/`[pol-gate]` show elf@2790=`0x0127A580`,
  savedCtxLr@2C80=`0x3050`, `elfAddr&2 = 0` (escape not taken). The TaskInfo is correct.
- **The halt is a gs_task internal bug / a lift bug at `0x14A0`/`0x14CC`** — NO. `0x14A0`=`ila $7,0x10000`
  and `0x14CC`=`bi $7` are correctly lifted; that path is the GUID-trace overlay (skipped via
  bandaid). The halt was purely `tasksetMgmtAddr=0` (now fixed).
- **gs_task is the io 0x1104D00 producer that must be force-launched** — gs_task PRODUCES geometry
  only after the PPU feeds it a job; force-launching it with an empty queue does nothing. The
  producer is the PPU (t1).
- **Forcing the flip counter / FORCE-FLIP** — pt14: unreliable, doesn't fix the derail.

## Oracle pointers

- RPCS3 source in-repo: `Emu/Cell/Modules/cellSpursSpu.cpp` (SPU-side taskset PM — the readable
  twin of `recomp_prx/policy_module.c`), `cellSpurs.cpp` (PPU APIs incl. task_start / SendSignal),
  `cellSpurs.h` (structs). Real run log: `RPCS3.log` (+ `rpcs3-v0.0.41-.../log/RPCS3.log`).
