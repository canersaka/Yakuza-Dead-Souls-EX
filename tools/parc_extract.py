#!/usr/bin/env python3
"""List or extract files from Yakuza PARC v2 archives.

The archive layout and SLLZ stream format were cross-checked against Rick
Gibbed's zlib-licensed Gibbed.Yakuza0 tools.  This implementation is written
for the recompilation project and intentionally supports extraction only.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import struct
import sys
from collections import deque


@dataclasses.dataclass(frozen=True)
class ParcEntry:
    path: str
    compressed: bool
    uncompressed_size: int
    compressed_size: int
    offset: int


def _read_c_string(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("shift_jis")


def read_entries(archive_path: pathlib.Path) -> list[ParcEntry]:
    with archive_path.open("rb") as stream:
        header = stream.read(32)
        if len(header) != 32 or header[:4] != b"PARC":
            raise ValueError(f"{archive_path} is not a PARC archive")
        if header[4] != 2:
            raise ValueError(f"unsupported PARC version {header[4]}")
        if header[5] not in (0, 1):
            raise ValueError(f"invalid PARC byte order {header[5]}")

        byte_order = "<" if header[5] == 0 else ">"
        (
            marker,
            _header_size_low,
            directory_count,
            directory_table_offset,
            file_count,
            file_table_offset,
        ) = struct.unpack_from(f"{byte_order}6I", header, 8)
        if marker != 0x00020001:
            raise ValueError(f"unexpected PARC marker 0x{marker:08X}")

        stream.seek(32)
        directory_names = [
            _read_c_string(stream.read(64)) for _ in range(directory_count)
        ]
        file_names = [_read_c_string(stream.read(64)) for _ in range(file_count)]

        stream.seek(directory_table_offset)
        directories = [
            struct.unpack(f"{byte_order}8I", stream.read(32))
            for _ in range(directory_count)
        ]

        stream.seek(file_table_offset)
        raw_files = [
            struct.unpack(f"{byte_order}8I", stream.read(32))
            for _ in range(file_count)
        ]

    if not directories and raw_files:
        raise ValueError("PARC archive has files but no root directory")

    entries: list[ParcEntry] = []
    pending: deque[tuple[int, str | None]] = deque([(0, None)])
    visited: set[int] = set()
    while pending:
        directory_index, parent_path = pending.popleft()
        if directory_index in visited or directory_index >= len(directories):
            raise ValueError("invalid or cyclic PARC directory table")
        visited.add(directory_index)

        name = directory_names[directory_index]
        is_root = directory_index == 0 and name == "."
        directory_path = parent_path if is_root else (
            name if parent_path is None else f"{parent_path}/{name}"
        )

        (
            child_count,
            child_index,
            child_file_count,
            child_file_index,
            _unknown10,
            _unknown14,
            _unknown18,
            _unknown1c,
        ) = directories[directory_index]

        for index in range(child_index, child_index + child_count):
            pending.append((index, directory_path))

        for index in range(child_file_index, child_file_index + child_file_count):
            if index >= len(raw_files):
                raise ValueError("invalid PARC file table index")
            (
                flags,
                uncompressed_size,
                compressed_size,
                offset_low,
                _file_unknown10,
                offset_high_and_flags,
                _file_unknown18,
                _file_unknown1c,
            ) = raw_files[index]
            if flags & 0x7FFFFFFF:
                raise ValueError(f"unsupported PARC flags 0x{flags:08X}")
            offset = offset_low | ((offset_high_and_flags & 0xF) << 32)
            path = file_names[index]
            if directory_path:
                path = f"{directory_path}/{path}"
            entries.append(
                ParcEntry(
                    path=path,
                    compressed=bool(flags & 0x80000000),
                    uncompressed_size=uncompressed_size,
                    compressed_size=compressed_size,
                    offset=offset,
                )
            )

    return entries


def decompress_sllz(data: bytes, expected_size: int) -> bytes:
    if len(data) < 16 or data[:4] != b"SLLZ":
        raise ValueError("compressed PARC member has no SLLZ header")
    if data[4] not in (0, 1) or data[5] != 1:
        raise ValueError("unsupported SLLZ stream")

    byte_order = "<" if data[4] == 0 else ">"
    header_size, uncompressed_size, compressed_size = struct.unpack_from(
        f"{byte_order}HII", data, 6
    )
    if header_size != 16:
        raise ValueError(f"unsupported SLLZ header size {header_size}")
    if uncompressed_size != expected_size:
        raise ValueError(
            f"SLLZ size mismatch: header={uncompressed_size}, PARC={expected_size}"
        )
    if compressed_size > len(data):
        raise ValueError("truncated SLLZ stream")

    output = bytearray()
    position = header_size
    payload_size = compressed_size - header_size
    compressed_count = 0
    literal_count = 0

    if payload_size <= 0:
        raise ValueError("empty SLLZ payload")
    flags = data[position]
    position += 1
    compressed_count += 1
    flag_bits = 8

    def flush_literals() -> None:
        nonlocal position, literal_count
        if literal_count == 0:
            return
        literal_end = position + literal_count
        if literal_end > compressed_size:
            raise ValueError("truncated SLLZ literal run")
        output.extend(data[position:literal_end])
        position = literal_end
        literal_count = 0

    while compressed_count < payload_size:
        is_copy = bool(flags & 0x80)
        flags = (flags << 1) & 0xFF
        flag_bits -= 1

        # SLLZ places the next control byte before the final operation's
        # payload at an eight-operation boundary.
        if flag_bits == 0:
            flush_literals()
            if position >= compressed_size:
                raise ValueError("truncated SLLZ control byte")
            flags = data[position]
            position += 1
            compressed_count += 1
            flag_bits = 8

        if not is_copy:
            literal_count += 1
            compressed_count += 1
            continue

        flush_literals()
        if position + 2 > compressed_size:
            raise ValueError("truncated SLLZ copy operation")
        copy_flags = int.from_bytes(data[position : position + 2], "little")
        position += 2
        compressed_count += 2
        distance = 1 + (copy_flags >> 4)
        count = 3 + (copy_flags & 0xF)
        if distance > len(output):
            raise ValueError("invalid SLLZ copy distance")
        for _ in range(count):
            output.append(output[-distance])

    flush_literals()

    if len(output) != uncompressed_size:
        raise ValueError(
            f"SLLZ output size mismatch: got {len(output)}, "
            f"expected {uncompressed_size}"
        )
    return bytes(output)


def read_member(archive_path: pathlib.Path, entry: ParcEntry) -> bytes:
    with archive_path.open("rb") as stream:
        stream.seek(entry.offset)
        data = stream.read(entry.compressed_size)
    if len(data) != entry.compressed_size:
        raise ValueError(f"truncated PARC member {entry.path}")
    if entry.compressed:
        return decompress_sllz(data, entry.uncompressed_size)
    if len(data) != entry.uncompressed_size:
        raise ValueError(f"stored PARC member size mismatch for {entry.path}")
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=pathlib.Path)
    parser.add_argument("--list", action="store_true", help="list archive members")
    parser.add_argument("--extract", metavar="MEMBER", help="extract one member")
    parser.add_argument("--output", type=pathlib.Path, help="extraction output path")
    args = parser.parse_args()

    if not args.list and args.extract is None:
        parser.error("choose --list or --extract MEMBER")
    if args.output is not None and args.extract is None:
        parser.error("--output requires --extract")

    entries = read_entries(args.archive)
    if args.list:
        for entry in entries:
            compression = "SLLZ" if entry.compressed else "stored"
            print(
                f"{entry.path}\t{compression}\t"
                f"{entry.uncompressed_size}\t0x{entry.offset:X}"
            )

    if args.extract is not None:
        normalized = args.extract.replace("\\", "/").casefold()
        matches = [entry for entry in entries if entry.path.casefold() == normalized]
        if len(matches) != 1:
            print(
                f"member {args.extract!r} matched {len(matches)} entries",
                file=sys.stderr,
            )
            return 2
        output_path = args.output or pathlib.Path(matches[0].path).name
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(read_member(args.archive, matches[0]))
        print(output_path)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
