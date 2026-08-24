"""Compare archived legacy and strict-native Hana render targets."""

from __future__ import annotations

import argparse
import json
from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image


def components(mask: np.ndarray) -> list[dict[str, int]]:
    height, width = mask.shape
    seen = np.zeros_like(mask, dtype=bool)
    found: list[dict[str, int]] = []
    for y, x in zip(*np.nonzero(mask)):
        if seen[y, x]:
            continue
        queue = deque([(int(y), int(x))])
        seen[y, x] = True
        count = 0
        x0 = x1 = int(x)
        y0 = y1 = int(y)
        while queue:
            cy, cx = queue.popleft()
            count += 1
            x0, x1 = min(x0, cx), max(x1, cx)
            y0, y1 = min(y0, cy), max(y1, cy)
            for ny, nx in ((cy - 1, cx), (cy + 1, cx),
                           (cy, cx - 1), (cy, cx + 1)):
                if (0 <= ny < height and 0 <= nx < width and
                        mask[ny, nx] and not seen[ny, nx]):
                    seen[ny, nx] = True
                    queue.append((ny, nx))
        found.append({"pixels": count, "x0": x0, "y0": y0,
                      "x1": x1, "y1": y1})
    return sorted(found, key=lambda entry: entry["pixels"], reverse=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("legacy", type=Path)
    parser.add_argument("native", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--threshold", type=int, default=16)
    args = parser.parse_args()

    legacy = np.asarray(Image.open(args.legacy).convert("RGB"), dtype=np.int16)
    native = np.asarray(Image.open(args.native).convert("RGB"), dtype=np.int16)
    if legacy.shape != native.shape:
        raise SystemExit(f"shape mismatch: {legacy.shape} vs {native.shape}")

    args.output.mkdir(parents=True, exist_ok=True)
    delta = np.abs(native - legacy)
    peak = delta.max(axis=2)
    mask = peak >= args.threshold
    heat = np.zeros((*mask.shape, 3), dtype=np.uint8)
    heat[..., 0] = peak.astype(np.uint8)
    heat[..., 1] = np.where(mask, 64, 0).astype(np.uint8)
    overlay = np.asarray(Image.open(args.legacy).convert("RGB"), dtype=np.uint8).copy()
    overlay[mask] = (overlay[mask] // 3 + np.array([170, 0, 0], dtype=np.uint8))

    Image.fromarray(legacy.astype(np.uint8)).save(args.output / "legacy.png")
    Image.fromarray(native.astype(np.uint8)).save(args.output / "native.png")
    Image.fromarray(heat).save(args.output / "diff_heat.png")
    Image.fromarray(overlay).save(args.output / "diff_overlay.png")

    channel_stats = []
    for channel, name in enumerate(("r", "g", "b")):
        values = delta[..., channel]
        channel_stats.append({
            "channel": name,
            "mean_abs": float(values.mean()),
            "p95_abs": float(np.percentile(values, 95)),
            "max_abs": int(values.max()),
            "changed_gt_16": int((values >= args.threshold).sum()),
        })
    report = {
        "shape": list(legacy.shape),
        "threshold": args.threshold,
        "pixels_over_threshold": int(mask.sum()),
        "fraction_over_threshold": float(mask.mean()),
        "bbox": None,
        "channel_stats": channel_stats,
        "components": components(mask)[:64],
    }
    if mask.any():
        ys, xs = np.nonzero(mask)
        report["bbox"] = [int(xs.min()), int(ys.min()),
                          int(xs.max()), int(ys.max())]
    (args.output / "report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
