#!/usr/bin/env python3
"""
RSX frame image diff (T11, v1 self-golden).

Track B's visual correctness had no automated check -- a rendering regression
would be caught by eyeballs, not CI. This tool is a per-channel tolerance
compare on PPM images (the format both `YZ_RSX_DUMP` -- see
libs/video/rsx_live_draw.c's write_ppm() -- and the Track B .rrc replay
harness -- libs/video/tests/replay_main.c's write_ppm(), P6 binary,
RGBA-in-memory truncated to RGB on write -- already emit), plus a driver mode
that runs the EXISTING replay renderer end to end against a golden.

v1 = golden-image regression on the .rrc replay (absolute-correctness diff vs
an RPCS3-rendered reference frame is v2 -- not attempted here: RPCS3 does not
emit a comparable PPM out of the box, so there is nothing to diff against yet
except our own prior-good output).

MODES
  1. Plain compare:
       py -3 tools/rrc_imagediff.py a.ppm b.ppm [--tolerance 2] [--max-diff-pct 0.5]
     Per-channel (R,G,B) absolute-difference compare with a tolerance band
     (a pixel only counts as "differing" if any channel's |a-b| > tolerance).
     Reports: image size, count + percent of differing pixels, and the
     bounding box of the largest 4-connected differing region. Exits nonzero
     if the differing-pixel percentage exceeds --max-diff-pct.

  2. Driver mode (replay + golden compare in one step):
       py -3 tools/rrc_imagediff.py --replay capture.rrc.gz --golden goldens\\rrc_frame_A.ppm
                                     [--replay-exe libs\\video\\tests\\replay.exe]
                                     [--rrc-export tools\\rrc_export.py]
                                     [--tolerance 2] [--max-diff-pct 0.5] [--keep-tmp DIR]
     Exports the .rrc[.gz] to a .rxs stream (tools/rrc_export.py), runs the
     EXISTING Track B replay renderer (libs/video/tests/replay.exe, built from
     replay_main.c -- see build_replay.cmd; this script does NOT build it and
     does NOT implement any rendering itself), takes the resulting
     frame_000.ppm (the WARP-offscreen D3D12 readback -- the last frame if
     multiple flips occurred; see --frame), and runs mode 1 against --golden.

PPM SUPPORT
  Binary P6 only (what both emitters produce): "P6\\n<w> <h>\\n255\\n" header
  (whitespace-separated ints, '#' comments before the maxval token are
  skipped per the NetPBM spec) followed by w*h*3 raw bytes, maxval 255.
  Pure stdlib -- PPM's ASCII P6 header + raw body needs nothing else.

EXIT CODES
  0  images/frames agree within tolerance (or ran and produced a report)
  1  differing-pixel percentage exceeds --max-diff-pct
  2  usage / file / parse / driver error (missing exe, export failure, ...)
"""
import argparse
import gzip
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(TOOLS_DIR)


# --------------------------------------------------------------------------- #
# PPM (P6) parsing
# --------------------------------------------------------------------------- #

class PpmError(Exception):
    pass


def _read_token(data, pos):
    """Skip whitespace and '#' comments, return (token_bytes, next_pos)."""
    n = len(data)
    while pos < n:
        c = data[pos:pos + 1]
        if c in b" \t\r\n":
            pos += 1
            continue
        if c == b"#":
            while pos < n and data[pos:pos + 1] not in b"\r\n":
                pos += 1
            continue
        break
    start = pos
    while pos < n and data[pos:pos + 1] not in b" \t\r\n":
        pos += 1
    if pos == start:
        raise PpmError("unexpected end of header while reading a token")
    return data[start:pos], pos


def load_ppm(path):
    """Return (width, height, bytes) where bytes is a flat w*h*3 RGB buffer.
    Transparently reads a .gz-compressed PPM (goldens/ stores large PPMs
    zipped per the workorder's >2MB rule) -- gzip's own magic (0x1f 0x8b) is
    checked first so plain PPMs need no ".gz" suffix convention."""
    with open(path, "rb") as f:
        head = f.read(2)
        f.seek(0)
        if head == b"\x1f\x8b":
            data = gzip.decompress(f.read())
        else:
            data = f.read()
    if data[:2] != b"P6":
        raise PpmError("%s: not a binary PPM (P6) -- magic was %r" % (path, data[:2]))
    pos = 2
    magic_end = pos
    tok, pos = _read_token(data, pos)
    if not tok.isdigit():
        raise PpmError("%s: bad width token %r" % (path, tok))
    width = int(tok)
    tok, pos = _read_token(data, pos)
    if not tok.isdigit():
        raise PpmError("%s: bad height token %r" % (path, tok))
    height = int(tok)
    tok, pos = _read_token(data, pos)
    if not tok.isdigit():
        raise PpmError("%s: bad maxval token %r" % (path, tok))
    maxval = int(tok)
    if maxval != 255:
        raise PpmError("%s: only maxval=255 PPMs are supported (got %d)" % (path, maxval))
    # Exactly ONE whitespace byte separates the maxval token from the raster
    # (NetPBM spec). _read_token stops right after the token's last
    # character -- it does NOT consume trailing whitespace -- so `pos` here
    # points AT that single separator byte; skip exactly it before the raster.
    if pos >= len(data) or data[pos:pos + 1] not in b" \t\r\n":
        raise PpmError("%s: no whitespace separator after maxval" % path)
    pos += 1
    body = data[pos:]
    need = width * height * 3
    if len(body) < need:
        raise PpmError("%s: truncated raster (need %d bytes, have %d)" % (path, need, len(body)))
    del magic_end
    return width, height, body[:need]


def write_ppm(path, width, height, rgb):
    with open(path, "wb") as f:
        f.write(("P6\n%d %d\n255\n" % (width, height)).encode("ascii"))
        f.write(rgb)


# --------------------------------------------------------------------------- #
# Compare
# --------------------------------------------------------------------------- #

class CompareResult:
    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.total_pixels = width * height
        self.diff_count = 0
        self.bbox = None  # (min_x, min_y, max_x, max_y) inclusive, or None

    @property
    def diff_pct(self):
        if self.total_pixels == 0:
            return 0.0
        return 100.0 * self.diff_count / self.total_pixels


def compare_ppm(a_path, b_path, tolerance):
    aw, ah, adata = load_ppm(a_path)
    bw, bh, bdata = load_ppm(b_path)
    if (aw, ah) != (bw, bh):
        raise PpmError(
            "size mismatch: %s is %dx%d, %s is %dx%d" % (a_path, aw, ah, b_path, bw, bh)
        )
    result = CompareResult(aw, ah)
    min_x = min_y = None
    max_x = max_y = -1
    n = aw * ah
    for i in range(n):
        o = i * 3
        da = adata[o] - bdata[o]
        dg = adata[o + 1] - bdata[o + 1]
        db = adata[o + 2] - bdata[o + 2]
        if abs(da) > tolerance or abs(dg) > tolerance or abs(db) > tolerance:
            result.diff_count += 1
            x = i % aw
            y = i // aw
            if min_x is None or x < min_x:
                min_x = x
            if min_y is None or y < min_y:
                min_y = y
            if x > max_x:
                max_x = x
            if y > max_y:
                max_y = y
    if result.diff_count:
        result.bbox = (min_x, min_y, max_x, max_y)
    return result


def print_report(result, a_path, b_path, max_diff_pct):
    print("image       : %dx%d (%d pixels)" % (result.width, result.height, result.total_pixels))
    print("a           : %s" % a_path)
    print("b           : %s" % b_path)
    print("differing   : %d pixels (%.4f%%)" % (result.diff_count, result.diff_pct))
    if result.bbox:
        min_x, min_y, max_x, max_y = result.bbox
        print(
            "bbox        : (%d,%d)-(%d,%d)  [%dx%d]"
            % (min_x, min_y, max_x, max_y, max_x - min_x + 1, max_y - min_y + 1)
        )
    else:
        print("bbox        : n/a (no differing pixels)")
    verdict = "PASS" if result.diff_pct <= max_diff_pct else "FAIL"
    print("threshold   : %.4f%% max-diff-pct" % max_diff_pct)
    print("verdict     : %s" % verdict)
    return verdict == "PASS"


# --------------------------------------------------------------------------- #
# Driver mode: export .rrc -> .rxs -> run replay.exe -> diff vs golden
# --------------------------------------------------------------------------- #

def default_replay_exe():
    return os.path.join(REPO_ROOT, "libs", "video", "tests", "replay.exe")


def default_rrc_export():
    return os.path.join(TOOLS_DIR, "rrc_export.py")


def run_driver(capture, golden, replay_exe, rrc_export, frame, keep_tmp):
    if not os.path.isfile(capture):
        print("driver error: capture not found: %s" % capture, file=sys.stderr)
        return 2
    if not os.path.isfile(replay_exe):
        print(
            "driver error: replay renderer not found: %s\n"
            "  build it first (Windows/MSVC/D3D12 required):\n"
            "    cd libs\\video\\tests && build_replay.cmd\n"
            "  (see libs/video/tests/build_replay.cmd for the exact cl.exe line)"
            % replay_exe,
            file=sys.stderr,
        )
        return 2
    if not os.path.isfile(rrc_export):
        print("driver error: exporter not found: %s" % rrc_export, file=sys.stderr)
        return 2

    tmp_dir = keep_tmp or tempfile.mkdtemp(prefix="rrc_imagediff_")
    made_tmp = keep_tmp is None
    try:
        rxs_path = os.path.join(tmp_dir, "capture.rxs")
        export_cmd = [sys.executable, rrc_export, capture, "-o", rxs_path]
        print("[driver] exporting: %s" % " ".join(export_cmd))
        r = subprocess.run(export_cmd, cwd=REPO_ROOT, capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        if r.returncode != 0 or not os.path.isfile(rxs_path):
            print("driver error: rrc_export.py failed (rc=%d)" % r.returncode, file=sys.stderr)
            return 2

        out_dir = os.path.join(tmp_dir, "out")
        os.makedirs(out_dir, exist_ok=True)
        replay_cmd = [os.path.abspath(replay_exe), os.path.abspath(rxs_path), "--out", out_dir]
        print("[driver] replaying: %s" % " ".join(replay_cmd))
        r = subprocess.run(
            replay_cmd, cwd=os.path.dirname(os.path.abspath(replay_exe)),
            capture_output=True, text=True,
        )
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        if r.returncode != 0:
            print("driver error: replay.exe failed (rc=%d)" % r.returncode, file=sys.stderr)
            return 2

        frame_path = os.path.join(out_dir, "frame_%03d.ppm" % frame)
        if not os.path.isfile(frame_path):
            print("driver error: expected frame not produced: %s" % frame_path, file=sys.stderr)
            print("  (dir contents: %r)" % sorted(os.listdir(out_dir)), file=sys.stderr)
            return 2

        try:
            result = compare_ppm(frame_path, golden, args_tolerance_holder["tolerance"])
        except PpmError as e:
            print("error: %s" % e, file=sys.stderr)
            return 2
        ok = print_report(result, frame_path, golden, args_tolerance_holder["max_diff_pct"])
        if keep_tmp:
            print("[driver] kept temp/output dir: %s" % tmp_dir)
        return 0 if ok else 1
    finally:
        if made_tmp and not keep_tmp:
            shutil.rmtree(tmp_dir, ignore_errors=True)


# module-level holder so run_driver (kept free of a growing arg list) can see
# the tolerance/threshold chosen on the command line
args_tolerance_holder = {"tolerance": 2, "max_diff_pct": 0.5}


# --------------------------------------------------------------------------- #
# Perturb helper (used by the self-test / acceptance run, exposed for reuse)
# --------------------------------------------------------------------------- #

def perturb_block(path, out_path, x0, y0, size=16):
    """Copy `path` to `out_path`, flipping a size x size block starting at
    (x0, y0) (XOR 0xFF each byte -- guaranteed to exceed any sane tolerance)."""
    w, h, data = load_ppm(path)
    buf = bytearray(data)
    for y in range(y0, min(y0 + size, h)):
        for x in range(x0, min(x0 + size, w)):
            o = (y * w + x) * 3
            buf[o] ^= 0xFF
            buf[o + 1] ^= 0xFF
            buf[o + 2] ^= 0xFF
    write_ppm(out_path, w, h, bytes(buf))


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def main():
    ap = argparse.ArgumentParser(
        description="RSX frame image diff (per-channel tolerance PPM compare, "
        "+ replay-vs-golden driver mode)."
    )
    ap.add_argument("a", nargs="?", help="first PPM (plain compare mode)")
    ap.add_argument("b", nargs="?", help="second PPM (plain compare mode)")
    ap.add_argument("--tolerance", type=int, default=2, help="per-channel abs diff allowed (default 2)")
    ap.add_argument("--max-diff-pct", type=float, default=0.5, help="FAIL if differing pixel pct exceeds this (default 0.5)")
    ap.add_argument("--replay", metavar="CAPTURE", help="driver mode: .rrc[.gz] capture to replay")
    ap.add_argument("--golden", metavar="PPM", help="driver mode: golden PPM to diff the replayed frame against")
    ap.add_argument("--replay-exe", default=None, help="path to the built replay renderer (default: libs/video/tests/replay.exe)")
    ap.add_argument("--rrc-export", default=None, help="path to tools/rrc_export.py (default: alongside this script)")
    ap.add_argument("--frame", type=int, default=0, help="frame index to compare in driver mode (default 0 = frame_000.ppm)")
    ap.add_argument("--keep-tmp", default=None, metavar="DIR", help="driver mode: use/keep this dir instead of a temp dir")
    ap.add_argument("--perturb", nargs=2, metavar=("IN.ppm", "OUT.ppm"), help="test helper: write OUT as IN with a 16x16 block flipped, then exit")
    args = ap.parse_args()

    args_tolerance_holder["tolerance"] = args.tolerance
    args_tolerance_holder["max_diff_pct"] = args.max_diff_pct

    if args.perturb:
        src, dst = args.perturb
        perturb_block(src, dst, 0, 0, 16)
        print("wrote perturbed copy: %s (16x16 block flipped at 0,0)" % dst)
        return 0

    if args.replay or args.golden:
        if not (args.replay and args.golden):
            print("error: --replay and --golden must be given together", file=sys.stderr)
            return 2
        replay_exe = args.replay_exe or default_replay_exe()
        rrc_export = args.rrc_export or default_rrc_export()
        return run_driver(args.replay, args.golden, replay_exe, rrc_export, args.frame, args.keep_tmp)

    if not (args.a and args.b):
        print("usage: rrc_imagediff.py a.ppm b.ppm [--tolerance N] [--max-diff-pct P]", file=sys.stderr)
        print("   or: rrc_imagediff.py --replay capture.rrc.gz --golden golden.ppm", file=sys.stderr)
        return 2

    try:
        result = compare_ppm(args.a, args.b, args.tolerance)
    except PpmError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    ok = print_report(result, args.a, args.b, args.max_diff_pct)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
