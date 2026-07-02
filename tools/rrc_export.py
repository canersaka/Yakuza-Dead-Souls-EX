#!/usr/bin/env python3
"""TRACK B: export an RPCS3 .rrc capture to the .rxs replay-stream binary.

The .rrc (format understood from RPCS3's rsx_replay.h serialization layout;
no code copied) is a flattened, as-executed FIFO command list for one frame
PLUS everything needed to re-execute it offline:

  - memory blocks:  (location, offset) -> byte blob snapshots of guest memory
                    (vertex buffers, index buffers, textures, fragment ucode)
                    referenced by the commands that need them applied first
  - initial rsx_state: the full 16384-word NV4097 register file and the
                    544-instruction transform (vertex) program at capture start
  - display buffer geometry (width/height/pitch/offset per scanout buffer)

The .rrc packs multi-word methods as ONE header entry (count in bits [18:29]
of word0, non-increment flag bit 30) followed by continuation entries with
word0 == 0. This tool EXPANDS that back into one (method, arg) pair per
register write -- exactly what a dispatch(method, arg) consumer wants -- and
interleaves "apply memory block" records at the positions the capture demands.

Output: <out>.rxs, little-endian, layout (see libs/video/tests/replay_main.c):

  header   : magic "RXS1", u32 version (2), u32 n_blocks, u32 n_records,
             u32 reg_words, u32 vp_words, u32 display_w, u32 display_h
  display  : u32 buffer count, then 8 * { u32 w, u32 h, u32 pitch, u32 offset }
             (the gcm display buffer table; flip's argument indexes it)
  regs     : reg_words   * u32   initial NV4097 register file (method = i*4)
  vp       : vp_words    * u32   initial transform program words
  blocks   : n_blocks    * { u32 location, u32 offset, u32 size, u32 data_off }
  data     : concatenated blob bytes (data_off relative to this section)
  records  : n_records   * { u32 a, u32 b }
             a bit31 clear -> method write: method = a & 0x3FFFC, arg = b
             a bit31 set   -> apply block index b to guest memory first

A text sidecar <out>.names.txt lists every distinct method in the stream with
its envytools name and count (the harness loads it for the coverage report).

Usage:
    py -3 tools/rrc_export.py <capture.rrc[.gz]> -o <out.rxs>
"""
import argparse
import collections
import gzip
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nv40_methods import resolve

RSX_METHOD_NON_INCREMENT = 0x40000000


class Reader:
    def __init__(self, data):
        self.d = data
        self.pos = 0

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.pos)[0]
        self.pos += 4
        return v

    def u64(self):
        v = struct.unpack_from("<Q", self.d, self.pos)[0]
        self.pos += 8
        return v

    def vle(self):
        val = 0
        i = 0
        while True:
            b = self.d[self.pos]
            self.pos += 1
            val |= (b & 0x7F) << i
            i += 7
            if not (b & 0x80):
                return val

    def bytes(self, n):
        v = self.d[self.pos:self.pos + n]
        self.pos += n
        return v


def parse_rrc(data):
    r = Reader(data)
    if r.bytes(4) != b"RRC\x00":
        sys.exit("not an .rrc capture (bad magic)")
    version = r.u32()
    le = r.u32()
    if version != 6 or le != 1:
        sys.exit("unsupported .rrc version=%d LE=%d (expected 6/1)" % (version, le))

    # tile_map: n * (432-byte tile_state, u64 id) -- not needed for replay yet
    n = r.vle()
    r.pos += n * (432 + 8)

    # memory_map: n * ({u32 offset, u32 location, u64 data_state}, u64 id)
    blocks_by_id = {}
    for _ in range(r.vle()):
        offset = r.u32()
        location = r.u32()
        data_state = r.u64()
        block_id = r.u64()
        blocks_by_id[block_id] = (offset, location, data_state)

    # memory_data_map: n * (vle len + bytes, u64 id)
    data_by_id = {}
    for _ in range(r.vle()):
        dlen = r.vle()
        blob = r.bytes(dlen)
        data_by_id[r.u64()] = blob

    # display_buffers_map: n * (8 * {w,h,pitch,offset} + u32 count, u64 id)
    display = []
    for _ in range(r.vle()):
        bufs = [struct.unpack_from("<4I", r.bytes(16)) for _ in range(8)]
        count = r.u32()
        r.u64()  # id
        display.append((bufs, count))

    # replay_commands: n * (u32 cmd, u32 value, vle set-size + u64*size,
    #                       u64 tile_state, u64 display_buffer_state)
    commands = []
    for _ in range(r.vle()):
        cmd = r.u32()
        val = r.u32()
        mem = [r.u64() for _ in range(r.vle())]
        r.u64()  # tile_state id
        r.u64()  # display_buffer_state id
        commands.append((cmd, val, mem))

    # trailing rsx_state: 544*4 transform program words + 16384 registers
    vp_words = 544 * 4
    reg_words = 0x10000 // 4
    expect = (vp_words + reg_words) * 4
    remaining = len(data) - r.pos
    if remaining != expect:
        sys.exit("reg_state size mismatch: %d bytes left, expected %d"
                 % (remaining, expect))
    vp = struct.unpack_from("<%dI" % vp_words, data, r.pos)
    regs = struct.unpack_from("<%dI" % reg_words, data, r.pos + vp_words * 4)

    return blocks_by_id, data_by_id, display, commands, vp, regs


def expand_stream(commands, block_index_of):
    """Expand header+continuation entries into (kind, a, b) records.

    kind 'M': method write (a=method, b=arg); kind 'B': apply block a.
    Mirrors the FIFO semantics the capture encodes: word0 carries
    count [18:29] and the non-increment flag; continuation entries
    (word0 == 0 while count remains) carry only args.
    """
    records = []
    hist = collections.Counter()
    pending = 0
    method = 0
    step = 4
    for cmd, val, mem in commands:
        for mid in mem:
            records.append(("B", block_index_of[mid], 0))
        if pending == 0:
            pending = (cmd >> 18) & 0x7FF
            method = cmd & 0x3FFFC
            step = 0 if (cmd & RSX_METHOD_NON_INCREMENT) else 4
            if pending == 0:
                continue  # bare header (capture-injected NOP anchor)
        records.append(("M", method, val))
        hist[method] += 1
        method += step
        pending -= 1
    return records, hist


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("-o", "--out", required=True, help="output .rxs path")
    args = ap.parse_args()

    op = gzip.open if args.capture.endswith(".gz") else open
    with op(args.capture, "rb") as f:
        data = f.read()

    blocks_by_id, data_by_id, display, commands, vp, regs = parse_rrc(data)

    # block table: stable order, resolve data blobs
    block_ids = sorted(blocks_by_id)
    block_index_of = {bid: i for i, bid in enumerate(block_ids)}
    table = []
    data_area = bytearray()
    loc_totals = collections.Counter()
    for bid in block_ids:
        offset, location, data_state = blocks_by_id[bid]
        blob = data_by_id[data_state]
        table.append((location, offset, len(blob), len(data_area)))
        data_area += blob
        loc_totals[location] += len(blob)

    records, hist = expand_stream(commands, block_index_of)

    disp_w = disp_h = disp_count = 0
    disp_bufs = [(0, 0, 0, 0)] * 8
    if display:
        bufs, disp_count = display[0]
        disp_bufs = [tuple(b) for b in bufs]
        if disp_count:
            disp_w, disp_h = bufs[0][0], bufs[0][1]

    with open(args.out, "wb") as f:
        f.write(b"RXS1")
        f.write(struct.pack("<7I", 2, len(table), len(records),
                            len(regs), len(vp), disp_w, disp_h))
        f.write(struct.pack("<I", disp_count))
        for b in disp_bufs:
            f.write(struct.pack("<4I", *b))
        f.write(struct.pack("<%dI" % len(regs), *regs))
        f.write(struct.pack("<%dI" % len(vp), *vp))
        for location, offset, size, doff in table:
            f.write(struct.pack("<4I", location, offset, size, doff))
        f.write(data_area)
        for kind, a, b in records:
            if kind == "B":
                f.write(struct.pack("<II", 0x80000000, a))
            else:
                f.write(struct.pack("<II", a, b))

    names_path = args.out + ".names.txt"
    with open(names_path, "w", newline="\n") as f:
        for m, c in sorted(hist.items()):
            nm, _ = resolve(m)
            f.write("%05X %d %s\n" % (m, c, nm or "UNKNOWN"))

    n_methods = sum(1 for k, _, _ in records if k == "M")
    n_applies = len(records) - n_methods
    print("exported %s" % args.out)
    print("  method writes    : %d (%d distinct methods)" % (n_methods, len(hist)))
    print("  block applies    : %d (%d unique blocks, %d unique blobs)"
          % (n_applies, len(table), len(data_by_id)))
    for loc in sorted(loc_totals):
        where = {0: "local (VRAM)", 1: "main (IO)"}.get(loc, "loc%d" % loc)
        print("  memory @%s: %s bytes" % (where, format(loc_totals[loc], ",")))
    print("  display buffer   : %dx%d (%d buffers)"
          % (disp_w, disp_h, display[0][1] if display else 0))
    print("  names sidecar    : %s" % names_path)


if __name__ == "__main__":
    main()
