# LESSONS.md — failure-mode rules distilled from this project's own history

**Mandatory session-start reading** (with STATUS.md and SUMMARY.md). Every rule below was paid
for with real lost sessions; the receipts are in `docs/archive/STATUS_ARCHIVE_2026-06.md`.
These are rules for HOW to work on this codebase, model-agnostic — they apply whether the
session runs on Fable, Opus, or anything else.

## Diagnosis discipline

1. **Verify-then-implement (user-set protocol).** Present the bug, its CLASS, and the
   oracle-verification path (which manual/source/log, what the rule says) so the user can
   confirm — implement after confirmation. Batch confirmed fixes into one relift+rebuild
   (builds are the expensive unit). Sweep bug CLASSES, not instances.
2. **Measured vs inferred — label every claim.** Never announce "root cause pinned" until a
   RUN confirms it. Reading code produces hypotheses, not roots. (The SPURS "scheduling gate"
   survived ~15 sessions of code-reading consensus and was wrong; the real bug was found by
   direct probing.)
3. **A stuck flag/bit/counter is a SYMPTOM.** Trace upstream to who should have changed it and
   why they didn't, instead of forcing it. Every force (`YZ_FRC`, `YZ_CLEARRUN`,
   `YZ_FORCE_START`…) was eventually refuted or retired.
4. **Trust the binary, not the source read.** Static reads of lifted C or disassembly have
   repeatedly "verified" chains that the binary didn't execute (the 5882fe4 wrong-image bug:
   gs_task was literally running another program's code while every source read said dispatch
   was fine). When behavior contradicts the source read, instrument the running binary.
5. **Convergence ≠ correctness.** Multi-agent agreement has been wrong (the 6-agent "lap
   causes defer" causality; Agent A anchoring on a stale address). Always re-verify agent
   conclusions against PRIMARY evidence (traces, logs, disasm) before acting. When your own
   diagnosis has failed repeatedly, dispatch fresh agents with RAW evidence and NONE of your
   conclusions.
6. **Beware trace-tool artifacts.** Known traps that produced false conclusions:
   sampling-window gaps (the RSX jump log "never sampled" the patch → wrong divergence claim),
   capped logs hiding late events, /OPT:ICF folding (profiler misattribution), service/policy
   LS overlap (disambiguate by image id), thread-spam global ring buffers (misattribution),
   trace windows that exclude the code that writes the register you're diffing.
6b. **A diagnostic must never perturb the system it measures — and check WHICH probes are
   live before trusting a pass/fail loop.** 2026-07-02: an always-on watchdog thread-dump
   (serial suspends + 60 s stack walks) froze the guest from +30 s and silently turned FOUR
   8-boot validation loops into garbage verdicts (a design was "refuted 0/8" that was never
   actually measured); after fixing that, even the brief t1-suspend dumps flipped a ~3/4
   baseline to 0/3. Rules: invasive dumps stay env-gated OFF; when a validation loop's rate
   shifts vs an earlier baseline, FIRST diff the two builds' active instrumentation, not the
   fix under test; re-baseline after ANY runtime rebuild before judging the change.
6c. **The observability floor is itself an instrument — and writers must never block.**
   2026-07-03: the always-on per-syscall stderr prints, piped through PowerShell's
   redirect pump, backed up under volume; every guest thread then serialized on the CRT
   stderr lock (t1 measured blocked 48 s inside one fprintf). A whole "3 Hz vsync
   ecosystem" pathology, a 17-second "shader-build crawl", and likely the parked ~1/5
   "boot stall" were artifacts of the logging path. Rules: launch tools use NATIVE
   (kernel-handle) redirection, never a userspace pipe pump; any print a guest thread
   can reach must be nonblocking or volume-bounded; keep a QUIET-baseline boot as a
   standing control; when a rate seems impossibly slow, FIRST compare against a
   quiet run. Corollary: page-guard write-watches (YZ_WATCH_EA) guard the whole 4 KB
   page — never aim one at an address sharing a page with hot lock lines (the SPURS
   mgmt page): it single-steps every atomic and wrecks the run.
7. **Check the oracle FIRST, and check the right layer.** RPCS3 source + a real BLUS30826 run
   log + an instrumentable RPCS3 build are all in the map (STATUS.md). "RPCS3 HLEs/stubs X"
   claims are usually FALSE — check the `Loaded module: X.sprx` log lines; RPCS3 LLE-loads
   libsre/libgcm_sys/libfs/libsysmodule for this game. The `.rrc` capture is a FLATTENED
   method stream: valid LAYER-2 oracle, WRONG for LAYER-1 pacing questions.
8. **Search the archive before proposing.** `docs/archive/STATUS_ARCHIVE_2026-06.md` holds
   every tested-negative ("don't retry") ledger. Re-proposing a refuted fix costs a session.
9. **On compaction-resume, reconcile first.** A compaction summary can be stale: diff it
   against STATUS.md + `git log` before doing anything, or you'll redo committed work.

## Code-correctness classes (sweep these, don't spot-fix)

10. **Endianness.** Guest is big-endian. EVERY HLE write of a multi-byte out-param/struct into
    guest memory must be guest-BE; guest structs need PS3-ABI packing (CellFsStat pack(4)
    class). Proven repeatedly: cellSysutil, cellVideoOut, cellFs, cellAudio.
11. **SPU quadword semantics.** Preferred-slot/word-order and byte-position bugs (shufb,
    cbd/chd/cwd/cdd, fsmb/gbb, quadword shifts, RRR operand order, link-register
    preferred-word, rchcnt preferred-slot) are THE recurring SPU lift class. Verify against
    CBEA + the trace-diff harness (`docs/TRACEDIFF.md`), not by eyeball.
11b. **Immediate-field decode: never sign-extend twice.** Extract-and-extend ONCE at field
    decode; an operand formatter re-extending an already-negative Python int silently maps
    imm → imm−2^n (Python `-4 & 0x8000` is truthy). This was the `il` bug that held the
    geometry wall for ~12 sessions (2026-07-01): every negative `il` in every SPU module
    lifted as imm−0x10000 — positives fine, so the boot mostly worked. When ONE constant in
    a trace disagrees with the raw encoding, decode the raw word by hand before trusting
    either tool (the disassembler and the lifter shared the bug, so they agreed with each
    other and both lied).
12. **Symbolization lies.** Crash/watchdog `func_…` names are wrong for runtime/HLE faults
    (no PDB; helpers not in the func table) — resolve RVAs against `yakuza_recomp.map`.
    PPU back-chain walkers can mis-symbolize string data as functions.
13. **Band-aid hygiene.** Any force/heuristic must be env-gated, default-OFF unless
    load-bearing, have a kill-switch, and carry a retirement condition — and be registered in
    `docs/FLAGS.md`. The stated goal: the default boot reaches its frontier with ZERO lossy
    forces.

## Strategy rules

14. **Accuracy-first / LLE for control flow.** Run original PS3 code (game + firmware)
    wherever possible; HLE only the OS boundary. REFINEMENT (2026-07-01): pure data
    transforms (ADX/codec decode) MAY be HLE'd on host when the LLE path stalls — output
    equality is checkable and codecs can't cause scheduler-class deadlocks. Control/scheduling
    code (SPURS, libgcm) stays LLE, always.
15. **Clean-room constraints.** NEVER use the Sony SDK (NDA). RPCS3 is GPLv2: read to verify
    semantics, never copy code into this MIT repo. gnome41's game repo has no license: read
    design only. Lifted firmware stays gitignored.
16. **No AI attribution anywhere** — commits, PRs, docs. Rewrite if one slips in.
17. **Challenge the plan at session start.** Re-derive the current step against what's now
    known; propose cheaper/more accurate paths with evidence (this converted "reimplement
    SPURS" into "run Sony's SPURS", and "serial rendering plan" into the parallel
    capture-replay track). Don't churn working layers without evidence.
18. **Keep the handoff docs current as part of every change** (STATUS.md, SUMMARY.md,
    checklist PDF, FLAGS.md when flags change, CHEATSHEET.md when workflows change). A stale
    handoff is a bug. STATUS.md has a ~60 KB cap — displace old handoffs into the archive.

## Environment traps (verified)

19. `python` is the Store stub — use `py -3`. PS 5.1: no `&&`/`||`, avoid `2>&1` on native
    exes. Build env: vcvars64 cmd env-import (Launch-VsDevShell is blocked in the harness).
    Shell state doesn't persist between tool calls — chain dev-shell + build in one command.
20. `git push ex` uses Windows Credential Manager; NEVER `gh auth setup-git` (read-only PAT →
    403). Fetch+diff `origin` (upstream) before non-trivial toolkit work so you don't redo
    what upstream already fixed.
