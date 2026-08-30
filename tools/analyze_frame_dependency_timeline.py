#!/usr/bin/env python3
"""Analyze the fixed-memory gun2 semantic timeline inside its clean QPC window."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
import statistics


HEADER = re.compile(r"^\[frame-dep\].*frequency=(\d+).*$")
EVENT = re.compile(
    r"^\[frame-dep-event\] seq=(\d+) qpc=(\d+) type=([A-Z_]+) "
    r"frame=(\d+) dep=(\d+) tid=(\d+) a0=([0-9A-F]+) "
    r"a1=([0-9A-F]+) a2=([0-9A-F]+) a3=([0-9A-F]+)$"
)
TAIL_FRAME = re.compile(
    r"^\[nr-rsx-tail-frame qpc=(\d+) gpu-frequency=(\d+) "
    r"present=(\d+) end=(\d+) .*$"
)
TAIL_SUBMIT = re.compile(
    r"^\[nr-rsx-tail-frame-submit present=(\d+) cause=([^ ]+) "
    r"submits=(\d+) cpu=(\d+) gpu=(\d+)/(\d+)\]$"
)


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1,
                       round((len(ordered) - 1) * fraction))]


def stats(values):
    if not values:
        return {"count": 0}
    return {
        "count": len(values), "mean_ms": statistics.fmean(values),
        "median_ms": statistics.median(values),
        "p95_ms": percentile(values, .95),
        "p99_ms": percentile(values, .99), "max_ms": max(values),
    }


def pair_intervals(events, start_type, complete_type, key):
    open_events = defaultdict(list)
    result = []
    for event in events:
        identity = key(event)
        if event["type"] == start_type:
            open_events[identity].append(event)
        elif event["type"] == complete_type and open_events[identity]:
            start = open_events[identity].pop()
            if event["qpc"] >= start["qpc"]:
                result.append((start, event))
    return result


def union_ticks(intervals, begin, end):
    clipped = sorted((max(begin, left), min(end, right))
                     for left, right in intervals
                     if right > begin and left < end)
    total = 0
    cursor = begin
    for left, right in clipped:
        left = max(left, cursor)
        if right > left:
            total += right - left
            cursor = right
    return total


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("route_result", type=Path)
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()
    route = json.loads(args.route_result.read_text(encoding="utf-8"))
    begin = int(route["measurement_start_qpc"])
    end = int(route["measurement_end_qpc"])
    frequency = 0
    events = []
    tail_frames = {}
    tail_submits = defaultdict(list)
    for raw in args.log.read_text(encoding="utf-8", errors="replace").splitlines():
        if match := HEADER.match(raw):
            frequency = int(match.group(1))
        if match := EVENT.match(raw):
            values = match.groups()
            event = {
                "seq": int(values[0]), "qpc": int(values[1]),
                "type": values[2], "frame": int(values[3]),
                "dep": int(values[4]), "tid": int(values[5]),
                "a": tuple(int(value, 16) for value in values[6:]),
            }
            if begin <= event["qpc"] <= end:
                events.append(event)
        if match := TAIL_FRAME.match(raw):
            qpc_freq, gpu_freq, present, qpc_end = map(int, match.groups())
            tail_frames[present] = (qpc_freq, gpu_freq, qpc_end)
        if match := TAIL_SUBMIT.match(raw):
            present, cause, submits, cpu, intervals, gpu = match.groups()
            tail_submits[int(present)].append({
                "cause": cause, "submits": int(submits), "cpu": int(cpu),
                "intervals": int(intervals), "gpu": int(gpu),
            })
    if not frequency or not events:
        raise SystemExit("no timeline events in clean measurement window")
    to_ms = 1000.0 / frequency
    presents = [event for event in events if event["type"] == "PRESENT"]
    frames = list(zip(presents, presents[1:]))

    updates = pair_intervals(
        events, "PPU_UPDATE_START", "PPU_UPDATE_COMPLETE",
        lambda event: (event["tid"], event["frame"]),
    )
    jobs = pair_intervals(
        events, "SPU_JOB_START", "SPU_JOB_COMPLETE",
        lambda event: (event["tid"], event["a"]),
    )
    tasks = pair_intervals(
        events, "SPU_TASK_START", "SPU_TASK_COMPLETE",
        lambda event: (event["tid"], event["a"][:3]),
    )
    waits = pair_intervals(
        events, "PPU_WAIT_ENTER", "PPU_WAIT_EXIT",
        lambda event: event["dep"],
    )
    wait_by_dep = {left["dep"]: (left, right) for left, right in waits}
    dma_by_dep = defaultdict(list)
    fifo_by_dep = defaultdict(list)
    consume_by_dep = defaultdict(list)
    for event in events:
        if event["type"] == "DMA_PUBLISH": dma_by_dep[event["dep"]].append(event)
        elif event["type"] == "FIFO_PUBLISH": fifo_by_dep[event["dep"]].append(event)
        elif event["type"] == "RSX_CONSUME": consume_by_dep[event["dep"]].append(event)

    stage_values = defaultdict(list)
    # Version 2 did not print its private monotonic FIFO generation.  Recover
    # the exact relation from the recorder's invariant: one RSX_CONSUME is
    # emitted only by the CAS that consumes the newest pending FIFO generation.
    # Preserve collapse counts rather than assuming timestamp proximity.
    pending_fifo = []
    fifo_consume_pairs = []
    consume_to_fifo = {}
    for event in events:
        if event["type"] == "FIFO_PUBLISH":
            pending_fifo.append(event)
        elif event["type"] == "RSX_CONSUME" and pending_fifo:
            fifo = pending_fifo[-1]
            fifo_consume_pairs.append((fifo, event, len(pending_fifo)))
            consume_to_fifo[event["seq"]] = fifo
            pending_fifo.clear()
    stage_values["fifo_publish_to_rsx_consume"] = [
        (consume["qpc"] - fifo["qpc"]) * to_ms
        for fifo, consume, _ in fifo_consume_pairs
    ]

    latest_workers = Counter()
    representative = None
    frame_rows = []
    update_plain = [(a["qpc"], b["qpc"]) for a, b in updates]
    job_plain = [(a["qpc"], b["qpc"]) for a, b in jobs]
    task_plain = [(a["qpc"], b["qpc"]) for a, b in tasks]
    stage_values["ppu_dependency_wait"] = [
        (right["qpc"] - left["qpc"]) * to_ms for left, right in waits
    ]
    exact_correlated_frames = 0
    for previous, present in frames:
        left, right = previous["qpc"], present["qpc"]
        period = (right - left) * to_ms
        stage_values["present_to_present"].append(period)
        frame_events = [e for e in events if left < e["qpc"] <= right]
        consumes = [e for e in frame_events if e["type"] == "RSX_CONSUME"]
        completes = [e for e in frame_events if e["type"] == "FRAME_COMPLETE"]
        submissions = [e for e in frame_events if e["type"] == "SUBMISSION"]
        row = {
            "present": present["a"][1], "period_ms": period,
            "submissions": len(submissions),
            "ppu_update_union_ms": union_ticks(update_plain, left, right) * to_ms,
            "spu_job_union_ms": union_ticks(job_plain, left, right) * to_ms,
            "spu_task_union_ms": union_ticks(task_plain, left, right) * to_ms,
        }
        stage_values["ppu_update_wall_union"].append(row["ppu_update_union_ms"])
        stage_values["spu_job_wall_union"].append(row["spu_job_union_ms"])
        contained_updates = [(start, complete) for start, complete in updates
                             if left <= start["qpc"] and
                             complete["qpc"] <= right]
        if contained_updates:
            update_start, update_complete = contained_updates[-1]
            stage_values["previous_present_to_update_start"].append(
                (update_start["qpc"] - left) * to_ms)
            stage_values["ppu_update_complete_to_present"].append(
                (right - update_complete["qpc"]) * to_ms)
        if consumes:
            consume = consumes[-1]
            exact_fifo = consume_to_fifo.get(consume["seq"])
            dep = consume["dep"]
            row["dependency_generation"] = dep
            stage_values["latest_fifo_consumption_to_present"].append(
                (right - consume["qpc"]) * to_ms)
            chain = {"consume": consume}
            if exact_fifo:
                chain["fifo"] = exact_fifo
                stage_values["latest_fifo_publish_to_present"].append(
                    (right - exact_fifo["qpc"]) * to_ms)
            pair = wait_by_dep.get(dep)
            if pair:
                wait_enter, wait_exit = pair
                chain["wait_enter"] = wait_enter
                chain["wait_exit"] = wait_exit
                dmas = [event for event in dma_by_dep.get(dep, [])
                        if event["qpc"] <= wait_exit["qpc"]]
                if dmas:
                    dma = dmas[-1]
                    chain["dma"] = dma
                    stage_values["dma_publish_to_wait_exit"].append(
                        (wait_exit["qpc"] - dma["qpc"]) * to_ms)
                    enclosing = [(a, b) for a, b in jobs
                                 if a["tid"] == dma["tid"] and
                                 a["qpc"] <= dma["qpc"] <= b["qpc"]]
                    if enclosing:
                        job_start, job_end = enclosing[-1]
                        identity = (job_start["a"][0], job_start["a"][2],
                                    job_start["a"][3], job_start["tid"])
                        latest_workers[identity] += 1
                        chain["job_start"] = job_start
                        chain["job_complete"] = job_end
                        stage_values["controlling_job_start_to_dma"].append(
                            (dma["qpc"] - job_start["qpc"]) * to_ms)
                fifos = [event for event in fifo_by_dep.get(dep, [])
                         if wait_exit["qpc"] <= event["qpc"] <= consume["qpc"]
                         and event["frame"] == wait_exit["frame"]]
                # A dependency identity is exact only when its publication,
                # wait completion, FIFO publication, and consumption all
                # appear in this frame.  The latest-completed marker may
                # otherwise remain unchanged for many unrelated frames.
                if fifos and dmas and wait_exit["qpc"] >= left:
                    fifo = fifos[-1]
                    chain["fifo"] = fifo
                    exact_correlated_frames += 1
                    stage_values["wait_exit_to_fifo_publish"].append(
                        (fifo["qpc"] - wait_exit["qpc"]) * to_ms)
                    stage_values["fifo_publish_to_rsx_consume"].append(
                        (consume["qpc"] - fifo["qpc"]) * to_ms)
            if representative is None or period > representative[0]:
                representative = (period, present, chain)
        frame_rows.append(row)

    measurement_frames = max(1, int(route.get("measurement_presents", 1)) - 1)
    thread_cpu = {
        int(tid): ticks / 10000.0 / measurement_frames
        for tid, ticks in route.get("thread_cpu_100ns", {}).items()
    }
    thread_roles = defaultdict(set)
    for event in events:
        if event["type"].startswith("PPU_"): thread_roles[event["tid"]].add("PPU")
        elif event["type"].startswith("SPU_") or event["type"] == "SPURS_SCHEDULE":
            thread_roles[event["tid"]].add("SPU/SPURS")
        elif event["type"] in {"RSX_CONSUME", "FRAME_COMPLETE"}:
            thread_roles[event["tid"]].add("RSX")

    gpu_frame_ms = []
    gpu_submit_count = []
    for present, (qpc_freq, gpu_freq, qpc_end) in tail_frames.items():
        if not (begin <= qpc_end <= end) or not gpu_freq:
            continue
        submits = tail_submits.get(present, [])
        gpu_frame_ms.append(sum(item["gpu"] for item in submits) * 1000.0 / gpu_freq)
        gpu_submit_count.append(sum(item["submits"] for item in submits))

    output = {
        "benchmark": route.get("benchmark"),
        "measurement_frames": len(frames),
        "full_qpc_measurement_frames": measurement_frames,
        "event_counts": Counter(event["type"] for event in events),
        "serial_stage_ms": {name: stats(values)
                            for name, values in stage_values.items()},
        "gpu_active_ms": stats(gpu_frame_ms),
        "gpu_submissions": stats(gpu_submit_count),
        "thread_cpu_ms_per_present": [
            {"thread_id": tid, "roles": sorted(thread_roles.get(tid, {"other"})),
             "cpu_ms": value}
            for tid, value in sorted(thread_cpu.items(),
                                     key=lambda item: item[1], reverse=True)
        ],
        "latest_dependency_workers": [
            {"image_id": identity[0], "workload_id": identity[1],
             "descriptor_ea": f"0x{identity[2]:08X}",
             "thread_id": identity[3], "frames": count,
             "share_percent": 100.0 * count / max(1, len(frames))}
            for identity, count in latest_workers.most_common(12)
        ],
        "exact_correlated_dependency_frames": exact_correlated_frames,
        "exact_fifo_consume_pairs": len(fifo_consume_pairs),
        "collapsed_fifo_publications": sum(
            max(0, count - 1) for _, _, count in fifo_consume_pairs),
    }
    if representative:
        period, present, chain = representative
        output["representative_frame"] = {
            "present_id": present["a"][1], "period_ms": period,
            "dependency_generation": chain["consume"]["dep"],
            "events": {
                name: {"qpc": event["qpc"], "thread_id": event["tid"],
                       "frame_generation": event["frame"],
                       "args": [f"0x{value:08X}" for value in event["a"]]}
                for name, event in chain.items()
            },
        }
    present_median = output["serial_stage_ms"].get(
        "present_to_present", {}).get("median_ms", 0.0)
    explained_names = (
        "controlling_job_start_to_dma", "dma_publish_to_wait_exit",
        "wait_exit_to_fifo_publish", "fifo_publish_to_rsx_consume",
        "latest_fifo_consumption_to_present",
    )
    explained = sum(output["serial_stage_ms"].get(name, {}).get("median_ms", 0.0)
                    for name in explained_names)
    output["median_serial_explained_ms"] = explained
    output["median_unattributed_ms"] = max(0.0, present_median - explained)
    output["notes"] = [
        "SPU/PPU interval unions are wall intervals and may overlap; they are not summed as frame time.",
        "Per-thread CPU is exact OS CPU delta divided by presented frames.",
        "GPU active time comes from asynchronous D3D12 timestamps resolved only at shutdown.",
        "Jobs spanning Presents retain their real start/completion frame generations.",
        "FRAME_COMPLETE is guest flip/publication completion after Present in this build; it is not counted as pre-Present RSX work.",
    ]
    if args.json_out:
        args.json_out.write_text(json.dumps(output, indent=2) + "\n",
                                 encoding="utf-8")
    print(json.dumps(output, indent=2))


if __name__ == "__main__":
    main()
