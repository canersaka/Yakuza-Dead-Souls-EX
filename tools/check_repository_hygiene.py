#!/usr/bin/env python3
"""Reject tracked generated lifts, proprietary payloads, and accidental large blobs."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
MAX_TRACKED_FILE_SIZE = 5 * 1024 * 1024

FORBIDDEN_EXACT_PATHS = {
    "functions.json",
    "yakuza/func_table_gen.cpp",
    "yakuza/import_bridges_gen.cpp",
    "yakuza/imports.json",
}

FORBIDDEN_PREFIXES = (
    "game/",
    "gamedata/",
    "recomp/",
    "recompiled/",
    "recomp_prx/",
    "refs/",
    "scratch/",
    "scratchpad/",
    "yakuza/generated/",
)

FORBIDDEN_EXTENSIONS = {
    ".bin",
    ".edat",
    ".elf",
    ".iso",
    ".keys",
    ".pkg",
    ".png",
    ".ppm",
    ".prx",
    ".pup",
    ".rap",
    ".rrc",
    ".self",
    ".sfo",
    ".sprx",
    ".tty",
}

FORBIDDEN_COMPOUND_EXTENSIONS = (".rrc.gz",)

FORBIDDEN_GENERATED_NAMES = re.compile(
    r"(?:^|/)(?:"
    r"ppu_recomp(?:_\d+)?|"
    r"(?:libgcm_sys|libsre|pxd_shader)_recomp(?:_\d+)?|"
    r"spuimg_[0-9a-f]+(?:_fast)?|"
    r"job_bin_[^/]+(?:_fast)?|"
    r"(?:cri_audio|spurs_kernel2|spurs_sysservice|policy_module|job_module|ts_exit)(?:_fast)?"
    r")\.(?:c|cc|cpp|h|hpp)$",
    re.IGNORECASE,
)


def tracked_paths() -> list[str]:
    output = subprocess.check_output(
        ["git", "ls-files", "-z"], cwd=REPOSITORY_ROOT
    )
    return [
        item.decode("utf-8", errors="surrogateescape").replace("\\", "/")
        for item in output.split(b"\0")
        if item
    ]


def main() -> int:
    violations: list[str] = []

    for relative_path in tracked_paths():
        folded = relative_path.casefold()
        path = REPOSITORY_ROOT / relative_path

        if folded in FORBIDDEN_EXACT_PATHS or folded.startswith(FORBIDDEN_PREFIXES):
            violations.append(f"forbidden path: {relative_path}")
            continue

        if (
            Path(folded).suffix in FORBIDDEN_EXTENSIONS
            or folded.endswith(FORBIDDEN_COMPOUND_EXTENSIONS)
        ):
            violations.append(f"forbidden payload type: {relative_path}")
            continue

        if FORBIDDEN_GENERATED_NAMES.search(relative_path):
            violations.append(f"generated lift filename: {relative_path}")
            continue

        if path.is_file() and path.stat().st_size > MAX_TRACKED_FILE_SIZE:
            violations.append(
                f"tracked file exceeds 5 MiB review limit: {relative_path} "
                f"({path.stat().st_size} bytes)"
            )

    if violations:
        print("Repository hygiene check failed:", file=sys.stderr)
        for violation in violations:
            print(f"  - {violation}", file=sys.stderr)
        print(
            "Generated lifts, game/firmware payloads, captures, and local oracle data "
            "must remain outside Git history.",
            file=sys.stderr,
        )
        return 1

    print("Repository hygiene check passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
