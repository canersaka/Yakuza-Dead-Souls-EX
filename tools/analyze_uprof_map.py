#!/usr/bin/env python3
"""Resolve address-only AMD uProf CSV samples with an MSVC linker MAP."""

from __future__ import annotations

import argparse
import bisect
import csv
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
import re


MAP_SYMBOL = re.compile(
    r"^\s+[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+(\S+)\s+"
    r"([0-9A-Fa-f]{16})"
)
THREAD_REPORT = re.compile(r"^PROFILE REPORT FOR THREAD - Thread-(\d+)")
YAKUZA_ADDRESS = re.compile(r"^yakuza_recomp\.exe!0x([0-9A-Fa-f]+)$")


@dataclass(frozen=True)
class Symbol:
    address: int
    name: str


class MapResolver:
    def __init__(self, path: Path) -> None:
        symbols = []
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            match = MAP_SYMBOL.match(line)
            if match:
                symbols.append(Symbol(int(match.group(2), 16), match.group(1)))
        self.symbols = sorted(symbols, key=lambda symbol: symbol.address)
        self.addresses = [symbol.address for symbol in self.symbols]

    def resolve(self, function: str) -> tuple[str, int | None]:
        match = YAKUZA_ADDRESS.match(function)
        if not match:
            return function, None
        address = int(match.group(1), 16)
        index = bisect.bisect_right(self.addresses, address) - 1
        if index < 0:
            return function, None
        symbol = self.symbols[index]
        return symbol.name, address - symbol.address


def parse_function_rows(lines: list[str], start: int) -> tuple[list[list[str]], int]:
    rows: list[list[str]] = []
    index = start
    while index < len(lines):
        line = lines[index]
        if not line.strip():
            break
        row = next(csv.reader([line]))
        if row and row[0] != "FUNCTION":
            rows.append(row)
        index += 1
    return rows, index


def aggregate_rows(
    rows: list[list[str]], resolver: MapResolver
) -> list[tuple[float, str, set[int]]]:
    totals: dict[str, float] = defaultdict(float)
    offsets: dict[str, set[int]] = defaultdict(set)
    for row in rows:
        if len(row) < 2:
            continue
        try:
            seconds = float(row[1])
        except ValueError:
            continue
        name, offset = resolver.resolve(row[0])
        totals[name] += seconds
        if offset is not None:
            offsets[name].add(offset)
    return sorted(
        ((seconds, name, offsets[name]) for name, seconds in totals.items()),
        reverse=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    parser.add_argument("map", type=Path)
    parser.add_argument("--threads", type=int, default=16)
    parser.add_argument("--functions", type=int, default=15)
    args = parser.parse_args()

    resolver = MapResolver(args.map)
    lines = args.report.read_text(encoding="utf-8", errors="replace").splitlines()

    thread_totals: dict[str, float] = {}
    thread_rows: dict[str, list[list[str]]] = {}
    current_thread: str | None = None
    index = 0
    while index < len(lines):
        thread_match = THREAD_REPORT.match(lines[index])
        if thread_match:
            current_thread = thread_match.group(1)
        if current_thread and lines[index] == "THREAD,\"CPU_TIME\" (seconds),THREAD ID":
            if index + 1 < len(lines):
                row = next(csv.reader([lines[index + 1]]))
                if len(row) >= 2:
                    thread_totals[current_thread] = float(row[1])
        if current_thread and lines[index] == "FUNCTION SUMMARY":
            rows, index = parse_function_rows(lines, index + 1)
            thread_rows[current_thread] = rows
        index += 1

    print("THREAD EXCLUSIVE HOTSPOTS")
    for tid, total in sorted(thread_totals.items(), key=lambda item: -item[1])[
        : args.threads
    ]:
        print(f"\nTID {tid} total={total:.3f}s")
        for seconds, name, offsets in aggregate_rows(
            thread_rows.get(tid, []), resolver
        )[: args.functions]:
            offset_note = f" addresses={len(offsets)}" if offsets else ""
            print(f"  {seconds:8.3f}s  {name}{offset_note}")

    callgraph_header = (
        "FUNCTION,SELF SAMPLES (seconds),DEEP SAMPLES (seconds),"
        "% DEEP SAMPLES,PATH COUNT,SOURCE FILE,MODULE"
    )
    if callgraph_header in lines:
        print("\nPROCESS CALLGRAPH ROOTS")
        start = lines.index(callgraph_header) + 1
        rows, _ = parse_function_rows(lines, start)
        resolved = []
        for row in rows:
            if len(row) < 5:
                continue
            try:
                self_seconds = float(row[1])
                deep_seconds = float(row[2])
                path_count = int(row[4])
            except ValueError:
                continue
            name, offset = resolver.resolve(row[0])
            resolved.append((deep_seconds, self_seconds, path_count, name, offset))
        for deep, self_seconds, paths, name, offset in sorted(
            resolved, reverse=True
        )[:80]:
            offset_note = f"+0x{offset:x}" if offset is not None else ""
            print(
                f"  deep={deep:8.3f}s self={self_seconds:7.3f}s "
                f"paths={paths:6d}  {name}{offset_note}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
