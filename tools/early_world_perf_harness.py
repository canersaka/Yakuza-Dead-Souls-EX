#!/usr/bin/env python3
"""Matched diagnostics-off early-world performance gate.

Measures the authored orphanage sequence from its first stable world walkway
anchor to the sink,
then stops both legacy and strict-native lanes at the same archived visual
anchor.  Route START injection is deliberately delayed until after this
window so renderer speed cannot make one lane skip draws the other executes.
then combines the built-in Present QPC ring with process-scoped RSX thread
times and Windows' cumulative per-process GPU-engine counter.  It launches
and closes only its exact child PID.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import re
import subprocess
import time

import akiyama_perf_harness as base


THREAD_QUERY_LIMITED_INFORMATION = 0x0800
PDH_MORE_DATA = 0x800007D2


# The beach shot is only a few rendered frames long and strict-native can pass
# through it between two bounded framebuffer samples.  The following walkway
# is the first repeatable world-rendering anchor shared by both lanes.  It is
# still before any synthetic route input and is followed by the same authored
# lion/sign/sink sequence in each lane.
EARLY_WORLD_ANCHORS = {
    "orphanage_walkway": {"serials": (14, 15, 16, 17)},
    "sink": {"serials": (23, 24)},
}


class PDH_RAW_COUNTER(ctypes.Structure):
    _fields_ = [
        ("CStatus", wintypes.DWORD),
        ("TimeStamp", wintypes.FILETIME),
        ("FirstValue", ctypes.c_longlong),
        ("SecondValue", ctypes.c_longlong),
        ("MultiCount", wintypes.DWORD),
    ]


def filetime_value(value):
    return (value.dwHighDateTime << 32) | value.dwLowDateTime


def thread_cpu_100ns(thread_id):
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL,
                                     wintypes.DWORD]
    kernel32.OpenThread.restype = wintypes.HANDLE
    kernel32.GetThreadTimes.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
    ]
    kernel32.GetThreadTimes.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel32.OpenThread(
        THREAD_QUERY_LIMITED_INFORMATION, False, thread_id
    )
    if not handle:
        return None
    try:
        creation = wintypes.FILETIME()
        exit_time = wintypes.FILETIME()
        kernel = wintypes.FILETIME()
        user = wintypes.FILETIME()
        if not kernel32.GetThreadTimes(
            handle, ctypes.byref(creation), ctypes.byref(exit_time),
            ctypes.byref(kernel), ctypes.byref(user)
        ):
            return None
        return {
            "kernel_100ns": filetime_value(kernel),
            "user_100ns": filetime_value(user),
        }
    finally:
        kernel32.CloseHandle(handle)


def gpu_running_time_100ns(process_id):
    # Running Time is a cumulative KMT process-engine counter.  Sum all
    # engines owned by the exact child PID.  Read PDH directly: launching
    # PowerShell/Get-Counter here cost over two seconds and visibly distorted
    # the QPC frame window it was intended to accompany.
    pdh = ctypes.WinDLL("pdh", use_last_error=True)
    pdh.PdhOpenQueryW.argtypes = [wintypes.LPCWSTR, ctypes.c_size_t,
                                  ctypes.POINTER(wintypes.HANDLE)]
    pdh.PdhOpenQueryW.restype = wintypes.LONG
    pdh.PdhExpandWildCardPathW.argtypes = [
        wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.LPWSTR,
        ctypes.POINTER(wintypes.DWORD), wintypes.DWORD,
    ]
    pdh.PdhExpandWildCardPathW.restype = wintypes.LONG
    pdh.PdhAddEnglishCounterW.argtypes = [
        wintypes.HANDLE, wintypes.LPCWSTR, ctypes.c_size_t,
        ctypes.POINTER(wintypes.HANDLE),
    ]
    pdh.PdhAddEnglishCounterW.restype = wintypes.LONG
    pdh.PdhCollectQueryData.argtypes = [wintypes.HANDLE]
    pdh.PdhCollectQueryData.restype = wintypes.LONG
    pdh.PdhGetRawCounterValue.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD),
        ctypes.POINTER(PDH_RAW_COUNTER),
    ]
    pdh.PdhGetRawCounterValue.restype = wintypes.LONG
    pdh.PdhCloseQuery.argtypes = [wintypes.HANDLE]
    pdh.PdhCloseQuery.restype = wintypes.LONG

    wildcard = rf"\GPU Engine(pid_{process_id}_*)\Running Time"
    size = wintypes.DWORD()
    status = pdh.PdhExpandWildCardPathW(
        None, wildcard, None, ctypes.byref(size), 0
    )
    if (status & 0xFFFFFFFF) != PDH_MORE_DATA or size.value == 0:
        return 0 if status == 0 else None
    buffer = ctypes.create_unicode_buffer(size.value)
    if pdh.PdhExpandWildCardPathW(
        None, wildcard, buffer, ctypes.byref(size), 0
    ) != 0:
        return None
    paths = [path for path in buffer[:size.value].split("\0") if path]
    if not paths:
        return 0

    query = wintypes.HANDLE()
    if pdh.PdhOpenQueryW(None, 0, ctypes.byref(query)) != 0:
        return None
    try:
        counters = []
        for path in paths:
            counter = wintypes.HANDLE()
            if pdh.PdhAddEnglishCounterW(
                query, path, 0, ctypes.byref(counter)
            ) == 0:
                counters.append(counter)
        if not counters or pdh.PdhCollectQueryData(query) != 0:
            return None
        total = 0
        valid = 0
        for counter in counters:
            value = PDH_RAW_COUNTER()
            counter_type = wintypes.DWORD()
            if pdh.PdhGetRawCounterValue(
                counter, ctypes.byref(counter_type), ctypes.byref(value)
            ) == 0 and value.CStatus == 0:
                total += value.FirstValue
                valid += 1
        return total if valid else None
    finally:
        pdh.PdhCloseQuery(query)


def make_sample(process_id, thread_id):
    before = time.perf_counter_ns()
    thread = thread_cpu_100ns(thread_id)
    gpu = gpu_running_time_100ns(process_id)
    after = time.perf_counter_ns()
    return {
        "sample_midpoint_perf_ns": (before + after) // 2,
        "sample_wall_ms": round((after - before) / 1_000_000.0, 3),
        "rsx_thread": thread,
        "gpu_running_100ns": gpu,
    }


def refs_for(reference_dir, name):
    return [
        base.scene_features(reference_dir / f"frontier_probe_{serial:03d}.ppm")
        for serial in EARLY_WORLD_ANCHORS[name]["serials"]
    ]


def run(args):
    executable = args.exe.resolve()
    game_dir = args.game_dir.resolve()
    game_elf = game_dir / "game" / "EBOOT.elf"
    cache_path = executable.parent / "CMakeCache.txt"
    for path, label in ((executable, "executable"), (game_elf, "game ELF"),
                        (cache_path, "CMake cache")):
        if not path.is_file():
            raise FileNotFoundError(f"missing {label}: {path}")
    cache = base.parse_cache(cache_path)
    mismatches = {
        key: {"expected": expected, "actual": cache.get(key)}
        for key, expected in base.EXPECTED_CACHE.items()
        if cache.get(key) != expected
    }
    if mismatches:
        raise RuntimeError(f"production configuration mismatch: {mismatches}")
    existing = base.game_processes()
    if existing:
        raise RuntimeError(f"game process already running: {existing}")

    scratch = game_dir / "scratch"
    run_dir = scratch / args.tag
    capture_dir = scratch / f"{args.tag}-capture"
    if run_dir.exists() or capture_dir.exists():
        raise FileExistsError(f"refusing to reuse run tag {args.tag}")
    run_dir.mkdir()
    capture_dir.mkdir()
    stdout_path = run_dir / "game.out"
    stderr_path = run_dir / "game.err"
    input_stop = run_dir / "early-world-input-stop.txt"
    capture_stop = run_dir / "early-world-capture-stop.txt"
    result_path = run_dir / "result.json"

    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("YZ_")}
    yz = {
        "YZ_MOVIE_HLE": "1",
        "YZ_AUTO_START": "1",
        "YZ_AUTO_NEW_GAME": "1",
        "YZ_A010_ACCEPT_FAST": "1",
        "YZ_FRONTIER_ACCEPT_FAST": "1",
        "YZ_AKIYAMA_DIALOGUE_ROUTE": "1",
        "YZ_AKIYAMA_DIALOGUE_STOP_FILE": str(input_stop),
        "YZ_AKIYAMA_DIALOGUE_CAPTURE_STOP_FILE": str(capture_stop),
        "YZ_AKIYAMA_ROUTE_START_DELAY_MS": str(args.route_start_delay_ms),
        "YZ_MOVEMENT_PROOF_DELAY_MS": str(args.capture_delay_ms),
        "YZ_MOVEMENT_PROBE_INTERVAL_MS": "2000",
        "YZ_RSX_VALIDATION_DIR": str(capture_dir),
    }
    if args.native:
        yz["YZ_NR_VERTICAL"] = "full-native"
    environment.update(yz)
    result = {
        "tag": args.tag,
        "lane": "strict-native" if args.native else "legacy",
        "status": "launching",
        "executable": str(executable),
        "executable_sha256": base.sha256_file(executable),
        "configuration": {key: cache.get(key) for key in base.EXPECTED_CACHE},
        "active_yz": yz,
        "active_diagnostics": {},
        "reference_dir": str(args.reference_dir.resolve()),
        "captures": [],
    }
    references = {
        name: refs_for(args.reference_dir.resolve(), name)
        for name in ("orphanage_walkway", "sink")
    }
    expected = ["orphanage_walkway", "sink"]
    anchor_index = 0
    seen = set()
    process = None
    forced = False
    out_handle = stdout_path.open("wb")
    err_handle = stderr_path.open("wb")
    try:
        process = subprocess.Popen(
            [str(executable), str(game_elf)], cwd=str(game_dir),
            env=environment, stdout=out_handle, stderr=err_handle,
        )
        result["pid"] = process.pid
        result["status"] = "routing"
        print(f"[early-world] launched exact PID {process.pid}", flush=True)
        time.sleep(1.0)
        if list(base.game_processes()) != [process.pid]:
            raise RuntimeError(
                f"single-game-process invariant failed: {base.game_processes()}"
            )
        config, _ = base.wait_log(
            stderr_path, r"\[config\].*lane=clean.*YZ_AKIYAMA_DIALOGUE_ROUTE=1",
            60, process,
        )
        result["runtime_config_line"] = config.group(0)
        tid_match, _ = base.wait_log(
            stderr_path, r"\[rsx\].*host_tid=([0-9]+)", 60, process,
        )
        rsx_tid = int(tid_match.group(1))
        result["rsx_consumer_thread_id"] = rsx_tid

        deadline = time.monotonic() + args.route_timeout
        while time.monotonic() < deadline and anchor_index < len(expected):
            if process.poll() is not None:
                raise RuntimeError(f"game exited during route: {process.returncode}")
            if stderr_path.exists():
                tail = stderr_path.read_text(
                    encoding="utf-8", errors="replace"
                )[-262144:]
                fatal = re.findall(
                    r"\[(?:nr-vertical-section-fatal|nr-full-native-fatal)"
                    r"[^\r\n]*", tail,
                )
                if fatal:
                    raise RuntimeError(fatal[-1])
            for path in sorted(capture_dir.glob("frontier_probe_*.ppm")):
                if path in seen:
                    continue
                try:
                    features = base.scene_features(path)
                except (OSError, ValueError):
                    continue
                seen.add(path)
                serial = int(path.stem.rsplit("_", 1)[-1])
                maes = {
                    name: min(base.coarse_scene_mae(ref, features)
                              for ref in refs)
                    for name, refs in references.items()
                }
                result["captures"].append({
                    "serial": serial, "path": str(path),
                    "mae": {key: round(value, 6)
                            for key, value in maes.items()},
                })
                name = expected[anchor_index]
                maximum = min(
                    args.anchor_mae,
                    EARLY_WORLD_ANCHORS[name].get(
                        "maximum_mae", args.anchor_mae
                    ),
                )
                if maes[name] <= maximum:
                    # The point GPU counter query is intentionally outside
                    # the QPC frame window.  Take it at the first walkway
                    # match, then require a second positive walkway frame as
                    # the actual measured start anchor.
                    if (name == "orphanage_walkway" and
                            "resource_sample_start" not in result):
                        result["resource_start_visual"] = {
                            "serial": serial, "path": str(path),
                            "coarse_mae": round(maes[name], 6),
                        }
                        print(
                            f"[early-world] resource start at walkway probe "
                            f"{serial}", flush=True,
                        )
                        result["resource_sample_start"] = make_sample(
                            process.pid, rsx_tid
                        )
                        continue
                    result.setdefault("anchors", {})[name] = {
                        "serial": serial, "path": str(path),
                        "coarse_mae": round(maes[name], 6),
                    }
                    print(f"[early-world] anchor {name} at probe {serial}",
                          flush=True)
                    if name == "sink":
                        result["resource_sample_end"] = make_sample(
                            process.pid, rsx_tid
                        )
                    anchor_index += 1
            time.sleep(0.25)
        if anchor_index != len(expected):
            raise TimeoutError(
                f"early world anchors incomplete: {anchor_index}/2"
            )

        input_stop.write_text("early world endpoint reached\n", encoding="ascii")
        capture_stop.write_text("early world endpoint reached\n", encoding="ascii")
        time.sleep(0.5)
        result["status"] = "closing"
        result["wm_close_windows"] = base.post_close(process.pid)
        if not result["wm_close_windows"]:
            raise RuntimeError("no visible exact-PID window found for shutdown")
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            forced = True
            process.terminate()
            process.wait(timeout=10)
        result["exit_code"] = process.returncode
        result["forced_close"] = forced
        out_handle.flush()
        err_handle.flush()
        stderr_text = stderr_path.read_text(encoding="utf-8", errors="replace")
        present_ids = {
            int(serial): int(present_id)
            for serial, present_id in re.findall(
                r"\[(?:akiyama-route|movement-proof)\] "
                r"clean-frontier-probe serial=([0-9]+).*? "
                r"present_id=([0-9]+)", stderr_text,
            )
        }
        for name, anchor in result["anchors"].items():
            anchor["present_id"] = present_ids.get(anchor["serial"])
        start_id = result["anchors"]["orphanage_walkway"].get("present_id")
        end_id = result["anchors"]["sink"].get("present_id")
        if start_id is None or end_id is None or end_id <= start_id + 1:
            raise RuntimeError(f"invalid present anchor IDs: {start_id}/{end_id}")
        qpc_path = game_dir / "scratch" / f"present_qpc_{process.pid}.csv"
        result["present_qpc_path"] = str(qpc_path)
        result["qpc"] = base.qpc_metrics(
            qpc_path, start_id, end_id - 1, bucket_seconds=1.0
        )

        first = result["resource_sample_start"]
        last = result["resource_sample_end"]
        wall_s = (last["sample_midpoint_perf_ns"] -
                  first["sample_midpoint_perf_ns"]) / 1_000_000_000.0
        result["resource_window_seconds"] = round(wall_s, 6)
        if first["rsx_thread"] and last["rsx_thread"]:
            cpu_100ns = (
                last["rsx_thread"]["kernel_100ns"] -
                first["rsx_thread"]["kernel_100ns"] +
                last["rsx_thread"]["user_100ns"] -
                first["rsx_thread"]["user_100ns"]
            )
            result["rsx_consumer_cpu_ms"] = round(cpu_100ns / 10000.0, 3)
            result["rsx_consumer_cpu_percent_one_core"] = round(
                cpu_100ns / 10_000_000.0 / wall_s * 100.0, 3
            )
        if (first["gpu_running_100ns"] is not None and
                last["gpu_running_100ns"] is not None):
            gpu_100ns = (last["gpu_running_100ns"] -
                         first["gpu_running_100ns"])
            result["gpu_engine_time_ms"] = round(gpu_100ns / 10000.0, 3)

        native = re.findall(r"^\[nr-vertical-d3d .*\]$", stderr_text,
                            re.MULTILINE)
        legacy = re.findall(r"^\[live-draw-clean .*\]$", stderr_text,
                            re.MULTILINE)
        result["native_shutdown"] = native
        result["legacy_shutdown"] = legacy
        if args.native:
            full = re.findall(r"^\[nr-full-native .*\]$", stderr_text,
                              re.MULTILINE)
            result["full_native_shutdown"] = full
            if (len(full) != 1 or "fatal=0" not in full[0] or
                    len(native) != 1 or "legacy-groups=0" not in native[0] or
                    "fallback=0" not in native[0]):
                raise RuntimeError("strict native shutdown was not clean")
        elif native:
            raise RuntimeError("native renderer was active in legacy lane")
        result["status"] = (
            "passed" if not forced and process.returncode == 0 else "failed"
        )
    except Exception as exc:
        result["status"] = "failed"
        result["failure"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        if process is not None and process.poll() is None:
            base.post_close(process.pid)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                forced = True
                process.terminate()
                process.wait(timeout=10)
        out_handle.close()
        err_handle.close()
        result["forced_close"] = forced
        result_path.write_text(json.dumps(result, indent=2) + "\n",
                               encoding="utf-8")
        print(f"[early-world] result: {result_path}", flush=True)
    return 0 if result["status"] == "passed" else 2


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--reference-dir", type=Path, required=True)
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--route-timeout", type=float, default=420)
    parser.add_argument("--capture-delay-ms", type=int, default=30000)
    parser.add_argument("--route-start-delay-ms", type=int, default=120000)
    parser.add_argument("--anchor-mae", type=float, default=0.10)
    args = parser.parse_args()
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
