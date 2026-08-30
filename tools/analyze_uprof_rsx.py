#!/usr/bin/env python3
"""Resolve and categorize one AMD uProf RSX-consumer sample stream.

AMD uProf 5.3 stores sampled instruction pointers in ``UnifiedSampleSeries``
and caller frames as compact ``(module_id << 32) | module_rva`` values.  This
tool resolves both against the production linker MAP.  Samples in D3D12 or the
user-mode display driver are attributed through their nearest yakuza caller,
which keeps exclusive CPU accounting separate while making the call path
actionable.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from pathlib import Path
import subprocess
import sys

from analyze_uprof_map import MapResolver


IMAGE_BASE = 0x140000000


def query(query_tool: Path, database: Path, sql: str) -> list[dict[str, str]]:
    completed = subprocess.run(
        [sys.executable, str(query_tool), str(database), sql],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    lines = completed.stdout.splitlines()
    if not lines:
        return []
    columns = lines[0].split("\t")
    return [dict(zip(columns, line.split("\t"))) for line in lines[1:]]


def resolve_rva(resolver: MapResolver, rva: int) -> str:
    name, _ = resolver.resolve(
        f"yakuza_recomp.exe!0x{IMAGE_BASE + rva:x}"
    )
    return name


def category(name: str) -> str:
    lowered = name.lower()

    # Order is intentional: resource and vertex preparation may happen under
    # draw recording, but they remain distinct requested buckets.
    if any(token in lowered for token in (
        "gpu_mirror", "guest_pages", "gmb_", "image4_service",
        "native_residency",
    )):
        return "GPU-mirror synchronization"
    if any(token in lowered for token in (
        "prepare_textures", "texture", "sampler", "resolve_fp",
        "fragment_program", "fp_", "get_rt", "get_depth", "surface",
        "prepare_draw_residency", "required_span", "watch_guest_span",
        "res_lookup", "pso", "shader",
    )):
        return "texture/resource preparation"
    if any(token in lowered for token in (
        "index", "vertex", "triangle_strip", "batch", "pull_plan",
        "attr_element", "prepare_draw_constants",
    )):
        return "vertex/index preparation"
    if any(token in lowered for token in (
        "ymml", "ymmloop", "movewith", "moveabove", "setupto",
        "set0ymm", "memcpy", "memmove", "mcmp", "malloc", "calloc",
        "realloc", "operator new", "operator delete",
    )):
        return "copying/allocation"
    if any(token in lowered for token in (
        "service_report", "present", "timeline_acquire", "timeline_release",
        "flush_mode", "stall_now", "ensure_graphics", "submission",
        "submit", "fence", "queue",
    )):
        return "submission/presentation"
    if any(token in lowered for token in (
        "record_prepared_draw", "backend_execute_op", "nrb_draw_impl",
        "nrb_draw_op", "nrb_clear", "open_list", "gpu_draw",
    )):
        return "D3D12 command recording"
    if any(token in lowered for token in (
        "frame_resume_packet", "method_supported", "adapter_method",
        "dispatch_method", "fifo_step", "frame_owner_step", "frame_drain",
        "ring_", "io_to_ea", "frame_read32", "vm_read32", "vm_write32",
        "registered_data_island", "fifo_section", "release_published",
    )):
        return "command decoding/adaptation"
    if any(token in lowered for token in (
        "consume_frame", "flush_state", "stage_state", "apply_state_op",
        "nir_em_", "hash", "sink_draw", "sink_end", "draw",
        "backend_step", "live_draw",
    )):
        return "state/draw processing"
    return "other exclusive work"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("session", type=Path)
    parser.add_argument("map", type=Path)
    parser.add_argument("--thread", type=int, required=True)
    parser.add_argument("--frames", type=float, required=True)
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument(
        "--query-tool",
        type=Path,
        default=Path(__file__).with_name("query_uprof_db.py"),
    )
    args = parser.parse_args()

    resolver = MapResolver(args.map)
    cpu_db = args.session / "cpu.db"
    samples = query(
        args.query_tool,
        cpu_db,
        (
            "SELECT moduleId,moduleRVA,callsiteId FROM UnifiedSampleSeries "
            f"WHERE threadId={args.thread}"
        ),
    )
    frames = query(
        args.query_tool,
        cpu_db,
        "SELECT callstackId,functionId,depth FROM CallstackFrame ORDER BY depth",
    )

    nearest_yakuza: dict[int, int] = {}
    for frame in frames:
        function_id = int(frame["functionId"])
        if function_id >> 32 != 78:
            continue
        callstack_id = int(frame["callstackId"])
        nearest_yakuza.setdefault(callstack_id, function_id & 0xFFFFFFFF)

    category_counts: Counter[str] = Counter()
    function_counts: Counter[str] = Counter()
    path_counts: Counter[str] = Counter()
    category_functions: dict[str, Counter[str]] = defaultdict(Counter)

    for sample in samples:
        module_id = int(sample["moduleId"])
        rva = int(sample["moduleRVA"])
        callsite_id = int(sample["callsiteId"])
        if module_id == 78:
            function = resolve_rva(resolver, rva)
            path = function
            classify_name = function
        else:
            caller_rva = nearest_yakuza.get(callsite_id)
            caller = (
                resolve_rva(resolver, caller_rva)
                if caller_rva is not None
                else "<no-yakuza-caller>"
            )
            module = f"module-{module_id}"
            function = module
            path = f"{module} via {caller}"
            classify_name = caller
        group = category(classify_name)
        category_counts[group] += 1
        function_counts[function] += 1
        path_counts[path] += 1
        category_functions[group][path] += 1

    total = sum(category_counts.values())
    print(
        f"RSX TID {args.thread}: {total} samples, "
        f"{total / args.frames:.3f} sampled CPU-ms/present"
    )
    print("\nCATEGORY EXCLUSIVE ACCOUNTING")
    for group, count in category_counts.most_common():
        print(
            f"{group}\t{count / 1000.0:.3f}s\t"
            f"{100.0 * count / total:.2f}%\t"
            f"{count / args.frames:.3f}ms/profiled-present"
        )
        for path, path_count in category_functions[group].most_common(5):
            print(
                f"  {path_count / 1000.0:.3f}s\t"
                f"{path_count / args.frames:.3f}ms/present\t{path}"
            )

    print("\nTOP CALLER-RESOLVED EXCLUSIVE PATHS")
    for path, count in path_counts.most_common(args.top):
        print(
            f"{count / 1000.0:.3f}s\t{count / args.frames:.3f}ms/present\t"
            f"{category(path.split(' via ')[-1])}\t{path}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
