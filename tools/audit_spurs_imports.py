#!/usr/bin/env python3
"""Produce a deterministic SPURS/cellSync import and static call-site audit.

The generated files are intentionally not committed for every title.  This
tool accepts an arbitrary generated title directory so the evidence can be
recreated without teaching the runtime about Yakuza-specific addresses.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

IMPORT_RE = re.compile(
    r"0x(?P<stub>[0-9A-Fa-f]{8})[uU]?[^\r\n]*/\*\s*"
    r"(?P<module>cellSpurs|cellSync)::(?P<name>[A-Za-z0-9_]+)"
)
FUNC_RE = re.compile(r"^\s*void\s+func_(?P<ea>[0-9A-Fa-f]{8})\s*\(")
CALL_RE = re.compile(r"\bfunc_(?P<ea>[0-9A-Fa-f]{8})\s*\(\s*ctx\s*\)")
LR_RE = re.compile(r"ctx->lr\s*=\s*0x(?P<lr>[0-9A-Fa-f]+)")
PROTOTYPE_RE = re.compile(
    r"\b(?:s32|u32|void|size_t)\s+(?P<name>[_A-Za-z][_A-Za-z0-9]*)"
    r"\s*\((?P<args>.*?)\)\s*;",
    re.DOTALL,
)


def imports_from(path: Path) -> dict[str, dict[str, str]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    result: dict[str, dict[str, str]] = {}
    for match in IMPORT_RE.finditer(text):
        stub = match.group("stub").upper()
        result[stub] = {
            "module": match.group("module"),
            "name": match.group("name"),
        }
    return result


def callsites(recomp: Path, imports: dict[str, dict[str, str]]) -> list[dict[str, object]]:
    found: list[dict[str, object]] = []
    for path in sorted(recomp.glob("ppu_recomp_*.cpp")):
        caller = ""
        recent_lr = ""
        with path.open(encoding="utf-8", errors="replace") as source:
            for line_no, line in enumerate(source, 1):
                func = FUNC_RE.match(line)
                if func:
                    caller = func.group("ea").upper()
                    recent_lr = ""
                lr = LR_RE.search(line)
                if lr:
                    recent_lr = lr.group("lr").upper()
                for call in CALL_RE.finditer(line):
                    stub = call.group("ea").upper()
                    if stub in imports:
                        found.append(
                            {
                                "stub": stub,
                                **imports[stub],
                                "caller": caller,
                                "return_ea": recent_lr,
                                "file": path.name,
                                "line": line_no,
                            }
                        )
    return found


def prototypes_from(paths: list[Path]) -> dict[str, str]:
    result: dict[str, str] = {}
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in PROTOTYPE_RE.finditer(text):
            args = " ".join(match.group("args").split())
            result[match.group("name")] = f"{match.group('name')}({args})"
    return result


def contract_class(name: str) -> str:
    if name.startswith("cellSyncMutex"):
        return "SYNC-MUTEX"
    if "Attribute" in name:
        return "ATTRIBUTE"
    if name in {
        "cellSpursInitializeWithAttribute",
        "cellSpursInitializeWithAttribute2",
        "cellSpursFinalize",
    }:
        return "INSTANCE"
    if "EventFlag" in name:
        return "EVENT"
    if "LFQueue" in name:
        return "LFQUEUE"
    if "Queue" in name:
        return "QUEUE"
    if "Barrier" in name:
        return "BARRIER"
    if "JobGuard" in name:
        return "GUARD"
    if "JobChain" in name:
        return "JOB"
    if "Taskset" in name:
        return "TASKSET"
    return "TASK"


def markdown(
    imports: dict[str, dict[str, str]],
    calls: list[dict[str, object]],
    prototypes: dict[str, str],
) -> str:
    by_stub: dict[str, list[dict[str, object]]] = {stub: [] for stub in imports}
    for call in calls:
        by_stub[str(call["stub"])].append(call)

    lines = [
        "# Generated SPURS import audit",
        "",
        "Execution status is deliberately reported as **static-only** here. "
        "The generated call sites prove reachability, not execution in a "
        "particular boot; runtime traces are recorded separately.",
        "",
        "| Stub | Module | Import | SDK ABI/type signature | Contract | Static call sites |",
        "| --- | --- | --- | --- | --- | ---: |",
    ]
    for stub, info in sorted(imports.items()):
        name = info["name"]
        lines.append(
            f"| `0x{stub}` | `{info['module']}` | `{name}` | "
            f"`{prototypes.get(name, 'signature unavailable')}` | "
            f"{contract_class(name)} | "
            f"{len(by_stub[stub])} |"
        )
    lines.extend(
        [
            "",
            "## Contract legend",
            "",
            "| Contract | Guest structures and cross-access | Ordering, result, and native disposition | Evidence |",
            "| --- | --- | --- | --- |",
            "| ATTRIBUTE | SDK attribute object; PPU-owned bytes. | Validate alignment/arguments and publish big-endian fields; a layout wrapper is sufficient. | SDK 4.75 ABI plus Era B lifted call flow. |",
            "| INSTANCE | `CellSpurs`/`CellSpurs2` guest object plus host lifecycle side table. | Publish guest state before host wakeups; real initialize/finalize errors, no firmware address registration. | SDK layout; Era B firmware oracle; Era C lifecycle fixes. |",
            "| TASKSET | Taskset guest header and 128-bit task state masks, also read by task SPU code. | Predicate-based shutdown/join and real task completion; host worker lifecycle in side tables. | SDK taskset ABI; Era A scaffold corrected by Era B/C context evidence. |",
            "| TASK | Task info, LS pattern/context, ready/running/waiting/signalled/exited masks; task SPU syscall area is shared. | Exact image identity, SDK task ABI, signal/wait/yield/poll/exit transitions; unknown image/syscall returns an error. | SDK 4.75 task ABI; Era B firmware lift; Era C resume/image-residency fixes. |",
            "| EVENT | 128-byte event flag, accessed by PPU and SPU DMA. | Atomic guest-bit predicates for AND/OR and auto/manual clear; publish bytes before wake, never synthesize bits. | SDK layout; Era B lifted accesses; Era C wake/coherence findings. |",
            "| QUEUE | `CellSpursQueue` counters and guest buffer, directly shared with SPU code. | Reservation-lockline serialization and predicate waits; direction errors are preserved. Attach/detach are layout-level adapters. | SDK inline/lifted queue code and Era C reservation semantics. |",
            "| LFQUEUE | `CellSyncLFQueue` counters and guest buffer, directly shared with SPU code. | Direction-2 PPU-to-SPU push is native; any-to-any fails explicitly. | SDK ABI and RPCS3 used only as a semantic oracle. |",
            "| BARRIER | 128-byte guest barrier object shared with its taskset. | Initialize the documented count/state; no fabricated arrival. | SDK 4.75 ABI; static Yakuza call site. |",
            "| JOB | Job-chain guest object, command list, descriptors, `CellSpursJobContext2`, LS slots and DMA lists. | Asynchronous command interpreter, exact binaries/slots, real DMA writeback and completion/errors. | SDK job ABI; Era B lifted job policy; Era C descriptor/slot evidence. |",
            "| GUARD | 128-byte guest guard plus job-chain side-table predicate. | Counted notify/wait with publish-before-wake and configured auto reset. | SDK ABI and Era B lifted guard flow. |",
            "| SYNC-MUTEX | Four-byte big-endian ticket word, also accessed by PPU inline atomics. | Atomic ticket compare/exchange; busy/error semantics remain guest-visible. | SDK 4.75 layout; RPCS3 semantic comparison; Era C reservation rules. |",
            "",
            "## Static call sites",
            "",
            "| Import | Caller | Return EA | Generated source |",
            "| --- | --- | --- | --- |",
        ]
    )
    for call in calls:
        return_ea = f"`0x{call['return_ea']}`" if call["return_ea"] else "unknown"
        lines.append(
            f"| `{call['module']}::{call['name']}` (`0x{call['stub']}`) | "
            f"`0x{call['caller']}` | {return_ea} | "
            f"`{call['file']}:{call['line']}` |"
        )
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bridges", type=Path, required=True)
    parser.add_argument("--recomp", type=Path, required=True)
    parser.add_argument("--headers", type=Path, nargs="*", default=[])
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    parser.add_argument(
        "--output",
        type=Path,
        help="write the report to this path instead of standard output",
    )
    args = parser.parse_args()

    imports = imports_from(args.bridges)
    if not imports:
        raise SystemExit(f"no cellSpurs/cellSync imports found in {args.bridges}")
    calls = callsites(args.recomp, imports)
    prototypes = prototypes_from(args.headers)
    if args.format == "json":
        report = json.dumps(
            {
                "imports": imports,
                "callsites": calls,
                "prototypes": prototypes,
            },
            indent=2,
        ) + "\n"
    else:
        report = markdown(imports, calls, prototypes)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report, encoding="utf-8", newline="\n")
    else:
        print(report, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
