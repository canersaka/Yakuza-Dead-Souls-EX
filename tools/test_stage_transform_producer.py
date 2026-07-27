#!/usr/bin/env python3
"""Deterministic oracle for the orphanage camera/object transform chain.

The executable lifted-instruction regression for the proven EBA630/EBA634
failure lives in test_ppu_lift.py.  This companion test locks the matched
RPCS3 camera vectors, final object writer chain, and divergence classifier
together so a later checkpoint cannot silently move the failure downstream.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from stage_checkpoint import classify_transform_divergence  # noqa: E402


def _event(
    source: list[str],
    object_transform: list[str],
    primary_slots: list[list[str]],
) -> dict[str, Any]:
    return {
        "transform_pipeline": {
            "primary_bank": {
                "slots": [
                    {
                        "label": f"primary_transform_bank[{index}]",
                        "words": words,
                    }
                    for index, words in enumerate(primary_slots)
                ]
            },
            "secondary_bank": {"slots": []},
            "shared_source": {"words": source},
            "object_transform": {"words": object_transform},
        }
    }


def _replay_final_writer(source: list[str]) -> list[str]:
    """Model the eight 64-bit stores at 0x112348..0x112364."""

    assert len(source) == 16
    destination = ["0x00000000"] * 16
    for pair in range(8):
        destination[pair * 2:pair * 2 + 2] = source[pair * 2:pair * 2 + 2]
    return destination


def main() -> int:
    fixture_path = ROOT / "tests" / "stage_transform_replay" / "beach_object_writer.json"
    fixture = json.loads(fixture_path.read_text(encoding="utf-8"))
    valid = fixture["rpcs3_transform_words"]
    poison = fixture["recomp_poison_words"]
    camera = fixture["camera_track_producer"]

    assert camera["entry_pc"] == "0x00eba570"
    assert camera["conversion_pcs"] == ["0x00eba630", "0x00eba634"]
    assert camera["classification"] == "different_generated_instruction_semantics"
    assert camera["local_time"] == 0.0
    assert camera["track_count"] == 207
    assert camera["track_flags"] == "0x00000004"
    assert camera["expected_raw_vectors"] == [
        ["0x408c094b", "0x3e2fe401", "0x4231af03", "0x00000000"],
        ["0x402a1c66", "0xbe6433bb", "0x423fd532", "0x00000000"],
        ["0xbdc31dd5", "0x3f7e4ca0", "0x3d842f5e", "0x00000000"],
    ]

    assert fixture["writer_stores"] == [
        f"0x{address:08x}" for address in range(0x00112348, 0x00112368, 4)
    ]
    assert _replay_final_writer(valid) == valid
    assert _replay_final_writer(poison) == poison

    reference = _event(valid, valid, [valid])
    broken = _event(poison, _replay_final_writer(poison), [poison])
    result = classify_transform_divergence(broken, reference)
    assert result["classification"] == "upstream_input_or_control_flow"

    stale = _event(poison, poison, [valid])
    result = classify_transform_divergence(stale, reference)
    assert result["classification"] == "stale_publication"
    assert result["reference_transform_present_at"] == "primary_transform_bank[0]"

    bad_final_copy = _event(valid, poison, [valid])
    assert (
        classify_transform_divergence(bad_final_copy, reference)["classification"]
        == "final_object_copy"
    )

    # A recurring PC must not make the collector stop at occurrence zero.  A
    # matching occurrence is followed by one more sample for forward progress.
    occurrences = [broken, reference, reference]
    first_match = next(
        index
        for index, event in enumerate(occurrences)
        if classify_transform_divergence(event, reference)["classification"] == "match"
    )
    assert first_match == 1
    assert first_match + 1 < len(occurrences)

    print("stage transform producer replay: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
