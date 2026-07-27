#!/usr/bin/env python3
"""Build and diff the narrowed orphanage stage-render checkpoint.

The reference input is RPCS3's sparse ``guest_memory.bin`` plus a small JSON
sidecar containing the stopped PPU registers.  The recomp input may use the
same address-space image or a compact ``memory_regions`` list emitted by the
exact-PC hook.  In both cases this tool follows only the object-selection
chain and object graph rooted at r24; it never scans or diffs whole memory.

The final output is one ``STAGE_RENDER_CHECKPOINT`` JSON event.  Pointer
values are retained under ``raw`` for forensics but excluded from the
semantic comparison.  The comparison uses nullness, stable roles,
asset-relative offsets, and pointed-to content hashes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import struct
import sys
from collections import OrderedDict
from pathlib import Path
from typing import Any, Iterable


CHECKPOINT_PC = 0x00113584
CHECKPOINT_END_PC = 0x0011486C
HOUSE_GMD = 0x429D8A00
OBJECT_BYTES = 0x134
MAX_MESH_COUNT = 4096
RECORD_STRIDE = 12
GROUP_COUNT = 16
GROUP_DESCRIPTOR_SIZE = 16
PRIMARY_TRANSFORM_BANK = 0x015BCF40
SECONDARY_TRANSFORM_BANK = 0x015BD080
SHARED_STAGE_TRANSFORM = 0x01622590
TRANSFORM_SLOTS = 5
TRANSFORM_BYTES = 0x40

# Globals read or changed while establishing the renderer's early gates.
EARLY_GLOBALS = OrderedDict(
    (
        ("manager_slot", 0x01364960),
        ("render_state_slot", 0x01367100),
        ("render_mode_bits", 0x0163B0AC),
        ("global_14ee738", 0x014EE738),
        ("global_14ee734", 0x014EE734),
        ("global_14ee728", 0x014EE728),
        ("command_arena_slot", 0x0163B980),
    )
)


class MemoryReadError(RuntimeError):
    pass


def _u32(value: Any) -> int:
    if isinstance(value, int):
        return value & 0xFFFFFFFF
    if isinstance(value, str):
        return int(value, 0) & 0xFFFFFFFF
    raise TypeError(f"expected integer/hex string, got {type(value).__name__}")


def _u64(value: Any) -> int:
    if isinstance(value, int):
        return value & 0xFFFFFFFFFFFFFFFF
    if isinstance(value, str):
        return int(value, 0) & 0xFFFFFFFFFFFFFFFF
    raise TypeError(f"expected integer/hex string, got {type(value).__name__}")


def _hex32(value: int) -> str:
    return f"0x{value & 0xFFFFFFFF:08x}"


def _hex64(value: int) -> str:
    return f"0x{value & 0xFFFFFFFFFFFFFFFF:016x}"


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _normalized_hash(value: Any) -> str:
    def strip(item: Any) -> Any:
        if isinstance(item, dict):
            return {
                key: strip(child)
                for key, child in item.items()
                if key not in {"raw", "raw_hex", "raw_sha256"}
            }
        if isinstance(item, list):
            return [strip(child) for child in item]
        return item

    encoded = json.dumps(
        strip(value), sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return _sha(encoded)


def _be16(data: bytes, offset: int = 0) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def _be32(data: bytes, offset: int = 0) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def _be64(data: bytes, offset: int = 0) -> int:
    return struct.unpack_from(">Q", data, offset)[0]


class GuestMemory:
    """Read-only PS3 address-space view with allocation-range validation."""

    def __init__(self, regions: Iterable[tuple[int, int]]):
        self._regions = sorted((int(a), int(b)) for a, b in regions)

    def contains(self, address: int, size: int = 1) -> bool:
        if size < 0 or address < 0 or address + size > 0x1_0000_0000:
            return False
        end = address + size
        return any(address >= lo and end <= hi for lo, hi in self._regions)

    def read(self, address: int, size: int) -> bytes:
        raise NotImplementedError

    def read8(self, address: int) -> int:
        return self.read(address, 1)[0]

    def read16(self, address: int) -> int:
        return _be16(self.read(address, 2))

    def read32(self, address: int) -> int:
        return _be32(self.read(address, 4))

    def read64(self, address: int) -> int:
        return _be64(self.read(address, 8))


class SparseDumpMemory(GuestMemory):
    def __init__(self, dump_dir: Path):
        manifest_path = dump_dir / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest.get("format") != "rpcs3-guest-memory-dump":
            raise MemoryReadError(f"{manifest_path} is not an RPCS3 guest-memory dump")
        regions = [
            (_u32(item["start"]), int(_u64(item["end_exclusive"])))
            for item in manifest["regions"]
        ]
        super().__init__(regions)
        self.dump_dir = dump_dir
        self.manifest = manifest
        self.image_path = dump_dir / manifest["address_space_file"]
        self._fh = self.image_path.open("rb", buffering=0)

    def read(self, address: int, size: int) -> bytes:
        if not self.contains(address, size):
            raise MemoryReadError(
                f"unallocated RPCS3 guest range {_hex32(address)}+0x{size:x}"
            )
        self._fh.seek(address)
        data = self._fh.read(size)
        if len(data) != size:
            raise MemoryReadError(
                f"short RPCS3 dump read at {_hex32(address)}: {len(data)}/{size}"
            )
        return data


class RegionMemory(GuestMemory):
    """Compact exact-PC backing store used by the recomp hook."""

    def __init__(self, entries: Iterable[dict[str, Any]]):
        chunks: list[tuple[int, bytes]] = []
        for item in entries:
            address = _u32(item["address"])
            data = bytes.fromhex(item["bytes"])
            chunks.append((address, data))
        self._chunks = sorted(chunks)
        super().__init__((address, address + len(data)) for address, data in chunks)

    def contains(self, address: int, size: int = 1) -> bool:
        end = address + size
        return any(address >= base and end <= base + len(data)
                   for base, data in self._chunks)

    def read(self, address: int, size: int) -> bytes:
        end = address + size
        for base, data in self._chunks:
            if address >= base and end <= base + len(data):
                offset = address - base
                return data[offset:offset + size]
        raise MemoryReadError(
            f"range absent from compact checkpoint {_hex32(address)}+0x{size:x}"
        )


def load_metadata(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if _u32(data.get("pc", 0)) != CHECKPOINT_PC:
        raise ValueError(
            f"metadata PC is {_hex32(_u32(data.get('pc', 0)))}, "
            f"expected {_hex32(CHECKPOINT_PC)}"
        )
    regs = data.get("registers", {})
    missing = [f"r{i}" for i in range(32) if f"r{i}" not in regs]
    if missing:
        raise ValueError(f"metadata is missing registers: {', '.join(missing)}")
    return data


def load_backing(args: argparse.Namespace) -> tuple[GuestMemory, dict[str, Any]]:
    metadata = load_metadata(Path(args.metadata))
    if args.dump:
        return SparseDumpMemory(Path(args.dump)), metadata
    raw = json.loads(Path(args.regions).read_text(encoding="utf-8"))
    entries = raw.get("memory_regions", raw)
    if not isinstance(entries, list):
        raise ValueError("compact backing store needs a memory_regions array")
    return RegionMemory(entries), metadata


def _read_or_none(mem: GuestMemory, address: int, size: int) -> bytes | None:
    return mem.read(address, size) if address and mem.contains(address, size) else None


def _content_identity(mem: GuestMemory, address: int, size: int = 64) -> dict[str, Any]:
    if address == 0:
        return {"null": True}
    readable = mem.contains(address, 1)
    result: dict[str, Any] = {"null": False, "readable": readable}
    if readable:
        available = 0
        for candidate in (size, 32, 16, 8, 4, 1):
            if mem.contains(address, candidate):
                available = candidate
                break
        data = mem.read(address, available)
        result.update({"bytes": available, "sha256": _sha(data)})
    return result


class PointerNormalizer:
    def __init__(
        self,
        mem: GuestMemory,
        *,
        object_address: int,
        gmd_address: int,
        stack_address: int,
        roles: dict[int, str],
    ):
        self.mem = mem
        self.object_address = object_address
        self.gmd_address = gmd_address
        self.stack_address = stack_address
        self.roles = roles

    def normalize(
        self,
        value: int,
        *,
        pointed_size: int = 0,
        include_raw: bool = True,
    ) -> dict[str, Any]:
        value &= 0xFFFFFFFF
        out: dict[str, Any] = {}
        if include_raw:
            out["raw"] = _hex32(value)
        if value == 0:
            out.update({"kind": "null", "null": True})
            return out
        out["null"] = False
        if value in self.roles:
            out.update({"kind": "role", "identity": self.roles[value]})
            return out
        if value < 0x02000000:
            out.update({"kind": "static", "identity": _hex32(value)})
            return out
        if self.gmd_address and abs(value - self.gmd_address) <= 0x01000000:
            delta = value - self.gmd_address
            out.update({"kind": "asset_relative", "asset": "house_gmd", "offset": delta})
            return out
        if self.stack_address and abs(value - self.stack_address) <= 0x00020000:
            out.update(
                {
                    "kind": "stack_relative",
                    "offset": value - self.stack_address,
                }
            )
            return out
        out["kind"] = "content"
        if pointed_size:
            out["content"] = _content_identity(self.mem, value, pointed_size)
        return out


def _record_sample(
    data: bytes,
    index: int,
    normalizer: PointerNormalizer,
    stride: int = RECORD_STRIDE,
) -> dict[str, Any]:
    start = index * stride
    record = data[start:start + stride]
    return {
        "index": index,
        "raw_hex": record.hex(),
        "h0": f"0x{_be16(record, 0):04x}",
        "h2": f"0x{_be16(record, 2):04x}",
        "h4": f"0x{_be16(record, 4):04x}",
        "h6": f"0x{_be16(record, 6):04x}",
        "word8": normalizer.normalize(_be32(record, 8)),
    }


def _normalized_records(
    data: bytes,
    count: int,
    normalizer: PointerNormalizer,
) -> list[dict[str, Any]]:
    return [_record_sample(data, index, normalizer) for index in range(count)]


def describe_record_array(
    mem: GuestMemory,
    normalizer: PointerNormalizer,
    pointer: int,
    count: int,
    *,
    label: str,
) -> dict[str, Any]:
    out: dict[str, Any] = OrderedDict()
    size = count * RECORD_STRIDE
    out["label"] = label
    out["pointer"] = normalizer.normalize(pointer)
    out["count"] = count
    out["stride"] = RECORD_STRIDE
    data = _read_or_none(mem, pointer, size)
    if data is None:
        out["readable"] = False
        return out
    out["readable"] = True
    records = _normalized_records(data, count, normalizer)
    out["raw_sha256"] = _sha(data)
    out["sha256"] = _normalized_hash(records)
    sample_indices = sorted(set(i for i in (0, count // 2, count - 1) if 0 <= i < count))
    out["samples"] = [records[i] for i in sample_indices]
    return out


def describe_pointer_table(
    mem: GuestMemory,
    normalizer: PointerNormalizer,
    pointer: int,
    count: int,
    mesh_count: int,
    *,
    label: str,
) -> dict[str, Any]:
    out: dict[str, Any] = OrderedDict()
    out["label"] = label
    out["pointer"] = normalizer.normalize(pointer)
    out["count"] = count
    table = _read_or_none(mem, pointer, count * 4)
    if table is None:
        out["readable"] = False
        return out
    out["readable"] = True
    out["raw_sha256"] = _sha(table)
    entries = []
    for index in range(count):
        target = _be32(table, index * 4)
        entry = OrderedDict()
        entry["index"] = index
        entry["pointer"] = normalizer.normalize(target)
        region = _read_or_none(mem, target, mesh_count * RECORD_STRIDE)
        entry["readable"] = region is not None
        if region is not None:
            entry["count"] = mesh_count
            entry["stride"] = RECORD_STRIDE
            records = _normalized_records(region, mesh_count, normalizer)
            entry["raw_sha256"] = _sha(region)
            entry["sha256"] = _normalized_hash(records)
            sample_indices = sorted(
                set(i for i in (0, mesh_count // 2, mesh_count - 1)
                    if 0 <= i < mesh_count)
            )
            entry["samples"] = [records[i] for i in sample_indices]
        entries.append(entry)
    out["entries"] = entries
    out["sha256"] = _normalized_hash(entries)
    return out


def describe_render_groups(
    mem: GuestMemory,
    normalizer: PointerNormalizer,
    gmd: int,
) -> dict[str, Any]:
    out: dict[str, Any] = OrderedDict()
    table = mem.read32(gmd + 0xF4) if mem.contains(gmd + 0xF4, 4) else 0
    out["pointer"] = normalizer.normalize(table)
    raw = _read_or_none(mem, table, GROUP_COUNT * GROUP_DESCRIPTOR_SIZE)
    if raw is None:
        out["readable"] = False
        return out
    out["readable"] = True
    out["raw_sha256"] = _sha(raw)
    descriptors = []
    for index in range(GROUP_COUNT):
        off = index * GROUP_DESCRIPTOR_SIZE
        desc = raw[off:off + GROUP_DESCRIPTOR_SIZE]
        count, indices, word8, wordc = struct.unpack(">IIII", desc)
        item: dict[str, Any] = OrderedDict()
        item["index"] = index
        item["count"] = count
        item["indices"] = normalizer.normalize(indices)
        item["word8"] = _hex32(word8)
        item["wordc"] = _hex32(wordc)
        if count <= MAX_MESH_COUNT:
            index_data = _read_or_none(mem, indices, count * 2)
        else:
            index_data = None
        item["indices_readable"] = index_data is not None
        if index_data is not None:
            item["indices_sha256"] = _sha(index_data)
            values = [
                _be16(index_data, i * 2)
                for i in sorted(set(
                    j for j in (0, count // 2, count - 1) if 0 <= j < count
                ))
            ]
            item["representative_indices"] = values
        descriptors.append(item)
    out["descriptors"] = descriptors
    out["sha256"] = _normalized_hash(descriptors)
    return out


def describe_transform(
    mem: GuestMemory,
    address: int,
    *,
    label: str,
) -> dict[str, Any]:
    out: dict[str, Any] = OrderedDict()
    out["label"] = label
    out["address"] = _hex32(address)
    raw = _read_or_none(mem, address, TRANSFORM_BYTES)
    out["readable"] = raw is not None
    if raw is None:
        return out
    out["sha256"] = _sha(raw)
    out["words"] = [
        _hex32(_be32(raw, offset))
        for offset in range(0, TRANSFORM_BYTES, 4)
    ]
    out["finite"] = all(
        math.isfinite(value)
        for value in struct.unpack(">16f", raw)
    )
    return out


def describe_transform_bank(
    mem: GuestMemory,
    address: int,
    *,
    label: str,
) -> dict[str, Any]:
    out: dict[str, Any] = OrderedDict()
    out["label"] = label
    out["address"] = _hex32(address)
    out["slots"] = [
        describe_transform(
            mem,
            address + index * TRANSFORM_BYTES,
            label=f"{label}[{index}]",
        )
        for index in range(TRANSFORM_SLOTS)
    ]
    return out


def _normalize_object_words(
    raw: bytes,
    normalizer: PointerNormalizer,
) -> list[dict[str, Any]]:
    pointer_offsets = {
        0x00, 0x04, 0x0C, 0x10, 0x14, 0x24,
        0xCC, 0xE4, 0xE8, 0xF4, 0xF8, 0xFC, 0x100,
        0x10C, 0x118, 0x128,
    }
    words = []
    for offset in range(0, 0x134, 4):
        value = _be32(raw, offset)
        item: dict[str, Any] = OrderedDict()
        item["offset"] = f"0x{offset:03x}"
        # Interpret only structurally known pointer fields as pointers.  Float
        # bit patterns in the transform (notably +0x30..+0x68) routinely fall
        # inside the guest heap range and must remain scalar values.
        if offset in pointer_offsets:
            item["value"] = normalizer.normalize(value)
        else:
            item["value"] = {"kind": "scalar", "value": _hex32(value)}
        words.append(item)
    return words


def _command_state(
    mem: GuestMemory,
    arena: int,
    normalizer: PointerNormalizer,
) -> dict[str, Any]:
    result: dict[str, Any] = OrderedDict()
    result["readable"] = bool(arena and mem.contains(arena, 0x20))
    if result["readable"]:
        head = mem.read32(arena + 0x00)
        end = mem.read32(arena + 0x04)
        result["head"] = {
            "raw": _hex32(head),
            "arena_offset": head - arena,
        }
        result["end"] = {
            "raw": _hex32(end),
            "arena_offset": end - arena,
        }
        result["count"] = mem.read32(arena + 0x1C)
    return result


def _command_state_from_metadata(
    value: Any,
    normalizer: PointerNormalizer,
) -> dict[str, Any] | None:
    if not isinstance(value, dict):
        return None
    arena = _u32(value.get("pointer", 0))
    out: dict[str, Any] = OrderedDict()
    out["pointer"] = normalizer.normalize(arena)
    out["readable"] = bool(value.get("readable", arena != 0))
    if "head" in value:
        head = _u32(value["head"])
        out["head"] = {"raw": _hex32(head), "arena_offset": head - arena}
    if "end" in value:
        end = _u32(value["end"])
        out["end"] = {"raw": _hex32(end), "arena_offset": end - arena}
    if "count" in value:
        out["count"] = int(value["count"])
    return out


def extract_checkpoint(
    mem: GuestMemory,
    metadata: dict[str, Any],
    *,
    engine: str,
) -> dict[str, Any]:
    regs_raw = metadata["registers"]
    regs = {name: _u64(value) for name, value in regs_raw.items() if name.startswith("r")}
    r1 = regs["r1"] & 0xFFFFFFFF
    r24 = regs["r24"] & 0xFFFFFFFF

    # Reconstruct the exact selection chain that produced r24 at 0x113584.
    root = mem.read64(r1 + 0xE0) & 0xFFFFFFFF
    object_offset = mem.read64(r1 + 0xB8) & 0xFFFFFFFF
    list_base = mem.read32(root + 0x108)
    wrapper_slot = (list_base + object_offset) & 0xFFFFFFFF
    wrapper = mem.read32(wrapper_slot)
    selected = mem.read32(wrapper + 0x12C) if wrapper else 0
    gmd = mem.read32(r24 + 0x04)

    roles = {
        r24: "house_object",
        root: "scene_root",
        list_base: "scene_object_list",
        wrapper: "scene_wrapper",
        gmd: "house_gmd",
    }
    normalizer = PointerNormalizer(
        mem,
        object_address=r24,
        gmd_address=gmd,
        stack_address=r1,
        roles=roles,
    )

    object_raw = mem.read(r24, OBJECT_BYTES)
    flags_begin = mem.read32(r24 + 0x10)
    flags_end = mem.read32(r24 + 0x0C)
    mesh_count = (flags_end - flags_begin) // 2 if flags_end >= flags_begin else 0
    if mesh_count > MAX_MESH_COUNT:
        mesh_count = 0
    pass_count = mem.read8(r24 + 0xB2)
    pass_index = mem.read64(r1 + 0xC0) & 0xFFFFFFFF

    event: dict[str, Any] = OrderedDict()
    event["schema"] = 1
    event["engine"] = engine
    event["checkpoint"] = {
        "pc": _hex32(CHECKPOINT_PC),
        "end_pc": _hex32(CHECKPOINT_END_PC),
        "sequence": int(metadata.get("sequence", 0)),
        "thread_id": str(metadata.get("thread_id", "")),
        "frame": metadata.get("frame", {}),
        "pass_index": pass_index,
        "object_index": mem.read32(r1 + 0xA4),
    }

    chain: dict[str, Any] = OrderedDict()
    chain["stack"] = normalizer.normalize(r1)
    chain["root"] = normalizer.normalize(root)
    chain["object_offset"] = object_offset
    chain["list_base"] = normalizer.normalize(list_base)
    chain["wrapper_slot"] = normalizer.normalize(wrapper_slot)
    chain["wrapper"] = normalizer.normalize(wrapper)
    chain["wrapper_plus_12c"] = normalizer.normalize(selected)
    chain["r24"] = normalizer.normalize(r24)
    chain["selected_equals_r24"] = selected == r24
    event["selection_chain"] = chain

    reg_event: dict[str, Any] = OrderedDict()
    for index in range(32):
        name = f"r{index}"
        value64 = regs[name]
        low = value64 & 0xFFFFFFFF
        if name == "r1":
            reg_event[name] = {"kind": "role", "identity": "stack"}
        elif name == "r24":
            reg_event[name] = {"kind": "role", "identity": "house_object"}
        elif value64 <= 0xFFFFFFFF and low in normalizer.roles:
            # Heap bases differ between RPCS3 and recomp.  Recognize pointers
            # to nodes in the reconstructed selection chain by role before
            # applying the generic guest-address heuristic below.
            reg_event[name] = normalizer.normalize(low)
        elif value64 <= 0xFFFFFFFF and 0x40000000 <= low < 0xE0000000:
            reg_event[name] = normalizer.normalize(low)
        else:
            reg_event[name] = {"kind": "scalar", "value": _hex64(value64)}
    for name in ("cr", "lr", "ctr"):
        if name in metadata and metadata[name] is not None:
            reg_event[name] = _hex64(_u64(metadata[name]))
        elif name in regs_raw:
            reg_event[name] = _hex64(_u64(regs_raw[name]))
    event["registers"] = reg_event

    transform_pipeline: dict[str, Any] = OrderedDict()
    transform_pipeline["primary_bank"] = describe_transform_bank(
        mem, PRIMARY_TRANSFORM_BANK, label="primary_transform_bank"
    )
    transform_pipeline["secondary_bank"] = describe_transform_bank(
        mem, SECONDARY_TRANSFORM_BANK, label="secondary_transform_bank"
    )
    transform_pipeline["shared_source"] = describe_transform(
        mem, SHARED_STAGE_TRANSFORM, label="shared_stage_transform"
    )
    transform_pipeline["object_transform"] = describe_transform(
        mem, r24 + 0x30, label="object+0x30"
    )
    source_words = transform_pipeline["shared_source"].get("words")
    object_words = transform_pipeline["object_transform"].get("words")
    transform_pipeline["source_equals_object"] = (
        source_words is not None and source_words == object_words
    )
    event["transform_pipeline"] = transform_pipeline

    house: dict[str, Any] = OrderedDict()
    house["pointer"] = normalizer.normalize(r24)
    house["gmd"] = normalizer.normalize(gmd)
    house["gmd_matches_house_asset"] = gmd == HOUSE_GMD
    house["raw_hex"] = object_raw.hex()
    house["raw_sha256"] = _sha(object_raw)
    house["words_000_130"] = _normalize_object_words(object_raw, normalizer)
    house["b0"] = f"0x{mem.read16(r24 + 0xB0):04x}"
    house["b2"] = pass_count
    house["mesh_count"] = mesh_count
    house["mesh_flags_begin"] = normalizer.normalize(flags_begin, pointed_size=mesh_count * 2)
    house["mesh_flags_end"] = normalizer.normalize(flags_end)
    flags = _read_or_none(mem, flags_begin, mesh_count * 2)
    if flags is not None:
        house["mesh_flags_sha256"] = _sha(flags)
        house["mesh_flags_samples"] = [
            {"index": i, "value": f"0x{_be16(flags, i * 2):04x}"}
            for i in sorted(set(
                j for j in (0, mesh_count // 2, mesh_count - 1)
                if 0 <= j < mesh_count
            ))
        ]
    event["house"] = house

    event["mesh_arrays"] = OrderedDict(
        (
            (
                "object_f4",
                describe_record_array(
                    mem, normalizer, mem.read32(r24 + 0xF4), mesh_count,
                    label="object+0xF4",
                ),
            ),
            (
                "object_f8",
                describe_record_array(
                    mem, normalizer, mem.read32(r24 + 0xF8), mesh_count,
                    label="object+0xF8",
                ),
            ),
        )
    )
    event["pass_tables"] = OrderedDict(
        (
            (
                "object_fc",
                describe_pointer_table(
                    mem, normalizer, mem.read32(r24 + 0xFC), pass_count,
                    mesh_count, label="object+0xFC",
                ),
            ),
            (
                "object_100",
                describe_pointer_table(
                    mem, normalizer, mem.read32(r24 + 0x100), pass_count,
                    mesh_count, label="object+0x100",
                ),
            ),
        )
    )
    event["gmd_render_groups"] = describe_render_groups(mem, normalizer, gmd)

    global_event: dict[str, Any] = OrderedDict()
    for name, address in EARLY_GLOBALS.items():
        value = mem.read32(address)
        item: dict[str, Any] = OrderedDict()
        item["slot"] = _hex32(address)
        if name.endswith("_slot"):
            item["value"] = normalizer.normalize(value)
        else:
            item["value"] = {"kind": "scalar", "value": _hex32(value)}
        global_event[name] = item

    manager = mem.read32(EARLY_GLOBALS["manager_slot"])
    global_event["manager_plus_400"] = (
        mem.read32(manager + 0x400) if manager and mem.contains(manager + 0x400, 4) else None
    )
    global_event["manager_plus_498"] = (
        mem.read32(manager + 0x498) if manager and mem.contains(manager + 0x498, 4) else None
    )
    render_state = mem.read32(EARLY_GLOBALS["render_state_slot"])
    global_event["render_state_plus_514"] = (
        mem.read32(render_state + 0x514)
        if render_state and mem.contains(render_state + 0x514, 4) else None
    )
    event["early_globals"] = global_event

    arena = mem.read32(EARLY_GLOBALS["command_arena_slot"])
    command: dict[str, Any] = OrderedDict()
    command["pointer"] = normalizer.normalize(arena)
    command["before"] = _command_state(mem, arena, normalizer)
    command["after"] = _command_state_from_metadata(
        metadata.get("command_arena_after"), normalizer
    )
    event["command_arena"] = command
    return event


_DROP_KEYS = {
    "engine",
    "thread_id",
    "raw",
    "raw_hex",
    "raw_sha256",
    "slot",
    "sequence",
}


def _semantic_copy(value: Any) -> Any:
    if isinstance(value, dict):
        out = OrderedDict()
        for key, item in value.items():
            if key in _DROP_KEYS:
                continue
            if key == "frame":
                # Host timing/counter metadata documents the sample but does
                # not define guest state equivalence.
                continue
            out[key] = _semantic_copy(item)
        return out
    if isinstance(value, list):
        return [_semantic_copy(item) for item in value]
    return value


def _flatten(value: Any, prefix: str = "") -> list[tuple[str, Any]]:
    out: list[tuple[str, Any]] = []
    if isinstance(value, dict):
        for key, item in value.items():
            path = f"{prefix}.{key}" if prefix else key
            out.extend(_flatten(item, path))
    elif isinstance(value, list):
        for index, item in enumerate(value):
            out.extend(_flatten(item, f"{prefix}[{index}]"))
    else:
        out.append((prefix, value))
    return out


def _stable_word(value: Any) -> int:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return int.from_bytes(hashlib.sha256(encoded).digest()[:8], "big")


def first_difference(
    ours: dict[str, Any],
    reference: dict[str, Any],
) -> tuple[str, Any, Any] | None:
    """Use the existing tracediff engine over ordered synthetic field events."""

    # Import locally so this tool remains directly runnable from any cwd.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import tracediff  # type: ignore

    # Compare the renderer's causal state before incidental live-register
    # allocation/input values.  This preserves the event's field order within
    # each section while making "first difference" mean the first difference
    # that can affect this render call.
    section_order = (
        "schema",
        "checkpoint",
        "selection_chain",
        "transform_pipeline",
        "house",
        "mesh_arrays",
        "pass_tables",
        "gmd_render_groups",
        "early_globals",
        "command_arena",
        "registers",
    )

    def ordered_semantic(event: dict[str, Any]) -> OrderedDict[str, Any]:
        semantic = _semantic_copy(event)
        return OrderedDict(
            (key, semantic[key])
            for key in (
                *section_order,
                *(name for name in semantic if name not in section_order),
            )
            if key in semantic
        )

    flat_ours = _flatten(ordered_semantic(ours))
    flat_ref = _flatten(ordered_semantic(reference))
    ordered_paths = list(OrderedDict.fromkeys(
        [path for path, _ in flat_ours] + [path for path, _ in flat_ref]
    ))
    map_ours = dict(flat_ours)
    map_ref = dict(flat_ref)

    events_ours = []
    events_ref = []
    for path in ordered_paths:
        ea = tracediff.Event(CHECKPOINT_PC)
        eb = tracediff.Event(CHECKPOINT_PC)
        ea.writes.append(("field", (_stable_word(map_ours.get(path, "<missing>")),)))
        eb.writes.append(("field", (_stable_word(map_ref.get(path, "<missing>")),)))
        events_ours.append(ea)
        events_ref.append(eb)

    divergence, _ = tracediff.diff(events_ours, events_ref, resync_window=0)
    if divergence is None:
        return None
    index = divergence.index
    path = ordered_paths[index]
    return path, map_ours.get(path, "<missing>"), map_ref.get(path, "<missing>")


def _transform_words(event: dict[str, Any], *path: str) -> list[str] | None:
    value: Any = event
    for key in path:
        if not isinstance(value, dict) or key not in value:
            return None
        value = value[key]
    if not isinstance(value, list) or not all(isinstance(word, str) for word in value):
        return None
    return value


def classify_transform_divergence(
    ours: dict[str, Any],
    reference: dict[str, Any],
) -> dict[str, Any]:
    """Classify the first transform-publication divergence without guessing.

    The final object writer is a byte-for-byte copy.  Comparing the object,
    its shared source, and the five upstream bank slots distinguishes a bad
    final copy from stale publication and an already-bad producer input.
    """

    ours_source = _transform_words(
        ours, "transform_pipeline", "shared_source", "words"
    )
    ours_object = _transform_words(
        ours, "transform_pipeline", "object_transform", "words"
    )
    reference_source = _transform_words(
        reference, "transform_pipeline", "shared_source", "words"
    )
    reference_object = _transform_words(
        reference, "transform_pipeline", "object_transform", "words"
    )
    if None in (ours_source, ours_object, reference_source, reference_object):
        return {
            "classification": "insufficient_checkpoint",
            "first_divergence": "transform_pipeline",
        }
    if reference_source != reference_object:
        return {
            "classification": "invalid_reference",
            "first_divergence": "transform_pipeline.source_equals_object",
        }
    if ours_object != ours_source:
        return {
            "classification": "final_object_copy",
            "first_divergence": "transform_pipeline.object_transform",
        }
    if ours_source == reference_source:
        return {
            "classification": "match",
            "first_divergence": None,
        }

    primary_slots = (
        ours.get("transform_pipeline", {})
        .get("primary_bank", {})
        .get("slots", [])
    )
    secondary_slots = (
        ours.get("transform_pipeline", {})
        .get("secondary_bank", {})
        .get("slots", [])
    )
    published_slot = next(
        (
            slot.get("label")
            for slot in [*primary_slots, *secondary_slots]
            if isinstance(slot, dict) and slot.get("words") == reference_source
        ),
        None,
    )
    if published_slot is not None:
        return {
            "classification": "stale_publication",
            "first_divergence": "transform_pipeline.shared_source",
            "reference_transform_present_at": published_slot,
        }
    return {
        "classification": "upstream_input_or_control_flow",
        "first_divergence": "transform_pipeline.primary_bank",
    }


def load_event(path: Path) -> dict[str, Any]:
    text = path.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        if line.startswith("STAGE_RENDER_CHECKPOINT "):
            return json.loads(line.split(" ", 1)[1])
    data = json.loads(text)
    if not isinstance(data, dict):
        raise ValueError(f"{path} does not contain a checkpoint object")
    return data


def command_extract(args: argparse.Namespace) -> int:
    mem, metadata = load_backing(args)
    event = extract_checkpoint(mem, metadata, engine=args.engine)
    encoded = json.dumps(event, separators=(",", ":"), ensure_ascii=True)
    line = f"STAGE_RENDER_CHECKPOINT {encoded}\n"
    Path(args.output).write_text(line, encoding="utf-8")
    print(line, end="")
    return 0


def command_diff(args: argparse.Namespace) -> int:
    ours = load_event(Path(args.ours))
    reference = load_event(Path(args.reference))
    difference = first_difference(ours, reference)
    if difference is None:
        print("STAGE_RENDER_CHECKPOINTS_AGREE")
        return 0
    path, ours_value, ref_value = difference
    print(f"FIRST_STAGE_RENDER_DIFFERENCE {path}")
    print(f"  recomp: {json.dumps(ours_value, sort_keys=True)}")
    print(f"  rpcs3:  {json.dumps(ref_value, sort_keys=True)}")
    return 1


def command_classify(args: argparse.Namespace) -> int:
    ours = load_event(Path(args.ours))
    reference = load_event(Path(args.reference))
    result = classify_transform_divergence(ours, reference)
    print("STAGE_TRANSFORM_CLASSIFICATION " + json.dumps(result, sort_keys=True))
    return 0 if result["classification"] == "match" else 1


def command_self_test(_: argparse.Namespace) -> int:
    page = bytearray(0x4000)
    base = 0x40000000
    # RegionMemory must honor address-based lookup and big-endian reads.
    page[0x20:0x24] = struct.pack(">I", 0x12345678)
    mem = RegionMemory([{"address": _hex32(base), "bytes": page.hex()}])
    assert mem.read32(base + 0x20) == 0x12345678

    a = {"schema": 1, "house": {"b0": "0x2508", "b2": 4}}
    b = {"schema": 1, "house": {"b0": "0x2518", "b2": 4}}
    diff = first_difference(a, b)
    assert diff and diff[0] == "house.b0"

    # Raw allocation addresses must not create a semantic divergence.
    a = {"pointer": {"raw": "0x60000000", "kind": "role", "identity": "house"}}
    b = {"pointer": {"raw": "0x70000000", "kind": "role", "identity": "house"}}
    assert first_difference(a, b) is None

    identity = [
        "0x3f800000", "0x00000000", "0x00000000", "0x00000000",
        "0x00000000", "0x3f800000", "0x00000000", "0x00000000",
        "0x00000000", "0x00000000", "0x3f800000", "0x00000000",
        "0x00000000", "0x00000000", "0x00000000", "0x3f800000",
    ]
    poison = [
        "0xffc00000", "0xffc00000", "0xffc00000", "0x00000000",
        "0xffc00000", "0xffc00000", "0xffc00000", "0x00000000",
        "0xffc00000", "0xffc00000", "0xffc00000", "0x00000000",
        "0x7fc00000", "0x7fc00000", "0x7fc00000", "0x3f800000",
    ]

    def event(source: list[str], obj: list[str], bank: list[list[str]]) -> dict[str, Any]:
        return {
            "transform_pipeline": {
                "primary_bank": {
                    "slots": [
                        {"label": f"primary_transform_bank[{index}]", "words": words}
                        for index, words in enumerate(bank)
                    ]
                },
                "secondary_bank": {"slots": []},
                "shared_source": {"words": source},
                "object_transform": {"words": obj},
            }
        }

    reference = event(identity, identity, [identity])
    assert classify_transform_divergence(
        event(poison, identity, [poison]), reference
    )["classification"] == "final_object_copy"
    assert classify_transform_divergence(
        event(poison, poison, [identity]), reference
    )["classification"] == "stale_publication"
    assert classify_transform_divergence(
        event(poison, poison, [poison]), reference
    )["classification"] == "upstream_input_or_control_flow"
    assert classify_transform_divergence(reference, reference)["classification"] == "match"
    print("stage_checkpoint self-test: PASS")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    extract = sub.add_parser("extract", help="build a structured checkpoint")
    extract.add_argument("--engine", required=True, choices=("rpcs3", "recomp"))
    backing = extract.add_mutually_exclusive_group(required=True)
    backing.add_argument("--dump", help="RPCS3 sparse guest-memory dump directory")
    backing.add_argument("--regions", help="compact recomp memory_regions JSON")
    extract.add_argument("--metadata", required=True, help="exact-PC register metadata")
    extract.add_argument("--output", required=True)
    extract.set_defaults(func=command_extract)

    diff = sub.add_parser("diff", help="report the first semantic field divergence")
    diff.add_argument("--ours", required=True)
    diff.add_argument("--reference", required=True)
    diff.set_defaults(func=command_diff)

    classify = sub.add_parser(
        "classify-transform",
        help="classify the first transform producer/publication divergence",
    )
    classify.add_argument("--ours", required=True)
    classify.add_argument("--reference", required=True)
    classify.set_defaults(func=command_classify)

    test = sub.add_parser("self-test")
    test.set_defaults(func=command_self_test)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
