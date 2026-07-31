#!/usr/bin/env python3
"""Verify that host-movie command-list ownership is serialized with RSX FIFO."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "yakuza" / "import_overrides.cpp"


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
    body = function_body(source, "yz_movie_set_live_draw_mode")
    operations = re.findall(
        r"\b(yz_rsx_fifo_acquire|rsx_live_draw_set_movie_mode|"
        r"yz_rsx_fifo_release)\s*\(", body
    )
    assert operations == [
        "yz_rsx_fifo_acquire",
        "rsx_live_draw_set_movie_mode",
        "yz_rsx_fifo_release",
    ], operations

    direct_mode_calls = len(re.findall(r"\brsx_live_draw_set_movie_mode\s*\(", source))
    wrapper_calls = len(re.findall(r"\byz_movie_set_live_draw_mode\s*\(", source))
    assert direct_mode_calls == 1, direct_mode_calls
    assert wrapper_calls == 5, wrapper_calls

    for symbol in ("yz_rsx_fifo_acquire", "yz_rsx_fifo_release"):
        assert re.search(rf'extern\s+"C"\s+void\s+{symbol}\s*\(void\)', source)
        assert re.search(rf'extern\s+"C"\s+void\s+{symbol}\s*\(void\)\s*\{{', source)

    print("movie handoff: PASS (four transitions serialized acquire/mode/release)")


if __name__ == "__main__":
    main()
