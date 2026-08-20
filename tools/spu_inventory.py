#!/usr/bin/env python3
"""Inventory SPU payloads and reject incomplete lift manifests.

The title's EBOOT keeps its raw SPURS jobs and embedded SPU ELFs in a dense,
file-backed section.  A finished CellSpursJob64/128/256 descriptor is useful
evidence when it exists, but descriptors may be constructed dynamically (the
frontier Job E is one such case).  This tool therefore combines four evidence
sources instead of relying on descriptor scanning alone:

* raw job identities declared in tools/spu_region_manifest.json;
* embedded EM_SPU ELF images found directly in every input ELF;
* plausible static legacy job descriptors;
* optional native-SPURS unknown-job log/capture records.

For dense SPU payload sections, every declared/discovered image contributes a
covered address interval.  A large unexplained gap is a high-confidence
missing payload and fails --strict.  This caught Job E at 0x01241400 without
needing its runtime-built descriptor or guessing SPU instruction boundaries.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
from typing import Iterable

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))

from elf_parser import ELFFile, PT_LOAD, SHT_PROGBITS, vaddr_to_offset  # noqa: E402
from extract_spu_images import find_spu_images, parse_spu_elf_size  # noqa: E402


def parse_int(value: int | str) -> int:
    return value if isinstance(value, int) else int(value, 0)


def hex32(value: int) -> str:
    return f"0x{value:08X}"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fnv1a64(data: bytes) -> int:
    value = 1469598103934665603
    for byte in data:
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


class InputImage:
    def __init__(self, path: Path):
        self.path = path.resolve()
        self.elf = ELFFile(str(self.path))
        self.elf.load()
        self.data = self.elf.raw_data

    def offset_to_vaddr(self, offset: int) -> int | None:
        for ph in self.elf.program_headers:
            start = int(ph.p_offset)
            end = start + int(ph.p_filesz)
            if ph.p_type == PT_LOAD and start <= offset < end:
                return int(ph.p_vaddr + offset - start)
        return None

    def bytes_at(self, vaddr: int, size: int) -> bytes | None:
        off = vaddr_to_offset(self.elf.program_headers, vaddr)
        if off is None or off + size > len(self.data):
            return None
        return self.data[off : off + size]

    def section_at(self, vaddr: int):
        for index, sh in enumerate(self.elf.section_headers):
            if (sh.sh_type == SHT_PROGBITS and sh.sh_size and
                    sh.sh_addr <= vaddr < sh.sh_addr + sh.sh_size):
                return index, sh
        return None


def resolve_artifact(relative: str, roots: Iterable[Path]) -> Path | None:
    path = Path(relative)
    if path.is_absolute() and path.is_file():
        return path
    for root in roots:
        candidate = root / path
        if candidate.is_file():
            return candidate
    return None


def spu_load_segment(path: Path) -> tuple[bytes, int, int]:
    """Return the first PT_LOAD's bytes, base and entry from a BE32 SPU ELF."""
    data = path.read_bytes()
    if len(data) < 0x34 or data[:4] != b"\x7fELF" or data[4:6] != b"\x01\x02":
        raise ValueError(f"{path}: not a 32-bit big-endian ELF")
    if struct.unpack_from(">H", data, 0x12)[0] != 23:
        raise ValueError(f"{path}: ELF is not EM_SPU")
    entry = struct.unpack_from(">I", data, 0x18)[0]
    phoff = struct.unpack_from(">I", data, 0x1C)[0]
    phentsize = struct.unpack_from(">H", data, 0x2A)[0]
    phnum = struct.unpack_from(">H", data, 0x2C)[0]
    for index in range(phnum):
        off = phoff + index * phentsize
        if off + 32 > len(data):
            break
        p_type, p_offset, p_vaddr, _paddr, p_filesz = struct.unpack_from(
            ">IIIII", data, off
        )
        if p_type == PT_LOAD and p_offset + p_filesz <= len(data):
            return data[p_offset : p_offset + p_filesz], p_vaddr, entry
    raise ValueError(f"{path}: no valid PT_LOAD")


def find_input_for_range(images: list[InputImage], ea: int, size: int):
    for image in images:
        data = image.bytes_at(ea, size)
        if data is not None:
            return image, data
    return None, None


def manifest_jobs(manifest: dict, images: list[InputImage], roots: list[Path],
                  warnings: list[dict], errors: list[dict]) -> list[dict]:
    records: list[dict] = []
    for family, spec in manifest.get("families", {}).items():
        if spec.get("kind") != "job":
            continue
        try:
            ea = parse_int(spec["eboot_ea"])
            logical_size = parse_int(spec["logical_size"])
        except (KeyError, TypeError, ValueError) as exc:
            errors.append({
                "kind": "invalid_manifest_job",
                "family": family,
                "detail": str(exc),
            })
            continue

        runtime_sizes = {logical_size}
        runtime_sizes.update(parse_int(v) for v in spec.get("runtime_sizes", []))
        source_code = None
        source_path = None
        for key in ("raw", "ref_elf"):
            if not spec.get(key):
                continue
            source_path = resolve_artifact(spec[key], roots)
            if source_path is None:
                continue
            try:
                source_code = (source_path.read_bytes() if key == "raw" else
                               spu_load_segment(source_path)[0])
            except (OSError, ValueError) as exc:
                errors.append({
                    "kind": "invalid_job_artifact",
                    "family": family,
                    "path": str(source_path),
                    "detail": str(exc),
                })
            break

        declared_code_size = parse_int(spec.get(
            "code_size", spec.get("eboot_size", logical_size)
        ))
        code_size = len(source_code) if source_code is not None else declared_code_size
        coverage_size = parse_int(spec.get("eboot_size", code_size))
        input_image, eboot_code = find_input_for_range(images, ea, code_size)
        if input_image is None:
            errors.append({
                "kind": "job_outside_inputs",
                "family": family,
                "binary_ea": hex32(ea),
                "code_size": f"0x{code_size:X}",
            })
            continue
        runtime_fingerprints = {}
        for runtime_size in sorted(runtime_sizes):
            runtime_code = input_image.bytes_at(ea, runtime_size)
            if runtime_code is None:
                errors.append({
                    "kind": "job_runtime_identity_outside_input",
                    "family": family,
                    "binary_ea": hex32(ea),
                    "binary_size": f"0x{runtime_size:X}",
                })
                continue
            runtime_fingerprints[runtime_size] = fnv1a64(runtime_code)
        if source_code is not None and eboot_code != source_code:
            errors.append({
                "kind": "job_bytes_mismatch",
                "family": family,
                "binary_ea": hex32(ea),
                "code_size": f"0x{code_size:X}",
                "artifact": str(source_path),
                "expected_sha256": sha256(source_code),
                "actual_sha256": sha256(eboot_code),
            })
        elif source_code is None and not spec.get("eboot"):
            warnings.append({
                "kind": "job_artifact_unavailable",
                "family": family,
                "detail": "verified range in input, but no raw/ref_elf artifact was found",
            })

        placements = []
        for placement in spec.get("placements", []):
            try:
                placements.append({
                    "stem": placement["stem"],
                    "base": hex32(parse_int(placement["base"])),
                    "image_id": int(placement["image_id"]),
                })
            except (KeyError, TypeError, ValueError) as exc:
                errors.append({
                    "kind": "invalid_manifest_placement",
                    "family": family,
                    "detail": str(exc),
                })

        records.append({
            "family": family,
            "binary_ea": hex32(ea),
            "logical_size": f"0x{logical_size:X}",
            "runtime_sizes": [f"0x{v:X}" for v in sorted(runtime_sizes)],
            "runtime_fingerprints": {
                f"0x{size:X}": f"0x{fingerprint:016X}"
                for size, fingerprint in runtime_fingerprints.items()
            },
            "code_size": f"0x{code_size:X}",
            "coverage_size": f"0x{coverage_size:X}",
            "sha256": sha256(eboot_code),
            "input": str(input_image.path),
            "placements": placements,
            "_ea": ea,
            "_runtime_sizes": runtime_sizes,
            "_runtime_fingerprints": runtime_fingerprints,
            "_coverage_size": coverage_size,
        })
    return records


def manifest_task_declarations(manifest: dict) -> list[dict]:
    declarations = []
    for family, spec in manifest.get("families", {}).items():
        if spec.get("kind") != "elf" or spec.get("experimental"):
            continue
        for image in spec.get("images", []):
            if "eboot_ea" not in image:
                continue
            declarations.append({
                "family": family,
                "stem": image["stem"],
                "image_id": image.get("image_id"),
                "ea": parse_int(image["eboot_ea"]),
                "elf": image.get("elf"),
            })
    return declarations


def embedded_spu_images(inputs: list[InputImage], declarations: list[dict],
                        roots: list[Path], warnings: list[dict],
                        errors: list[dict]) -> list[dict]:
    by_location = {(d["ea"]): d for d in declarations}
    seen: set[int] = set()
    records = []
    for image in inputs:
        for offset, size in find_spu_images(image.data):
            ea = image.offset_to_vaddr(offset)
            if ea is None:
                continue
            data = image.data[offset : offset + size]
            declared = by_location.get(ea)
            record = {
                "input": str(image.path),
                "file_offset": f"0x{offset:X}",
                "elf_ea": hex32(ea),
                "elf_size": f"0x{size:X}",
                "sha256": sha256(data),
                "matched": declared is not None,
                "stem": declared["stem"] if declared else None,
                "image_id": declared.get("image_id") if declared else None,
                "_ea": ea,
                "_size": size,
            }
            records.append(record)
            if declared is None:
                errors.append({
                    "kind": "unregistered_embedded_spu_elf",
                    "input": str(image.path),
                    "elf_ea": hex32(ea),
                    "elf_size": f"0x{size:X}",
                    "sha256": record["sha256"],
                })
                continue
            seen.add(ea)
            artifact_name = declared.get("elf")
            artifact = resolve_artifact(artifact_name, roots) if artifact_name else None
            if artifact is not None:
                artifact_data = artifact.read_bytes()
                artifact_size = parse_spu_elf_size(artifact_data, 0)
                if not artifact_size or artifact_data[:artifact_size] != data:
                    errors.append({
                        "kind": "embedded_spu_artifact_mismatch",
                        "stem": declared["stem"],
                        "elf_ea": hex32(ea),
                        "artifact": str(artifact),
                    })
            elif artifact_name:
                warnings.append({
                    "kind": "embedded_spu_artifact_unavailable",
                    "stem": declared["stem"],
                    "artifact": artifact_name,
                })

    for declared in declarations:
        if declared["ea"] not in seen:
            errors.append({
                "kind": "declared_spu_elf_not_found",
                "stem": declared["stem"],
                "elf_ea": hex32(declared["ea"]),
            })
    return records


def merge_intervals(intervals: list[tuple[int, int, str]]):
    merged: list[list] = []
    for start, end, label in sorted(intervals):
        if not merged or start > merged[-1][1]:
            merged.append([start, end, [label]])
        else:
            merged[-1][1] = max(merged[-1][1], end)
            merged[-1][2].append(label)
    return merged


def dense_payload_sections(inputs: list[InputImage], jobs: list[dict],
                           embedded: list[dict], max_padding: int,
                           errors: list[dict]) -> list[dict]:
    all_records = []
    for job in jobs:
        all_records.append((Path(job["input"]), job["_ea"],
                            job["_coverage_size"], job["family"]))
    for item in embedded:
        all_records.append((Path(item["input"]), item["_ea"], item["_size"],
                            item["stem"] or "unregistered-elf"))

    sections = []
    for image in inputs:
        grouped: dict[int, list[tuple[int, int, str]]] = {}
        headers = {}
        for input_path, ea, size, label in all_records:
            if input_path.resolve() != image.path:
                continue
            found = image.section_at(ea)
            if found is None:
                continue
            index, sh = found
            if ea + size > sh.sh_addr + sh.sh_size:
                errors.append({
                    "kind": "spu_payload_crosses_section",
                    "label": label,
                    "start": hex32(ea),
                    "end": hex32(ea + size),
                    "section_end": hex32(sh.sh_addr + sh.sh_size),
                })
                continue
            grouped.setdefault(index, []).append((ea, ea + size, label))
            headers[index] = sh

        for index, intervals in grouped.items():
            sh = headers[index]
            merged = merge_intervals(intervals)
            covered = sum(end - start for start, end, _labels in merged)
            density = covered / sh.sh_size
            # Two independent payloads and >=50% coverage is deliberately
            # conservative: ordinary mixed data sections are never gap-gated.
            if len(intervals) < 2 or density < 0.50:
                continue
            cursor = int(sh.sh_addr)
            gaps = []
            for start, end, _labels in merged:
                if start > cursor:
                    gaps.append((cursor, start))
                cursor = max(cursor, end)
            section_end = int(sh.sh_addr + sh.sh_size)
            if cursor < section_end:
                gaps.append((cursor, section_end))
            large_gaps = []
            for start, end in gaps:
                if end - start > max_padding:
                    finding = {
                        "kind": "uncovered_dense_spu_payload_range",
                        "input": str(image.path),
                        "section_index": index,
                        "start": hex32(start),
                        "end": hex32(end),
                        "size": f"0x{end - start:X}",
                        "confidence": "high",
                    }
                    errors.append(finding)
                    large_gaps.append(finding)
            sections.append({
                "input": str(image.path),
                "section_index": index,
                "start": hex32(int(sh.sh_addr)),
                "end": hex32(section_end),
                "size": f"0x{int(sh.sh_size):X}",
                "declared_or_discovered_payloads": len(intervals),
                "coverage_percent": round(density * 100, 3),
                "allowed_padding": f"0x{max_padding:X}",
                "large_gaps": large_gaps,
            })
    return sections


def scan_static_descriptors(inputs: list[InputImage], jobs: list[dict],
                            payload_sections: list[dict]) -> list[dict]:
    known = {}
    for job in jobs:
        for size in job["_runtime_sizes"]:
            known[(job["_ea"], size)] = job["family"]
    payload_ranges = [
        (parse_int(section["start"]), parse_int(section["end"]))
        for section in payload_sections
    ]
    dedup: dict[tuple[int, int], dict] = {}
    for image in inputs:
        data = image.data
        for ph in image.elf.program_headers:
            if ph.p_type != PT_LOAD or ph.p_filesz < 0x30:
                continue
            first = (int(ph.p_offset) + 15) & ~15
            last = min(len(data) - 0x30, int(ph.p_offset + ph.p_filesz - 0x30))
            for off in range(first, last + 1, 16):
                raw_ea = struct.unpack_from(">Q", data, off)[0]
                if raw_ea >> 32:
                    continue
                binary_ea = int(raw_ea & ~1)
                units, dma_size = struct.unpack_from(">HH", data, off + 8)
                binary_size = units * 16
                if (binary_ea < 0x10000 or binary_ea & 0xF or
                        binary_size < 0x100 or binary_size > 0x3B400):
                    continue
                if image.bytes_at(binary_ea, binary_size) is None:
                    continue
                if data[off + 0x2C] != 0:
                    continue
                cache_size = struct.unpack_from(">I", data, off + 0x24)[0]
                if (dma_size & 7 or cache_size & 7 or cache_size > 32 or
                        dma_size + cache_size > 0x3D0):
                    continue
                stack_units, scratch_units = struct.unpack_from(">HH", data, off + 0x1C)
                if stack_units * 16 > 0x40000 or scratch_units * 16 > 0x40000:
                    continue
                descriptor_ea = image.offset_to_vaddr(off)
                if descriptor_ea is None:
                    continue
                key = (binary_ea, binary_size)
                record = dedup.setdefault(key, {
                    "binary_ea": hex32(binary_ea),
                    "binary_size": f"0x{binary_size:X}",
                    "matched_family": known.get(key),
                    "confidence": "high" if known.get(key) else "medium",
                    "descriptor_locations": [],
                })
                record["descriptor_locations"].append({
                    "input": str(image.path),
                    "descriptor_ea": hex32(descriptor_ea),
                })
                if (record["matched_family"] is None and
                        any(start <= binary_ea < end for start, end in payload_ranges)):
                    record["confidence"] = "medium"
    return sorted(dedup.values(), key=lambda x: parse_int(x["binary_ea"]))


UNKNOWN_JOB_RE = re.compile(
    r"unregistered job image:\s*descriptor=(0x[0-9A-Fa-f]+)\s+"
    r"bin=(0x[0-9A-Fa-f]+)\s+size=(0x[0-9A-Fa-f]+)\s+"
    r"fingerprint=(0x[0-9A-Fa-f]+)(?:\s+next-slot=(0x[0-9A-Fa-f]+))?"
)


def runtime_evidence(paths: list[Path], jobs: list[dict], errors: list[dict]):
    known = {
        (job["_ea"], size): (job["family"], fingerprint)
        for job in jobs
        for size, fingerprint in job["_runtime_fingerprints"].items()
    }
    records = []
    for path in paths:
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        parsed = []
        for match in UNKNOWN_JOB_RE.finditer(text):
            parsed.append({
                "descriptor_ea": match.group(1),
                "binary_ea": match.group(2),
                "binary_size": match.group(3),
                "fingerprint": match.group(4),
                "placement": match.group(5),
            })
        if not parsed:
            try:
                doc = json.loads(text)
                parsed = doc if isinstance(doc, list) else [doc]
            except json.JSONDecodeError:
                for line in text.splitlines():
                    try:
                        parsed.append(json.loads(line))
                    except json.JSONDecodeError:
                        continue
        for item in parsed:
            try:
                ea = parse_int(item.get("binary_ea", item.get("bin")))
                size = parse_int(item.get("binary_size", item.get("size")))
            except (TypeError, ValueError):
                continue
            captured_fingerprint = item.get("fingerprint")
            try:
                captured_fingerprint_value = (
                    parse_int(captured_fingerprint)
                    if captured_fingerprint is not None else None
                )
            except (TypeError, ValueError):
                captured_fingerprint_value = None
            expected = known.get((ea, size))
            family = None
            if expected and (captured_fingerprint_value is None or
                             captured_fingerprint_value == expected[1]):
                family = expected[0]
            record = {
                "source": str(path.resolve()),
                "binary_ea": hex32(ea),
                "binary_size": f"0x{size:X}",
                "fingerprint": item.get("fingerprint"),
                "expected_fingerprint": (
                    f"0x{expected[1]:016X}" if expected else None
                ),
                "matched_family": family,
            }
            records.append(record)
            if family is None:
                errors.append({
                    "kind": "unregistered_runtime_job_evidence",
                    **record,
                    "confidence": "certain",
                })
    unique = {(r["source"], r["binary_ea"], r["binary_size"]): r for r in records}
    return list(unique.values())


def public_record(record: dict) -> dict:
    return {key: value for key, value in record.items() if not key.startswith("_")}


def build_inventory(input_paths: list[Path], manifest_path: Path,
                    artifact_roots: list[Path], max_padding: int,
                    evidence_paths: list[Path] | None = None) -> dict:
    inputs = [InputImage(path) for path in input_paths]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    roots = []
    for root in [manifest_path.resolve().parent.parent, *artifact_roots]:
        resolved = root.resolve()
        if resolved not in roots:
            roots.append(resolved)
    warnings: list[dict] = []
    errors: list[dict] = []
    jobs = manifest_jobs(manifest, inputs, roots, warnings, errors)
    declarations = manifest_task_declarations(manifest)
    embedded = embedded_spu_images(inputs, declarations, roots, warnings, errors)
    sections = dense_payload_sections(inputs, jobs, embedded, max_padding, errors)
    descriptors = scan_static_descriptors(inputs, jobs, sections)
    runtime = runtime_evidence(evidence_paths or [], jobs, errors)
    report = {
        "schema": 1,
        "manifest": str(manifest_path.resolve()),
        "inputs": [str(path.resolve()) for path in input_paths],
        "artifact_roots": [str(path) for path in roots],
        "summary": {
            "job_families": len(jobs),
            "job_placements": sum(len(j["placements"]) for j in jobs),
            "embedded_spu_elfs": len(embedded),
            "matched_embedded_spu_elfs": sum(1 for e in embedded if e["matched"]),
            "dense_payload_sections": len(sections),
            "static_descriptor_identities": len(descriptors),
            "runtime_evidence_records": len(runtime),
            "warnings": len(warnings),
            "errors": len(errors),
        },
        "jobs": [public_record(job) for job in jobs],
        "embedded_spu_elfs": [public_record(item) for item in embedded],
        "dense_payload_sections": sections,
        "static_descriptors": descriptors,
        "runtime_evidence": runtime,
        "warnings": warnings,
        "errors": errors,
    }
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", action="append", required=True, type=Path,
                        help="PPU ELF/EBOOT/PRX to inventory (repeatable)")
    parser.add_argument("--manifest", type=Path,
                        default=TOOLS / "spu_region_manifest.json")
    parser.add_argument("--artifact-root", action="append", type=Path, default=[],
                        help="additional root for manifest raw/ref ELF artifacts")
    parser.add_argument("--runtime-evidence", action="append", type=Path, default=[],
                        help="native-SPURS log, JSON, or JSONL capture (repeatable)")
    parser.add_argument("--max-padding", type=lambda value: int(value, 0),
                        default=0x400,
                        help="largest allowed unexplained dense-section gap")
    parser.add_argument("--report", type=Path,
                        help="write the deterministic JSON inventory here")
    parser.add_argument("--strict", action="store_true",
                        help="return failure when high-confidence errors exist")
    args = parser.parse_args()

    try:
        report = build_inventory(args.input, args.manifest, args.artifact_root,
                                 args.max_padding, args.runtime_evidence)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"SPU inventory: configuration/input error: {exc}", file=sys.stderr)
        return 2
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(encoded, encoding="utf-8")

    summary = report["summary"]
    print(
        "SPU inventory: "
        f"{summary['job_families']} job families / "
        f"{summary['job_placements']} placements, "
        f"{summary['matched_embedded_spu_elfs']}/"
        f"{summary['embedded_spu_elfs']} embedded ELFs matched, "
        f"{summary['dense_payload_sections']} dense section(s), "
        f"{summary['errors']} error(s), {summary['warnings']} warning(s)"
    )
    for error in report["errors"]:
        where = error.get("start", error.get("elf_ea", error.get("binary_ea", "")))
        print(f"  ERROR {error['kind']} {where}".rstrip(), file=sys.stderr)
    if args.report:
        print(f"  report: {args.report.resolve()}")
    return 1 if args.strict and report["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
