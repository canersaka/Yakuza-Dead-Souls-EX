#!/usr/bin/env python3
"""Emit RPCS3's executed NV-method stream from an RSX capture (.rrc / .rrc.gz).

The .rrc is RPCS3's flattened replay of one fully-rendered frame: a vector of
replay_commands, each a (cmd, value) pair, recorded AS EXECUTED by RSXFIFO.cpp
(run_FIFO). All FIFO flow-control (GET/PUT, jumps, CALLs, jump-to-self stoppers)
is resolved away -- only the resulting (method, value) sequence remains.

Output is the canonical comparison format shared with our consumer's
YZ_FIFO_TRACE 'M' lines (see yakuza/import_overrides.cpp), so the two streams
diff perfectly with tools/cmp_fifo.py:

    M <idx> 0x<method> 0x<value>

method = cmd & 0x3FFFC (the same decode our consumer uses for cmd & 0x3FFFC).

Usage:
    py -3 tools/rrc_methods.py <capture.rrc[.gz]> [-o out.txt]
"""
import gzip, struct, sys, argparse


def load(path):
    op = gzip.open if path.endswith(".gz") else open
    with op(path, "rb") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("-o", "--out", default=None,
                    help="output file (default: stdout)")
    ap.add_argument("--header", action="store_true",
                    help="print a parse summary to stderr")
    args = ap.parse_args()

    data = load(args.capture)
    pos = 0

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
                break
        return val

    magic = data[pos:pos + 4]; pos += 4
    if magic != b"RRC\x00":
        sys.exit("not an .rrc (bad magic %r)" % magic)
    version = u32(); le = u32()

    # tile_map: unordered_map<tile_state(432B), u64>
    n = vle(); pos += n * (432 + 8)
    # memory_map: unordered_map<memory_block(16B), u64>
    n = vle(); pos += n * (16 + 8)
    # memory_data_map: unordered_map<memory_block_data{vector<u8>}, u64>
    n = vle()
    for _ in range(n):
        dl = vle(); pos += dl + 8
    # display_buffers_map: unordered_map<display_buffers_state(132B), u64>
    n = vle(); pos += n * (132 + 8)
    # replay_commands: vector<replay_command>
    n = vle()

    out = open(args.out, "w") if args.out else sys.stdout
    out.write("# RPCS3 .rrc executed-method stream  (%s, version=%d, %d cmds)\n"
              % (args.capture, version, n))
    for k in range(n):
        cmd = u32(); val = u32()
        method = cmd & 0x3FFFC
        out.write("M\t%u\t0x%05X\t0x%08X\n" % (k, method, val))
        ms = vle(); pos += ms * 8   # memory_state set<u64>
        pos += 8                    # tile_state u64
        pos += 8                    # display_buffer_state u64
    if args.out:
        out.close()

    if args.header:
        sys.stderr.write("magic=%r version=%d LE=%d replay_commands=%d\n"
                         % (magic, version, le, n))


if __name__ == "__main__":
    main()
