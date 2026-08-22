#!/usr/bin/env python3
"""Run an interleaved, retry-bounded scalar/SIMD multi-scene comparison."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys


RUNS_PER_LANE = 2
MAX_ATTEMPTS = 3
HOLD_SECONDS = 30


def mean(values):
    return sum(values) / len(values) if values else None


def main():
    worktree = Path(__file__).resolve().parents[1]
    root = Path(__file__).resolve().parents[3]
    scratch = root / "scratch"
    harness = worktree / "tools" / "akiyama_perf_harness.py"
    reference_dir = (
        scratch / "multiscene-anchor-calibration-simd-20260821-b-capture"
    )
    lanes = {
        "scalar": {
            "toggle": "OFF",
            "exe": worktree / "yakuza" / "lane_shufb_scalar_20260821" /
                   "yakuza_recomp.exe",
        },
        "simd": {
            "toggle": "ON",
            "exe": worktree / "yakuza" / "lane_shufb_simd_20260821" /
                   "yakuza_recomp.exe",
        },
    }
    for lane in lanes.values():
        if not lane["exe"].is_file():
            raise FileNotFoundError(lane["exe"])
    if not reference_dir.is_dir():
        raise FileNotFoundError(reference_dir)

    completed = {name: [] for name in lanes}
    failures = []
    # Interleave lanes so temperature/background drift cannot favor one side.
    plan = [
        (lane, repeat)
        for repeat in range(1, RUNS_PER_LANE + 1)
        for lane in ("scalar", "simd")
    ]
    for lane_name, repeat in plan:
        lane = lanes[lane_name]
        for attempt in range(1, MAX_ATTEMPTS + 1):
            tag = (
                f"multiscene-shufb-{lane_name}-r{repeat}-"
                f"attempt{attempt}-20260821"
            )
            result_path = scratch / tag / "result.json"
            command = [
                sys.executable, str(harness),
                "--exe", str(lane["exe"]),
                "--expect-shufb", lane["toggle"],
                "--tag", tag,
                "--capture-delay-ms", "0",
                "--capture-interval-ms", "2000",
                "--route-start-delay-ms", "30000",
                "--hold-seconds", str(HOLD_SECONDS),
                "--route-timeout", "720",
                "--multi-scene-reference-dir", str(reference_dir),
            ]
            print(
                f"[multiscene-ab] {lane_name} repeat={repeat} "
                f"attempt={attempt}", flush=True
            )
            completed_process = subprocess.run(command, cwd=worktree)
            result = None
            if result_path.is_file():
                result = json.loads(result_path.read_text(encoding="utf-8"))
            if completed_process.returncode == 0 and result and (
                    result.get("status") == "passed"):
                invalid = [
                    name for name, scene in result.get("scenes", {}).items()
                    if not scene.get("route_valid")
                ]
                if invalid:
                    result["suite_failure"] = f"invalid scenes: {invalid}"
                else:
                    completed[lane_name].append(result)
                    break
            failures.append({
                "lane": lane_name,
                "repeat": repeat,
                "attempt": attempt,
                "returncode": completed_process.returncode,
                "result": str(result_path),
                "status": result.get("status") if result else "missing",
                "failure_class": result.get("route_failure_class")
                if result else None,
                "failure": result.get("failure") if result else None,
            })
        else:
            raise RuntimeError(
                f"{lane_name} repeat {repeat} failed after {MAX_ATTEMPTS} attempts"
            )

    scene_names = list(completed["scalar"][0]["scenes"])
    comparison = {}
    for scene_name in scene_names:
        scalar = [
            run["scenes"][scene_name]["fps_mean"]
            for run in completed["scalar"]
        ]
        simd = [
            run["scenes"][scene_name]["fps_mean"]
            for run in completed["simd"]
        ]
        scalar_mean = mean(scalar)
        simd_mean = mean(simd)
        comparison[scene_name] = {
            "scalar_runs": scalar,
            "simd_runs": simd,
            "scalar_mean": round(scalar_mean, 3),
            "simd_mean": round(simd_mean, 3),
            "simd_delta_fps": round(simd_mean - scalar_mean, 3),
            "simd_delta_percent": round(
                (simd_mean / scalar_mean - 1.0) * 100.0, 2
            ),
        }

    configurations = {
        lane_name: [run["configuration"] for run in runs]
        for lane_name, runs in completed.items()
    }
    scalar_common = dict(configurations["scalar"][0])
    simd_common = dict(configurations["simd"][0])
    scalar_toggle = scalar_common.pop("YZ_SPU_SIMD_SHUFB")
    simd_toggle = simd_common.pop("YZ_SPU_SIMD_SHUFB")
    if scalar_common != simd_common or scalar_toggle != "OFF" or simd_toggle != "ON":
        raise AssertionError("lane configuration differs beyond the shufb toggle")

    output_dir = scratch / "multiscene-shufb-ab-20260821"
    output_dir.mkdir(exist_ok=True)
    summary = {
        "status": "passed",
        "policy": "representative-multi-scene",
        "runs_per_lane": RUNS_PER_LANE,
        "hold_seconds": HOLD_SECONDS,
        "reference_dir": str(reference_dir),
        "common_configuration": scalar_common,
        "lane_toggles": {"scalar": scalar_toggle, "simd": simd_toggle},
        "lane_executable_sha256": {
            lane_name: runs[0]["executable_sha256"]
            for lane_name, runs in completed.items()
        },
        "results": {
            lane_name: [
                str(scratch / run["tag"] / "result.json") for run in runs
            ] for lane_name, runs in completed.items()
        },
        "failures": failures,
        "comparison": comparison,
    }
    summary_path = output_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(comparison, indent=2), flush=True)
    print(f"[multiscene-ab] summary: {summary_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
