#!/usr/bin/env python3
"""Offline source contract for yz_rsx_fifo_step wait classifications."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "yakuza" / "import_overrides.cpp"
CLASSIFIER = ROOT / "yakuza" / "rsx_wait_classifier.cpp"


def require(text: str, pattern: str, label: str) -> None:
    if not re.search(pattern, text, re.S):
        raise AssertionError(f"missing {label}: /{pattern}/")


def braced_body(text: str, marker: str) -> str:
    start = text.index(marker)
    opening = text.index("{", start)
    depth = 0
    for pos in range(opening, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1:pos]
    raise AssertionError(f"unterminated braced body after {marker!r}")


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    classifier = CLASSIFIER.read_text(encoding="utf-8")
    begin = source.index(
        "static yz_rsx_wait_category yz_rsx_fifo_step_impl(void)")
    end = source.index(
        "static yz_rsx_wait_category yz_rsx_fifo_step(void)", begin)
    step = source[begin:end]

    contracts = [
        (r"if \(!g_rsx_ctx_ready\).*?YZ_RSX_WAIT_NO_CONTEXT", "no context"),
        (r"if \(get == put\).*?finish\(YZ_RSX_WAIT_EMPTY\)", "empty FIFO"),
        (r"if \(tgt == get\).*?yz_rsx_wait_classifier_stopper_observe\("
         r"&stopper, 1\)",
         "self stopper entry"),
        (r"journal_hle.*?finish\(YZ_RSX_WAIT_SELF_STOPPER\)",
         "journal-owned stopper wait"),
        (r"not yet committed.*?finish\(YZ_RSX_WAIT_SELF_STOPPER\)",
         "unreleased stopper wait"),
        (r"if \(cmd & 0xA0030003u\).*?transition\(\s*YZ_RSX_WAIT_UNFINALIZED_HOLE\)",
         "unfinalized hole entry"),
        (r"if \(count && get < put && pkt_end > put\).*?finish\(\s*YZ_RSX_WAIT_PARTIAL_PACKET\)",
         "partial packet"),
        (r"if \(stalled\).*?finish\(YZ_RSX_WAIT_SEMAPHORE\)",
         "semaphore acquire"),
        (r"not io-mapped; idling.*?finish\(YZ_RSX_WAIT_BAD_FLOW\)",
         "unmapped GET"),
        (r"RETURN without CALL.*?finish\(YZ_RSX_WAIT_BAD_FLOW\)",
         "return without call"),
    ]
    for pattern, label in contracts:
        require(step, pattern, label)

    if re.search(r"\breturn\s+[01]\s*;", step):
        raise AssertionError("yz_rsx_fifo_step still contains an unclassified boolean return")

    finish = braced_body(step, "const auto finish =")
    if re.search(r"\bvm_write|\bg_fifo_ret\s*=|\brsx_live_draw_set_", finish):
        raise AssertionError("finish helper must remain observation-only")
    if finish.count("LeaveCriticalSection(&g_rsx_fifo_lock)") != 1:
        raise AssertionError("finish helper must release the existing FIFO lock exactly once")

    # The production-disabled specialization must compile out every observation
    # below the one immutable wrapper branch.
    wrapper = braced_body(
        source, "static yz_rsx_wait_category yz_rsx_fifo_step(void)")
    require(wrapper,
            r"g_yz_rsx_wait_classifier_enabled.*?"
            r"yz_rsx_fifo_step_impl<true>.*?yz_rsx_fifo_step_impl<false>",
            "single default-off dispatch branch")

    # A stalled acquire leaves GET on the packet header; classification cannot
    # add a write to this return path.
    stalled = braced_body(step, "if (stalled)")
    if "vm_write" in stalled or "YZ_RSX_WAIT_SEMAPHORE" not in stalled:
        raise AssertionError("stalled semaphore path must classify without mutating GET")

    for category in (
        "YZ_RSX_WAIT_ADVANCING", "YZ_RSX_WAIT_EMPTY",
        "YZ_RSX_WAIT_PARTIAL_PACKET", "YZ_RSX_WAIT_SELF_STOPPER",
        "YZ_RSX_WAIT_SEMAPHORE", "YZ_RSX_WAIT_UNFINALIZED_HOLE",
        "YZ_RSX_WAIT_BAD_FLOW", "YZ_RSX_WAIT_NO_CONTEXT",
    ):
        if category not in step:
            raise AssertionError(f"FIFO step never maps {category}")

    forbidden = (
        "vm_write", "RSX_DMACTL", "g_rsx_fifo_lock", "pkg001",
        "movie_mode", "semaphore_release", "WakeByAddress",
    )
    for token in forbidden:
        if token in classifier:
            raise AssertionError(
                f"classifier must remain observation-only; found {token!r}")

    # pkg001 retains the original lock/write order and has no classifier hook.
    package = braced_body(source, "case 0x001:")
    pkg_ops = re.findall(
        r"\b(yz_rsx_fifo_lock_ensure|EnterCriticalSection|vm_write32|"
        r"LeaveCriticalSection)\s*\(", package)
    if pkg_ops[-5:] != [
        "yz_rsx_fifo_lock_ensure", "EnterCriticalSection", "vm_write32",
        "vm_write32", "LeaveCriticalSection",
    ]:
        raise AssertionError(f"pkg001 FIFO write order changed: {pkg_ops}")
    if "wait_classifier" in package:
        raise AssertionError("pkg001 must remain outside classifier accounting")

    # QueryPerformanceCounter may occur at initialization, a real transition,
    # and shutdown, but the transition path must test the phase first.
    transition = braced_body(
        classifier, 'extern "C" void yz_rsx_wait_classifier_transition')
    phase_check = transition.find("g_classifier.phase == category")
    clock_read = transition.find("classifier_now()")
    if phase_check < 0 or clock_read < 0 or phase_check > clock_read:
        raise AssertionError(
            "per-poll QPC regression: phase check must precede classifier_now")

    # The dispatch loop must not claim ADVANCING before the method result is
    # known.  Failed retries register their exact key before finish records the
    # unchanged WAIT_SEMAPHORE phase.
    dispatch = step[step.index("int stalled = 0;"):step.index("if (stalled)")]
    # The call may be the right arm of the native-method ternary; anchor on
    # the exact dispatch function rather than one formatting of its assignment.
    method_call = dispatch.index("yz_rsx_method")
    if "transition(YZ_RSX_WAIT_ADVANCING)" in dispatch[:method_call]:
        raise AssertionError("ADVANCING is entered before yz_rsx_method succeeds")
    sem_attempt = dispatch.find("yz_rsx_wait_classifier_semaphore_attempt")
    if sem_attempt < method_call:
        raise AssertionError("semaphore episode registration precedes method result")

    print("rsx FIFO wait mapping source contract: passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"rsx FIFO wait mapping source contract: FAIL: {exc}", file=sys.stderr)
        raise
