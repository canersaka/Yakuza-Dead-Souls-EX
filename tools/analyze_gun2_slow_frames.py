#!/usr/bin/env python3
"""Correlate the fixed Present ring with shutdown-only RSX tail buckets."""

import argparse
import csv
import json
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path


TAIL_RE = re.compile(r"^\[nr-rsx-tail-frame (.*)\]$")
SUBMIT_RE = re.compile(r"^\[nr-rsx-tail-frame-submit (.*)\]$")


def fields(text):
    out = {}
    for token in text.split():
        if "=" in token:
            key, value = token.split("=", 1)
            out[key] = value
    return out


def ints(value, count=None):
    result = [int(item) for item in value.split("/")]
    if count is not None and len(result) != count:
        raise ValueError(f"expected {count} values in {value!r}")
    return result


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return 0.0
    position = (len(ordered) - 1) * fraction
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    weight = position - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def summarize(rows):
    numeric = [
        "interval_ms", "rsx_cpu_ms", "gpu_active_ms", "gpu_span_ms",
        "live_submit_count", "live_submit_cpu_ms", "live_fence_wait_ms",
        "backend_submit_count", "backend_submit_cpu_ms",
        "transfer_readback_count", "transfer_readback_ms",
        "transfer_upload_count", "transfer_upload_ms",
        "allocator_waits", "allocator_wait_ms", "queue_depth",
        "report_early_drains", "report_required_waits", "last_block_ms",
    ]
    result = {"frames": len(rows)}
    for name in numeric:
        values = [float(row.get(name, 0.0)) for row in rows]
        result[name] = {
            "mean": round(statistics.fmean(values), 6) if values else 0.0,
            "median": round(statistics.median(values), 6) if values else 0.0,
            "p95": round(percentile(values, 0.95), 6) if values else 0.0,
        }
    result["last_block_causes"] = dict(Counter(
        str(row.get("last_block_cause", "none")) for row in rows
    ).most_common())
    submit_causes = Counter()
    for row in rows:
        submit_causes.update(row.get("backend_submit_causes", {}))
    result["backend_submit_causes"] = dict(submit_causes.most_common())
    live_causes = Counter()
    for row in rows:
        live_causes.update(row.get("live_submit_causes", {}))
    result["live_submit_causes"] = dict(live_causes.most_common())
    return result


def analyze(run_dir):
    run_dir = run_dir.resolve()
    result = json.loads((run_dir / "route-result.json").read_text())
    qpc_frequency = int(result["qpc_frequency"])
    start = int(result["measurement_start_qpc"])
    end = int(result["measurement_end_qpc"])

    with Path(result["present_qpc_path"]).open(newline="") as handle:
        first = handle.readline()
        if not first.startswith("# qpc_frequency="):
            raise ValueError("missing QPC header")
        all_rows = list(csv.DictReader(handle))

    tails = {}
    submits = defaultdict(dict)
    qpc_tail_frequency = 0
    gpu_frequency = 0
    for line in (run_dir / "game.err").read_text(
            encoding="utf-8", errors="replace").splitlines():
        match = TAIL_RE.match(line)
        if match:
            item = fields(match.group(1))
            present = int(item["present"])
            tails[present] = item
            qpc_tail_frequency = int(item["qpc"])
            gpu_frequency = int(item["gpu-frequency"])
            continue
        match = SUBMIT_RE.match(line)
        if match:
            item = fields(match.group(1))
            submits[int(item["present"])][item["cause"]] = item

    rows = []
    previous = None
    for raw in all_rows:
        qpc = int(raw["qpc"])
        if previous is not None and start <= qpc <= end:
            present = int(raw["present_id"])
            tail = tails.get(present)
            if tail is None:
                previous = raw
                continue
            interval_ms = (qpc - int(previous["qpc"])) * 1000 / qpc_frequency
            rsx_100ns = (
                int(raw["rsx_thread_kernel_100ns"]) +
                int(raw["rsx_thread_user_100ns"]) -
                int(previous["rsx_thread_kernel_100ns"]) -
                int(previous["rsx_thread_user_100ns"])
            )
            live_count = 0
            live_cpu = 0
            live_fence = 0
            live_causes = {}
            for name in (
                "present", "guest_reference", "vertex_ring",
                "vertex_constant_ring", "retire_queue", "movie",
                "movie_present", "readback", "pixel_constant_ring",
                "descriptor_ring", "shutdown",
            ):
                count = int(raw[f"{name}_count"]) - int(
                    previous[f"{name}_count"])
                cpu = int(raw[f"{name}_cpu_ticks"]) - int(
                    previous[f"{name}_cpu_ticks"])
                fence = int(raw[f"{name}_fence_ticks"]) - int(
                    previous[f"{name}_fence_ticks"])
                if count:
                    live_causes[name] = count
                live_count += count
                live_cpu += cpu
                live_fence += fence

            backend_count = 0
            backend_cpu = 0
            gpu_active = 0
            backend_causes = {}
            for name, submit in submits.get(present, {}).items():
                count = int(submit["submits"])
                cpu = int(submit["cpu"])
                gpu = ints(submit["gpu"], 2)
                backend_causes[name] = count
                backend_count += count
                backend_cpu += cpu
                gpu_active += gpu[1]

            readback = ints(tail["readback"], 3)
            upload = ints(tail["upload"], 3)
            queue = ints(tail["queue"], 3)
            allocator = ints(tail["allocator"], 3)
            reports = ints(tail["reports"], 4)
            last_block = ints(tail["last-block"], 5)
            gpu_span = ints(tail["gpu-span"], 2)
            rows.append({
                "present": present,
                "interval_ms": interval_ms,
                "methods": int(raw["methods"]) - int(previous["methods"]),
                "draws": int(raw["draws"]) - int(previous["draws"]),
                "game_updates": int(raw["game_updates"]) -
                    int(previous["game_updates"]),
                "image4_rounds": int(raw["image4_rounds"]) -
                    int(previous["image4_rounds"]),
                "rsx_cpu_ms": rsx_100ns / 10000.0,
                "live_submit_count": live_count,
                "live_submit_cpu_ms": live_cpu * 1000.0 / qpc_tail_frequency,
                "live_fence_wait_ms": live_fence * 1000.0 / qpc_tail_frequency,
                "live_submit_causes": live_causes,
                "backend_submit_count": backend_count,
                "backend_submit_cpu_ms":
                    backend_cpu * 1000.0 / qpc_tail_frequency,
                "backend_submit_causes": backend_causes,
                "gpu_active_ms": gpu_active * 1000.0 / gpu_frequency,
                "gpu_span_ms": (gpu_span[1] - gpu_span[0]) * 1000.0 /
                    gpu_frequency if gpu_span[1] >= gpu_span[0] else 0.0,
                "transfer_readback_count": readback[0],
                "transfer_readback_ms": readback[1] * 1000.0 /
                    qpc_tail_frequency,
                "transfer_readback_bytes": readback[2],
                "transfer_upload_count": upload[0],
                "transfer_upload_ms": upload[1] * 1000.0 /
                    qpc_tail_frequency,
                "transfer_upload_bytes": upload[2],
                "queue_depth": max(queue[0] - queue[1], 0),
                "fence_submitted": queue[0],
                "fence_completed": queue[1],
                "oldest_incomplete_fence": queue[2],
                "allocator_waits": allocator[1],
                "allocator_wait_ms": allocator[2] * 1000.0 /
                    qpc_tail_frequency,
                "allocator_slot": allocator[0],
                "report_natural_submissions": reports[0],
                "report_early_drains": reports[1],
                "report_required_waits": reports[2],
                "report_fallbacks": reports[3],
                "last_block_cause": last_block[0],
                "last_block_ms": last_block[2] * 1000.0 /
                    qpc_tail_frequency,
                "last_block_required_fence": last_block[3],
                "last_block_completed_fence": last_block[4],
            })
        previous = raw

    intervals = [row["interval_ms"] for row in rows]
    p95 = percentile(intervals, 0.95)
    p99 = percentile(intervals, 0.99)
    normal = [row for row in rows if row["interval_ms"] < p95]
    slow95 = [row for row in rows if row["interval_ms"] >= p95]
    slow99 = [row for row in rows if row["interval_ms"] >= p99]
    invariant_counts = Counter(
        (row["draws"], row["image4_rounds"]) for row in rows
    )
    return {
        "run": str(run_dir),
        "route_status": result["status"],
        "route_summary": {
            "fps": result["fps_mean"],
            "median_ms": result["frame_time_ms"]["median"],
            "p95_ms": result["frame_time_ms"]["p95"],
            "p99_ms": result["frame_time_ms"]["p99"],
            "rsx_cpu_ms_per_present": result["rsx_cpu_ms_per_present"],
            "process_cpu_ms_per_present": result["process_cpu_ms_per_present"],
            "gpu_duty_percent": result["gpu_engine_duty_percent"],
            "stability": result["stability"],
        },
        "correlated_frames": len(rows),
        "invariant_draw_image4_counts": {
            f"draws={key[0]},image4={key[1]}": value
            for key, value in invariant_counts.most_common()
        },
        "normal_below_p95": summarize(normal),
        "p95_and_slower": summarize(slow95),
        "p99_and_slower": summarize(slow99),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dirs", nargs="+", type=Path)
    args = parser.parse_args()
    print(json.dumps([analyze(path) for path in args.run_dirs], indent=2))


if __name__ == "__main__":
    main()
