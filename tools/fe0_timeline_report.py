#!/usr/bin/env python3
"""Summarize a shutdown-only FE0 timeline from a game stderr log."""

from __future__ import annotations

import argparse
import collections
import pathlib
import re
import statistics


EVENT_RE = re.compile(
    r"\[fe0-event\] seq=(\d+) qpc=(\d+)(?: cpu100ns=(\d+))?"
    r"(?: cycles=(\d+))? "
    r"type=(\w+) tid=(\d+) "
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
                "cpu100ns": int(fields[2]) if fields[2] else 0,
                "cycles": int(fields[3]) if fields[3] else 0,
                "type": fields[4],
                "tid": int(fields[5]),
                "cause": int(fields[6], 16),
                "actor": int(fields[7], 16),
                "args": tuple(int(value, 16) for value in fields[8:]),
            }
        )
    return frequency, events


def milliseconds(ticks: int, frequency: int) -> float:
    return ticks * 1000.0 / frequency


def describe_ms(name: str, values: list[float]) -> None:
    if not values:
        return
    ordered = sorted(values)
    print(
        f"{name} median={statistics.median(values):.3f} "
        f"mean={statistics.fmean(values):.3f} "
        f"p95={ordered[max(0, int(len(ordered) * .95) - 1)]:.3f} "
        f"max={max(values):.3f}"
    )


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
    wake_origins = collections.Counter()
    stage_values: dict[str, list[float]] = collections.defaultdict(list)
    execution_cpu_ms = []
    segment_stats: dict[tuple[int, int, int, int], dict[str, int]] = (
        collections.defaultdict(lambda: {
            "count": 0, "wall_ticks": 0, "cpu100ns": 0, "cycles": 0
        })
    )
    for cause, cause_events, dispatch, wait, ready in complete:
        callbacks = sum(e["type"] == "CALLBACK_BEGIN" for e in cause_events)
        signals = [e for e in cause_events if e["type"] == "WKL4_SIGNAL"]
        callback_counts.append(callbacks)
        signal_counts.append(len(signals))
        origins.update(e["args"][3] for e in signals)
        wake_origins.update(
            e["args"][3] >> 8
            for e in cause_events
            if e["type"] == "WKL4_WAKE"
        )
        callback_begin = next(
            (e for e in cause_events if e["type"] == "CALLBACK_BEGIN"), None
        )
        callback_end = next(
            (e for e in cause_events if e["type"] == "CALLBACK_END"), None
        )
        records = [e for e in cause_events if e["type"] == "WKL4_RECORD"]
        dma = next((e for e in cause_events if e["type"] == "DMA_BEGIN"), None)
        published_event = next(
            (e for e in cause_events if e["type"] == "PUBLISHED"), None
        )
        if callback_begin:
            stage_values["dispatch_to_callback_ms"].append(
                milliseconds(callback_begin["qpc"] - dispatch["qpc"], frequency)
            )
        if callback_begin and callback_end:
            stage_values["callback_ms"].append(
                milliseconds(callback_end["qpc"] - callback_begin["qpc"], frequency)
            )
        if callback_end and records:
            stage_values["callback_to_first_record_ms"].append(
                milliseconds(records[0]["qpc"] - callback_end["qpc"], frequency)
            )
        if records and dma:
            stage_values["image4_record_to_dma_ms"].append(
                milliseconds(dma["qpc"] - records[0]["qpc"], frequency)
            )
        if dma and published_event:
            stage_values["dma_to_published_ms"].append(
                milliseconds(published_event["qpc"] - dma["qpc"], frequency)
            )
        if published_event:
            stage_values["published_to_rsx_ready_ms"].append(
                milliseconds(ready["qpc"] - published_event["qpc"], frequency)
            )

        active: dict[tuple[int, int], dict] = {}
        cpu_ticks = 0
        for event in sorted(cause_events, key=lambda item: item["seq"]):
            key = (event["tid"], event["actor"])
            if event["type"] == "WKL4_RESUME" and event["cpu100ns"]:
                active[key] = event
            elif event["type"] == "WKL4_HANDOFF" and event["cpu100ns"]:
                resumed = active.pop(key, None)
                if resumed and event["cpu100ns"] >= resumed["cpu100ns"]:
                    cpu_ticks += event["cpu100ns"] - resumed["cpu100ns"]
                if resumed:
                    segment = segment_stats[(
                        event["actor"], resumed["args"][3],
                        event["args"][3], event["args"][2]
                    )]
                    segment["count"] += 1
                    segment["wall_ticks"] += max(
                        0, event["qpc"] - resumed["qpc"]
                    )
                    if event["cpu100ns"] >= resumed["cpu100ns"]:
                        segment["cpu100ns"] += (
                            event["cpu100ns"] - resumed["cpu100ns"]
                        )
                    if event["cycles"] >= resumed["cycles"]:
                        segment["cycles"] += event["cycles"] - resumed["cycles"]
        if cpu_ticks:
            execution_cpu_ms.append(cpu_ticks / 10000.0)
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
    print("wake_origins " + " ".join(
        f"{key}={value}" for key, value in sorted(wake_origins.items())
    ))
    print("stages:")
    for name in (
        "dispatch_to_callback_ms", "callback_ms",
        "callback_to_first_record_ms", "image4_record_to_dma_ms",
        "dma_to_published_ms", "published_to_rsx_ready_ms",
    ):
        describe_ms(f"  {name}", stage_values[name])
    describe_ms("  image4_paired_thread_cpu_ms", execution_cpu_ms)
    if segment_stats:
        print("image4_segments:")
        ranked = sorted(
            segment_stats.items(),
            key=lambda item: (item[1]["cycles"], item[1]["cpu100ns"],
                              item[1]["wall_ticks"]),
            reverse=True,
        )
        for (actor, resume_pc, handoff_pc, op), stat in ranked[:20]:
            count = stat["count"]
            print(
                f"  task={actor} resume=0x{resume_pc:05X} "
                f"handoff=0x{handoff_pc:05X} op={op} n={count} "
                f"wall_ms={milliseconds(stat['wall_ticks'], frequency):.3f} "
                f"cpu_ms={stat['cpu100ns'] / 10000.0:.3f} "
                f"cycles={stat['cycles']} cycles_each={stat['cycles'] / count:.1f}"
            )
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
