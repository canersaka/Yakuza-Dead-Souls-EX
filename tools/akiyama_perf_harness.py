#!/usr/bin/env python3
"""Unattended production route and visual/QPC performance measurement.

The route uses only Start after New Game has been accepted.  Sparse renderer
PPMs are compared with the archived first Akiyama/Hana dialogue reference.
After consecutive visual matches, a marker permanently disables both input
and readback before the measured interval begins.
"""

from __future__ import annotations

import argparse
import csv
import ctypes
from ctypes import wintypes
import hashlib
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
    "YZ_A010_FUNC_TRACE": "OFF",
    "YZ_RECOMP_OPT": "ON",
    "YZ_SPU_FAST": "ON",
    "YZ_SPU_FAST_CRI": "ON",
    "YZ_SPU_FAST_WKL4": "ON",
    "YZ_SPU_FAST_SPUIMG": "ON",
    "YZ_SPU_FAST_JOB_A": "ON",
    "YZ_SPU_FAST_JOB_B": "ON",
    "YZ_SPU_FAST_JOB_C": "ON",
    "YZ_SPU_FAST_JOB_D": "ON",
    "YZ_SPU_FAST_JOB_E": "ON",
    "YZ_SPU_FAST_ORPHANAGE": "ON",
    "YZ_SPU_FAST_SPURS_EXPERIMENTAL": "OFF",
    "YZ_SPU_SIMD_ABSDB": "ON",
    "YZ_SPU_SIMD_XFLOAT": "ON",
    "YZ_WKL4_CYCLE_DIAGNOSTIC": "OFF",
}


def process_cpu_seconds(pid: int) -> float:
    """Return aggregate kernel+user CPU for one exact process."""
    process_query_limited_information = 0x1000
    handle = ctypes.windll.kernel32.OpenProcess(
        process_query_limited_information, False, pid
    )
    if not handle:
        raise ctypes.WinError()
    try:
        created = wintypes.FILETIME()
        exited = wintypes.FILETIME()
        kernel = wintypes.FILETIME()
        user = wintypes.FILETIME()
        if not ctypes.windll.kernel32.GetProcessTimes(
            handle,
            ctypes.byref(created), ctypes.byref(exited),
            ctypes.byref(kernel), ctypes.byref(user),
        ):
            raise ctypes.WinError()
        def ticks(value):
            return (value.dwHighDateTime << 32) | value.dwLowDateTime
        return (ticks(kernel) + ticks(user)) / 10_000_000.0
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


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


def region_black_fraction(width: int, height: int, pixels: bytes, box):
    """Fraction of near-black pixels in a normalized RGB image box."""
    x0, y0, x1, y1 = box
    left = max(0, min(width, int(width * x0)))
    right = max(left, min(width, int(width * x1)))
    top = max(0, min(height, int(height * y0)))
    bottom = max(top, min(height, int(height * y1)))
    black = 0
    total = 0
    for y in range(top, bottom):
        row = y * width * 3
        for x in range(left, right):
            offset = row + x * 3
            r, g, b = pixels[offset : offset + 3]
            # Integer Rec.709 luma < 12. This catches the known solid-black
            # character fill while tolerating dark fabric and background.
            if 2126 * r + 7152 * g + 722 * b < 120000:
                black += 1
            total += 1
    return black / total if total else 1.0


def gun_hud_mask(width: int, height: int, pixels: bytes):
    """Fixed-screen pale-glyph mask for the post-Frontier gun HUD.

    Full-frame colour distance is deliberately insufficient here: bright city
    dialogue frames can be closer to the archived gun scene than a different
    camera angle inside the gun tutorial.  The health/ammo panel and KILLS
    counter occupy invariant screen-space rectangles, so their glyph overlap
    is a much stronger semantic gate and is independent of world geometry.
    """
    mask = []
    boxes_1280x720 = (
        (40, 35, 390, 145),
        (960, 115, 1240, 215),
    )
    for left, top, right, bottom in boxes_1280x720:
        x0 = left * width // 1280
        x1 = right * width // 1280
        y0 = top * height // 720
        y1 = bottom * height // 720
        step_x = max(1, width // 640)
        step_y = max(1, height // 360)
        for y in range(y0, y1, step_y):
            row = y * width * 3
            for x in range(x0, x1, step_x):
                offset = row + x * 3
                r, g, b = pixels[offset : offset + 3]
                maximum = max(r, g, b)
                minimum = min(r, g, b)
                mask.append(1 if maximum > 160 and maximum - minimum < 65 else 0)
    return mask


def gun_hud_iou(reference, candidate):
    mask_a = reference["gun_hud"]
    mask_b = candidate["gun_hud"]
    if len(mask_a) != len(mask_b):
        return 0.0
    intersection = sum(a and b for a, b in zip(mask_a, mask_b))
    union = sum(a or b for a, b in zip(mask_a, mask_b))
    return intersection / union if union else 0.0


def scene_features(path: Path):
    width, height, pixels = read_ppm(path)
    rgb = sample_rgb(width, height, pixels)
    return {
        "width": width,
        "height": height,
        "rgb": rgb,
        "mean_rgb": sum(rgb) / (len(rgb) * 255.0),
        "dialogue": dialogue_mask(width, height, pixels),
        "prompt_blue_pixels": prompt_blue_pixels(width, height, pixels),
        # Stable torso boxes shared by the first Hana X-prompt cameras. The
        # previous coarse whole-frame gate missed black characters because the
        # bright city background dominated its mean and sampled distance.
        "character_black": {
            "akiyama": region_black_fraction(
                width, height, pixels, (0.27, 0.31, 0.40, 0.66)
            ),
            "hana": region_black_fraction(
                width, height, pixels, (0.58, 0.35, 0.70, 0.69)
            ),
        },
        "gun_hud": gun_hud_mask(width, height, pixels),
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
    reference_mean = reference["mean_rgb"]
    scene_mean_ratio = (candidate["mean_rgb"] / reference_mean
                        if reference_mean else 0.0)
    character_black = candidate["character_black"]
    characters_intact = (
        character_black["akiyama"] <= 0.45 and
        character_black["hana"] <= 0.45
    )
    # The requested checkpoint is semantic: the first Akiyama/Hana city
    # dialogue that displays an X-to-continue prompt.  The archived subtitle
    # frame used as the scene anchor is transient and has no prompt itself.
    # Scene similarity rejects orphanage/cutscene prompts; the blue-glyph
    # count distinguishes a completed, stationary dialogue from typed text.
    # The luminance ratio rejects the captured native-clear failure: its
    # dialogue and prompt survived but both characters and almost all scene
    # lighting were absent (0.10x reference brightness). A prompt alone is
    # never a visual pass. The lower bound remains tolerant of camera/exposure
    # variation in the archived healthy prompt corpus.
    prompt_scene = (coarse_mae <= 0.21 and mask_difference <= 0.05 and
                    0.65 <= scene_mean_ratio <= 1.5 and prompt_blue >= 20)
    arrival = (coarse_mae <= 0.18 and mask_difference <= 0.05 and
               0.65 <= scene_mean_ratio <= 1.5 and characters_intact)
    match = (arrival and prompt_blue >= 20)
    return {
        "match": match,
        "arrival": arrival,
        # This is deliberately not an acceptance result.  It exists only so
        # a fixed-memory renderer oracle can stop and shut down cleanly at the
        # exact prompt even when the character-integrity gate rejects it.
        "prompt_scene": prompt_scene,
        "coarse_mae": round(coarse_mae, 6),
        "dialogue_mask_difference": round(mask_difference, 6),
        "dialogue_text_iou": round(text_iou, 6),
        "prompt_blue_pixels": prompt_blue,
        "scene_mean_ratio": round(scene_mean_ratio, 6),
        "character_black_fraction": {
            key: round(value, 6) for key, value in character_black.items()
        },
        "characters_intact": characters_intact,
    }


def coarse_scene_mae(reference, candidate):
    if (reference["width"], reference["height"]) != (
        candidate["width"], candidate["height"]
    ):
        return None
    return sum(
        abs(a - b) for a, b in zip(reference["rgb"], candidate["rgb"])
    ) / (len(reference["rgb"]) * 255.0)


MULTI_SCENE_ANCHORS = {
    "beach": {"serials": (11, 12, 13)},
    "orphanage_sign": {"serials": (20, 21, 22)},
    "sink": {"serials": (23, 24)},
    # Title cards are mostly black, so their generic coarse distance from a
    # blank transition frame is deceptively small.  Require a near-exact
    # authored card match for these two anchors.
    "part_title": {"serials": (27, 28), "maximum_mae": 0.02},
    "chapter_title": {"serials": (30, 31), "maximum_mae": 0.02},
}


def locate_anchor(captures, references, minimum_serial, maximum_mae):
    best = None
    for capture in captures:
        if capture["serial"] < minimum_serial or capture.get("present_id") is None:
            continue
        features = scene_features(Path(capture["path"]))
        mae = min(
            value for value in (
                coarse_scene_mae(reference, features)
                for reference in references
            ) if value is not None
        )
        if mae <= maximum_mae:
            return {
                "serial": capture["serial"],
                "present_id": capture["present_id"],
                "path": capture["path"],
                "coarse_mae": round(mae, 6),
            }
        if best is None or mae < best["coarse_mae"]:
            best = {
                "serial": capture["serial"],
                "present_id": capture["present_id"],
                "path": capture["path"],
                "coarse_mae": round(mae, 6),
            }
    return {"match": False, "best": best}


def multi_scene_metrics(result, qpc_path, reference_dir, maximum_mae):
    reference_dir = reference_dir.resolve()
    anchor_refs = {}
    for name, spec in MULTI_SCENE_ANCHORS.items():
        serials = spec["serials"]
        paths = [
            reference_dir / f"frontier_probe_{serial:03d}.ppm"
            for serial in serials
        ]
        missing = [str(path) for path in paths if not path.is_file()]
        if missing:
            raise FileNotFoundError(
                f"missing multi-scene {name} reference(s): {missing}"
            )
        anchor_refs[name] = [scene_features(path) for path in paths]

    captures = result["captures"]
    anchors = {}
    next_serial = 1
    for name, spec in MULTI_SCENE_ANCHORS.items():
        anchor = locate_anchor(
            captures, anchor_refs[name], next_serial,
            min(maximum_mae, spec.get("maximum_mae", maximum_mae)),
        )
        anchors[name] = anchor
        if anchor.get("present_id") is not None:
            next_serial = anchor["serial"] + 1
    result["multi_scene_reference_dir"] = str(reference_dir)
    result["multi_scene_anchor_mae"] = maximum_mae
    result["multi_scene_anchors"] = anchors

    windows = {
        "orphanage_route": ("beach", "sink"),
        "orphanage_moving_exterior": ("beach", "orphanage_sign"),
        "part_to_chapter_transition": ("part_title", "chapter_title"),
    }
    scenes = {}
    for scene_name, (start_name, end_name) in windows.items():
        start = anchors[start_name]
        end = anchors[end_name]
        valid = (
            start.get("present_id") is not None and
            end.get("present_id") is not None and
            end["present_id"] > start["present_id"] + 1
        )
        scene = {
            "kind": "moving",
            "route_valid": valid,
            "start_anchor": start,
            "end_anchor": end,
        }
        if valid:
            scene.update(qpc_metrics(
                qpc_path,
                start["present_id"],
                end["present_id"] - 1,
                bucket_seconds=1.0,
            ))
        scenes[scene_name] = scene

    stationary = qpc_metrics(
        qpc_path, int(result["measurement_start_present_id"]),
        bucket_seconds=5.0,
    )
    stationary.update({
        "kind": "stationary",
        "route_valid": result.get("checkpoint_match", {}).get("match", False),
        "input_stopped": True,
        "readback_stopped": (
            result.get("capture_count_at_checkpoint") ==
            result.get("capture_count_after_shutdown")
        ),
    })
    scenes["akiyama_hana_prompt"] = stationary
    result["scenes"] = scenes
    return scenes


def parse_cache(cache_path: Path):
    values = {}
    for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("//") or line.startswith("#") or "=" not in line:
            continue
        lhs, value = line.split("=", 1)
        name = lhs.split(":", 1)[0]
        values[name] = value
    return values


def sha256_file(path: Path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


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


def game_processes():
    """Return every live yakuza_recomp process without mutating any of them."""
    return {
        pid: path
        for pid, path in process_paths().items()
        if os.path.basename(path).lower() == "yakuza_recomp.exe"
    }


def visible_windows_for_pid(pid: int):
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
    return windows


def capture_process_window(pid: int, path: Path):
    """Capture the largest visible window owned by exactly *pid*.

    This is a route-health discriminator only. Scene acceptance still uses the
    renderer-owned framebuffer PPMs, which are independent of desktop state.
    """
    if os.name != "nt":
        return None
    user32 = ctypes.windll.user32
    rect_type = wintypes.RECT
    candidates = []
    for hwnd in visible_windows_for_pid(pid):
        rect = rect_type()
        if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
            continue
        width = max(0, rect.right - rect.left)
        height = max(0, rect.bottom - rect.top)
        if width and height:
            candidates.append((
                width * height, hwnd,
                (rect.left, rect.top, rect.right, rect.bottom),
            ))
    if not candidates:
        return None
    _, hwnd, bounds = max(candidates)
    try:
        from PIL import ImageGrab

        image = ImageGrab.grab(window=int(hwnd), include_layered_windows=True)
        if image.width <= 0 or image.height <= 0:
            return None
        method = "window"
        sample = image.convert("RGB").resize((64, 36)).tobytes()
        # Pillow's HWND capture can return a synthetic all-white surface for
        # this D3D12 window even while the visible client area is rendering.
        # Fall back to the exact screen rectangle; never use a blank capture
        # as evidence that the game image itself stopped progressing.
        if not window_sample_usable(sample):
            screen = ImageGrab.grab(bbox=bounds, all_screens=True)
            screen_sample = screen.convert("RGB").resize((64, 36)).tobytes()
            if window_sample_usable(screen_sample):
                image = screen
                sample = screen_sample
                method = "screen-bounds"
        path.parent.mkdir(parents=True, exist_ok=True)
        image.save(path)
        return {
            "path": str(path),
            "width": image.width,
            "height": image.height,
            "sample_sha256": hashlib.sha256(sample).hexdigest(),
            "mean_channel": round(sum(sample) / len(sample), 3),
            "sample_range": max(sample) - min(sample),
            "capture_method": method,
            "usable": window_sample_usable(sample),
            "sample": sample,
        }
    except (OSError, ImportError):
        return None


def sample_mae(first: bytes, second: bytes):
    if not first or len(first) != len(second):
        return float("inf")
    return sum(abs(a - b) for a, b in zip(first, second)) / len(first)


def window_sample_usable(sample: bytes):
    if not sample:
        return False
    # Uniform black is a valid authored transition/loading image. Uniform
    # white is the observed failed HWND-capture sentinel, not a useful visual
    # route oracle.
    return not (sum(sample) / len(sample) >= 252.0 and
                max(sample) - min(sample) <= 6)


def post_close(pid: int):
    windows = visible_windows_for_pid(pid)
    for hwnd in windows:
        ctypes.windll.user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
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


def percentile(sorted_values, fraction):
    if not sorted_values:
        return None
    position = (len(sorted_values) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(sorted_values) - 1)
    weight = position - lower
    return sorted_values[lower] * (1.0 - weight) + sorted_values[upper] * weight


def qpc_metrics(qpc_path: Path, start_present_id: int, end_present_id=None,
                bucket_seconds=5.0):
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
    selected = []
    for row in rows:
        present_id = int(row["present_id"])
        if present_id <= start_present_id:
            continue
        if end_present_id is not None and present_id > end_present_id:
            continue
        selected.append((present_id, int(row["qpc"])))
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
        "frame_time_ms": {},
    }
    if len(selected) < 2:
        return metrics
    elapsed = (selected[-1][1] - selected[0][1]) / frequency
    metrics["measurement_qpc_seconds"] = round(elapsed, 6)
    metrics["fps_mean"] = round((len(selected) - 1) / elapsed, 3)
    intervals = sorted(
        (selected[index][1] - selected[index - 1][1]) * 1000.0 / frequency
        for index in range(1, len(selected))
    )
    metrics["frame_time_ms"] = {
        "min": round(intervals[0], 3),
        "median": round(percentile(intervals, 0.5), 3),
        "p95": round(percentile(intervals, 0.95), 3),
        "p99": round(percentile(intervals, 0.99), 3),
        "max": round(intervals[-1], 3),
    }
    full_buckets = int(elapsed // bucket_seconds)
    first_tick = selected[0][1]
    for bucket in range(full_buckets):
        lo = first_tick + int(bucket * bucket_seconds * frequency)
        hi = first_tick + int((bucket + 1) * bucket_seconds * frequency)
        count = sum(lo <= qpc < hi for _, qpc in selected)
        metrics["fps_samples"].append(round(count / bucket_seconds, 3))
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
    if window_sample_usable(bytes([255]) * 64):
        raise AssertionError("all-white failed capture accepted as visual")
    if not window_sample_usable(bytes([0]) * 64):
        raise AssertionError("authored black transition rejected as visual")
    reference = scene_features(reference_path)
    runs = [
        root / "scratch" / "frontier-fullroute-20260820-accept19-capture",
        root / "scratch" / "frontier-fullroute-20260820-accept20-capture",
    ]
    expected_first = {
        str(runs[0].resolve()): None,
        # The older accept20 camera is outside the intentionally tighter
        # whole-scene distance bound. It remains a healthy character sample,
        # but is no longer the checkpoint reference.
        str(runs[1].resolve()): None,
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
    healthy_prompt = (
        root / "scratch" / "native-coherent-pass-566febd-hana-20260824-capture"
        / "frontier_probe_006.ppm"
    )
    rejected_prompt = (
        root / "scratch"
        / "native-coherent-flow-txl-72a0463-hana-r2-20260824-capture"
        / "frontier_probe_006.ppm"
    )
    if healthy_prompt.is_file():
        healthy_result = compare_scene(reference, scene_features(healthy_prompt))
        if not healthy_result["match"] or not healthy_result["characters_intact"]:
            raise AssertionError(
                f"healthy Hana prompt rejected: {healthy_result}"
            )
    if rejected_prompt.is_file():
        rejected_result = compare_scene(reference, scene_features(rejected_prompt))
        if (rejected_result["match"] or rejected_result["characters_intact"] or
                not rejected_result["prompt_scene"]):
            raise AssertionError(
                f"black-character Hana prompt escaped gate: {rejected_result}"
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

    gun_dir = (
        root / "scratch" / "frontier-fullroute-20260820-accept20-capture"
    )
    gun_refs = [
        scene_features(gun_dir / f"frontier_probe_{serial:03d}.ppm")
        for serial in (20, 21, 23, 28)
    ]
    stable = scene_features(gun_dir / "frontier_probe_028.ppm")
    best = min(coarse_scene_mae(reference, stable) for reference in gun_refs)
    best_hud = max(gun_hud_iou(reference, stable) for reference in gun_refs)
    if best > 0.22:
        raise AssertionError(f"archived gun reference mismatch: {best}")
    if best_hud < 0.20:
        raise AssertionError(f"archived gun HUD mismatch: {best_hud}")
    hana_path = (
        root / "scratch" / "native-vertical-shadow-vp-gun-20260822-f-capture"
        / "frontier_probe_028.ppm"
    )
    hana_hud = None
    if hana_path.is_file():
        hana = scene_features(hana_path)
        hana_hud = max(gun_hud_iou(reference, hana) for reference in gun_refs)
        if hana_hud >= 0.20:
            raise AssertionError(
                f"Hana dialogue falsely recognized as gun HUD: {hana_hud}"
            )
    print(
        "[akiyama-harness-test] PASS: archived leg-3 gun tutorial frame "
        f"recognized (best coarse MAE {best:.6f}, HUD IoU {best_hud:.6f})" +
        (f"; Hana dialogue rejected (HUD IoU {hana_hud:.6f})"
         if hana_hud is not None else "")
    )


def run_gun(args):
    """Drive the established three-leg route and measure stable gun gameplay."""
    executable = args.exe.resolve()
    game_dir = args.game_dir.resolve()
    game_elf = game_dir / "game" / "EBOOT.elf"
    cache_path = executable.parent / "CMakeCache.txt"
    reference_dir = args.gun_reference_dir.resolve()
    reference_paths = [
        reference_dir / f"frontier_probe_{serial:03d}.ppm"
        for serial in (20, 21, 23, 28)
    ]
    for path, label in (
        (executable, "executable"),
        (game_elf, "game ELF"),
        (cache_path, "CMake cache"),
        *((path, "gun scene reference") for path in reference_paths),
    ):
        if not path.is_file():
            raise FileNotFoundError(f"missing {label}: {path}")

    cache = parse_cache(cache_path)
    expected_cache = dict(EXPECTED_CACHE)
    if args.expect_shufb:
        expected_cache["YZ_SPU_SIMD_SHUFB"] = args.expect_shufb
    if args.expect_ls128:
        expected_cache["YZ_SPU_SIMD_LS128"] = args.expect_ls128
    if args.expect_absdb:
        expected_cache["YZ_SPU_SIMD_ABSDB"] = args.expect_absdb
    if args.expect_xfloat:
        expected_cache["YZ_SPU_SIMD_XFLOAT"] = args.expect_xfloat
    if args.expect_exact_image_bytes:
        expected_cache["YZ_SPU_EXACT_IMAGE_BYTES"] = (
            args.expect_exact_image_bytes
        )
    mismatches = {
        name: {"expected": expected, "actual": cache.get(name)}
        for name, expected in expected_cache.items()
        if cache.get(name) != expected
    }
    if mismatches:
        raise RuntimeError(f"production configuration mismatch: {mismatches}")
    existing = game_processes()
    if existing:
        raise RuntimeError(f"game process already running: {existing}")

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
    stop_path = run_dir / "gun-route-ready.txt"
    frontier_visual_gate = run_dir / "frontier-gun-visual.txt"
    hana_visual_gate = run_dir / "first-hana-prompt.txt"
    result_path = run_dir / "result.json"
    first_arm = capture_dir / "arm-movement.txt"

    environment = {k: v for k, v in os.environ.items() if not k.startswith("YZ_")}
    if args.d3d_debug:
        environment["RSX_D3D_DEBUG"] = "1"
    yz = {
        "YZ_MOVIE_HLE": "1",
        "YZ_AUTO_START": "1",
        "YZ_AUTO_NEW_GAME": "1",
        "YZ_A010_ACCEPT_FAST": "1",
        "YZ_FRONTIER_ACCEPT_FAST": "1",
        "YZ_MOVEMENT_PROOF": "1",
        "YZ_MOVEMENT_PROOF_DELAY_MS": "180000",
        "YZ_MOVEMENT_PROOF_ARM_FILE": str(first_arm),
        "YZ_MOVEMENT_PROOF_DIALOGUE_ARM_FILE": str(hana_visual_gate),
        "YZ_MOVEMENT_PROOF_MAX_LEGS": "3",
        "YZ_MOVEMENT_PROOF_FRONTIER_LEG": "2",
        "YZ_MOVEMENT_PROOF_READY_MIN_SERIAL": "1",
        "YZ_MOVEMENT_PROOF_READY_NONBLACK": "100000",
        "YZ_MOVEMENT_PROOF_READY_HUD_PALE_PPM": "100000",
        "YZ_MOVEMENT_PROOF_READY_VISIBLE_PROBES": "3",
        # Two consecutive positive gameplay probes are required.  At the
        # first city return, authored dialogue obscures the minimap on every
        # third 30-second sample; requiring three uninterrupted minimap
        # samples therefore cannot complete even though the same city state
        # is positively re-established twice.  Frontier/gun acceptance still
        # additionally requires the transition marker, three-region gun HUD,
        # leg-3 state marker, and archived framebuffer match.
        "YZ_MOVEMENT_PROOF_STABLE_VISIBLE_PROBES": "2",
        "YZ_MOVEMENT_PROOF_HOLD_MS": "60000",
        "YZ_MOVEMENT_PROOF_FINAL_HOLD_MS": "10000",
        "YZ_MOVEMENT_PROOF_LX": "128",
        "YZ_MOVEMENT_PROOF_LY": "0",
        "YZ_MOVEMENT_PROOF_FINAL_HUD_PPM": "0",
        "YZ_MOVEMENT_PROOF_LOADING_NONBLACK_MAX": "20000",
        "YZ_MOVEMENT_PROOF_GUN_HUD_PALE_PPM": "5000",
        "YZ_MOVEMENT_PROOF_SKIP_CAMERA": "1",
        # Route readback is deliberately frequent until the final checkpoint:
        # it bounds the interval in which Confirm remains active after the
        # first Hana prompt.  Capture is stopped before QPC measurement.
        "YZ_MOVEMENT_PROBE_INTERVAL_MS": "2000",
        "YZ_MOVEMENT_PROOF_STOP_FILE": str(stop_path),
        "YZ_MOVEMENT_PROOF_FRONTIER_VISUAL_FILE": str(frontier_visual_gate),
        "YZ_RSX_VALIDATION_CAPTURE": "1",
        "YZ_RSX_VALIDATION_DIR": str(capture_dir),
        "YZ_DIALOGUE_PULSE_PERIOD_MS": "2200",
        "YZ_DIALOGUE_PULSE_HOLD_MS": "1400",
    }
    if args.nr_vertical_active_basic:
        yz["YZ_NR_VERTICAL"] = "active-basic"
    if args.nr_vertical_active_present:
        yz["YZ_NR_VERTICAL"] = "active-present"
    if args.nr_vertical_active_graphics:
        yz["YZ_NR_VERTICAL"] = "active-graphics"
        if args.nr_graphics_families:
            yz["YZ_NR_GRAPHICS_FAMILIES"] = args.nr_graphics_families
        if args.nr_clear_scope:
            yz["YZ_NR_CLEAR_SCOPE"] = args.nr_clear_scope
        if args.nr_frame_islands:
            yz["YZ_NR_FRAME_ISLANDS"] = "1"
        if args.nr_pass_diag:
            yz["YZ_NR_PASS_DIAG"] = "1"
        if args.nr_shadow_consumer_admit:
            yz["YZ_NR_SHADOW_CONSUMER_ADMIT"] = "1"
        if args.nr_force_draw_input_refresh:
            yz["YZ_NR_FORCE_DRAW_INPUT_REFRESH"] = "1"
        if args.nr_draw_primitive is not None:
            yz["YZ_NR_DRAW_PRIMITIVE"] = str(args.nr_draw_primitive)
        if args.nr_hana_depth_oracle:
            yz["YZ_NR_HANA_DEPTH_ORACLE"] = "1"
    if args.nr_vertical_full_native:
        yz["YZ_NR_VERTICAL"] = "full-native"
        if args.nr_scanout_provenance:
            yz["YZ_NR_SCANOUT_PROVENANCE"] = "1"
        if args.nr_hana_input_oracle:
            yz["YZ_NR_HANA_INPUT_ORACLE"] = "1"
    if args.nr_vertical_shadow:
        # Shutdown-only fixed-memory producer/FIFO equivalence census.  This
        # is safe on the extended route: it neither owns commands nor emits
        # per-event output, timing, or synthetic graphics work.
        yz["YZ_NR_VERTICAL"] = "shadow"
    if args.nr_submit_attribution:
        yz["YZ_NR_SUBMIT_ATTRIBUTION"] = "1"
    if args.nr_defer_reports:
        yz["YZ_NR_DEFER_REPORTS"] = "1"
    if args.nr_report_audit:
        yz["YZ_NR_REPORT_AUDIT"] = "1"
    if args.nr_graph_execute:
        yz["YZ_NR_GRAPH"] = "execute"
    if args.nr_single_pass_graph:
        yz["YZ_NR_SINGLE_PASS_GRAPH"] = args.nr_single_pass_graph
    if args.nr_single_pass_graph_timing:
        yz["YZ_NR_SINGLE_PASS_GRAPH_TIMING"] = "1"
    environment.update(yz)
    result = {
        "tag": args.tag,
        "route": "three-leg-post-frontier-gun",
        "status": "launching",
        "executable": str(executable),
        "executable_sha256": sha256_file(executable),
        "game_elf": str(game_elf),
        "configuration": {
            name: cache.get(name)
            for name in (
                *EXPECTED_CACHE,
                "YZ_SPU_SIMD_SHUFB",
                "YZ_SPU_SIMD_LS128",
                "YZ_SPU_SIMD_XFLOAT",
                "YZ_SPU_EXACT_IMAGE_BYTES",
            )
        },
        "active_yz": yz,
        "active_diagnostics": (
            ({"RSX_D3D_DEBUG": "1"} if args.d3d_debug else {}) |
            ({"YZ_NR_SCANOUT_PROVENANCE": "1"}
             if args.nr_scanout_provenance else {}) |
            ({"YZ_NR_REPORT_AUDIT": "1"}
             if args.nr_report_audit else {})
        ),
        "gun_reference_dir": str(reference_dir),
        "captures": [],
        "route_markers": {},
    }
    gun_references = [scene_features(path) for path in reference_paths]
    hana_reference = scene_features(args.reference.resolve())
    stdout_handle = stdout_path.open("wb")
    stderr_handle = stderr_path.open("wb")
    process = None
    forced = False
    seen = set()
    try:
        process = subprocess.Popen(
            [str(executable), str(game_elf)], cwd=str(game_dir),
            env=environment, stdout=stdout_handle, stderr=stderr_handle,
        )
        result["pid"] = process.pid
        stale_qpc = scratch / f"present_qpc_{process.pid}.csv"
        if stale_qpc.exists():
            stale_qpc.unlink()
        result["status"] = "routing"
        print(f"[gun-harness] launched exact PID {process.pid}", flush=True)
        time.sleep(1.0)
        live_games = game_processes()
        if live_games != {process.pid: str(executable)}:
            normalized = {
                pid: os.path.normcase(os.path.abspath(path))
                for pid, path in live_games.items()
            }
            wanted = os.path.normcase(str(executable))
            if normalized != {process.pid: wanted}:
                raise RuntimeError(f"single-game-process invariant failed: {live_games}")
        config_match, _ = wait_log(
            stderr_path, r"\[config\].*lane=clean", 60, process
        )
        result["runtime_config_line"] = config_match.group(0)

        deadline = time.monotonic() + args.gun_route_timeout
        last_capture_progress = time.monotonic()
        stable_three = None
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"game exited during gun route: {process.returncode}")
            if stderr_path.exists():
                stderr_tail = stderr_path.read_text(
                    encoding="utf-8", errors="replace"
                )[-262144:]
                fatal = re.findall(
                    r"\[(?:nr-vertical-section-fatal|nr-full-native-fatal)"
                    r"[^\r\n]*",
                    stderr_tail,
                )
                if fatal:
                    result["route_failure_class"] = (
                        "native-section-execution-fatal"
                    )
                    result["route_failure_state"] = fatal[-1]
                    raise RuntimeError(fatal[-1])
            for path in sorted(capture_dir.glob("frontier_probe_*.ppm")):
                if path in seen:
                    continue
                try:
                    features = scene_features(path)
                except (OSError, ValueError):
                    continue
                seen.add(path)
                last_capture_progress = time.monotonic()
                serial = int(path.stem.rsplit("_", 1)[-1])
                best_gun_mae = min(
                    coarse_scene_mae(reference, features)
                    for reference in gun_references
                )
                best_gun_hud_iou = max(
                    gun_hud_iou(reference, features)
                    for reference in gun_references
                )
                hana_comparison = compare_scene(hana_reference, features)
                result["captures"].append({
                    "serial": serial,
                    "path": str(path),
                    "best_gun_mae": round(best_gun_mae, 6),
                    "best_gun_hud_iou": round(best_gun_hud_iou, 6),
                    "hana_prompt_match": hana_comparison["match"],
                })
                print(
                    f"[gun-harness] {path.name}: "
                    f"best_gun_mae={best_gun_mae:.4f} "
                    f"gun_hud_iou={best_gun_hud_iou:.4f}",
                    flush=True,
                )
                if hana_comparison["match"] and not hana_visual_gate.exists():
                    hana_visual_gate.write_text(
                        f"verified {path.name} "
                        f"mae={hana_comparison['coarse_mae']:.6f}\n",
                        encoding="ascii",
                    )
                    result["route_markers"]["first_hana_prompt"] = str(
                        hana_visual_gate
                    )
                    result["first_hana_serial"] = serial
                    print(
                        f"[gun-harness] verified first Hana X-prompt at "
                        f"{path.name}; enabling bounded Confirm",
                        flush=True,
                    )
                if (best_gun_mae <= args.gun_anchor_mae and
                        best_gun_hud_iou >= args.gun_hud_iou and
                        not frontier_visual_gate.exists()):
                    frontier_visual_gate.write_text(
                        f"verified {path.name} mae={best_gun_mae:.6f} "
                        f"hud_iou={best_gun_hud_iou:.6f}\n",
                        encoding="ascii",
                    )
                    result["route_markers"]["frontier_gun_visual"] = str(
                        frontier_visual_gate
                    )
                    print(
                        f"[gun-harness] verified post-Frontier gun HUD at "
                        f"{path.name}", flush=True,
                    )

            ready = sorted(capture_dir.glob("frontier_ready_*.txt"))
            first_hana_serial = result.get("first_hana_serial")
            post_hana_ready = next((
                marker for marker in ready
                if first_hana_serial is not None and
                int(marker.stem.rsplit("_", 1)[-1]) > first_hana_serial and
                not next((
                    item["hana_prompt_match"] for item in result["captures"]
                    if item["serial"] == int(marker.stem.rsplit("_", 1)[-1])
                ), True)
            ), None)
            if post_hana_ready and not first_arm.exists():
                first_arm.write_text(
                    f"armed after {post_hana_ready.name}\n", encoding="ascii"
                )
                result["route_markers"]["initial_ready"] = str(post_hana_ready)
                print(
                    f"[gun-harness] Hana cleared and city HUD restored; "
                    f"armed leg 1 after {post_hana_ready.name}", flush=True,
                )
            for leg in (1, 2):
                stable = sorted(capture_dir.glob(f"stable_gameplay_leg_{leg}_*.txt"))
                next_arm = capture_dir / f"arm-movement-{leg + 1}.txt"
                if stable and not next_arm.exists():
                    next_arm.write_text(
                        f"armed after {stable[0].name}\n", encoding="ascii"
                    )
                    result["route_markers"][f"stable_leg_{leg}"] = str(stable[0])
                    print(
                        f"[gun-harness] armed leg {leg + 1} after {stable[0].name}",
                        flush=True,
                    )

            stable_files = sorted(
                capture_dir.glob("stable_gameplay_leg_3_*.txt")
            )
            if stable_files:
                candidate = stable_files[0]
                serial = int(candidate.stem.rsplit("_", 1)[-1])
                capture = next(
                    (item for item in result["captures"]
                     if item["serial"] == serial), None
                )
                if (capture and
                        capture["best_gun_mae"] <= args.gun_anchor_mae and
                        capture["best_gun_hud_iou"] >= args.gun_hud_iou):
                    stable_three = candidate
                    result["route_markers"]["stable_leg_3"] = str(candidate)
                    result["gun_checkpoint_capture"] = capture
                    stop_path.write_text(
                        f"stable gun checkpoint {candidate.name}\n",
                        encoding="ascii",
                    )
                    break

            no_capture_for = time.monotonic() - last_capture_progress
            if no_capture_for >= 150.0 and stderr_path.exists():
                tail = stderr_path.read_text(
                    encoding="utf-8", errors="replace"
                )[-262144:]
                idle_states = re.findall(
                    r"\[rsx-idle\] 10s no-advance GET=([^\r\n]+)", tail
                )
                if len(idle_states) >= 3 and len(set(idle_states[-3:])) == 1:
                    result["route_failure_class"] = "fifo-no-progress"
                    result["route_failure_state"] = idle_states[-1]
                    raise RuntimeError(
                        "gun route repeated one FIFO no-progress state"
                    )
            time.sleep(0.25)
        if stable_three is None:
            raise TimeoutError("stable post-Frontier gun tutorial not reached")

        route_match, _ = wait_log(
            stderr_path,
            r"\[movement-proof\] route complete legs=3; stable gameplay "
            r"confirmed; synthetic input stopped",
            20, process,
        )
        stop_match, _ = wait_log(
            stderr_path,
            r"\[movement-proof\] visual probe stopped at confirmed route "
            r"checkpoint present_id=([0-9]+)",
            20, process,
        )
        result["route_complete_line"] = route_match.group(0)
        result["measurement_start_present_id"] = int(stop_match.group(1))
        result["capture_count_at_checkpoint"] = len(
            list(capture_dir.glob("frontier_probe_*.ppm"))
        )
        result["status"] = "measuring"
        measurement_cpu_start = process_cpu_seconds(process.pid)
        print(
            f"[gun-harness] stable gun scene; measuring {args.hold_seconds}s",
            flush=True,
        )
        end = time.monotonic() + args.hold_seconds
        while time.monotonic() < end:
            if process.poll() is not None:
                raise RuntimeError(
                    f"game exited during gun measurement: {process.returncode}"
                )
            time.sleep(min(0.5, end - time.monotonic()))

        result["measurement_process_cpu_seconds"] = round(
            process_cpu_seconds(process.pid) - measurement_cpu_start, 6
        )
        result["status"] = "closing"
        windows = post_close(process.pid)
        result["wm_close_windows"] = windows
        if not windows:
            raise RuntimeError("no visible exact-PID window found for normal shutdown")
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            forced = True
            process.terminate()
            process.wait(timeout=10)
        result["exit_code"] = process.returncode
        result["forced_close"] = forced

        stderr_text = stderr_path.read_text(encoding="utf-8", errors="replace")
        vertical_shadow = re.findall(
            r"^\[nr-vertical-(?:shadow|exact|observed-ea|source).*$",
            stderr_text, re.MULTILINE,
        )
        result["nr_vertical_shadow"] = vertical_shadow
        full_native_lines = re.findall(
            r"^\[nr-full-native .*\]$", stderr_text, re.MULTILINE
        )
        full_native_d3d = re.findall(
            r"^\[nr-vertical-d3d .*\]$", stderr_text, re.MULTILINE
        )
        single_graph_lines = re.findall(
            r"^\[nr-single-graph .*\]$", stderr_text, re.MULTILINE
        )
        submit_lines = re.findall(
            r"^\[nr-submit-attribution .*\]$", stderr_text, re.MULTILINE
        )
        submit_transfer_lines = re.findall(
            r"^\[nr-submit-transfer .*\]$", stderr_text, re.MULTILINE
        )
        live_submit_lines = re.findall(
            r"^\[live-submit-attribution .*\]$", stderr_text, re.MULTILINE
        )
        result["nr_full_native"] = full_native_lines
        result["nr_full_native_d3d"] = full_native_d3d
        result["nr_single_graph"] = single_graph_lines
        result["nr_submit_attribution"] = submit_lines
        result["nr_submit_transfer"] = submit_transfer_lines
        result["live_submit_attribution"] = live_submit_lines
        report_scoreboard_lines = re.findall(
            r"^\[nr-report-scoreboard .*\]$", stderr_text, re.MULTILINE
        )
        report_fallback_lines = re.findall(
            r"^\[nr-report-fallback .*\]$", stderr_text, re.MULTILINE
        )
        report_family_lines = re.findall(
            r"^\[nr-report-family .*\]$", stderr_text, re.MULTILINE
        )
        report_family_fallback_lines = re.findall(
            r"^\[nr-report-family-fallback .*\]$", stderr_text, re.MULTILINE
        )
        result["nr_report_scoreboard"] = report_scoreboard_lines
        result["nr_report_fallback"] = report_fallback_lines
        result["nr_report_family"] = report_family_lines
        result["nr_report_family_fallback"] = report_family_fallback_lines
        if args.nr_report_audit:
            if len(report_scoreboard_lines) != 1:
                raise RuntimeError(
                    "report audit aggregate incomplete: expected one scoreboard "
                    f"summary, found {len(report_scoreboard_lines)}"
                )
        elif (report_scoreboard_lines or report_fallback_lines or
              report_family_lines or report_family_fallback_lines):
            raise RuntimeError("report audit output was active outside its requested lane")
        if args.nr_submit_attribution:
            if (len(submit_lines) != 14 or
                    len(submit_transfer_lines) != 1 or
                    len(live_submit_lines) != 11):
                raise RuntimeError(
                    "submission attribution aggregate incomplete: "
                    f"native={len(submit_lines)} "
                    f"transfer={len(submit_transfer_lines)} "
                    f"timeline={len(live_submit_lines)}"
                )
        elif submit_lines or submit_transfer_lines or live_submit_lines:
            raise RuntimeError(
                "submission attribution was active outside its requested lane"
            )
        if args.nr_vertical_full_native:
            if len(full_native_lines) != 1 or "fatal=0" not in full_native_lines[0]:
                raise RuntimeError(
                    "strict full-native gun run did not close with one clean "
                    f"summary: {full_native_lines}"
                )
            if (len(full_native_d3d) != 1 or
                    "legacy-groups=0" not in full_native_d3d[0] or
                    "fallback=0" not in full_native_d3d[0]):
                raise RuntimeError(
                    "strict full-native gun run reached legacy work or a "
                    f"native refusal: {full_native_d3d}"
                )
            if args.nr_single_pass_graph and len(single_graph_lines) != 1:
                raise RuntimeError(
                    "single-pass graph aggregate incomplete: "
                    f"{single_graph_lines}"
                )
            if (args.nr_single_pass_graph == "passive" and
                    (not single_graph_lines or
                     not re.search(r"passive=([0-9]+)/\1/0(?: |\])",
                                   single_graph_lines[0]))):
                raise RuntimeError(
                    "single-pass passive equivalence failed: "
                    f"{single_graph_lines}"
                )
            if not args.nr_single_pass_graph and single_graph_lines:
                raise RuntimeError(
                    "single-pass graph was active outside its requested lane"
                )
        elif full_native_lines:
            raise RuntimeError("strict full-native owner was active in another lane")
        if args.nr_vertical_shadow:
            summaries = [line for line in vertical_shadow
                         if line.startswith("[nr-vertical-shadow ")]
            exact = [line for line in vertical_shadow
                     if line.startswith("[nr-vertical-exact ")]
            if len(summaries) != 1 or len(exact) != 1:
                raise RuntimeError(
                    "vertical shadow gun run must emit exactly one summary "
                    f"and exact line: found {len(summaries)}/{len(exact)}"
                )
        elif vertical_shadow:
            raise RuntimeError("vertical shadow was active in the clean gun lane")
        if not re.search(
                r"Frontier (?:loading observed|transition inferred|"
                r"transition verified by archived gun-HUD framebuffer gate)",
                         stderr_text):
            raise RuntimeError("route lacked a positive Frontier transition marker")
        probe_present_ids = {
            int(serial): int(present_id)
            for serial, present_id in re.findall(
                r"\[movement-proof\] clean-frontier-probe serial=([0-9]+).*? "
                r"present_id=([0-9]+)", stderr_text,
            )
        }
        result["probe_present_ids"] = probe_present_ids
        for capture in result["captures"]:
            capture["present_id"] = probe_present_ids.get(capture["serial"])
        capture_count_after = len(list(capture_dir.glob("frontier_probe_*.ppm")))
        result["capture_count_after_shutdown"] = capture_count_after
        if capture_count_after != result["capture_count_at_checkpoint"]:
            raise RuntimeError(
                "renderer capture continued after gun checkpoint: "
                f"{result['capture_count_at_checkpoint']} -> {capture_count_after}"
            )
        qpc_path = scratch / f"present_qpc_{process.pid}.csv"
        result["present_qpc_path"] = str(qpc_path)
        if not qpc_path.is_file():
            raise RuntimeError(f"shutdown did not preserve Present QPC ring: {qpc_path}")
        metrics = qpc_metrics(
            qpc_path, result["measurement_start_present_id"], bucket_seconds=5.0
        )
        result.update(metrics)
        if metrics["measurement_presents"] > 1:
            result["process_cpu_ms_per_present"] = round(
                result["measurement_process_cpu_seconds"] * 1000.0 /
                (metrics["measurement_presents"] - 1), 3
            )
        result["scenes"] = {
            "post_frontier_gun_tutorial": {
                "kind": "stationary",
                "route_valid": True,
                "input_stopped": True,
                "readback_stopped": True,
                **metrics,
            }
        }
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
        print(f"[gun-harness] result: {result_path}", flush=True)
    return 0 if result["status"] == "passed" else 2


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
    expected_cache = dict(EXPECTED_CACHE)
    if args.expect_shufb:
        expected_cache["YZ_SPU_SIMD_SHUFB"] = args.expect_shufb
    if args.expect_ls128:
        expected_cache["YZ_SPU_SIMD_LS128"] = args.expect_ls128
    if args.expect_absdb:
        expected_cache["YZ_SPU_SIMD_ABSDB"] = args.expect_absdb
    if args.expect_xfloat:
        expected_cache["YZ_SPU_SIMD_XFLOAT"] = args.expect_xfloat
    if args.expect_exact_image_bytes:
        expected_cache["YZ_SPU_EXACT_IMAGE_BYTES"] = (
            args.expect_exact_image_bytes
        )
    if args.wkl4_cycle:
        expected_cache["YZ_WKL4_CYCLE_DIAGNOSTIC"] = "ON"
    mismatches = {
        name: {"expected": expected, "actual": cache.get(name)}
        for name, expected in expected_cache.items()
        if cache.get(name) != expected
    }
    if mismatches:
        raise RuntimeError(f"production configuration mismatch: {mismatches}")
    existing = game_processes()
    if existing:
        raise RuntimeError(f"game process already running: {existing}")

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
    input_stop_path = run_dir / "akiyama-dialogue-input-stopped.txt"
    stop_path = run_dir / "akiyama-dialogue-ready.txt"
    result_path = run_dir / "result.json"

    environment = {k: v for k, v in os.environ.items() if not k.startswith("YZ_")}
    if args.d3d_debug:
        environment["RSX_D3D_DEBUG"] = "1"
    yz = {
        "YZ_MOVIE_HLE": "1",
        "YZ_AUTO_START": "1",
        "YZ_AUTO_NEW_GAME": "1",
        "YZ_A010_ACCEPT_FAST": "1",
        "YZ_FRONTIER_ACCEPT_FAST": "1",
        "YZ_AKIYAMA_DIALOGUE_ROUTE": "1",
        # Stop Start-only navigation as soon as the Hana scene itself is
        # positively identified. Keep sparse readback alive under the final
        # path until the fully typed X prompt is independently confirmed.
        "YZ_AKIYAMA_DIALOGUE_STOP_FILE": str(input_stop_path),
        "YZ_AKIYAMA_DIALOGUE_CAPTURE_STOP_FILE": str(stop_path),
        "YZ_AKIYAMA_ROUTE_START_DELAY_MS": str(args.route_start_delay_ms),
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
    if args.nr_vertical_shadow:
        yz["YZ_NR_VERTICAL"] = "shadow"
    if args.nr_vertical_active_basic:
        yz["YZ_NR_VERTICAL"] = "active-basic"
    if args.nr_vertical_active_present:
        yz["YZ_NR_VERTICAL"] = "active-present"
    if args.nr_vertical_active_graphics:
        yz["YZ_NR_VERTICAL"] = "active-graphics"
        if args.nr_graphics_families:
            yz["YZ_NR_GRAPHICS_FAMILIES"] = args.nr_graphics_families
        if args.nr_clear_scope:
            yz["YZ_NR_CLEAR_SCOPE"] = args.nr_clear_scope
        if args.nr_frame_islands:
            yz["YZ_NR_FRAME_ISLANDS"] = "1"
        if args.nr_pass_diag:
            yz["YZ_NR_PASS_DIAG"] = "1"
        if args.nr_shadow_consumer_admit:
            yz["YZ_NR_SHADOW_CONSUMER_ADMIT"] = "1"
        if args.nr_force_draw_input_refresh:
            yz["YZ_NR_FORCE_DRAW_INPUT_REFRESH"] = "1"
        if args.nr_draw_primitive is not None:
            yz["YZ_NR_DRAW_PRIMITIVE"] = str(args.nr_draw_primitive)
        if args.nr_hana_depth_oracle:
            yz["YZ_NR_HANA_DEPTH_ORACLE"] = "1"
    if args.nr_vertical_full_native:
        yz["YZ_NR_VERTICAL"] = "full-native"
        if args.nr_scanout_provenance:
            yz["YZ_NR_SCANOUT_PROVENANCE"] = "1"
        if args.nr_hana_input_oracle:
            yz["YZ_NR_HANA_INPUT_ORACLE"] = "1"
    if args.draw_phases:
        yz["YZ_RSX_DRAW_PHASES"] = "1"
    if args.wkl4_cycle:
        yz["YZ_WKL4_CYCLE"] = "1"
    if args.xf_ieee:
        yz["YZ_XF_IEEE"] = "1"
    if args.nr_submit_attribution:
        yz["YZ_NR_SUBMIT_ATTRIBUTION"] = "1"
    if args.nr_defer_reports:
        yz["YZ_NR_DEFER_REPORTS"] = "1"
    if args.nr_report_audit:
        yz["YZ_NR_REPORT_AUDIT"] = "1"
    if args.nr_graph_execute:
        yz["YZ_NR_GRAPH"] = "execute"
    if args.nr_single_pass_graph:
        yz["YZ_NR_SINGLE_PASS_GRAPH"] = args.nr_single_pass_graph
    if args.nr_single_pass_graph_timing:
        yz["YZ_NR_SINGLE_PASS_GRAPH_TIMING"] = "1"
    environment.update(yz)

    result = {
        "tag": args.tag,
        "status": "launching",
        "executable": str(executable),
        "executable_sha256": sha256_file(executable),
        "game_elf": str(game_elf),
        "configuration": {
            name: cache.get(name)
            for name in (
                *EXPECTED_CACHE,
                "YZ_SPU_SIMD_SHUFB",
                "YZ_SPU_SIMD_LS128",
                "YZ_SPU_SIMD_ABSDB",
                "YZ_SPU_SIMD_XFLOAT",
                "YZ_SPU_EXACT_IMAGE_BYTES",
            )
        },
        "active_yz": yz,
        "active_diagnostics": (
            ({"RSX_D3D_DEBUG": "1"} if args.d3d_debug else {}) |
            ({"YZ_NR_SCANOUT_PROVENANCE": "1"}
             if args.nr_scanout_provenance else {}) |
            ({"YZ_NR_REPORT_AUDIT": "1"}
             if args.nr_report_audit else {})
        ),
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
        if list(game_processes()) != [process.pid]:
            raise RuntimeError(
                f"single-game-process invariant failed: {game_processes()}"
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
        arrival_consecutive = 0
        last_capture_progress = time.monotonic()
        startup_idle_visual = []
        startup_idle_invalid = []
        startup_idle_attempts = 0
        next_startup_idle_visual = 0.0
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"game exited during route: {process.returncode}")
            if stderr_path.exists():
                stderr_tail = stderr_path.read_text(
                    encoding="utf-8", errors="replace"
                )[-262144:]
                fatal = re.findall(
                    r"\[(?:nr-vertical-section-fatal|nr-full-native-fatal)"
                    r"[^\r\n]*",
                    stderr_tail,
                )
                if fatal:
                    result["route_failure_class"] = (
                        "native-section-execution-fatal"
                    )
                    result["route_failure_state"] = fatal[-1]
                    raise RuntimeError(fatal[-1])
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
                comparison["serial"] = int(path.stem.rsplit("_", 1)[-1])
                result["captures"].append(comparison)
                oracle_prompt = (
                    args.nr_hana_input_oracle and comparison["prompt_scene"]
                )
                arrival_consecutive = (
                    arrival_consecutive + 1
                    if comparison["arrival"] or oracle_prompt else 0
                )
                if arrival_consecutive >= 1 and not input_stop_path.exists():
                    input_stop_path.write_text(
                        f"Hana scene arrival confirmed by {path.name}\n",
                        encoding="ascii",
                    )
                    result["input_stop_capture"] = str(path)
                accepted_oracle_prompt = (
                    args.nr_hana_input_oracle and oracle_prompt
                )
                consecutive = (
                    consecutive + 1
                    if comparison["match"] or accepted_oracle_prompt else 0
                )
                print(
                    f"[akiyama-harness] {path.name}: match={comparison['match']} "
                    f"arrival={comparison['arrival']} "
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
                    result["diagnostic_corrupt_checkpoint"] = (
                        accepted_oracle_prompt and not comparison["match"]
                    )
                    result["capture_count_at_checkpoint"] = len(seen)
                    break
            if stop_path.exists():
                break
            # A healthy route produces a new sparse framebuffer every few
            # seconds.  Fail early only when visual progress has stopped AND
            # the runtime itself reports a durable FIFO no-progress state;
            # slow but advancing boots remain governed by route_timeout.
            no_capture_for = time.monotonic() - last_capture_progress
            if (not seen and no_capture_for >= 75.0 and stderr_path.exists()):
                tail = stderr_path.read_text(
                    encoding="utf-8", errors="replace"
                )[-262144:]
                idle_states = re.findall(
                    r"\[rsx-idle\] 10s no-advance GET=([^\r\n]+)", tail
                )
                if (len(idle_states) >= 3 and
                        len(set(idle_states[-3:])) == 1):
                    now = time.monotonic()
                    if now >= next_startup_idle_visual:
                        startup_idle_attempts += 1
                        serial = startup_idle_attempts
                        capture = capture_process_window(
                            process.pid,
                            run_dir / f"startup_idle_{serial:03d}.png",
                        )
                        next_startup_idle_visual = now + 10.0
                        if capture:
                            capture["fifo_state"] = idle_states[-1]
                            capture["seconds_without_route_capture"] = round(
                                no_capture_for, 3
                            )
                            target = (startup_idle_visual
                                      if capture["usable"]
                                      else startup_idle_invalid)
                            target.append(capture)
                            result["startup_idle_visual_probes"] = [
                                {key: value for key, value in item.items()
                                 if key != "sample"}
                                for item in startup_idle_visual
                            ]
                            result["startup_idle_invalid_visual_probes"] = [
                                {key: value for key, value in item.items()
                                 if key != "sample"}
                                for item in startup_idle_invalid
                            ]
                            print(
                                "[akiyama-harness] startup idle visual "
                                f"{serial}: mean={capture['mean_channel']} "
                                f"method={capture['capture_method']} "
                                f"usable={capture['usable']}",
                                flush=True,
                            )
                    if len(startup_idle_visual) >= 3:
                        recent = startup_idle_visual[-3:]
                        visual_mae = max(
                            sample_mae(recent[0]["sample"], recent[1]["sample"]),
                            sample_mae(recent[1]["sample"], recent[2]["sample"]),
                        )
                        result["startup_idle_visual_mae"] = round(visual_mae, 3)
                        if (len({item["fifo_state"] for item in recent}) == 1 and
                                visual_mae <= 2.5):
                            result["route_failure_class"] = (
                                "startup-fifo-and-visual-no-progress"
                            )
                            result["route_failure_state"] = idle_states[-1]
                            raise RuntimeError(
                                "startup FIFO and game image both stopped "
                                "progressing"
                            )
                    if no_capture_for >= 150.0:
                        result["route_failure_class"] = (
                            "startup-route-no-progress-timeout"
                        )
                        result["route_failure_state"] = idle_states[-1]
                        raise RuntimeError(
                            "startup route made no framebuffer progress for "
                            "150 seconds"
                        )
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
        measurement_cpu_start = process_cpu_seconds(process.pid)
        print(
            f"[akiyama-harness] checkpoint stable; measuring {args.hold_seconds}s",
            flush=True,
        )
        end = time.monotonic() + args.hold_seconds
        while time.monotonic() < end:
            if process.poll() is not None:
                raise RuntimeError(f"game exited during measurement: {process.returncode}")
            time.sleep(min(0.5, end - time.monotonic()))
        result["measurement_process_cpu_seconds"] = round(
            process_cpu_seconds(process.pid) - measurement_cpu_start, 6
        )
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
        probe_present_ids = {
            int(serial): int(present_id)
            for serial, present_id in re.findall(
                r"\[(?:akiyama-route|movement-proof)\] "
                r"clean-frontier-probe serial=([0-9]+).*? "
                r"present_id=([0-9]+)",
                stderr_text,
            )
        }
        result["probe_present_ids"] = probe_present_ids
        for capture in result["captures"]:
            capture["present_id"] = probe_present_ids.get(capture["serial"])
        shadow_lines = re.findall(r"^\[nr-shadow:.*\]$", stderr_text, re.MULTILINE)
        live_lines = re.findall(r"^\[nr-live:.*\]$", stderr_text, re.MULTILINE)
        draw_phase_lines = re.findall(
            r"^\[draw-phase:.*\]$", stderr_text, re.MULTILINE
        )
        wkl4_cycle_lines = re.findall(
            r"^\[wkl4-cycle\].*$", stderr_text, re.MULTILINE
        )
        result["nr_shadow_census"] = shadow_lines
        result["nr_live_census"] = live_lines
        result["draw_phase_aggregate"] = draw_phase_lines
        result["wkl4_cycle_aggregate"] = wkl4_cycle_lines
        full_native_lines = re.findall(
            r"^\[nr-full-native .*\]$", stderr_text, re.MULTILINE
        )
        full_native_d3d = re.findall(
            r"^\[nr-vertical-d3d .*\]$", stderr_text, re.MULTILINE
        )
        single_graph_lines = re.findall(
            r"^\[nr-single-graph .*\]$", stderr_text, re.MULTILINE
        )
        submit_lines = re.findall(
            r"^\[nr-submit-attribution .*\]$", stderr_text, re.MULTILINE
        )
        submit_transfer_lines = re.findall(
            r"^\[nr-submit-transfer .*\]$", stderr_text, re.MULTILINE
        )
        live_submit_lines = re.findall(
            r"^\[live-submit-attribution .*\]$", stderr_text, re.MULTILINE
        )
        result["nr_full_native"] = full_native_lines
        result["nr_full_native_d3d"] = full_native_d3d
        result["nr_single_graph"] = single_graph_lines
        result["nr_submit_attribution"] = submit_lines
        result["nr_submit_transfer"] = submit_transfer_lines
        result["live_submit_attribution"] = live_submit_lines
        report_scoreboard_lines = re.findall(
            r"^\[nr-report-scoreboard .*\]$", stderr_text, re.MULTILINE
        )
        report_fallback_lines = re.findall(
            r"^\[nr-report-fallback .*\]$", stderr_text, re.MULTILINE
        )
        report_family_lines = re.findall(
            r"^\[nr-report-family .*\]$", stderr_text, re.MULTILINE
        )
        report_family_fallback_lines = re.findall(
            r"^\[nr-report-family-fallback .*\]$", stderr_text, re.MULTILINE
        )
        result["nr_report_scoreboard"] = report_scoreboard_lines
        result["nr_report_fallback"] = report_fallback_lines
        result["nr_report_family"] = report_family_lines
        result["nr_report_family_fallback"] = report_family_fallback_lines
        if args.nr_report_audit:
            if len(report_scoreboard_lines) != 1:
                raise RuntimeError(
                    "report audit aggregate incomplete: expected one scoreboard "
                    f"summary, found {len(report_scoreboard_lines)}"
                )
        elif (report_scoreboard_lines or report_fallback_lines or
              report_family_lines or report_family_fallback_lines):
            raise RuntimeError("report audit output was active outside its requested lane")
        if args.nr_submit_attribution:
            if (len(submit_lines) != 14 or
                    len(submit_transfer_lines) != 1 or
                    len(live_submit_lines) != 11):
                raise RuntimeError(
                    "submission attribution aggregate incomplete: "
                    f"native={len(submit_lines)} "
                    f"transfer={len(submit_transfer_lines)} "
                    f"timeline={len(live_submit_lines)}"
                )
        elif submit_lines or submit_transfer_lines or live_submit_lines:
            raise RuntimeError(
                "submission attribution was active outside its requested lane"
            )
        if args.nr_vertical_full_native:
            if len(full_native_lines) != 1 or "fatal=0" not in full_native_lines[0]:
                raise RuntimeError(
                    "strict full-native run did not close with one clean "
                    f"summary: {full_native_lines}"
                )
            if (len(full_native_d3d) != 1 or
                    "legacy-groups=0" not in full_native_d3d[0] or
                    "fallback=0" not in full_native_d3d[0]):
                raise RuntimeError(
                    "strict full-native run reached legacy work or a native "
                    f"refusal: {full_native_d3d}"
                )
            if args.nr_single_pass_graph and len(single_graph_lines) != 1:
                raise RuntimeError(
                    "single-pass graph aggregate incomplete: "
                    f"{single_graph_lines}"
                )
            if (args.nr_single_pass_graph == "passive" and
                    (not single_graph_lines or
                     not re.search(r"passive=([0-9]+)/\1/0(?: |\])",
                                   single_graph_lines[0]))):
                raise RuntimeError(
                    "single-pass passive equivalence failed: "
                    f"{single_graph_lines}"
                )
            if not args.nr_single_pass_graph and single_graph_lines:
                raise RuntimeError(
                    "single-pass graph was active outside its requested lane"
                )
        elif full_native_lines:
            raise RuntimeError("strict full-native owner was active in another lane")
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
        if args.draw_phases and len(draw_phase_lines) < 2:
            raise RuntimeError("draw-phase run did not emit bounded shutdown aggregate")
        if not args.draw_phases and draw_phase_lines:
            raise RuntimeError("draw-phase classifier was active in a clean lane")
        if args.wkl4_cycle and len(wkl4_cycle_lines) != 18:
            raise RuntimeError(
                "image-4 cycle run must emit one total plus fourteen segment lines: "
                f"found {len(wkl4_cycle_lines)}"
            )
        if not args.wkl4_cycle and wkl4_cycle_lines:
            raise RuntimeError("image-4 cycle classifier was active in a clean lane")
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
        if result["measurement_presents"] > 1:
            result["process_cpu_ms_per_present"] = round(
                result["measurement_process_cpu_seconds"] * 1000.0 /
                (result["measurement_presents"] - 1), 3
            )
        if args.multi_scene_reference_dir:
            multi_scene_metrics(
                result,
                qpc_path,
                args.multi_scene_reference_dir,
                args.anchor_mae,
            )
        fps = result["fps_samples"]
        if forced:
            result["status"] = "forced-close"
        elif process.returncode != 0:
            result["status"] = "nonzero-exit"
        elif result.get("diagnostic_corrupt_checkpoint"):
            result["status"] = "diagnostic-visual-failure"
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
    parser.add_argument("--route-start-delay-ms", type=int, default=0)
    parser.add_argument("--expect-shufb", choices=("ON", "OFF"))
    parser.add_argument("--expect-ls128", choices=("ON", "OFF"))
    parser.add_argument("--expect-absdb", choices=("ON", "OFF"))
    parser.add_argument("--expect-xfloat", choices=("ON", "OFF"))
    parser.add_argument(
        "--expect-exact-image-bytes", choices=("ON", "OFF")
    )
    parser.add_argument("--multi-scene-reference-dir", type=Path)
    parser.add_argument("--anchor-mae", type=float, default=0.10)
    parser.add_argument("--gun-route", action="store_true")
    parser.add_argument("--gun-reference-dir", type=Path)
    parser.add_argument("--gun-anchor-mae", type=float, default=0.22)
    parser.add_argument("--gun-hud-iou", type=float, default=0.20)
    parser.add_argument("--gun-route-timeout", type=float, default=1500)
    parser.add_argument("--required-matches", type=int, default=3)
    parser.add_argument("--hold-seconds", type=float, default=35)
    parser.add_argument("--fe0", action="store_true")
    parser.add_argument("--fe0-callback-replay", action="store_true")
    parser.add_argument("--nr-shadow-census", action="store_true")
    parser.add_argument("--nr-flip", action="store_true")
    parser.add_argument("--nr-clear", action="store_true")
    parser.add_argument("--nr-draw", action="store_true")
    parser.add_argument("--nr-vertical-shadow", action="store_true")
    parser.add_argument("--nr-vertical-active-basic", action="store_true")
    parser.add_argument("--nr-vertical-active-present", action="store_true")
    parser.add_argument("--nr-vertical-active-graphics", action="store_true")
    parser.add_argument("--nr-vertical-full-native", action="store_true")
    parser.add_argument("--nr-scanout-provenance", action="store_true")
    parser.add_argument("--nr-hana-input-oracle", action="store_true")
    parser.add_argument("--nr-submit-attribution", action="store_true")
    parser.add_argument("--nr-defer-reports", action="store_true")
    parser.add_argument("--nr-report-audit", action="store_true")
    parser.add_argument(
        "--nr-graph-execute", action="store_true",
        help="execute strict-native FIFO dependency islands through the fixed graph",
    )
    parser.add_argument(
        "--nr-single-pass-graph", choices=("passive", "execute"),
        help="record strict-owner dependency islands once at decode time",
    )
    parser.add_argument("--nr-single-pass-graph-timing", action="store_true")
    parser.add_argument(
        "--nr-graphics-families",
        help="comma-separated active-graphics rollout: draw,clear,transfer,sync,report",
    )
    parser.add_argument(
        "--nr-clear-scope",
        choices=("all", "color-only", "depth-only", "combined"),
        help="limit native clear ownership to one semantic clear class",
    )
    parser.add_argument(
        "--nr-frame-islands",
        action="store_true",
        help="transactionally own only complete preflighted FIFO sections",
    )
    parser.add_argument("--nr-pass-diag", action="store_true")
    parser.add_argument("--nr-shadow-consumer-admit", action="store_true")
    parser.add_argument("--nr-hana-depth-oracle", action="store_true")
    parser.add_argument("--nr-force-draw-input-refresh", action="store_true")
    parser.add_argument("--nr-draw-primitive", type=int, choices=range(0, 11))
    parser.add_argument("--draw-phases", action="store_true")
    parser.add_argument("--wkl4-cycle", action="store_true")
    parser.add_argument("--xf-ieee", action="store_true")
    parser.add_argument("--d3d-debug", action="store_true")
    parser.add_argument("--scan", nargs="+")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--reprocess-result", type=Path)
    args = parser.parse_args()
    if args.fe0_callback_replay and not args.fe0:
        parser.error("--fe0-callback-replay requires --fe0")
    if sum((args.fe0, args.nr_shadow_census, args.nr_flip, args.nr_clear,
            args.nr_draw, args.nr_vertical_shadow,
            args.nr_vertical_active_basic, args.nr_vertical_active_present,
            args.nr_vertical_active_graphics,
            args.nr_vertical_full_native,
            args.draw_phases,
            args.wkl4_cycle)) > 1:
        parser.error("select only one diagnostic/family lane")
    if args.nr_graphics_families and not args.nr_vertical_active_graphics:
        parser.error("--nr-graphics-families requires --nr-vertical-active-graphics")
    if args.nr_clear_scope and not args.nr_vertical_active_graphics:
        parser.error("--nr-clear-scope requires --nr-vertical-active-graphics")
    if args.nr_clear_scope and args.nr_graphics_families != "clear":
        parser.error("--nr-clear-scope requires --nr-graphics-families clear")
    if args.nr_frame_islands and not args.nr_vertical_active_graphics:
        parser.error("--nr-frame-islands requires --nr-vertical-active-graphics")
    if args.nr_pass_diag and not args.nr_frame_islands:
        parser.error("--nr-pass-diag requires --nr-frame-islands")
    if args.nr_shadow_consumer_admit and not args.nr_frame_islands:
        parser.error("--nr-shadow-consumer-admit requires --nr-frame-islands")
    if args.nr_force_draw_input_refresh and not args.nr_shadow_consumer_admit:
        parser.error(
            "--nr-force-draw-input-refresh requires --nr-shadow-consumer-admit"
        )
    if args.nr_draw_primitive is not None and not args.nr_frame_islands:
        parser.error("--nr-draw-primitive requires --nr-frame-islands")
    if args.nr_hana_depth_oracle and not args.nr_frame_islands:
        parser.error("--nr-hana-depth-oracle requires --nr-frame-islands")
    if args.nr_scanout_provenance and not args.nr_vertical_full_native:
        parser.error(
            "--nr-scanout-provenance requires --nr-vertical-full-native"
        )
    if args.nr_hana_input_oracle and not args.nr_vertical_full_native:
        parser.error(
            "--nr-hana-input-oracle requires --nr-vertical-full-native"
        )
    if args.nr_defer_reports and not args.nr_vertical_full_native:
        parser.error("--nr-defer-reports requires --nr-vertical-full-native")
    if args.nr_report_audit and not args.nr_defer_reports:
        parser.error("--nr-report-audit requires --nr-defer-reports")
    if args.nr_graph_execute and not args.nr_vertical_full_native:
        parser.error("--nr-graph-execute requires --nr-vertical-full-native")
    if args.nr_single_pass_graph and not args.nr_vertical_full_native:
        parser.error(
            "--nr-single-pass-graph requires --nr-vertical-full-native"
        )
    if args.nr_single_pass_graph and args.nr_graph_execute:
        parser.error("scanner graph and single-pass graph are mutually exclusive")
    if args.nr_single_pass_graph_timing and not args.nr_single_pass_graph:
        parser.error(
            "--nr-single-pass-graph-timing requires --nr-single-pass-graph"
        )

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
    if args.gun_reference_dir is None:
        args.gun_reference_dir = (
            root
            / "scratch"
            / "frontier-fullroute-20260820-accept20-capture"
        )
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
        if args.multi_scene_reference_dir:
            multi_scene_metrics(
                result,
                Path(result["present_qpc_path"]),
                args.multi_scene_reference_dir,
                args.anchor_mae,
            )
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
    if args.gun_route:
        if any((args.fe0, args.fe0_callback_replay, args.nr_shadow_census,
                args.nr_flip, args.nr_clear, args.nr_draw,
                args.draw_phases, args.wkl4_cycle)):
            parser.error("gun route accepts no diagnostic/family lane")
        return run_gun(args)
    return run(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"[akiyama-harness] ERROR: {error}", file=sys.stderr, flush=True)
        raise
