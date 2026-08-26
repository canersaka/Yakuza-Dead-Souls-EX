#!/usr/bin/env python3
"""Matched, retry-bounded full-native A/B for one whole-job SPU family.

The ordinary route supplies anchored orphanage, transition, and Hana windows;
the extended route proves Frontier and supplies the stationary gun window.
Only the requested CMake family toggle may differ between lanes.  Active
render-graph execution is never requested.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import time


def mean(values):
    return sum(values) / len(values) if values else None


def percentile_mean(runs, scene, field):
    values = [run["scenes"][scene]["frame_time_ms"][field] for run in runs]
    return round(mean(values), 3)


def validate_result(result, suite):
    failures = []
    if result.get("status") != "passed":
        failures.append(f"status={result.get('status')}")
    if result.get("exit_code") != 0 or result.get("forced_close"):
        failures.append(
            f"shutdown exit={result.get('exit_code')} "
            f"forced={result.get('forced_close')}"
        )
    invalid = [
        name for name, scene in result.get("scenes", {}).items()
        if not scene.get("route_valid")
    ]
    if invalid:
        failures.append(f"invalid scenes={invalid}")
    active = result.get("active_yz", {})
    if active.get("YZ_NR_VERTICAL") != "full-native":
        failures.append("strict full-native was not active")
    if "YZ_NR_GRAPH" in active or "YZ_NR_SINGLE_PASS_GRAPH" in active:
        failures.append("active render graph was unexpectedly enabled")
    owner = "\n".join(result.get("nr_full_native", []))
    d3d = "\n".join(result.get("nr_full_native_d3d", []))
    if not re.search(r"\bfatal=0\b", owner):
        failures.append("native owner did not report fatal=0")
    for label, pattern in (
        ("100% native draw coverage", r"\bcoverage-ppm=1000000\b"),
        ("zero D3D fallback", r"\bfallback=0\b"),
        ("zero shader compile failure", r"\bcompile-fail=0\b"),
        ("zero texture failure", r"\btexture-fail=0\b"),
        ("zero residency failure", r"\bresidency-fail=0\b"),
    ):
        if not re.search(pattern, d3d):
            failures.append(label)
    if suite == "gun":
        markers = result.get("route_markers", {})
        if not markers.get("frontier_gun_visual"):
            failures.append("Frontier/gun visual marker missing")
        if result.get("route") != "three-leg-post-frontier-gun":
            failures.append(f"unexpected gun route={result.get('route')}")
    return failures


def execute_one(args, lane_name, lane, repeat, suite, scratch, harness, worktree):
    reference_dir = scratch / "multiscene-anchor-calibration-simd-20260821-b-capture"
    rejected = []
    for attempt in range(1, args.attempts + 1):
        tag = (
            f"wj-{args.family}-{suite}-{lane_name}-r{repeat}-"
            f"attempt{attempt}-{args.date}"
        )
        result_path = scratch / tag / "result.json"
        command = [
            sys.executable, str(harness),
            "--exe", str(lane["exe"]),
            "--expect-shufb", "ON",
            "--expect-ls128", "ON",
            "--expect-absdb", "ON",
            "--expect-xfloat", "ON",
            "--expect-exact-image-bytes", "ON",
            args.harness_toggle_arg, lane["toggle"],
            "--nr-vertical-full-native",
            "--tag", tag,
            "--hold-seconds", str(args.hold_seconds),
        ]
        if suite == "multiscene":
            command += [
                "--capture-delay-ms", "0",
                "--capture-interval-ms", "2000",
                "--route-start-delay-ms", "30000",
                "--route-timeout", "720",
                "--multi-scene-reference-dir", str(reference_dir),
            ]
        else:
            command += [
                "--gun-route",
                "--gun-entry-checkpoint",
                "--gun-route-timeout", "1500",
            ]
        print(
            f"[wj-{args.family}] {suite} {lane_name} "
            f"repeat={repeat} attempt={attempt}",
            flush=True,
        )
        run = subprocess.run(command, cwd=worktree)
        result = None
        if result_path.is_file():
            result = json.loads(result_path.read_text(encoding="utf-8"))
        problems = validate_result(result, suite) if result else ["result missing"]
        if run.returncode == 0 and not problems:
            return result, rejected
        failure = {
            "suite": suite,
            "lane": lane_name,
            "repeat": repeat,
            "attempt": attempt,
            "returncode": run.returncode,
            "result": str(result_path),
            "status": result.get("status") if result else "missing",
            "route_failure_class": result.get("route_failure_class")
            if result else None,
            "problems": problems,
        }
        rejected.append(failure)
        print(f"[wj-{args.family}] rejected attempt: {failure}", flush=True)
    raise RuntimeError(
        f"{suite} {lane_name} repeat {repeat} failed after {args.attempts} attempts"
    )


def summarize_scene(off_runs, on_runs, scene):
    off_fps = [run["scenes"][scene]["fps_mean"] for run in off_runs]
    on_fps = [run["scenes"][scene]["fps_mean"] for run in on_runs]
    off_mean, on_mean = mean(off_fps), mean(on_fps)
    return {
        "off_fps": off_fps,
        "on_fps": on_fps,
        "off_mean_fps": round(off_mean, 3),
        "on_mean_fps": round(on_mean, 3),
        "delta_fps": round(on_mean - off_mean, 3),
        "delta_percent": round((on_mean / off_mean - 1.0) * 100.0, 2),
        "off_median_ms": percentile_mean(off_runs, scene, "median"),
        "on_median_ms": percentile_mean(on_runs, scene, "median"),
        "off_p95_ms": percentile_mean(off_runs, scene, "p95"),
        "on_p95_ms": percentile_mean(on_runs, scene, "p95"),
        "off_p99_ms": percentile_mean(off_runs, scene, "p99"),
        "on_p99_ms": percentile_mean(on_runs, scene, "p99"),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--family", required=True)
    parser.add_argument("--toggle-key", required=True)
    parser.add_argument("--harness-toggle-arg", required=True)
    parser.add_argument("--off-exe", type=Path, required=True)
    parser.add_argument("--on-exe", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--attempts", type=int, default=3)
    parser.add_argument("--hold-seconds", type=int, default=30)
    parser.add_argument("--date", default=time.strftime("%Y%m%d-%H%M%S"))
    args = parser.parse_args()

    worktree = Path(__file__).resolve().parents[1]
    root = Path(__file__).resolve().parents[3]
    scratch = root / "scratch"
    harness = worktree / "tools" / "akiyama_perf_harness.py"
    lanes = {
        "off": {"toggle": "OFF", "exe": args.off_exe.resolve()},
        "on": {"toggle": "ON", "exe": args.on_exe.resolve()},
    }
    for lane in lanes.values():
        if not lane["exe"].is_file():
            raise FileNotFoundError(lane["exe"])

    completed = {
        suite: {lane: [] for lane in lanes}
        for suite in ("multiscene", "gun")
    }
    failures = []
    for suite in ("multiscene", "gun"):
        for repeat in range(1, args.runs + 1):
            for lane_name in ("off", "on"):
                result, rejected = execute_one(
                    args, lane_name, lanes[lane_name], repeat, suite,
                    scratch, harness, worktree,
                )
                completed[suite][lane_name].append(result)
                failures.extend(rejected)

    off_config = dict(completed["multiscene"]["off"][0]["configuration"])
    on_config = dict(completed["multiscene"]["on"][0]["configuration"])
    off_toggle = off_config.pop(args.toggle_key)
    on_toggle = on_config.pop(args.toggle_key)
    if off_config != on_config or off_toggle != "OFF" or on_toggle != "ON":
        raise AssertionError(
            f"lane configuration differs beyond {args.toggle_key}: "
            f"off={off_toggle} on={on_toggle}"
        )
    for suite in completed.values():
        for lane_name, runs in suite.items():
            expected = off_config | {args.toggle_key: lanes[lane_name]["toggle"]}
            for run in runs:
                if run["configuration"] != expected:
                    raise AssertionError("configuration changed across matched runs")

    comparisons = {}
    for scene in completed["multiscene"]["off"][0]["scenes"]:
        comparisons[scene] = summarize_scene(
            completed["multiscene"]["off"],
            completed["multiscene"]["on"],
            scene,
        )
    gun_scene = "post_frontier_gun_tutorial"
    comparisons[gun_scene] = summarize_scene(
        completed["gun"]["off"], completed["gun"]["on"], gun_scene
    )

    output_dir = scratch / f"wj-{args.family}-ab-{args.date}"
    output_dir.mkdir(exist_ok=False)
    summary = {
        "status": "passed",
        "family": args.family,
        "toggle_key": args.toggle_key,
        "runs_per_lane_per_suite": args.runs,
        "hold_seconds": args.hold_seconds,
        "graph_execution": "default-off",
        "common_configuration": off_config,
        "lane_toggle": {"off": off_toggle, "on": on_toggle},
        "executables": {
            lane: {
                "path": str(values["exe"]),
                "sha256": completed["multiscene"][lane][0]["executable_sha256"],
            }
            for lane, values in lanes.items()
        },
        "route_status": {
            "orphanage_hana_transition": "passed",
            "frontier_gun": "passed",
            "full_native_coverage": "100%",
            "native_errors_or_fallback": 0,
        },
        "results": {
            suite: {
                lane: [
                    str(scratch / result["tag"] / "result.json")
                    for result in runs
                ]
                for lane, runs in lanes_data.items()
            }
            for suite, lanes_data in completed.items()
        },
        "rejected_route_attempts": failures,
        "comparison": comparisons,
    }
    summary_path = output_dir / "summary.json"
    summary_path.write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(comparisons, indent=2), flush=True)
    print(f"[wj-{args.family}] summary: {summary_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
