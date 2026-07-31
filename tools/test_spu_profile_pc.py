#!/usr/bin/env python3
"""Verify SPU profiling attributes region hops by architectural guest PC."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "runtime" / "spu" / "spu_channels.c"


def function_body(text: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", text)
    if not match:
        raise AssertionError(f"missing function {name}")
    start = match.end() - 1
    depth = 0
    for pos in range(start, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1 : pos]
    raise AssertionError(f"unterminated function {name}")


def main() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    hop = function_body(source, "spu_prof_hop")
    register = function_body(source, "spu_register_function")

    assert "g_spu_cur_ctx" in hop
    assert re.search(r"ctx\s*->\s*pc\s*&\s*SPU_LS_MASK", hop)
    assert "spu_prof_addr_of" not in hop
    assert "spu_prof_insert" not in register

    for obsolete in (
        "SPU_PROF_HSZ",
        "spu_prof_insert",
        "spu_prof_addr_of",
        "s_p_fn",
        "s_p_addr",
    ):
        assert obsolete not in source, obsolete

    print("SPU profile identity: PASS (architectural guest PC, no host-pointer map)")


if __name__ == "__main__":
    main()
