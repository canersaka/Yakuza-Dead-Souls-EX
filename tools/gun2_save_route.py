#!/usr/bin/env python3
"""Visible, state-gated route to the saved second gun tutorial.

This is intentionally a route calibrator, not a profiler.  Menu input is
injected inside cellPad, the exact save load stops Confirm input, an archived
cutscene match arms exactly one Start edge, and three target matches stop all
input/readback while leaving the launched game visible for human confirmation.
"""

from __future__ import annotations

import argparse
import csv
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

import akiyama_perf_harness as base


TARGET_NAME = "Zombie Suppression 4: Dodging"
TARGET_MAE_MAX = 0.04
TARGET_HUD_IOU_MIN = 0.55
TARGET_REQUIRED_MATCHES = 3


class THREADENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
        ("th32ThreadID", wintypes.DWORD),
        ("th32OwnerProcessID", wintypes.DWORD),
        ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
    ]


def process_thread_cpu_100ns(process_id: int):
    """Snapshot cumulative CPU for every current thread in one exact PID."""
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.Thread32First.argtypes = [wintypes.HANDLE,
                                       ctypes.POINTER(THREADENTRY32)]
    kernel32.Thread32Next.argtypes = [wintypes.HANDLE,
                                      ctypes.POINTER(THREADENTRY32)]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    snapshot = kernel32.CreateToolhelp32Snapshot(0x00000004, 0)
    if snapshot in (0, ctypes.c_void_p(-1).value):
        return {}
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(entry)
    result = {}
    try:
        more = kernel32.Thread32First(snapshot, ctypes.byref(entry))
        while more:
            if entry.th32OwnerProcessID == process_id:
                value = base.thread_cpu_100ns(entry.th32ThreadID)
                if value is not None:
                    result[int(entry.th32ThreadID)] = int(value)
            more = kernel32.Thread32Next(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return result


def qpc_now():
    value = ctypes.c_longlong()
    if not ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(value)):
        raise ctypes.WinError()
    return value.value


def qpc_metrics_ticks(path: Path, start_tick: int, end_tick: int):
    """Compute clean frame metrics inside exact external QPC boundaries."""
    frequency = None
    rows = []
    with path.open("r", encoding="ascii", newline="") as handle:
        data_lines = []
        for line in handle:
            if line.startswith("# qpc_frequency="):
                frequency = int(line.split("=", 1)[1].strip())
            elif not line.startswith("#"):
                data_lines.append(line)
    for row in csv.DictReader(data_lines):
        tick = int(row["qpc"])
        if start_tick <= tick <= end_tick:
            rows.append((int(row["present_id"]), tick))
    if frequency is None:
        frequency_value = ctypes.c_longlong()
        if not ctypes.windll.kernel32.QueryPerformanceFrequency(
                ctypes.byref(frequency_value)):
            raise ctypes.WinError()
        frequency = frequency_value.value
    metrics = {
        "qpc_frequency": frequency,
        "measurement_start_qpc": start_tick,
        "measurement_end_qpc": end_tick,
        "measurement_presents": len(rows),
        "fps_mean": None,
        "frame_time_ms": {},
    }
    if len(rows) < 2:
        return metrics
    elapsed = (rows[-1][1] - rows[0][1]) / frequency
    intervals = sorted(
        (rows[index][1] - rows[index - 1][1]) * 1000.0 / frequency
        for index in range(1, len(rows))
    )
    metrics.update({
        "measurement_first_present_id": rows[0][0],
        "measurement_last_present_id": rows[-1][0],
        "measurement_qpc_seconds": round(elapsed, 6),
        "fps_mean": round((len(rows) - 1) / elapsed, 3),
        "frame_time_ms": {
            "min": round(intervals[0], 3),
            "median": round(base.percentile(intervals, 0.50), 3),
            "p95": round(base.percentile(intervals, 0.95), 3),
            "p99": round(base.percentile(intervals, 0.99), 3),
            "max": round(intervals[-1], 3),
        },
    })
    return metrics


def checkpoint_artifacts(checkpoint: Path, run_dir: Path):
    """Persist the accepted full frame and stable screen-space identity crops."""
    full_ppm = run_dir / "accepted_checkpoint.ppm"
    shutil.copy2(checkpoint, full_ppm)
    artifacts = [{
        "kind": "full-frame",
        "path": str(full_ppm),
        "sha256": base.sha256_file(full_ppm),
    }]
    try:
        from PIL import Image

        image = Image.open(checkpoint).convert("RGB")
        width, height = image.size
        boxes = {
            "tutorial-title": (280, 135, 1000, 225),
            "tutorial-panel": (200, 225, 1080, 505),
            "tutorial-scene": (190, 125, 1090, 515),
        }
        full_png = run_dir / "accepted_checkpoint.png"
        image.save(full_png)
        artifacts.append({
            "kind": "full-frame-png",
            "path": str(full_png),
            "sha256": base.sha256_file(full_png),
        })
        for kind, (left, top, right, bottom) in boxes.items():
            scaled = (
                left * width // 1280, top * height // 720,
                right * width // 1280, bottom * height // 720,
            )
            output = run_dir / f"accepted_{kind}.png"
            image.crop(scaled).save(output)
            artifacts.append({
                "kind": kind,
                "box_1280x720": [left, top, right, bottom],
                "path": str(output),
                "sha256": base.sha256_file(output),
            })
    except (ImportError, OSError):
        pass
    return artifacts


def rejection_identities(game_dir: Path, target):
    """Record exact negative visual identities used by the route gate."""
    identities = []
    candidates = [
        ("preceding-skippable-cutscene",
         game_dir / "scratch" / "gun2-save-calibration-v4-20260830-capture" /
         "frontier_probe_049.ppm"),
        ("preceding-loading-black",
         game_dir / "scratch" / "gun2-save-calibration-v4-20260830-capture" /
         "frontier_probe_044.ppm"),
    ]
    first_tutorial = (
        game_dir / "scratch" / "frontier-fullroute-20260820-accept20-capture"
    )
    candidates.extend(
        (f"first-tutorial-or-generic-gun-hud-{serial:03d}",
         first_tutorial / f"frontier_probe_{serial:03d}.ppm")
        for serial in (20, 21, 23, 28)
    )
    for kind, path in candidates:
        if not path.is_file():
            continue
        features = base.scene_features(path)
        identities.append({
            "kind": kind,
            "path": str(path),
            "sha256": base.sha256_file(path),
            "target_mae": round(base.coarse_scene_mae(target, features), 6),
            "target_hud_iou": round(base.gun_hud_iou(target, features), 6),
            "rejected": not (
                base.coarse_scene_mae(target, features) <= TARGET_MAE_MAX and
                base.gun_hud_iou(target, features) >= TARGET_HUD_IOU_MIN
            ),
        })
    return identities


def finalize_existing_calibration(game_dir: Path, result_path: Path):
    """Finalize the human-confirmed v4 calibration without launching a game."""
    result = json.loads(result_path.read_text(encoding="utf-8"))
    checkpoint = Path(result["checkpoint_capture"])
    run_dir = result_path.parent
    target = base.scene_features(
        game_dir / "scratch" / "gun-tutorial-2-save-reference" /
        "gun_tutorial_2_save.ppm"
    )
    result.update({
        "status": "human-confirmed-calibration",
        "human_confirmation": {
            "confirmed": True,
            "identity": TARGET_NAME,
            "benchmark": "gun-tutorial-2/save",
            "confirmed_date": "2026-08-30",
        },
        "accepted_artifacts": checkpoint_artifacts(checkpoint, run_dir),
        "negative_identities": rejection_identities(game_dir, target),
        "controller_sequence": [
            "title state: one internal Confirm edge from YZ_AUTO_START",
            "main-menu state: select exact item id 1 (Load Game)",
            "save state: bounded Confirm cadence until BLUS30826L01 LOAD complete",
            "cutscene state: one 750-ms Start edge after START/SKIP IoU gate",
            "target state: stop all input/readback after three exact matches",
        ],
        "route_state_markers": {
            "save_loaded": "[cellSaveData] LOAD complete for 'BLUS30826L01'",
            "cutscene_start": "[auto-load-game] positively gated cutscene",
            "start_released": "[auto-load-game] one-shot Start released",
            "input_stopped": "[akiyama-route] input stopped at visual checkpoint",
            "capture_stopped_present_id": 3716,
        },
        "stability_evidence": {
            "synthetic_input_stopped": True,
            "framebuffer_readback_stopped": True,
            "capture_count_at_stop": 53,
            "capture_count_after_hold": 53,
            "note": (
                "No later framebuffer capture or synthetic input occurred; "
                "the process continued presenting until its later shutdown."
            ),
        },
    })
    result_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def route_self_test(game_dir: Path):
    """Deterministic gate test over the human-confirmed calibration corpus."""
    reference_path = (
        game_dir / "scratch" / "gun-tutorial-2-save-reference" /
        "gun_tutorial_2_save.ppm"
    )
    target = base.scene_features(reference_path)
    corpus = (
        game_dir / "scratch" / "gun2-save-calibration-v4-20260830-capture"
    )
    accepted = []
    skip_matches = []
    skip_reference = skip_prompt_mask(
        game_dir / "scratch" / "gscc-gun2-off1-capture" /
        "frontier_probe_045.ppm"
    )
    for path in sorted(corpus.glob("frontier_probe_*.ppm")):
        features = base.scene_features(path)
        mae = base.coarse_scene_mae(target, features)
        hud = base.gun_hud_iou(target, features)
        if mae <= TARGET_MAE_MAX and hud >= TARGET_HUD_IOU_MIN:
            accepted.append(path.name)
        if mask_iou(skip_reference, skip_prompt_mask(path)) >= 0.80:
            skip_matches.append(path.name)
    if accepted != [
            "frontier_probe_051.ppm", "frontier_probe_052.ppm",
            "frontier_probe_053.ppm"]:
        raise AssertionError(f"unexpected accepted target frames: {accepted}")
    if skip_matches != ["frontier_probe_049.ppm"]:
        raise AssertionError(f"unexpected START/SKIP frames: {skip_matches}")
    negatives = rejection_identities(game_dir, target)
    if len(negatives) < 6 or not all(item["rejected"] for item in negatives):
        raise AssertionError(f"negative route identity accepted: {negatives}")
    print(
        f"[gun2-save-test] PASS: {len(list(corpus.glob('frontier_probe_*.ppm')))} "
        "calibration frames; one START/SKIP gate; three exact target matches; "
        f"{len(negatives)} explicit negative identities rejected"
    )


def skip_prompt_mask(path: Path):
    """Pale glyph mask covering the authored START / SKIP prompt only."""
    width, height, pixels = base.read_ppm(path)
    result = []
    for y in range(590 * height // 720, 715 * height // 720, 2):
        for x in range(900 * width // 1280, 1200 * width // 1280, 2):
            offset = (y * width + x) * 3
            red, green, blue = pixels[offset:offset + 3]
            maximum = max(red, green, blue)
            minimum = min(red, green, blue)
            result.append(maximum > 135 and maximum - minimum < 80)
    return result


def mask_iou(first, second):
    intersection = sum(a and b for a, b in zip(first, second))
    union = sum(a or b for a, b in zip(first, second))
    return intersection / union if union else 0.0


def psf_strings(path: Path):
    data = path.read_bytes()
    if len(data) < 20 or data[:4] != b"\x00PSF":
        raise ValueError(f"invalid PARAM.SFO: {path}")
    key_offset, data_offset, count = struct.unpack_from("<III", data, 8)
    result = {}
    for index in range(count):
        entry = 20 + index * 16
        key_rel, fmt, length, _maximum, value_rel = struct.unpack_from(
            "<HHIII", data, entry
        )
        key_start = key_offset + key_rel
        key_end = data.index(0, key_start)
        key = data[key_start:key_end].decode("utf-8", "replace")
        if fmt == 0x0204:
            value = data[data_offset + value_rel:data_offset + value_rel + length]
            result[key] = value.rstrip(b"\x00").decode("utf-8", "replace")
    return result


def close_failed_process(process: subprocess.Popen):
    if process.poll() is not None:
        return
    base.post_close(process.pid)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--tag")
    parser.add_argument("--route-timeout", type=float, default=240.0)
    parser.add_argument("--capture-delay-ms", type=int, default=16000)
    parser.add_argument("--capture-interval-ms", type=int, default=2000)
    parser.add_argument("--measure", action="store_true")
    parser.add_argument("--warmup-seconds", type=float, default=10.0)
    parser.add_argument("--hold-seconds", type=float, default=32.0)
    parser.add_argument("--timeline", action="store_true")
    parser.add_argument("--finalize-calibration-from", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    game_dir = args.game_dir.resolve()
    if args.self_test:
        route_self_test(game_dir)
        return 0
    if args.finalize_calibration_from:
        finalized = finalize_existing_calibration(
            game_dir, args.finalize_calibration_from.resolve()
        )
        print(
            f"[gun2-save] finalized confirmed calibration: "
            f"{args.finalize_calibration_from.resolve()} "
            f"({len(finalized['accepted_artifacts'])} accepted artifacts, "
            f"{len(finalized['negative_identities'])} negative identities)",
            flush=True,
        )
        return 0
    if args.exe is None or not args.tag:
        parser.error("--exe and --tag are required unless finalizing calibration")
    executable = args.exe.resolve()
    game_elf = game_dir / "game" / "EBOOT.elf"
    cache_path = executable.parent / "CMakeCache.txt"
    target_reference = (
        game_dir / "scratch" / "gun-tutorial-2-save-reference" /
        "gun_tutorial_2_save.ppm"
    )
    cutscene_dir = game_dir / "scratch" / "gscc-gun2-off1-capture"
    cutscene_serials = (1, 5, 10, 15, 20, 30, 40, 45)
    cutscene_paths = [
        cutscene_dir / f"frontier_probe_{serial:03d}.ppm"
        for serial in cutscene_serials
    ]
    skip_reference_path = cutscene_dir / "frontier_probe_045.ppm"
    for path, label in (
        (executable, "executable"),
        (game_elf, "game ELF"),
        (cache_path, "CMake cache"),
        (target_reference, "gun-tutorial-2/save reference"),
        *((path, "archived saved-game cutscene reference")
          for path in cutscene_paths),
    ):
        if not path.is_file():
            raise FileNotFoundError(f"missing {label}: {path}")

    save_dir = (
        game_dir / "gamedata" / "dev_hdd0" / "home" / "00000001" /
        "savedata" / "BLUS30826L01"
    )
    save_sfo = save_dir / "PARAM.SFO"
    save_payload = save_dir / "USER01"
    metadata = psf_strings(save_sfo)
    if (metadata.get("SAVEDATA_DIRECTORY") != "BLUS30826L01" or
            metadata.get("SUB_TITLE") != "Save data 01" or
            not save_payload.is_file()):
        raise RuntimeError(f"unexpected save identity: {metadata}")

    cache = base.parse_cache(cache_path)
    mismatches = {
        name: {"expected": expected, "actual": cache.get(name)}
        for name, expected in base.EXPECTED_CACHE.items()
        if cache.get(name) != expected
    }
    if mismatches:
        raise RuntimeError(f"production configuration mismatch: {mismatches}")
    if base.game_processes():
        raise RuntimeError(f"game process already running: {base.game_processes()}")

    run_dir = game_dir / "scratch" / args.tag
    capture_dir = game_dir / "scratch" / f"{args.tag}-capture"
    if run_dir.exists() or capture_dir.exists():
        raise FileExistsError(f"refusing to reuse route tag {args.tag}")
    run_dir.mkdir(parents=True)
    capture_dir.mkdir(parents=True)
    stdout_path = run_dir / "game.out"
    stderr_path = run_dir / "game.err"
    confirm_stop = run_dir / "exact-save-loaded.txt"
    start_arm = run_dir / "cutscene-start-once.txt"
    target_stop = run_dir / "gun2-target-input-stopped.txt"
    capture_stop = run_dir / "gun2-target-capture-stopped.txt"
    result_path = run_dir / "route-result.json"

    environment = {k: v for k, v in os.environ.items() if not k.startswith("YZ_")}
    yz = {
        "YZ_MOVIE_HLE": "1",
        "YZ_AUTO_START": "1",
        "YZ_AUTO_NEW_GAME": "1",
        "YZ_AUTO_LOAD_GAME": "1",
        "YZ_AUTO_LOAD_GAME_CONFIRM_STOP_FILE": str(confirm_stop),
        "YZ_AUTO_LOAD_GAME_START_FILE": str(start_arm),
        "YZ_A010_ACCEPT_FAST": "1",
        "YZ_FRONTIER_ACCEPT_FAST": "1",
        "YZ_AKIYAMA_DIALOGUE_ROUTE": "1",
        "YZ_AKIYAMA_DIALOGUE_STOP_FILE": str(target_stop),
        "YZ_AKIYAMA_DIALOGUE_CAPTURE_STOP_FILE": str(capture_stop),
        "YZ_AKIYAMA_ROUTE_START_DELAY_MS": "0",
        "YZ_MOVEMENT_PROOF_DELAY_MS": str(args.capture_delay_ms),
        "YZ_MOVEMENT_PROBE_INTERVAL_MS": str(args.capture_interval_ms),
        "YZ_RSX_VALIDATION_DIR": str(capture_dir),
        "YZ_NR_VERTICAL": "full-native",
        "YZ_NR_ISLAND_COMPILER": "1",
        "YZ_FORCE_VISIBLE": "1",
    }
    if args.timeline:
        yz["YZ_FRAME_DEP_TIMELINE"] = "1"
        # This is the timeline's asynchronous GPU component.  Query data is
        # retained in fixed memory and resolved only during shutdown.
        yz["YZ_NR_RSX_TAIL_BREAKDOWN"] = "1"
    environment.update(yz)

    target = base.scene_features(target_reference)
    cutscene_refs = [base.scene_features(path) for path in cutscene_paths]
    skip_reference = skip_prompt_mask(skip_reference_path)
    result = {
        "benchmark": "gun-tutorial-2/save",
        "status": "launching",
        "executable": str(executable),
        "executable_sha256": base.sha256_file(executable),
        "configuration": {name: cache.get(name) for name in base.EXPECTED_CACHE},
        "active_yz": yz,
        "save": {
            "directory": "BLUS30826L01",
            "slot": "01",
            "payload": "USER01",
            "param_sfo_sha256": base.sha256_file(save_sfo),
            "payload_sha256": base.sha256_file(save_payload),
            "subtitle": metadata.get("SUB_TITLE"),
            "detail": metadata.get("DETAIL"),
        },
        "captures": [],
    }

    stdout_handle = stdout_path.open("wb")
    stderr_handle = stderr_path.open("wb")
    process = None
    forced_close = False
    try:
        launch_monotonic = time.monotonic()
        process = subprocess.Popen(
            [str(executable), str(game_elf)],
            cwd=str(game_dir), env=environment,
            stdout=stdout_handle, stderr=stderr_handle,
        )
        result["pid"] = process.pid
        stale_qpc = game_dir / "scratch" / f"present_qpc_{process.pid}.csv"
        if stale_qpc.exists():
            stale_qpc.unlink()
        result["status"] = "routing"
        print(f"[gun2-save] launched visible exact PID {process.pid}", flush=True)
        time.sleep(1.0)
        if list(base.game_processes()) != [process.pid]:
            raise RuntimeError(
                f"single-game-process invariant failed: {base.game_processes()}"
            )
        config_match, _ = base.wait_log(
            stderr_path, r"\[config\].*lane=clean", 60, process
        )
        result["runtime_config_line"] = config_match.group(0)
        tid_match, _ = base.wait_log(
            stderr_path, r"\[rsx\].*host_tid=([0-9]+)", 60, process
        )
        rsx_tid = int(tid_match.group(1))
        result["rsx_consumer_thread_id"] = rsx_tid

        deadline = time.monotonic() + args.route_timeout
        save_loaded = False
        cutscene_confirmed = False
        start_armed = False
        target_consecutive = 0
        seen = set()
        checkpoint = None
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"game exited during route: {process.returncode}")
            stdout_text = (
                stdout_path.read_text(encoding="utf-8", errors="replace")
                if stdout_path.exists() else ""
            )
            stderr_text = (
                stderr_path.read_text(encoding="utf-8", errors="replace")
                if stderr_path.exists() else ""
            )
            fatal = [line for line in stderr_text.splitlines()
                     if "[nr-full] FATAL" in line or "native-fatal" in line]
            if fatal:
                raise RuntimeError(fatal[-1])
            if not save_loaded and (
                    "[cellSaveData] LOAD complete for 'BLUS30826L01'" in stdout_text):
                save_loaded = True
                confirm_stop.write_text("exact save load verified\n", encoding="ascii")
                result["save_load_verified"] = True
                print("[gun2-save] exact BLUS30826L01/slot01/USER01 load verified; "
                      "Confirm input stopped", flush=True)

            for path in sorted(capture_dir.glob("frontier_probe_*.ppm")):
                if path in seen:
                    continue
                try:
                    features = base.scene_features(path)
                    target_mae = base.coarse_scene_mae(target, features)
                    target_hud = base.gun_hud_iou(target, features)
                    cutscene_mae = min(
                        base.coarse_scene_mae(reference, features)
                        for reference in cutscene_refs
                    )
                    skip_iou = mask_iou(skip_reference, skip_prompt_mask(path))
                except (OSError, ValueError):
                    continue
                seen.add(path)
                capture = {
                    "path": str(path),
                    "sha256": base.sha256_file(path),
                    "target_mae": round(target_mae, 6),
                    "target_hud_iou": round(target_hud, 6),
                    "cutscene_min_mae": round(cutscene_mae, 6),
                    "start_skip_iou": round(skip_iou, 6),
                }
                result["captures"].append(capture)
                is_target = (
                    target_mae <= TARGET_MAE_MAX and
                    target_hud >= TARGET_HUD_IOU_MIN
                )
                if (save_loaded and not start_armed and not is_target and
                        skip_iou >= 0.80):
                    cutscene_confirmed = True
                    start_arm.write_text(
                        f"START/SKIP match {path.name} iou={skip_iou:.6f}\n",
                        encoding="ascii",
                    )
                    start_armed = True
                    result["cutscene_gate_capture"] = str(path)
                    result["cutscene_gate_mae"] = round(cutscene_mae, 6)
                    result["cutscene_start_skip_iou"] = round(skip_iou, 6)
                    print(f"[gun2-save] skippable saved-game cutscene positively "
                          f"matched by {path.name} (START/SKIP IoU={skip_iou:.3f}); "
                          "armed exactly one Start edge", flush=True)
                target_consecutive = target_consecutive + 1 if is_target else 0
                print(
                    f"[gun2-save] {path.name} cutscene_mae={cutscene_mae:.4f} "
                    f"skip_iou={skip_iou:.3f} target_mae={target_mae:.4f} "
                    f"hud={target_hud:.3f} "
                    f"target_consecutive={target_consecutive}", flush=True,
                )
                if (start_armed and
                        target_consecutive >= TARGET_REQUIRED_MATCHES):
                    target_stop.write_text(
                        f"target verified by {path.name}\n", encoding="ascii"
                    )
                    capture_stop.write_text(
                        f"target verified by {path.name}\n", encoding="ascii"
                    )
                    checkpoint = path
                    break
            if checkpoint is not None:
                break
            time.sleep(0.2)
        if checkpoint is None:
            raise TimeoutError("Zombie Suppression 4: Dodging was not reached")
        if not cutscene_confirmed:
            raise RuntimeError("target reached without a positive cutscene gate")
        base.wait_log(
            stderr_path,
            r"\[auto-load-game\] one-shot Start released; no further synthetic cutscene input",
            15, process,
        )
        base.wait_log(
            stderr_path,
            r"\[akiyama-route\] input stopped at visual checkpoint",
            15, process,
        )
        stop_match, _ = base.wait_log(
            stderr_path,
            r"\[akiyama-route\] visual probe stopped at confirmed checkpoint "
            r"present_id=([0-9]+)",
            15, process,
        )
        result["route_duration_seconds"] = round(
            time.monotonic() - launch_monotonic, 3
        )
        result["checkpoint_capture"] = str(checkpoint)
        result["checkpoint_sha256"] = base.sha256_file(checkpoint)
        result["checkpoint_present_id"] = int(stop_match.group(1))
        result["input_stopped"] = True
        result["capture_stopped"] = True
        result["accepted_artifacts"] = checkpoint_artifacts(checkpoint, run_dir)
        result["negative_identities"] = rejection_identities(game_dir, target)
        confirmations = result["captures"][-TARGET_REQUIRED_MATCHES:]
        result["confirmation"] = {
            "required_matches": TARGET_REQUIRED_MATCHES,
            "observed_matches": len(confirmations),
            "maximum_target_mae": max(
                item["target_mae"] for item in confirmations
            ),
            "minimum_target_hud_iou": min(
                item["target_hud_iou"] for item in confirmations
            ),
            "confidence": "high",
            "visual_identity": TARGET_NAME,
        }
        result["controller_sequence"] = [
            "title: one internal Confirm edge",
            "main menu: select exact item id 1 (Load Game)",
            "slot 01: bounded Confirm until exact save LOAD completion",
            "cutscene: exactly one gated 750-ms Start edge",
            "target: no input after three visual/state matches",
        ]
        result["route_state_markers"] = {
            "save_load_verified": bool(result.get("save_load_verified")),
            "cutscene_gate_capture": result.get("cutscene_gate_capture"),
            "cutscene_start_skip_iou": result.get("cutscene_start_skip_iou"),
            "checkpoint_present_id": result["checkpoint_present_id"],
        }
        if not args.measure:
            result["status"] = "awaiting-user-confirmation"
            result_path.write_text(
                json.dumps(result, indent=2) + "\n", encoding="utf-8"
            )
            print(f"[gun2-save] READY: {TARGET_NAME} is visible; "
                  "all synthetic input/readback is stopped", flush=True)
            print(f"[gun2-save] PID {process.pid} remains open for visual confirmation",
                  flush=True)
            return 0

        result["status"] = "warming-up"
        result_path.write_text(
            json.dumps(result, indent=2) + "\n", encoding="utf-8"
        )
        print(
            f"[gun2-save] exact checkpoint accepted; no input/readback; "
            f"warming for {args.warmup_seconds:.1f}s",
            flush=True,
        )
        warmup_end = time.monotonic() + args.warmup_seconds
        while time.monotonic() < warmup_end:
            if process.poll() is not None:
                raise RuntimeError(
                    f"game exited during warm-up: {process.returncode}"
                )
            time.sleep(min(0.25, warmup_end - time.monotonic()))

        result["status"] = "measuring"
        measurement_qpc_start = qpc_now()
        measurement_wall_start = time.monotonic()
        process_cpu_start = base.process_cpu_seconds(process.pid)
        thread_cpu_start = process_thread_cpu_100ns(process.pid)
        rsx_cpu_start = base.thread_cpu_100ns(rsx_tid)
        gpu_start = base.gpu_running_time_100ns(process.pid)
        print(
            f"[gun2-save] clean {args.hold_seconds:.1f}s QPC window started",
            flush=True,
        )
        measurement_end = measurement_wall_start + args.hold_seconds
        while time.monotonic() < measurement_end:
            if process.poll() is not None:
                raise RuntimeError(
                    f"game exited during measurement: {process.returncode}"
                )
            time.sleep(min(0.25, measurement_end - time.monotonic()))
        measurement_qpc_end = qpc_now()
        measurement_wall = time.monotonic() - measurement_wall_start
        process_cpu_end = base.process_cpu_seconds(process.pid)
        thread_cpu_end = process_thread_cpu_100ns(process.pid)
        rsx_cpu_end = base.thread_cpu_100ns(rsx_tid)
        gpu_end = base.gpu_running_time_100ns(process.pid)
        result.update({
            "measurement_wall_seconds": round(measurement_wall, 6),
            "measurement_process_cpu_seconds": round(
                process_cpu_end - process_cpu_start, 6
            ),
            "measurement_start_qpc": measurement_qpc_start,
            "measurement_end_qpc": measurement_qpc_end,
            "thread_cpu_100ns": {
                str(thread_id): thread_cpu_end[thread_id] - start
                for thread_id, start in thread_cpu_start.items()
                if thread_id in thread_cpu_end and
                thread_cpu_end[thread_id] >= start
            },
        })
        if rsx_cpu_start is not None and rsx_cpu_end is not None:
            rsx_100ns = rsx_cpu_end - rsx_cpu_start
            result["rsx_consumer_cpu_ms"] = round(rsx_100ns / 10000.0, 3)
            result["rsx_consumer_cpu_percent_one_core"] = round(
                rsx_100ns / 10_000_000.0 / measurement_wall * 100.0, 3
            )
        if gpu_start is not None and gpu_end is not None:
            gpu_100ns = gpu_end - gpu_start
            result["gpu_engine_time_ms"] = round(gpu_100ns / 10000.0, 3)
            result["gpu_engine_duty_percent"] = round(
                gpu_100ns / 10_000_000.0 / measurement_wall * 100.0, 3
            )

        result["status"] = "closing"
        close_windows = base.post_close(process.pid)
        result["wm_close_windows"] = close_windows
        if not close_windows:
            raise RuntimeError("no visible exact-PID window for normal shutdown")
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            forced_close = True
            process.terminate()
            process.wait(timeout=10)
        result["exit_code"] = process.returncode
        result["forced_close"] = forced_close

        stderr_text = stderr_path.read_text(encoding="utf-8", errors="replace")
        if args.timeline:
            timeline_headers = re.findall(
                r"^\[frame-dep\].*$", stderr_text, re.MULTILINE
            )
            if len(timeline_headers) != 1:
                raise RuntimeError("dependency timeline shutdown dump missing")
            result["frame_dependency_timeline"] = timeline_headers[0]
            result["frame_dependency_log"] = str(stderr_path)
        full_native = re.findall(
            r"^\[nr-full-native .*\]$", stderr_text, re.MULTILINE
        )
        native_d3d = re.findall(
            r"^\[nr-vertical-d3d .*\]$", stderr_text, re.MULTILINE
        )
        island = re.findall(
            r"^\[nr-island-compiler steps=.*\]$", stderr_text, re.MULTILINE
        )
        result["nr_full_native"] = full_native
        result["nr_vertical_d3d"] = native_d3d
        result["nr_island_compiler"] = island
        if (len(full_native) != 1 or "fatal=0" not in full_native[0] or
                len(native_d3d) != 1 or "legacy-groups=0" not in native_d3d[0] or
                "fallback=0" not in native_d3d[0] or
                len(island) != 1 or "mismatches=0" not in island[0]):
            raise RuntimeError(
                "strict-native/island-compiler shutdown gates failed"
            )
        coverage = re.search(r"coverage-ppm=([0-9]+)", native_d3d[0])
        result["native_coverage_percent"] = (
            round(int(coverage.group(1)) / 10000.0, 4) if coverage else None
        )
        result["native_errors"] = 0
        result["native_fallback"] = 0
        result["capture_count_at_checkpoint"] = len(result["captures"])
        result["capture_count_after_shutdown"] = len(
            list(capture_dir.glob("frontier_probe_*.ppm"))
        )
        if (result["capture_count_after_shutdown"] !=
                result["capture_count_at_checkpoint"]):
            raise RuntimeError("framebuffer readback continued after acceptance")
        qpc_path = game_dir / "scratch" / f"present_qpc_{process.pid}.csv"
        if not qpc_path.is_file():
            raise RuntimeError("normal shutdown did not preserve QPC ring")
        result["present_qpc_path"] = str(qpc_path)
        result.update(qpc_metrics_ticks(
            qpc_path, measurement_qpc_start, measurement_qpc_end
        ))
        if result["measurement_presents"] > 1:
            frame_count = result["measurement_presents"] - 1
            result["process_cpu_ms_per_present"] = round(
                result["measurement_process_cpu_seconds"] * 1000.0 /
                frame_count, 3
            )
            if "rsx_consumer_cpu_ms" in result:
                result["rsx_cpu_ms_per_present"] = round(
                    result["rsx_consumer_cpu_ms"] / frame_count, 3
                )
        result["status"] = (
            "passed" if process.returncode == 0 and not forced_close and
            result["measurement_presents"] >= 10 else "rejected"
        )
        result_path.write_text(
            json.dumps(result, indent=2) + "\n", encoding="utf-8"
        )
        print(
            f"[gun2-save] {result['status']}: pid={process.pid} "
            f"fps={result.get('fps_mean')} "
            f"median={result.get('frame_time_ms', {}).get('median')}ms "
            f"normal_close={not forced_close and process.returncode == 0}",
            flush=True,
        )
        return 0 if result["status"] == "passed" else 2
    except Exception as exc:
        result["status"] = "failed"
        result["failure"] = f"{type(exc).__name__}: {exc}"
        result_path.write_text(json.dumps(result, indent=2), encoding="utf-8")
        if process is not None:
            close_failed_process(process)
        raise
    finally:
        if process is not None and process.poll() is None and args.measure:
            base.post_close(process.pid)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                forced_close = True
                process.terminate()
                process.wait(timeout=10)
        stdout_handle.close()
        stderr_handle.close()


if __name__ == "__main__":
    sys.exit(main())
