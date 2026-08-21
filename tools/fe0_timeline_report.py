#!/usr/bin/env python3
"""Summarize a shutdown-only FE0 timeline from a game stderr log."""

from __future__ import annotations

import argparse
import collections
import pathlib
import re
import statistics


EVENT_RE = re.compile(
    r"\[fe0-event\] seq=(\d+) qpc=(\d+) type=(\w+) tid=(\d+) "
    r"cause=([0-9A-Fa-f]+) actor=([0-9A-Fa-f]+) "
    r"a0=([0-9A-Fa-f]+) a1=([0-9A-Fa-f]+) "
    r"a2=([0-9A-Fa-f]+) a3=([0-9A-Fa-f]+)"
)
FREQUENCY_RE = re.compile(r"\[fe0-timeline\].*?frequency=(\d+)")


def parse(path: pathlib.Path):
    text = path.read_text(encoding="utf-8", errors="replace")
    frequency_match = FREQUENCY_RE.search(text)
    frequency = int(frequency_match.group(1)) if frequency_match else 10_000_000
    events = []
    for match in EVENT_RE.finditer(text):
        fields = match.groups()
        events.append(
            {
                "seq": int(fields[0]),
                "qpc": int(fields[1]),
                "type": fields[2],
                "tid": int(fields[3]),
                "cause": int(fields[4], 16),
                "actor": int(fields[5], 16),
                "args": tuple(int(value, 16) for value in fields[6:]),
            }
        )
    return frequency, events


def milliseconds(ticks: int, frequency: int) -> float:
    return ticks * 1000.0 / frequency


def summarize(frequency: int, events: list[dict]) -> None:
    print(f"events={len(events)} frequency={frequency}")
    print("types:")
    for name, count in collections.Counter(e["type"] for e in events).most_common():
        print(f"  {name:<22} {count}")

    by_cause: dict[int, list[dict]] = collections.defaultdict(list)
    for event in events:
        if event["cause"]:
            by_cause[event["cause"]].append(event)

    complete = []
    for cause, cause_events in by_cause.items():
        dispatch = [e for e in cause_events if e["type"] == "UCMD_DISPATCH"]
        waits = [e for e in cause_events if e["type"] == "RSX_WAIT"]
        ready = [e for e in cause_events if e["type"] == "RSX_READY"]
        published = [e for e in cause_events if e["type"] == "PUBLISHED"]
        if dispatch and waits and ready and published:
            complete.append((cause, cause_events, dispatch[0], waits[0], ready[-1]))

    print(f"complete_causes={len(complete)}")
    if not complete:
        return

    latencies = []
    callback_counts = []
    signal_counts = []
    origins = collections.Counter()
    for cause, cause_events, dispatch, wait, ready in complete:
        callbacks = sum(e["type"] == "CALLBACK_BEGIN" for e in cause_events)
        signals = [e for e in cause_events if e["type"] == "WKL4_SIGNAL"]
        callback_counts.append(callbacks)
        signal_counts.append(len(signals))
        origins.update(e["args"][3] for e in signals)
        latencies.append(
            (
                milliseconds(ready["qpc"] - wait["qpc"], frequency),
                milliseconds(ready["qpc"] - dispatch["qpc"], frequency),
                cause,
                callbacks,
                len(signals),
            )
        )

    wait_ms = [value[0] for value in latencies]
    dispatch_ms = [value[1] for value in latencies]
    print(
        "rsx_wait_ms "
        f"median={statistics.median(wait_ms):.3f} "
        f"mean={statistics.fmean(wait_ms):.3f} "
        f"p95={sorted(wait_ms)[max(0, int(len(wait_ms) * .95) - 1)]:.3f} "
        f"max={max(wait_ms):.3f}"
    )
    print(
        "dispatch_to_ready_ms "
        f"median={statistics.median(dispatch_ms):.3f} "
        f"mean={statistics.fmean(dispatch_ms):.3f} "
        f"max={max(dispatch_ms):.3f}"
    )
    print(
        f"callbacks_per_cause min={min(callback_counts)} "
        f"median={statistics.median(callback_counts):g} "
        f"max={max(callback_counts)}"
    )
    print(
        f"signals_per_cause min={min(signal_counts)} "
        f"median={statistics.median(signal_counts):g} max={max(signal_counts)}"
    )
    print("signal_origins " + " ".join(f"{key}={value}" for key, value in sorted(origins.items())))
    print("slowest:")
    for wait, dispatch, cause, callbacks, signals in sorted(latencies, reverse=True)[:12]:
        print(
            f"  cause=0x{cause:08X} wait={wait:9.3f}ms "
            f"dispatch={dispatch:9.3f}ms callbacks={callbacks} signals={signals}"
        )


def dump_cause(frequency: int, events: list[dict], cause: int) -> None:
    selected = [
        event
        for event in events
        if event["cause"] == cause
        and event["type"]
        not in {"WKL4_WAIT_ABI", "WKL4_EVENT_OBSERVE", "WKL4_EVENT_WRITE"}
    ]
    if not selected:
        print(f"cause 0x{cause:08X}: no retained events")
        return
    base = selected[0]["qpc"]
    print(f"cause 0x{cause:08X}: {len(selected)} events")
    for event in selected:
        args = " ".join(f"{value:08X}" for value in event["args"])
        print(
            f"  {event['seq']:7d} "
            f"{milliseconds(event['qpc'] - base, frequency):10.3f}ms "
            f"{event['type']:<18} actor={event['actor']:08X} {args}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=pathlib.Path)
    parser.add_argument("--cause", type=lambda value: int(value, 0))
    args = parser.parse_args()
    frequency, events = parse(args.log)
    summarize(frequency, events)
    if args.cause is not None:
        dump_cause(frequency, events, args.cause)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
