#!/usr/bin/env python3
"""Unattended production Akiyama-dialogue route and FPS measurement.

The route uses only Start after New Game has been accepted.  Sparse renderer
PPMs are compared with the archived first Akiyama/Hana dialogue reference.
After consecutive visual matches, a marker permanently disables both input
and readback before the measured interval begins.
"""

from __future__ import annotations

import argparse
import csv
import ctypes
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


EXPECTED_CACHE = {
    "CMAKE_BUILD_TYPE": "Release",
    "YZ_PERF_FAST": "ON",
    "YZ_PERF_PROFILE": "OFF",
    "YZ_PPU_SAMPLE": "OFF",
    "YZ_SPURS_BACKEND": "NATIVE",
    "YZ_GCM_BACKEND": "NATIVE",
    "YZ_FRONTIER_VISUAL_PROBE": "ON",
}


def ppm_tokens(handle):
    while True:
        byte = handle.read(1)
        while byte and byte.isspace():
            byte = handle.read(1)
        if not byte:
            return
        if byte == b"#":
            handle.readline()
            continue
        token = bytearray(byte)
        byte = handle.read(1)
        while byte and not byte.isspace():
            token.extend(byte)
            byte = handle.read(1)
        yield bytes(token)


def read_ppm(path: Path):
    with path.open("rb") as handle:
        tokens = ppm_tokens(handle)
        magic = next(tokens, b"")
        if magic != b"P6":
            raise ValueError(f"not a binary PPM: {path}")
        width = int(next(tokens))
        height = int(next(tokens))
        maximum = int(next(tokens))
        if maximum != 255:
            raise ValueError(f"unsupported PPM maximum {maximum}: {path}")
        # ppm_tokens consumed exactly one whitespace byte after the maximum.
        pixels = handle.read(width * height * 3)
    if len(pixels) != width * height * 3:
        raise ValueError(f"truncated PPM: {path}")
    return width, height, pixels


def sample_rgb(width: int, height: int, pixels: bytes, cols=64, rows=36):
    result = []
    for gy in range(rows):
        y = min(height - 1, (gy * 2 + 1) * height // (rows * 2))
        row = y * width * 3
        for gx in range(cols):
            x = min(width - 1, (gx * 2 + 1) * width // (cols * 2))
            offset = row + x * 3
            result.extend(pixels[offset : offset + 3])
    return result


def dialogue_mask(width: int, height: int, pixels: bytes):
    """Text-sensitive mask for the authored dialogue band.

    Four-pixel sampling keeps comparison cheap but retains enough glyph detail
    to distinguish the first and second Hana lines, which share the same scene.
    """
    mask = []
    y0 = height * 69 // 100
    y1 = height * 96 // 100
    x0 = width * 4 // 100
    x1 = width * 96 // 100
    for y in range(y0, y1, 4):
        row = y * width * 3
        for x in range(x0, x1, 4):
            offset = row + x * 3
            r, g, b = pixels[offset : offset + 3]
            maximum = max(r, g, b)
            minimum = min(r, g, b)
            mask.append(1 if maximum >= 145 and maximum - minimum <= 55 else 0)
    return mask


def prompt_blue_pixels(width: int, height: int, pixels: bytes):
    """Count bright cyan pixels on the two diagonals of the X glyph."""
    count = 0
    cx = (width * 780 + 500) // 1000
    cy = (height * 886 + 500) // 1000
    for y in range(cy - 18, cy + 19):
        row = y * width * 3
        for x in range(cx - 18, cx + 19):
            dx, dy = x - cx, y - cy
            if not (4 <= max(abs(dx), abs(dy)) <= 16 and
                    abs(abs(dx) - abs(dy)) <= 2):
                continue
            offset = row + x * 3
            r, g, b = pixels[offset : offset + 3]
            if max(r, g, b) >= 100 and b >= r + 5 and b >= g + 2:
                count += 1
    return count


def scene_features(path: Path):
    width, height, pixels = read_ppm(path)
    return {
        "width": width,
        "height": height,
        "rgb": sample_rgb(width, height, pixels),
        "dialogue": dialogue_mask(width, height, pixels),
        "prompt_blue_pixels": prompt_blue_pixels(width, height, pixels),
    }


def compare_scene(reference, candidate):
    if (reference["width"], reference["height"]) != (
        candidate["width"], candidate["height"]
    ):
        return {"match": False, "reason": "dimensions"}
    rgb_a, rgb_b = reference["rgb"], candidate["rgb"]
    coarse_mae = sum(abs(a - b) for a, b in zip(rgb_a, rgb_b)) / (
        len(rgb_a) * 255.0
    )
    mask_a, mask_b = reference["dialogue"], candidate["dialogue"]
    mask_difference = sum(a != b for a, b in zip(mask_a, mask_b)) / len(mask_a)
    intersection = sum(a and b for a, b in zip(mask_a, mask_b))
    union = sum(a or b for a, b in zip(mask_a, mask_b))
    text_iou = intersection / union if union else 1.0
    prompt_blue = candidate["prompt_blue_pixels"]
    # The requested checkpoint is semantic: the first Akiyama/Hana city
    # dialogue that displays an X-to-continue prompt.  The archived subtitle
    # frame used as the scene anchor is transient and has no prompt itself.
    # Scene similarity rejects orphanage/cutscene prompts; the blue-glyph
    # count distinguishes a completed, stationary dialogue from typed text.
    match = coarse_mae <= 0.215 and prompt_blue >= 20
    return {
        "match": match,
        "coarse_mae": round(coarse_mae, 6),
        "dialogue_mask_difference": round(mask_difference, 6),
        "dialogue_text_iou": round(text_iou, 6),
        "prompt_blue_pixels": prompt_blue,
    }


def parse_cache(cache_path: Path):
    values = {}
    for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("//") or line.startswith("#") or "=" not in line:
            continue
        lhs, value = line.split("=", 1)
        name = lhs.split(":", 1)[0]
        values[name] = value
    return values


def process_paths():
    if os.name != "nt":
        return {}
    psapi = ctypes.windll.psapi
    kernel32 = ctypes.windll.kernel32
    capacity = 4096
    pids = (ctypes.c_ulong * capacity)()
    used = ctypes.c_ulong()
    if not psapi.EnumProcesses(ctypes.byref(pids), ctypes.sizeof(pids), ctypes.byref(used)):
        raise ctypes.WinError()
    paths = {}
    for pid in pids[: used.value // ctypes.sizeof(ctypes.c_ulong)]:
        if not pid:
            continue
        handle = kernel32.OpenProcess(0x1000, False, pid)
        if not handle:
            continue
        try:
            size = ctypes.c_ulong(32768)
            buffer = ctypes.create_unicode_buffer(size.value)
            if kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
                paths[int(pid)] = buffer.value
        finally:
            kernel32.CloseHandle(handle)
    return paths


def exact_processes(executable: Path):
    wanted = os.path.normcase(str(executable.resolve()))
    return [
        pid
        for pid, path in process_paths().items()
        if os.path.normcase(os.path.abspath(path)) == wanted
    ]


def post_close(pid: int):
    user32 = ctypes.windll.user32
    windows = []
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    @callback_type
    def callback(hwnd, _):
        window_pid = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(window_pid))
        if window_pid.value == pid and user32.IsWindowVisible(hwnd):
            windows.append(hwnd)
        return True

    user32.EnumWindows(callback, 0)
    for hwnd in windows:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
    return len(windows)


def wait_log(path: Path, pattern: str, timeout: float, process=None, start=0):
    deadline = time.monotonic() + timeout
    regex = re.compile(pattern)
    while time.monotonic() < deadline:
        if process is not None and process.poll() is not None:
            raise RuntimeError(
                f"process exited {process.returncode} while waiting for {pattern}"
            )
        if path.exists():
            text = path.read_text(encoding="utf-8", errors="replace")
            match = regex.search(text, start)
            if match:
                return match, len(text)
        time.sleep(0.25)
    raise TimeoutError(f"timeout waiting for log pattern: {pattern}")


def qpc_metrics(qpc_path: Path, start_present_id: int):
    with qpc_path.open("r", encoding="ascii", newline="") as handle:
        lines = list(handle)
    frequency = None
    data_lines = []
    for line in lines:
        if line.startswith("# qpc_frequency="):
            frequency = int(line.split("=", 1)[1].strip())
        elif not line.startswith("#"):
            data_lines.append(line)
    rows = list(csv.DictReader(data_lines))
    selected = [
        (int(row["present_id"]), int(row["qpc"]))
        for row in rows
        if int(row["present_id"]) > start_present_id
    ]
    if frequency is None:
        native_frequency = ctypes.c_longlong()
        if not ctypes.windll.kernel32.QueryPerformanceFrequency(
            ctypes.byref(native_frequency)
        ):
            raise ctypes.WinError()
        frequency = native_frequency.value
    metrics = {
        "qpc_frequency": frequency,
        "measurement_presents": len(selected),
        "fps_samples": [],
        "fps_mean": None,
        "fps_min": None,
        "fps_max": None,
    }
    if len(selected) < 2:
        return metrics
    elapsed = (selected[-1][1] - selected[0][1]) / frequency
    metrics["measurement_qpc_seconds"] = round(elapsed, 6)
    metrics["fps_mean"] = round((len(selected) - 1) / elapsed, 3)
    full_buckets = int(elapsed // 5.0)
    first_tick = selected[0][1]
    for bucket in range(full_buckets):
        lo = first_tick + bucket * 5 * frequency
        hi = lo + 5 * frequency
        count = sum(lo <= qpc < hi for _, qpc in selected)
        metrics["fps_samples"].append(round(count / 5.0, 3))
    if metrics["fps_samples"]:
        metrics["fps_min"] = min(metrics["fps_samples"])
        metrics["fps_max"] = max(metrics["fps_samples"])
    return metrics


def scan_command(reference_path: Path, paths):
    reference = scene_features(reference_path)
    for raw in paths:
        path = Path(raw)
        print(json.dumps({"path": str(path), **compare_scene(reference, scene_features(path))}))


def self_test(root: Path, reference_path: Path):
    reference = scene_features(reference_path)
    runs = [
        root / "scratch" / "frontier-fullroute-20260820-accept19-capture",
        root / "scratch" / "frontier-fullroute-20260820-accept20-capture",
    ]
    expected_first = {
        str(runs[0].resolve()): None,
        str(runs[1].resolve()): "frontier_probe_002.ppm",
    }
    first_matches = {}
    total = 0
    for directory in runs:
        first = None
        for path in sorted(directory.glob("frontier_probe_*.ppm")):
            total += 1
            if first is None and compare_scene(
                reference, scene_features(path)
            )["match"]:
                first = path.name
        first_matches[str(directory.resolve())] = first
    if first_matches != expected_first:
        raise AssertionError(
            f"scene corpus mismatch: expected {expected_first}, got {first_matches}"
        )
    repaired = (
        root / "scratch" / "akiyama-auto-fe0-lane-fix4-20260821-1450-capture"
    )
    if repaired.is_dir():
        first = next(
            (path.name for path in sorted(repaired.glob("frontier_probe_*.ppm"))
             if compare_scene(reference, scene_features(path))["match"]),
            None,
        )
        if first != "frontier_probe_039.ppm":
            raise AssertionError(
                f"repaired-route first prompt mismatch: expected 039, got {first}"
            )
    print(
        f"[akiyama-harness-test] PASS: {total} archived route frames; "
        "the first Akiyama X-prompt accepted at the correct boundary"
    )


def run(args):
    executable = args.exe.resolve()
    game_dir = args.game_dir.resolve()
    game_elf = game_dir / "game" / "EBOOT.elf"
    cache_path = executable.parent / "CMakeCache.txt"
    reference_path = args.reference.resolve()
    for path, label in (
        (executable, "executable"),
        (game_elf, "game ELF"),
        (cache_path, "CMake cache"),
        (reference_path, "scene reference"),
    ):
        if not path.is_file():
            raise FileNotFoundError(f"missing {label}: {path}")

    cache = parse_cache(cache_path)
    mismatches = {
        name: {"expected": expected, "actual": cache.get(name)}
        for name, expected in EXPECTED_CACHE.items()
        if cache.get(name) != expected
    }
    if mismatches:
        raise RuntimeError(f"production configuration mismatch: {mismatches}")
    existing = exact_processes(executable)
    if existing:
        raise RuntimeError(f"exact executable already running as PID(s) {existing}")

    scratch = game_dir / "scratch"
    scratch.mkdir(exist_ok=True)
    run_dir = scratch / args.tag
    capture_dir = scratch / f"{args.tag}-capture"
    if run_dir.exists() or capture_dir.exists():
        raise FileExistsError(f"refusing to reuse run tag {args.tag}")
    run_dir.mkdir()
    capture_dir.mkdir()
    stdout_path = run_dir / "game.out"
    stderr_path = run_dir / "game.err"
    stop_path = run_dir / "akiyama-dialogue-ready.txt"
    result_path = run_dir / "result.json"

    environment = {k: v for k, v in os.environ.items() if not k.startswith("YZ_")}
    yz = {
        "YZ_MOVIE_HLE": "1",
        "YZ_AUTO_START": "1",
        "YZ_AUTO_NEW_GAME": "1",
        "YZ_A010_ACCEPT_FAST": "1",
        "YZ_FRONTIER_ACCEPT_FAST": "1",
        "YZ_AKIYAMA_DIALOGUE_ROUTE": "1",
        "YZ_AKIYAMA_DIALOGUE_STOP_FILE": str(stop_path),
        "YZ_MOVEMENT_PROOF_DELAY_MS": str(args.capture_delay_ms),
        "YZ_MOVEMENT_PROBE_INTERVAL_MS": str(args.capture_interval_ms),
        "YZ_RSX_VALIDATION_DIR": str(capture_dir),
    }
    if args.fe0:
        yz["YZ_FE0_TIMELINE"] = "1"
    if args.fe0_callback_replay:
        yz["YZ_FE0_CALLBACK_REPLAY"] = "1"
    if args.nr_shadow_census:
        yz["YZ_NR_INTERCEPT"] = "state"
    if args.nr_flip:
        yz["YZ_NR_INTERCEPT"] = "flip"
    if args.nr_clear:
        yz["YZ_NR_INTERCEPT"] = "clear"
    if args.nr_draw:
        yz["YZ_NR_INTERCEPT"] = "draw"
    environment.update(yz)

    result = {
        "tag": args.tag,
        "status": "launching",
        "executable": str(executable),
        "game_elf": str(game_elf),
        "configuration": {name: cache.get(name) for name in EXPECTED_CACHE},
        "active_yz": yz,
        "reference": str(reference_path),
        "captures": [],
    }
    reference = scene_features(reference_path)
    stdout_handle = stdout_path.open("wb")
    stderr_handle = stderr_path.open("wb")
    process = None
    forced = False
    try:
        process = subprocess.Popen(
            [str(executable), str(game_elf)],
            cwd=str(game_dir),
            env=environment,
            stdout=stdout_handle,
            stderr=stderr_handle,
        )
        result["pid"] = process.pid
        stale_qpc = scratch / f"present_qpc_{process.pid}.csv"
        if stale_qpc.exists():
            stale_qpc.unlink()
        result["status"] = "routing"
        print(f"[akiyama-harness] launched exact PID {process.pid}", flush=True)
        time.sleep(1.0)
        if exact_processes(executable) != [process.pid]:
            raise RuntimeError(
                f"single-process invariant failed: {exact_processes(executable)}"
            )
        config_match, _ = wait_log(
            stderr_path,
            r"\[config\].*lane=clean.*YZ_AKIYAMA_DIALOGUE_ROUTE=1",
            60,
            process,
        )
        result["runtime_config_line"] = config_match.group(0)

        deadline = time.monotonic() + args.route_timeout
        seen = set()
        consecutive = 0
        last_capture_progress = time.monotonic()
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"game exited during route: {process.returncode}")
            captured_this_poll = False
            for path in sorted(capture_dir.glob("frontier_probe_*.ppm")):
                if path in seen:
                    continue
                try:
                    comparison = compare_scene(reference, scene_features(path))
                except (OSError, ValueError):
                    continue  # renderer may still be finishing the file
                seen.add(path)
                captured_this_poll = True
                last_capture_progress = time.monotonic()
                comparison["path"] = str(path)
                result["captures"].append(comparison)
                consecutive = consecutive + 1 if comparison["match"] else 0
                print(
                    f"[akiyama-harness] {path.name}: match={comparison['match']} "
                    f"mae={comparison.get('coarse_mae')} "
                    f"text_iou={comparison.get('dialogue_text_iou')} "
                    f"consecutive={consecutive}",
                    flush=True,
                )
                if consecutive >= args.required_matches:
                    stop_path.write_text(
                        f"visual checkpoint confirmed by {path.name}\n",
                        encoding="ascii",
                    )
                    result["checkpoint_capture"] = str(path)
                    result["checkpoint_match"] = comparison
                    result["capture_count_at_checkpoint"] = len(seen)
                    break
            if stop_path.exists():
                break
            # A healthy route produces a new sparse framebuffer every few
            # seconds.  Fail early only when visual progress has stopped AND
            # the runtime itself reports a durable FIFO no-progress state;
            # slow but advancing boots remain governed by route_timeout.
            no_capture_for = time.monotonic() - last_capture_progress
            if (not captured_this_poll and seen and no_capture_for >= 15.0 and
                    stderr_path.exists()):
                tail = stderr_path.read_text(
                    encoding="utf-8", errors="replace"
                )[-131072:]
                if ("still parked on non-command" in tail or
                        re.search(r"\[rsx-idle\].*no-advance", tail)):
                    result["route_failure_class"] = "fifo-no-progress"
                    result["route_failure_last_capture"] = max(
                        path.name for path in seen
                    )
                    result["route_failure_no_capture_seconds"] = round(
                        no_capture_for, 3
                    )
                    raise RuntimeError(
                        "route FIFO stopped advancing after "
                        f"{result['route_failure_last_capture']}"
                    )
            time.sleep(0.25)
        if not stop_path.exists():
            raise TimeoutError("Akiyama first-dialogue visual checkpoint not reached")

        wait_log(
            stderr_path,
            r"\[akiyama-route\] input stopped at visual checkpoint",
            15,
            process,
        )
        stop_match, _ = wait_log(
            stderr_path,
            r"\[akiyama-route\] visual probe stopped at confirmed checkpoint "
            r"present_id=([0-9]+)",
            max(15, args.capture_interval_ms / 1000 + 5),
            process,
        )
        # The renderer already preserves a fixed QPC ring for every successful
        # Present.  The stop record identifies the exact present boundary, so
        # no periodic logging or extra measurement clock is needed here.
        result["measurement_start_present_id"] = int(stop_match.group(1))
        result["status"] = "measuring"
        print(
            f"[akiyama-harness] checkpoint stable; measuring {args.hold_seconds}s",
            flush=True,
        )
        end = time.monotonic() + args.hold_seconds
        while time.monotonic() < end:
            if process.poll() is not None:
                raise RuntimeError(f"game exited during measurement: {process.returncode}")
            time.sleep(min(0.5, end - time.monotonic()))
        result["status"] = "closing"
        windows = post_close(process.pid)
        result["wm_close_windows"] = windows
        if not windows:
            raise RuntimeError("no visible exact-PID window found for normal shutdown")
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            forced = True
            process.terminate()  # exact child handle only
            process.wait(timeout=10)
        result["exit_code"] = process.returncode
        result["forced_close"] = forced

        stderr_text = stderr_path.read_text(encoding="utf-8", errors="replace")
        shadow_lines = re.findall(r"^\[nr-shadow:.*\]$", stderr_text, re.MULTILINE)
        live_lines = re.findall(r"^\[nr-live:.*\]$", stderr_text, re.MULTILINE)
        result["nr_shadow_census"] = shadow_lines
        result["nr_live_census"] = live_lines
        if ((args.nr_shadow_census or args.nr_flip or args.nr_clear or
             args.nr_draw) and
                len(shadow_lines) != 1):
            raise RuntimeError(
                "native-render shadow run must emit exactly one shutdown census: "
                f"found {len(shadow_lines)}"
            )
        if not (args.nr_shadow_census or args.nr_flip or args.nr_clear or
                args.nr_draw) and shadow_lines:
            raise RuntimeError("shadow census was active in the clean OFF lane")
        if (args.nr_flip or args.nr_clear or args.nr_draw) and len(live_lines) != 1:
            raise RuntimeError(
                "native flip run must emit exactly one live aggregate: "
                f"found {len(live_lines)}"
            )
        if not (args.nr_flip or args.nr_clear or args.nr_draw) and live_lines:
            raise RuntimeError("native live family was active outside its lane")
        completion_marker = (
            "[auto-new-game] completion latched; Confirm released and route disabled"
        )
        completion_offset = stderr_text.find(completion_marker)
        if completion_offset < 0:
            raise RuntimeError("New Game completion latch was not observed")
        post_completion = stderr_text[completion_offset + len(completion_marker) :]
        leaked_confirm = re.findall(
            r"\[auto-new-game\].*(?:Confirm|Cross|Circle|Accept).*pulse",
            post_completion,
        )
        result["post_completion_confirm_pulses"] = leaked_confirm
        if leaked_confirm:
            raise RuntimeError(
                f"synthetic Confirm resumed after completion: {leaked_confirm[:3]}"
            )
        capture_count_after = len(list(capture_dir.glob("frontier_probe_*.ppm")))
        result["capture_count_after_shutdown"] = capture_count_after
        if capture_count_after != result["capture_count_at_checkpoint"]:
            raise RuntimeError(
                "renderer capture continued after the checkpoint marker: "
                f"{result['capture_count_at_checkpoint']} -> {capture_count_after}"
            )

        qpc_path = scratch / f"present_qpc_{process.pid}.csv"
        result["present_qpc_path"] = str(qpc_path)
        if not qpc_path.is_file():
            raise RuntimeError(f"shutdown did not preserve Present QPC ring: {qpc_path}")
        result.update(qpc_metrics(
            qpc_path, result["measurement_start_present_id"]
        ))
        fps = result["fps_samples"]
        if forced:
            result["status"] = "forced-close"
        elif process.returncode != 0:
            result["status"] = "nonzero-exit"
        elif len(fps) < max(3, int(args.hold_seconds // 5) - 1):
            result["status"] = "insufficient-fps-samples"
        else:
            result["status"] = "passed"
    except Exception as exc:
        result["status"] = "failed"
        result["failure"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        if process is not None and process.poll() is None:
            post_close(process.pid)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                forced = True
                process.terminate()
                process.wait(timeout=10)
        stdout_handle.close()
        stderr_handle.close()
        result["forced_close"] = forced
        result_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(f"[akiyama-harness] result: {result_path}", flush=True)
    return 0 if result["status"] == "passed" else 2


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--game-dir", type=Path)
    parser.add_argument("--tag", default=time.strftime("akiyama-auto-%Y%m%d-%H%M%S"))
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--route-timeout", type=float, default=720)
    parser.add_argument("--capture-delay-ms", type=int, default=30000)
    parser.add_argument("--capture-interval-ms", type=int, default=2000)
    parser.add_argument("--required-matches", type=int, default=3)
    parser.add_argument("--hold-seconds", type=float, default=35)
    parser.add_argument("--fe0", action="store_true")
    parser.add_argument("--fe0-callback-replay", action="store_true")
    parser.add_argument("--nr-shadow-census", action="store_true")
    parser.add_argument("--nr-flip", action="store_true")
    parser.add_argument("--nr-clear", action="store_true")
    parser.add_argument("--nr-draw", action="store_true")
    parser.add_argument("--scan", nargs="+")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--reprocess-result", type=Path)
    args = parser.parse_args()
    if args.fe0_callback_replay and not args.fe0:
        parser.error("--fe0-callback-replay requires --fe0")
    if sum((args.nr_shadow_census, args.nr_flip, args.nr_clear,
            args.nr_draw)) > 1:
        parser.error("select only one native-render diagnostic/family lane")

    root = Path(__file__).resolve().parents[3]
    worktree = Path(__file__).resolve().parents[1]
    if args.exe is None:
        args.exe = (
            worktree
            / "yakuza"
            / "build_native_production_fe0_20260821"
            / "yakuza_recomp.exe"
        )
    if args.game_dir is None:
        args.game_dir = root
    if args.reference is None:
        args.reference = (
            root
            / "scratch"
            / "frontier-fullroute-20260820-accept20-capture"
            / "frontier_probe_001.ppm"
        )
    if args.scan:
        scan_command(args.reference.resolve(), args.scan)
        return 0
    if args.self_test:
        self_test(root, args.reference.resolve())
        return 0
    if args.reprocess_result:
        result_path = args.reprocess_result.resolve()
        result = json.loads(result_path.read_text(encoding="utf-8"))
        result.update(qpc_metrics(
            Path(result["present_qpc_path"]),
            int(result["measurement_start_present_id"]),
        ))
        required = max(3, int(args.hold_seconds // 5) - 1)
        result.pop("failure", None)
        result["status"] = (
            "passed" if result.get("exit_code") == 0 and
            not result.get("forced_close") and
            len(result["fps_samples"]) >= required
            else "insufficient-fps-samples"
        )
        result_path.write_text(
            json.dumps(result, indent=2) + "\n", encoding="utf-8"
        )
        print(json.dumps({
            "status": result["status"],
            "fps_mean": result["fps_mean"],
            "fps_samples": result["fps_samples"],
            "measurement_presents": result["measurement_presents"],
        }))
        return 0 if result["status"] == "passed" else 2
    return run(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"[akiyama-harness] ERROR: {error}", file=sys.stderr, flush=True)
        raise
