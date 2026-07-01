#!/usr/bin/env python3
"""TRACK B workbench: replay/analyze an RPCS3 .rrc capture's method stream.

The .rrc is a flattened, as-executed (method, value) stream for one rendered
frame -- exactly the input the LAYER-2 dispatcher (NV4097 -> D3D12) consumes.
This tool turns the capture into the Layer-2 work plan and test fixture:

  --coverage   (default) name every distinct method via tools/nv40_methods.py,
               sorted by frequency: the exact, prioritized to-implement list
               for the D3D12 backend. Unknown methods are flagged.
  --stream     dump the named method stream (like rrc_methods.py, but named),
               optionally windowed with --skip/--count.
  --draws      summarize the frame structure: surface setup, clears,
               VERTEX_BEGIN_END pairs, index/vertex batch sizes, textures
               bound, flips -- the staged targets (clear-color first).

Usage:
    py -3 tools/rrc_replay.py <capture.rrc[.gz]> [--coverage|--stream|--draws]
                              [--skip N] [--count N] [-o out.txt]
"""
import argparse
import collections
import gzip
import struct
import sys

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from nv40_methods import resolve


def load(path):
    op = gzip.open if path.endswith(".gz") else open
    with op(path, "rb") as f:
        return f.read()


def parse_commands(data):
    """Yield (idx, method, value) from the replay_commands vector."""
    pos = 4  # magic
    def u32():
        nonlocal pos
        v = struct.unpack_from("<I", data, pos)[0]; pos += 4; return v
    def vle():
        nonlocal pos
        val = 0; i = 0
        while True:
            b = data[pos]; pos += 1
            val |= (b & 0x7F) << i; i += 7
            if not (b & 0x80):
                return val
    if data[:4] != b"RRC\x00":
        sys.exit("not an .rrc (bad magic %r)" % data[:4])
    u32(); u32()                       # version, LE flag
    n = vle(); pos += n * (432 + 8)    # tile_map
    n = vle(); pos += n * (16 + 8)     # memory_map
    n = vle()                          # memory_data_map
    for _ in range(n):
        dl = vle(); pos += dl + 8
    n = vle(); pos += n * (132 + 8)    # display_buffers_map
    n = vle()                          # replay_commands
    for k in range(n):
        cmd = u32(); val = u32()
        yield k, cmd & 0x3FFFC, val
        ms = vle(); pos += ms * 8 + 16  # memory_state set + tile/display ids


def name_of(m):
    nm, _ = resolve(m)
    return nm or ("UNKNOWN_0x%05X" % m)


def cmd_coverage(cmds, out):
    hist = collections.Counter()
    for _, m, _v in cmds:
        hist[m] += 1
    known = sum(c for m, c in hist.items() if resolve(m)[0])
    total = sum(hist.values())
    out.write("# LAYER-2 method coverage: %d commands, %d distinct methods, "
              "%d%% name-resolved\n" % (total, len(hist), 100 * known // max(total, 1)))
    out.write("# freq  method   name\n")
    for m, c in hist.most_common():
        out.write("%7d  0x%05X  %s\n" % (c, m, name_of(m)))


def cmd_stream(cmds, out, skip, count):
    emitted = 0
    for k, m, v in cmds:
        if k < skip:
            continue
        out.write("M\t%u\t0x%05X\t0x%08X\t%s\n" % (k, m, v, name_of(m)))
        emitted += 1
        if count and emitted >= count:
            break


def cmd_draws(cmds, out):
    begin_end = clears = flips = 0
    draw_batches = idx_batches = 0
    prims = collections.Counter()
    textures = set()
    rt_setups = []
    for k, m, v in cmds:
        if m == 0x1808:                      # VERTEX_BEGIN_END
            if v:
                begin_end += 1
                prims[v] += 1
        elif m == 0x1D94:
            clears += 1
        elif m == 0xE944:
            flips += 1
        elif m == 0x1814:
            draw_batches += 1
        elif m == 0x1824:
            idx_batches += 1
        elif 0x1A00 <= m < 0x1B00 and (m - 0x1A00) % 0x20 == 0:
            textures.add((m - 0x1A00) // 0x20)
        elif m in (0x0208,):
            rt_setups.append((k, v))
    out.write("frame structure:\n")
    out.write("  RT_FORMAT setups : %d %s\n" % (len(rt_setups),
              ["@%d=0x%08X" % rv for rv in rt_setups[:4]]))
    out.write("  CLEAR_BUFFERS    : %d\n" % clears)
    out.write("  draw begin/end   : %d  (primitive types: %s)\n" % (begin_end,
              ", ".join("%d x%d" % (p, c) for p, c in prims.most_common())))
    out.write("  vertex batches   : %d   index batches: %d\n" % (draw_batches, idx_batches))
    out.write("  texture units    : %s\n" % sorted(textures))
    out.write("  driver flips     : %d\n" % flips)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--coverage", action="store_true")
    ap.add_argument("--stream", action="store_true")
    ap.add_argument("--draws", action="store_true")
    ap.add_argument("--skip", type=int, default=0)
    ap.add_argument("--count", type=int, default=0)
    ap.add_argument("-o", "--out", default=None)
    args = ap.parse_args()

    cmds = list(parse_commands(load(args.capture)))
    out = open(args.out, "w") if args.out else sys.stdout
    if args.stream:
        cmd_stream(cmds, out, args.skip, args.count)
    elif args.draws:
        cmd_draws(cmds, out)
    else:
        cmd_coverage(cmds, out)
    if args.out:
        out.close()


if __name__ == "__main__":
    main()
