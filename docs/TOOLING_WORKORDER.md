# TOOLING_WORKORDER.md — s14 build fleet: thirteen tools, built in parallel

_Written 2026-07-05 (session 13; T7 added same day; T8–T13 added same day on the user's
"earliness on everything" call). These are EXACT build specs (model policy: top model specs,
analyst executes). Next session: dispatch **thirteen parallel `analyst` agents**, one per
section T1–T13, each prompted: "Read docs/TOOLING_WORKORDER.md — build EXACTLY section Tn.
Follow the Shared constraints section. Report per the acceptance checklist."_

**Dispatch rules (parent session):**
- All thirteen run in parallel — file sets are disjoint (T1/T2/T4/T5/T8/T9/T11/T13 = new
  files in `tools/`; T3 = `yakuza/shims.cpp` + `docs/FLAGS.md`; T6 = `tools/ppu_diverge.ps1`
  + a `docs/TRACEDIFF.md` section; T7 = `tools/test_ppu_lift.py` only; T10 = new
  `tests/sync_stress/` + its OWN build dir; T12 = `libs/audio/cellAudio.c` + FLAGS.md +
  `tools/pcm_diff.py`; T9's oracle harness lives in the rpcs3clone tree OUTSIDE this repo).
- **Game boots are SERIALIZED and parent-controlled: only T3 may launch the game.** T12
  builds+compiles but its acceptance boot is RUN BY THE PARENT after T3's review/merge
  (never two game instances at once). Everything else is offline or uses existing files.
- **T3 and T12 get a parent review before merge** (they touch the boot/runtime path). The
  others are standalone tools — parent spot-checks acceptance output.
- After the fleet lands: run each acceptance command once yourself (LESSONS: verify agent
  claims), flip ⏳→✅ in CHEATSHEET.md's toolkit table, commit. If parallel-agent load is a
  concern, dispatch as two waves: wave 1 = T1–T7 (frontier-critical), wave 2 = T8–T13
  (earliness sweeps) — but a single 13-wide dispatch is fine, they don't contend.

## Coverage map — every bug surface, its net, and the deliberate residuals (s13)

The principle: the **execution layer (trace-diff vs RPCS3) is layer-complete for anything
that executes on a traced path** — a bug in ANY lower layer (decode, semantics, structure,
HLE, sync) shows up as a divergence there. Static sweeps are therefore prioritized by
HISTORICAL BITE RATE, not completeness for its own sake; the rest carry a pre-committed
trigger for when to build them. Don't silently treat a RESIDUAL as covered.

| # | Surface | Net | Status |
|---|---|---|---|
| 1 | PPU decode (word→mnemonic) | T4 cross-check vs Capstone | QUEUED |
| 2 | PPU semantics (mnemonic→C) | conformance suite (gate) + T7 fuzz sweep | BUILT + QUEUED |
| 3 | PPU lift STRUCTURE (function map, jump tables, fall-throughs, TOC) — blockers #6/#8/#14/#19b were THIS class | **T8 lift-structure census** + trace-diff + `scratch/fallthrough_sweep.py` | QUEUED (user call 07-05: earliness on everything) |
| 4 | SPU decode (il/opcode-table classes) | **T9 SPU decode cross-check** (rpcs3clone SPUDisAsm as output-oracle) + SPU trace-diff | QUEUED (SPU *semantics* fuzz stays with docs/SPU_INTERP_PLAN.md) |
| 5 | HLE lib/syscall semantics (endianness, packing, ABI, rc) — blockers #11/#16/#18/#19d | **T13 endian/out-param heuristic auditor (INFO-level)** + T2 eventdiff + trace-diff | QUEUED (heuristic — expect noise; whitelist-driven) |
| 6 | lv2 sync primitives (lost wakeups, races) | **T10 deterministic sync stress tests** + T1 waitgraph verdicts | QUEUED |
| 7 | Memory/DMA/lock-line atomics | execution symptoms + SPU-interp lockstep harness (docs/SPU_INTERP_PLAN.md, planned) | RESIDUAL per that plan (a real project, not a fleet item) |
| 8 | RSX visual correctness | **T11 frame image-diff (v1 self-golden on the .rrc replay)** + golden boot | QUEUED (v2 = cross-emulator diff vs RPCS3 replay frames) |
| 9 | Codec output correctness | **T12 PCM dump flag + diff tool** (mechanism now; real reference diff activates at first audio, M1) | QUEUED (LESSONS #14 sanctions codec HLE *because* this check exists) |
| 10 | Process/meta (handoffs, flags, evidence) | T5 linter + LESSONS #21/22 + DONT_RECHASE | QUEUED |
| 11 | Regression | golden_boot + conformance gate + cycle.ps1 | BUILT |

**Remaining backlog (not earliness checks — build on their own triggers):** the SPU
interpreter lockstep harness (#7, per its own plan doc); logging ring-buffer + writer thread
(observability infra — build when log volume next distorts a measurement, LESSONS 6c).

## Shared constraints (every agent reads this)

- Python: **stdlib-only**, snake_case, LF endings, 4-space indent (CONTRIBUTING.md). Run via
  `py -3` (plain `python` is broken). EXCEPTION: T4 may use `capstone` as an OPTIONAL
  dev-only dependency (see T4).
- PowerShell 5.1: no `&&`/`||`; shell state does not persist between tool calls.
- Never touch `recomp/`, `recomp_prx/`, `yakuza/build` internals (T3 rebuilds via
  `cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul && ninja -C yakuza\build yakuza_recomp"`).
- Evidence discipline (docs/LESSONS.md #21): every probe prints an arm-time liveness banner;
  cite only file paths that exist; label MEASURED vs INFERRED in your report.
- Do NOT edit CHEATSHEET.md / STATUS.md (the parent handles registries — avoids conflicts).

### Input formats (verbatim, verified 2026-07-05 — parse THESE, and where told, re-derive from source)

Our boot **stderr** (`scratch/*.err`):
```
[LV2 t11] sc 107 (r3=0x4 r4=0x0 r5=0xD00FFA20 r6=0xFFFFFFFFD00FFA20) -> 0x0
[LV2:first t1] sc 352 (r3=0xD0100D04 r4=0x11D3D60 r5=0xFDF0044 r6=0x2A0) -> 0x0
[t1-spin] #1 lr=0x00000000 r4=0x0 r5=0x016557E8 r30=0x016557E8 r31=0x01661470
[live-draw] frame 30 presented: draws=28 clears=59 (cumulative)
```
Our boot **stdout** (`scratch/*.out`):
```
[cellFs] t11 Open(path='/dev_bdvd/PS3_GAME/USRDIR/data/sound/stream/adv_voice_talk/adv_voice_talk.cvm', flags=0x0)
[cellFs] Open: fd=3 -> '.\gamedata\dev_bdvd\PS3_GAME\USRDIR\...\adv_voice_talk.cvm'
[cellFs] Fstat(fd=3 '/dev_bdvd/...') -> st_size=0xC175000
[cellFs] Lseek(fd=3 '/dev_bdvd/...') off=0x800 whence=0 -> pos=0x800
[cellFs] t11 Read(fd=3 '/dev_bdvd/...') off=0x0 nbytes=0x800 -> 0x800  hdr=43564D48 00000000 (CVMH)
[cellAudio] PortOpen(nChannel=8, nBlock=8)
```
(NOTE: a short `Fstat(fd=3)` form without the path exists when YZ_FS_TRACE is off; Open
lines are always on. Reads/Lseeks with paths need `YZ_FS_TRACE=1`.)

**RPCS3.log** (repo root; `·` is a literal byte in the log):
```
·W 0:30:47.641758 {PPU[0x100000c] Thread (cri_dlg) [libfs: 0x01a02090]} sys_fs: sys_fs_open(path="/dev_bdvd/PS3_GAME/USRDIR/data/sound/stream/adv_voice_talk/adv_voice_talk.cvm", flags=00, fd=*0xd009cce0, mode=00, arg=*0x0, size=0x0)
·W 0:30:47.652196 {PPU[0x100000c] Thread (cri_dlg) [libfs: 0x01a02090]} sys_fs: sys_fs_open(): fd=3, Regular file, '...', Mode: 0x0, Flags: 0x0, Pos/Size: 0/193.457MB (0x0/0xc175000)
·W 0:30:47.737314 {PPU[0x100000c] Thread (cri_dlg) [libfs: 0x01a01210]} sys_fs: sys_fs_close(fd=4): Regular file, '...', Pos/Size: 343.25KB/343.25KB (0x55d00/0x55d00)
·S 0:30:46.306453 {PPU[0x1000000] Thread (main_thread) [liblv2: 0x019614d0]} sys_prx: Loaded module: "/dev_flash/sys/external/libsre.sprx" (id=0x23001100)
```

**Syscall numbers** (working map — 92=sema_wait 94=sema_post 102=mutex_lock 103=mutex_trylock
104=mutex_unlock 107=cond_wait 108=cond_signal 109=cond_signal_all 130=event_queue_receive):
DO NOT trust this list blindly — **derive the authoritative number→name map from the runtime's
own lv2 dispatcher** (grep `runtime/syscalls/` + `yakuza/` for the `sc` dispatch switch /
registration table) and embed the derived map in your tool with a comment naming the source
file:line.

---

## T1 — `tools/waitgraph.py` (stall-shape analyzer)

**Purpose:** one command that turns a boot `.err` into the wait-graph picture that s11–s13
assembled by hand: who waits on what, who signals what, who is starved.

**CLI:** `py -3 tools\waitgraph.py <file.err> [file2.err ...] [--tail N] [--min-count N]`

**Output (plain text, three sections):**
1. **Per-thread:** for each tid seen in `[LV2 tN]` lines: total sync-class syscalls, the last
   `--tail` (default 8) raw sync lines, and a terminal classification:
   `CYCLING <pattern>` (detect a repeating tail window of 2–6 calls, e.g.
   `103/107/104 on mutex+cond id 4`) or `LAST-CALL <name>(<id>) rc=<rc>` when no cycle.
2. **Per-object:** key = (class, id) where class ∈ {mutex, cond, sema, equeue} and id = r3.
   Columns: waiter tids + wait counts, signaler tids + signal counts, wait return-code
   histogram (0x0 vs 0x8001xxxx), and a VERDICT column:
   - `SIGNALED-NEVER-WOKEN` — signals arrive but a waiter's waits never return (possible
     lost wakeup / wrong object)
   - `WOKEN-STARVED` — waits return 0x0 repeatedly and the thread re-waits (predicate false;
     look upstream at the producer)
   - `NO-SIGNALER` — waiters exist, zero signal/post calls on the object
   - `HEALTHY` — otherwise
3. **Suspects summary:** the top objects/threads by the above verdicts, one line each.

**Acceptance (run + paste output):**
- `py -3 tools\waitgraph.py scratch\t1spin.err` must show: t11 CYCLING on obj 4
  (trylock/cond_wait/unlock, cond_wait rc=0x0 → WOKEN-STARVED) and t1 as a signaler of cond 4
  (~172 signals). Must run clean on `scratch\_base1.err` and on both files together.

## T2 — `tools/eventdiff.py` (OS-boundary event differ vs RPCS3)

**Purpose:** generalize the diff that pinned the frontier (RPCS3 opens `all_csb.par`, we
don't): align our boot's file-event stream against RPCS3.log's per-thread and report the
first divergence.

**CLI:** `py -3 tools\eventdiff.py --ours scratch\x.out --ref RPCS3.log --map t11=cri_dlg[,t1=main_thread,...] [--events open,close] [--context 3]`

**v1 scope — file events only** (open/close; reads optional stretch). Normalize both sides to
per-thread sequences of `(op, normalized_path)` where normalized_path strips the mount prefix
difference and lowercases. Ours: `[cellFs] tN Open(path='...')` (+ Close if a close log
exists — check `libs/filesystem/cellFs.c` for the close log format; if absent, opens only).
Ref: `sys_fs_open(path="...")` / `sys_fs_close(fd=N)` — resolve close paths via the fd from
the preceding open on the same thread. Walk the two sequences in order; report the first
index where they differ, with `--context` events on each side, plus each side's NEXT few
events after the divergence ("ref continues with: ...").

**Acceptance:** `py -3 tools\eventdiff.py --ours scratch\_s13bracket.out --ref RPCS3.log --map t11=cri_dlg`
must report: sequences agree through `.../scenario/scenario.bin` open, then OURS ends while
REF continues with `player_pos.bin → boot.par → all_csb.par ...`. (Expect ref-only extras
like the arrow `.dds` — if the .dds breaks alignment, note it and add a `--ignore substr`
option.) Paste the verbatim output.

## T3 — runtime env write-watch `YZ_WATCH_WR` (no-rebuild write-watches) ⚠ boot-path change

**Purpose:** the compile-time `yz_watch_bd tg[]` means every "who writes this EA" question
costs a code edit + rebuild. Make it env-driven. This unblocks ⚡ NEXT ACTION step 1b (who
arms cri_dlg's work flag 0x01661474).

**Spec:** FIRST read the existing `yz_watch_bd` + `YZ_PEEK` machinery in `yakuza/shims.cpp`
(and wherever the page-guard/veh handler lives) — REUSE that mechanism, do not build a second
one. Add: `YZ_WATCH_WR=hexEA[,hexEA...]` (≤16 EAs, `0x` optional). Behavior:
- Env unset ⇒ ZERO new code runs (guard at init; no perturbation — LESSONS 6b).
- Arm-time liveness banner per EA: `[watch-wr] armed ea=0x01661474 page=0x01661000`
  (LESSONS #21). If a watched page is known-hot (SPURS mgmt page 0x40197xxx class), print a
  loud warning but proceed (LESSONS 6c: page guards single-step every access on that page).
- On a write landing in `[ea, ea+4)`: log
  `[watch-wr] tid=N ea=0x... old=0x... new=0x... bt: 0xPC1<-0xPC2<-0xPC3...` using the
  existing guest back-chain walker (the watchdog has one). Same-page writes outside watched
  dwords: count silently, report the count once per 4096 hits.
- Register the flag in `docs/FLAGS.md` (diag, default-off, retirement: superseded-by-nothing,
  it's permanent kit).

**Acceptance:** (1) build compiles; (2) a boot WITHOUT the flag reaches the CRI stall normally
(compare `[cellFs] ... scenario.bin` open appearing, ~10–15 min on the trace build — use
`tools/boot_until.ps1 -Pattern 'scenario\.bin'`); (3) a boot WITH
`YZ_WATCH_WR=0x01661474` (+ baseline flags `YZ_AUDIO_FORCE=1 YZ_VMGUARD=1 YZ_VMGUARD_SURVIVE=1
YZ_FS_TRACE=1`) shows the armed banner and captures the work-flag arm/clear writes
(expect ~51 arm events; report the writer tid + a sample backtrace — THIS RESULT FEEDS
NEXT ACTION 1b, paste it in full). **Do not commit; leave the diff for parent review.**

## T4 — `tools/disasm_audit.py` (decoder cross-check vs Capstone)

**Purpose:** catch the crnand/crnor bug class (decode-table swaps/scrambles — the most
expensive bug class in project history) exhaustively, in one run.

**Spec:** decode every 4-byte word of `game/EBOOT.elf`'s executable segments (reuse
`tools/elf_parser.py`) with (a) our `tools/ppu_disasm.py` and (b) Capstone in PPC64
big-endian mode (`pip: capstone` — OPTIONAL dev-only dependency; if `import capstone` fails,
exit with a one-line install hint; NEVER add it to runtime code). Compare **mnemonics only**
in v1 (operand normalization is v2). Handle known alias classes via a WHITELIST table in the
script (e.g., capstone simplified mnemonics `mr/li/nop/blr/bctr` ↔ our canonical `or/addi/
ori/bclr/bcctr` forms; extend as found). Output: mismatch report grouped by
`(ours_mn, capstone_mn)` pair, descending count, 3 sample addresses each; whitelisted pairs
listed separately as INFO. Also `--spu <elf>` stretch goal ONLY if a reference SPU decoder is
trivially available — otherwise skip, PPU is the payload.

**Acceptance:** run on `game/EBOOT.elf`; paste the top-20 mismatch pairs. Sanity: common ops
(`add`,`lwz`,`bl`,`stw`) must show ZERO non-whitelisted mismatches; a seeded self-test —
temporarily swapping two mnemonics in a COPY of the decode call (not the real file) — must
surface the pair at the top. State how many total words decoded and the residual
(unexplained) pair count.

## T5 — `tools/lint_handoff.py` (handoff/config linter)

**Purpose:** mechanically enforce the s13 audit rules so broken handoffs are caught the day
they're written (this is the Opus-proofing enforcement arm).

**Checks (exit nonzero if any FAIL):**
1. Every path-like citation in `STATUS.md` (regex over `scratch/…`, `tools/…`, `docs/…`,
   `yakuza/…`, `libs/…`, `runtime/…` tokens ending in a file extension) EXISTS on disk —
   except citations annotated `(GONE` (the established marker for known-lost evidence).
2. `STATUS.md` ≤ 62,000 bytes and contains a line starting `## ⚡ NEXT ACTION`.
3. FLAGS sync: collect `getenv("YZ_…")` names from `runtime/`, `libs/`, `yakuza/*.cpp|.c|.h`;
   collect `YZ_…` tokens from `docs/FLAGS.md`; FAIL listing any flag in code but not in
   FLAGS.md (flags in FLAGS.md but not in code = INFO "retired?", not FAIL).
4. `docs/DONT_RECHASE.md` exists and its table rows parse (| count ≥ 4 per row) — INFO-level.
**CLI:** `py -3 tools\lint_handoff.py [--verbose]` — summary table: check | PASS/FAIL | detail.

**Acceptance:** run it; it must PASS checks 1/2/4 on the current tree (s13 tagged all known-
lost citations with `(GONE`). Check 3 will likely list untracked flags — paste the list
verbatim (that's the tool working, not a build failure); if >0, ALSO append the missing
flags to `docs/FLAGS.md` under a "backfilled by lint (s14)" heading with a one-line
description each derived from the code context.

## T6 — `tools/ppu_diverge.ps1` (one-command PPU trace-diff driver)

**Purpose:** the PPU trace-diff (docs/TRACEDIFF.md §PPU) works but needs manual two-sided
setup. Wrap our side + the diff + divergence context into one command (the SPU side already
has `tools/diverge.ps1` — read it for house style).

**CLI:**
`.\tools\ppu_diverge.ps1 -Tid 11 -Arm 0xF00E80 [-N 2000000] [-Ref scratch\rpcs3_ppu_trace3.txt] [-AlignPc F00E80] [-PcRange 10000:1310768] [-Flags 'YZ_AUDIO_FORCE=1;YZ_VMGUARD=1;YZ_VMGUARD_SURVIVE=1'] [-SkipBoot]`
- Without `-SkipBoot`: set `YZ_PPU_TRACE=1 YZ_PPU_TRACE_TID=<Tid> YZ_PPU_TRACE_ARM=<Arm>
  YZ_PPU_TRACE_N=<N>` + `-Flags`, launch the game with NATIVE redirection, watch
  `scratch\ppu_trace.txt` until size is stable for 60 s (or 20-min timeout), kill the game.
- With `-SkipBoot`: use the existing `scratch\ppu_trace.txt`.
- Run `py -3 tools\tracediff.py scratch\ppu_trace.txt <Ref> --align-pc <AlignPc> --pc-only
  --pc-range <PcRange>`; print its output. On divergence: extract the divergent PC and run
  `py -3 tools\show_func.py <pc> --both` (truncate to ±30 lines around the PC) so the report
  lands with disassembly context. NOTE tracediff parses --pc-range as HEX.
- The RPCS3-side capture stays manual for v1: print a pointer to docs/TRACEDIFF.md §PPU's
  capture steps when `-Ref` is missing. Add a short "driver" subsection to docs/TRACEDIFF.md
  §PPU documenting this script.

**Acceptance:** `.\tools\ppu_diverge.ps1 -Tid 11 -Arm 0xF00E80 -SkipBoot -Ref scratch\rpcs3_ppu_trace3.txt`
must reproduce the known result (`No divergence ... Traces AGREE`, ours 11110 vs ref 263870
events) in one command. Paste the output.

## T7 — `tools/test_ppu_lift.py --fuzz` (semantic fuzzer mode for the conformance suite)

**Purpose:** the suite's ~905 hand vectors only catch what someone thought to test — the
crnand/crnor swap survived FIVE sessions with the suite GREEN because CR-field ops had zero
coverage. Add a generated sweep so the semantics layer has breadth, not just spot checks.
The suite ALREADY has instruction encoders (`xo_form`, `d_form`, `m_form`, …) and reference
semantics functions (`ref_add`, `ref_addc`, `ref_cntlzw`, … — verified 2026-07-05) — this is
a generation loop on existing infrastructure, NOT a new harness. **Extend the existing file
in place; do not fork it. The default (no-flag) run must remain byte-identical in behavior
and stay the relift gate.**

**CLI:** `py -3 tools\test_ppu_lift.py --fuzz N [--seed S]` — N generated cases per covered
family (suggest default 200), deterministic from `--seed` (default 0; PRINT the seed in the
output so failures are reproducible).

**Spec:**
- Operand generation: draw register inputs from an edge-biased pool — {0, 1, -1 (all-ones),
  0x7FFFFFFF, 0x80000000, 0x7FFF…F, 0x8000…0, values with low word set / high clear and
  vice-versa, sign-boundary halfword/byte values} plus uniform randoms. Randomize legal
  field values too (sh/mb/me for rotates, bf/l for compares, oe/rc variants) — Rc=1 CR0
  side effects must be part of the sweep wherever the emitted C computes them.
- Coverage: every instruction family that has a `ref_*` function gets fuzzed. THEN extend
  refs to the families that have historically bitten and are cheap to model in Python:
  **CR-field logic (crand/crnand/cror/crnor/crxor/creqv/crandc/crorc — the crnand class,
  mandatory)**, carry/overflow families, 32/64-bit rotates+shifts, sign-extension forms
  (extsb/extsh/extsw + D-form immediates — the il class analog). VMX: integer ops only if
  refs are straightforward; SKIP VMX float in v1 (rounding-mode refs are finicky — note it
  as future work rather than shipping a wrong oracle).
- Failure output: on mismatch print the ENCODED WORD (hex), the decoded mnemonic+operands,
  inputs, expected vs got — enough to file a verify-then-implement report without rerunning.

**Acceptance (all three, paste output):**
1. **Seeded-bug self-test, decode class:** in a TEMP COPY of the decoder (never the real
   file; e.g. copy to the session scratchpad and point the suite at it via sys.path or an
   env override), re-introduce the crnor/crnand swap → `--fuzz 200` must FAIL loudly on
   that family.
2. **Seeded-bug self-test, semantics class:** in a temp copy, introduce an il-style
   double-sign-extension (make a D-form immediate sign-extend twice) → must FAIL.
3. On the REAL tree: `py -3 tools\test_ppu_lift.py --fuzz 200 --seed 0` — GREEN, in ≤ ~5 min.
   If it finds REAL mislifts, that is a SUCCESS, not a build failure: report them
   (word/mnemonic/expected/got, MEASURED) per the verify-then-implement protocol and do NOT
   fix them yourself.

## T8 — `tools/lift_audit.py` (PPU lift-structure census + relift regression diff)

**Purpose:** the structure bug class (map gaps, stubbed jump-table dispatchers, fall-throughs,
unresolved indirect targets — boot blockers #6/#8/#14/#19b) has no static sweep. Build a
census that runs after every relift and diffs against a baseline, so a NEW structure anomaly
is flagged the day the relift creates it.

**Spec:** operate on `recomp/ppu_recomp.c` (NOTE: currently the --trace relift — markers are
unchanged), `functions.json`, and `game/EBOOT.elf` (+`tools/elf_parser.py`/`ppu_disasm.py`):
1. Marker census: count + group `/* TODO:` emissions by mnemonic and `unresolved` /
   catch-all markers by reason (discover the exact marker strings by grepping the lifter's
   emit sites in `tools/ppu_lifter.py` first — cite the line numbers in a comment).
2. Fall-through census: absorb the logic of `scratch/fallthrough_sweep.py` (functions whose
   final instruction is not an unconditional transfer and whose next address is a map gap).
3. Target coverage: decode all executable words; census `bl`/unconditional-`b` targets that
   land OUTSIDE any functions.json entry (gap targets) or MID-function (split candidates).
**CLI:** `py -3 tools\lift_audit.py [--baseline scratch\lift_audit_baseline.txt] [--update-baseline]`
— default mode prints the census AND the diff vs baseline (NEW anomalies = FAIL exit code).
**Acceptance:** run it; seed the baseline; paste the census summary (expect nonzero legacy
counts — e.g., ~708 `.word` data-in-text sites are KNOWN); verify the fall-through list is
consistent with `py -3 scratch\fallthrough_sweep.py`; then re-run → PASS (no diff vs fresh
baseline).

## T9 — `tools/spu_disasm_audit.py` (SPU decode cross-check via rpcs3clone oracle)

**Purpose:** the SPU side has the same decode-table bug history (il, RRR operand order) and
no static check. Cross-check `tools/spu_disasm.py` against RPCS3's SPUDisAsm used as an
OUTPUT ORACLE (GPL code stays in ITS tree; we only diff its text output — same clean-room
posture as every RPCS3 oracle use; copy no code).

**Spec:** (1) In `C:\Users\csaka\Downloads\rpcs3clone`, add a tiny standalone dump harness
(new .cpp + a console target, or a hidden CLI flag on an existing tool) that reads a raw SPU
binary and prints `offset: mnemonic operands` per word via SPUDisAsm — keep the harness in
the clone, NEVER in our repo. Build it with the existing VS2026 solution (build engineering
is in your remit; separate config OK). (2) Our side: `tools/spu_disasm_audit.py <binary>`
decodes the same bytes with `tools/spu_disasm.py`, normalizes (v1 mnemonic-only + a
whitelist alias table), diffs, reports grouped `(ours, oracle)` pairs with counts + samples.
(3) Targets: the lifted SPU images in `scratch\spu_imgs\` (gs_task, cri_audio, etc.).
**Acceptance:** run over at least gs_task + cri_audio images; paste top pairs; sanity: core
ops (`il`, `ai`, `lqd`, `brsl`, `shufb`) show zero non-whitelisted mismatches; seeded
self-test: swap two mnemonics in a TEMP COPY of our decoder → the pair surfaces at the top.
If the rpcs3clone harness can't be built inside the session, STOP and report exactly where
it failed (do not fake an oracle).

## T10 — `tests/sync_stress/` (deterministic lv2 sync stress tests)

**Purpose:** every stall investigation ends up re-litigating "is our sys_cond/mutex/sema
broken?" — answered so far by inference or per-incident measurement. Build a standing stress
test so the answer is one command.

**Spec:** new `tests/sync_stress/` (own small CMakeLists + OWN build dir, e.g.
`tests/sync_stress/build` — NEVER touch `yakuza\build`), compiling the runtime sync sources
(`runtime/syscalls/sys_mutex.c`/`sys_cond.c`/`sys_semaphore.c`/`sys_event.c` + whatever
minimal deps they pull; stub what's unneeded). Tests (all seeded-deterministic via argv
seed, default 0; ~10k iterations each; total runtime ≤ 60 s):
1. **No lost wakeup:** N producers signal a cond under mutex while M consumers wait with a
   predicate counter — every produced item is consumed within a bounded wait; a consumer
   stuck >2 s = FAIL with a dump.
2. **Timed-wait semantics:** cond/sema timed waits return the ETIMEDOUT code on expiry, 0 on
   signal; never hang.
3. **Semaphore counting:** post/wait storms preserve the count invariant.
4. **Event queue:** push/receive from many threads preserves FIFO-per-source + wakes exactly
   one receiver per event.
**Acceptance:** 3 consecutive green runs (`.\tests\sync_stress\build\sync_stress.exe 0/1/2`);
seeded-bug self-test: in a TEMP COPY of sys_cond.c, drop one wake path → test 1 must FAIL.
Document the build command at the top of the CMakeLists.

## T11 — `tools/rrc_imagediff.py` (RSX frame image diff — v1 self-golden)

**Purpose:** Track B's visual correctness currently has no automated check — a rendering
regression would be caught by eyeballs. v1 = golden-image regression on the .rrc replay
(absolute-correctness diff vs an RPCS3-rendered frame is v2).

**Spec:** (1) `tools/rrc_imagediff.py a.ppm b.ppm [--tolerance 2] [--max-diff-pct 0.5]` —
per-channel tolerance compare on PPM (the format `YZ_RSX_DUMP` and the replay renderer
already emit), report % differing pixels + bounding box of the largest diff region, FAIL
exit code over threshold. Pure stdlib (PPM is trivial to parse). (2) Driver mode:
`--replay <capture.rrc.gz> --golden goldens\rrc_frame_A.ppm` runs the EXISTING Track B
replay render path (find it: `tools/rrc_replay.py` and the stage-3/4 merge 63a84d7 — read
how the replay produces its frame dump; do NOT build a new renderer), dumps the final
frame, diffs vs the golden. (3) Seed `goldens/` from the current proven replay output and
commit-ready it (small PPM or PNG-converted; if >2 MB, store zipped).
**Acceptance:** self-diff of a frame = 0 differing pixels; a perturbed copy (script-flip a
16×16 block) FAILS with the region reported; golden seeded + the driver mode runs
end-to-end. Paste outputs.

## T12 — PCM output dump + `tools/pcm_diff.py` (codec output-equality mechanism) ⚠ runtime change

**Purpose:** LESSONS #14 permits codec HLE *because* output equality is checkable — but no
checker exists. Build the mechanism NOW so it's standing the day audio first flows (M1);
be honest that it captures nothing until then.

**Spec:** (1) Runtime: `YZ_PCM_DUMP=<path>` in `libs/audio/cellAudio.c` — at the point where
guest-submitted audio enters the port buffer (find the AddData/port-write path; read the
file first), append the raw samples to `<path>` with a small header line
(`rate/channels/format`) and a HARD CAP (default 64 MB, then stop + notice). Zero code runs
when env unset. Liveness banner on arm. Register in FLAGS.md. (2) `tools/pcm_diff.py a.raw
b.raw [--rms-threshold 1e-3] [--allow-offset N]` — v1: exact + RMS compare with optional
small alignment search. (3) Document (in the tool's docstring) the intended M1 workflow:
dump our PCM for a known clip vs a reference decode of the same clip (e.g., vgmstream CLI —
external tool as oracle; do not vendor it).
**Acceptance:** compiles; a synthetic self-test exercises the dump function directly (unit
harness or a debug call) and pcm_diff on the produced file vs itself = identical, vs a
perturbed copy = FAIL. **Do NOT boot the game — the parent runs the live acceptance boot
after T3's merge** (serialized boots), expecting: banner present, empty-or-tiny dump (audio
doesn't flow yet — that's the KNOWN state, not a failure). Leave the runtime diff
uncommitted for parent review.

## T13 — `tools/endian_audit.py` (HLE out-param endianness heuristic — INFO-level)

**Purpose:** the endianness/packing class (blockers #11/#16/#18/#19d) is swept manually,
module-by-module, as code gets exercised. A heuristic static pass won't be sound, but it
ranks suspects — earliness beats completeness here.

**Spec:** scan `libs/**/*.c`: for each exported function whose signature has pointer
out-params (u16*/u32*/u64*/struct*), flag it INFO if the body stores through the pointer
with NO `ps3_bswap*` / `vm_write*` call anywhere in the function. Maintain
`tools/endian_audit_whitelist.txt` (function names + one-line reason) — seed it by
triaging the first run's obvious false positives (byte buffers, host-only structs, already-
audited modules). Output: suspects grouped by module, sorted by module boot-relevance
(hardcode a small priority list: audio, fs, gcm/video, sysutil first). ALWAYS exit 0 (it's
advisory — never a gate).
**Acceptance:** run it; paste the per-module suspect counts + the top-10 suspects; seeded
self-test: in a TEMP COPY of one known-good file, remove a `ps3_bswap` → that function must
appear as a suspect. State the whitelist size you seeded and the rule you used.
