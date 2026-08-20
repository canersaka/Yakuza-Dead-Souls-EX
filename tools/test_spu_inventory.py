#!/usr/bin/env python3
"""Regression fixtures for the SPU inventory/completeness gate."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import struct
import tempfile

from spu_inventory import build_inventory
from wrap_spu_elf import wrap


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) & ~(boundary - 1)


def make_ppu_elf(payload: bytes, base: int) -> bytes:
    """Minimal BE64 PPC ELF with one loaded, writable PROGBITS section."""
    payload_offset = 0x100
    section_offset = align(payload_offset + len(payload), 8)
    ident = b"\x7fELF\x02\x02\x01\x00" + b"\x00" * 8
    header = ident + struct.pack(
        ">HHIQQQIHHHHHH",
        2, 21, 1, base, 64, section_offset, 0,
        64, 56, 1, 64, 2, 0,
    )
    program = struct.pack(
        ">IIQQQQQQ",
        1, 6, payload_offset, base, base, len(payload), len(payload), 0x100,
    )
    out = bytearray(header + program)
    out += b"\x00" * (payload_offset - len(out))
    out += payload
    out += b"\x00" * (section_offset - len(out))
    out += b"\x00" * 64
    out += struct.pack(
        ">IIQQQQIIQQ",
        0, 1, 3, base, payload_offset, len(payload), 0, 0, 0x80, 0,
    )
    return bytes(out)


def put_descriptor(payload: bytearray, offset: int, binary_ea: int,
                   binary_size: int, *, job_type: int = 0) -> None:
    assert binary_size % 16 == 0
    struct.pack_into(">QHH", payload, offset, binary_ea, binary_size // 16, 0)
    struct.pack_into(">II", payload, offset + 0x14, 0, 0)
    struct.pack_into(">HH", payload, offset + 0x1C, 0, 0)
    struct.pack_into(">I", payload, offset + 0x24, 0)
    payload[offset + 0x2C] = job_type


def fixture() -> tuple[bytes, dict, int]:
    base = 0x00100000
    job_size = 0x600
    payload = bytearray((index * 37 + 11) & 0xFF for index in range(job_size))
    # The same valid descriptor identity appears twice and must deduplicate.
    put_descriptor(payload, 0x100, base, job_size)
    put_descriptor(payload, 0x140, base, job_size)
    # Descriptor-shaped bytes with binary2/jobType set are not legacy jobs.
    put_descriptor(payload, 0x180, base, job_size, job_type=1)

    embedded = []
    for code_size in (0x440, 0x540):
        while len(payload) % 0x80:
            payload.append(0)
        ea = base + len(payload)
        elf = wrap(bytes((i * 13 + code_size) & 0xFF for i in range(code_size)))
        payload.extend(elf)
        embedded.append((ea, len(elf)))
    payload.extend(b"\x00" * 0x80)

    manifest = {
        "families": {
            "job_e": {
                "kind": "job",
                "eboot_ea": hex(base),
                "logical_size": hex(job_size),
                "placements": [
                    {"stem": "job_e_e400", "base": "0xE400", "image_id": 49},
                    {"stem": "job_e_4c00", "base": "0x4C00", "image_id": 50},
                ],
            },
            "task_images": {
                "kind": "elf",
                "images": [
                    {"stem": "task_0", "eboot_ea": hex(embedded[0][0]), "image_id": 0},
                    {"stem": "task_1", "eboot_ea": hex(embedded[1][0]), "image_id": 1},
                ],
            },
        }
    }
    return make_ppu_elf(payload, base), manifest, base


def run_inventory(temp: Path, elf_bytes: bytes, manifest: dict,
                  evidence: list[Path] | None = None):
    elf_path = temp / "fixture.elf"
    manifest_path = temp / "manifest.json"
    elf_path.write_bytes(elf_bytes)
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return build_inventory([elf_path], manifest_path, [temp], 0x100, evidence)


def main() -> None:
    elf_bytes, manifest, base = fixture()
    with tempfile.TemporaryDirectory(prefix="spu-inventory-") as directory:
        temp = Path(directory)

        complete = run_inventory(temp, elf_bytes, manifest)
        assert complete["summary"]["errors"] == 0, complete["errors"]
        assert complete["summary"]["matched_embedded_spu_elfs"] == 2
        assert complete["summary"]["dense_payload_sections"] == 1
        descriptors = complete["static_descriptors"]
        assert len(descriptors) == 1, descriptors
        assert descriptors[0]["matched_family"] == "job_e"
        assert len(descriptors[0]["descriptor_locations"]) == 2

        known_evidence = temp / "known.jsonl"
        known_evidence.write_text(json.dumps({
            "binary_ea": f"0x{base:08X}",
            "binary_size": "0x600",
            "fingerprint": complete["jobs"][0]["runtime_fingerprints"]["0x600"],
        }) + "\n", encoding="utf-8")
        known_report = run_inventory(temp, elf_bytes, manifest, [known_evidence])
        assert known_report["runtime_evidence"][0]["matched_family"] == "job_e"
        assert not any(e["kind"] == "unregistered_runtime_job_evidence"
                       for e in known_report["errors"])

        changed_evidence = temp / "changed.jsonl"
        changed_evidence.write_text(json.dumps({
            "binary_ea": f"0x{base:08X}",
            "binary_size": "0x600",
            "fingerprint": "0x0000000000000001",
        }) + "\n", encoding="utf-8")
        changed_report = run_inventory(temp, elf_bytes, manifest, [changed_evidence])
        assert any(e["kind"] == "unregistered_runtime_job_evidence"
                   for e in changed_report["errors"])

        # CMake always passes the default capture path, including before the
        # first boot has created it.
        missing_capture = run_inventory(
            temp, elf_bytes, manifest, [temp / "not-created-yet.jsonl"])
        assert missing_capture["summary"]["errors"] == 0

        # This models the original omission.  Even though the descriptor may
        # be runtime-built, deleting Job E exposes a large section-head hole.
        missing_job = copy.deepcopy(manifest)
        del missing_job["families"]["job_e"]
        incomplete = run_inventory(temp, elf_bytes, missing_job)
        gap_errors = [e for e in incomplete["errors"]
                      if e["kind"] == "uncovered_dense_spu_payload_range"]
        assert len(gap_errors) == 1, incomplete["errors"]
        assert gap_errors[0]["start"] == f"0x{base:08X}"
        assert int(gap_errors[0]["size"], 0) >= 0x600

        missing_elf = copy.deepcopy(manifest)
        missing_elf["families"]["task_images"]["images"].pop()
        elf_report = run_inventory(temp, elf_bytes, missing_elf)
        assert any(e["kind"] == "unregistered_embedded_spu_elf"
                   for e in elf_report["errors"])

        evidence = temp / "native.log"
        evidence.write_text(
            "[native-spurs] unregistered job image: descriptor=0x401B2900 "
            "bin=0x00102000 size=0x900 fingerprint=0x1234567890ABCDEF "
            "next-slot=0x0E400; extract/lift this exact binary\n",
            encoding="utf-8",
        )
        runtime_report = run_inventory(temp, elf_bytes, manifest, [evidence])
        assert any(e["kind"] == "unregistered_runtime_job_evidence"
                   for e in runtime_report["errors"])

    print("SPU inventory: PASS (dense gap, ELF match, descriptor dedup, runtime evidence)")


if __name__ == "__main__":
    main()
