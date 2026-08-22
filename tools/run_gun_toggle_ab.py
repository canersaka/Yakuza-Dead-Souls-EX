#!/usr/bin/env python3
"""Interleaved exact-PID A/B at the automated post-Frontier gun checkpoint."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import time


def mean(values):
    return sum(values) / len(values) if values else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--experiment", required=True)
    parser.add_argument("--toggle-key", required=True)
    parser.add_argument("--harness-toggle-arg", required=True)
    parser.add_argument("--off-exe", type=Path, required=True)
    parser.add_argument("--on-exe", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--attempts", type=int, default=2)
    parser.add_argument("--hold-seconds", type=int, default=35)
    parser.add_argument("--date", default=time.strftime("%Y%m%d"))
    args = parser.parse_args()

    worktree = Path(__file__).resolve().parents[1]
    root = Path(__file__).resolve().parents[3]
    scratch = root / "scratch"
    harness = worktree / "tools" / "akiyama_perf_harness.py"
    reference_dir = scratch / "frontier-fullroute-20260820-accept20-capture"
    lanes = {
        "off": {"toggle": "OFF", "exe": args.off_exe.resolve()},
        "on": {"toggle": "ON", "exe": args.on_exe.resolve()},
    }
    for lane in lanes.values():
        if not lane["exe"].is_file():
            raise FileNotFoundError(lane["exe"])
    if not reference_dir.is_dir():
        raise FileNotFoundError(reference_dir)

    completed = {name: [] for name in lanes}
    failures = []
    plan = [
        (lane, repeat)
        for repeat in range(1, args.runs + 1)
        for lane in ("off", "on")
    ]
    for lane_name, repeat in plan:
        lane = lanes[lane_name]
        for attempt in range(1, args.attempts + 1):
            tag = (
                f"gun-{args.experiment}-{lane_name}-r{repeat}-"
                f"attempt{attempt}-{args.date}"
            )
            result_path = scratch / tag / "result.json"
            command = [
                sys.executable,
                str(harness),
                "--exe", str(lane["exe"]),
                "--expect-shufb", "ON",
                "--expect-ls128", "ON",
                args.harness_toggle_arg, lane["toggle"],
                "--tag", tag,
                "--gun-route",
                "--gun-reference-dir", str(reference_dir),
                "--gun-route-timeout", "1500",
                "--hold-seconds", str(args.hold_seconds),
            ]
            print(
                f"[gun-{args.experiment}] {lane_name} "
                f"repeat={repeat} attempt={attempt}",
                flush=True,
            )
            run = subprocess.run(command, cwd=worktree)
            result = None
            if result_path.is_file():
                result = json.loads(result_path.read_text(encoding="utf-8"))
            if (
                run.returncode == 0
                and result
                and result.get("status") == "passed"
                and result.get("scenes", {})
                .get("post_frontier_gun_tutorial", {})
                .get("route_valid")
            ):
                completed[lane_name].append(result)
                break
            failures.append({
                "lane": lane_name,
                "repeat": repeat,
                "attempt": attempt,
                "returncode": run.returncode,
                "result": str(result_path),
                "status": result.get("status") if result else "missing",
                "failure_class": result.get("route_failure_class")
                if result else None,
                "failure": result.get("failure") if result else None,
            })
        else:
            raise RuntimeError(
                f"{lane_name} repeat {repeat} failed after "
                f"{args.attempts} attempts"
            )

    off = [run["fps_mean"] for run in completed["off"]]
    on = [run["fps_mean"] for run in completed["on"]]
    off_mean = mean(off)
    on_mean = mean(on)
    configurations = {
        name: [run["configuration"] for run in runs]
        for name, runs in completed.items()
    }
    off_common = dict(configurations["off"][0])
    on_common = dict(configurations["on"][0])
    off_toggle = off_common.pop(args.toggle_key)
    on_toggle = on_common.pop(args.toggle_key)
    if off_common != on_common or off_toggle != "OFF" or on_toggle != "ON":
        raise AssertionError(
            f"lane configuration differs beyond {args.toggle_key}"
        )

    output_dir = scratch / f"gun-{args.experiment}-ab-{args.date}"
    output_dir.mkdir(exist_ok=True)
    summary = {
        "status": "passed",
        "policy": "representative-multi-scene",
        "scene": "post_frontier_gun_tutorial",
        "experiment": args.experiment,
        "toggle_key": args.toggle_key,
        "runs_per_lane": args.runs,
        "hold_seconds": args.hold_seconds,
        "reference_dir": str(reference_dir),
        "common_configuration": off_common,
        "lane_toggles": {"off": off_toggle, "on": on_toggle},
        "lane_executable_sha256": {
            name: runs[0]["executable_sha256"]
            for name, runs in completed.items()
        },
        "results": {
            name: [str(scratch / run["tag"] / "result.json") for run in runs]
            for name, runs in completed.items()
        },
        "failures": failures,
        "comparison": {
            "off_runs": off,
            "on_runs": on,
            "off_mean": round(off_mean, 3),
            "on_mean": round(on_mean, 3),
            "on_delta_fps": round(on_mean - off_mean, 3),
            "on_delta_percent": round((on_mean / off_mean - 1.0) * 100.0, 2),
        },
    }
    summary_path = output_dir / "summary.json"
    summary_path.write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary["comparison"], indent=2), flush=True)
    print(f"[gun-{args.experiment}] summary: {summary_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
