#!/usr/bin/env python3
"""Extract an EBOOT-resident SPURS job binary and wrap it as an SPU ELF."""

from __future__ import annotations

import argparse
from pathlib import Path

from elf_parser import ELFFile, vaddr_to_offset
from wrap_spu_elf import wrap


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("eboot", type=Path)
    parser.add_argument("--ea", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--size", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--base", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--entry", type=lambda value: int(value, 0))
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    image = ELFFile(str(args.eboot))
    image.load()
    file_offset = vaddr_to_offset(image.program_headers, args.ea)
    if file_offset is None:
        raise SystemExit(f"EA 0x{args.ea:X} is not inside an EBOOT PT_LOAD segment")
    end = file_offset + args.size
    if end > len(image.raw_data):
        raise SystemExit(
            f"slice 0x{file_offset:X}..0x{end:X} exceeds the EBOOT file"
        )

    code = image.raw_data[file_offset:end]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(
        wrap(code, base=args.base, entry=args.entry or args.base)
    )
    print(
        f"Wrote {args.out}: EA 0x{args.ea:X}, size 0x{args.size:X}, "
        f"LS 0x{args.base:X}"
    )


if __name__ == "__main__":
    main()
