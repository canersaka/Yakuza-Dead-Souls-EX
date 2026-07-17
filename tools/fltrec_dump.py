#!/usr/bin/env python3
"""
fltrec_dump.py - decoder for the SPU flight recorder (runtime/spu/spu_fltrec.c).

Reads a dump directory written by yz_fltrec_dump() (scratch/fltrec/<ts>_d<N>/)
and prints its contents as plain text. Stdlib only.

RECORD FORMAT -- two on-disk shapes, auto-detected per dump directory via
meta.json's "format_version" (absent = v1, the original s41 phase-1 dumps):

  v1 (16 bytes, little-endian):
    struct yz_fltrec_rec {
        uint32_t pc;         // ctx->pc at record time (0 for FOREIGN_WRITE)
        uint8_t  kind;       // YZ_FR_*
        uint8_t  aux;        // kind-specific
        uint16_t len_or_ch;  // kind-specific
        uint32_t addr;       // LS address / branch target / EA-high / QPC-low
        uint32_t value;      // stored/read value / EA-low / cmd / QPC-high / checksum
    }

  v2 (28 bytes, little-endian; s42 -- adds cross-SPU source identity + a
      global sequence, and closes the direct-call/cross-image blind spots
      named by scratch/s42_order_reconstruction.md):
    struct yz_fltrec_rec {
        uint64_t seq;        // this ring's own atomic fetch-add sequence
                              // (global arrival order across every writer ctx)
        uint32_t pc;
        uint32_t spu_id;     // ctx->spu_id -- the writer/subject SPU's identity
        uint8_t  kind;
        uint8_t  aux;
        uint16_t len_or_ch;
        uint32_t addr;
        uint32_t value;
    }

KINDS (0-9 unchanged since phase 1; 10-11 added in v2)
    0 SYNC          addr=QPC-low32   value=QPC-high32
    1 STORE32       addr=LS addr     value=stored word
    2 STORE128      addr=LS addr (quadword base, same for all 4 lanes)
                    aux=lane 0..3    value=that lane's word
    3 BRANCH        addr=target pc   (cross-function tail branch; see spu_fltrec.h
                                       for the function-granularity caveat)
    4 RDCH          len_or_ch=channel value=value read
    5 WRCH          len_or_ch=channel value=value written
    6 DMA_GET       addr=LSA         value=EA low32
    7 DMA_PUT       addr=LSA         value=EA low32
    8 DMA_META      (follows a DMA_GET/PUT) addr=EA high32 value=raw cmd byte
                    aux=tag len_or_ch=size
    9 FOREIGN_WRITE addr=LS addr     value=XOR-folded checksum of the written span
                    aux=site (0=ctxsave hdr, 1=ctxsave region,
                              2=shadow hdr, 3=shadow region,
                              4=RO-guard heal, 5=BSS zero (fresh entry),
                              6=mod-reload guard)
                    len_or_ch=length in bytes
   10 XIMG          (v2 only) cross-image adoption at a computed branch
                    addr=LS target   aux=from image_id  len_or_ch=to image_id
   11 CALL_RET      (v2 only) call/return boundary via spu_img_restore -- the
                    RETURN edge of every direct+indirect lifted call (the
                    callee/target is NOT visible, see spu_fltrec.h)
                    addr=ctx->pc (resumed addr)  aux=image_id before the call
                    len_or_ch=image_id after the call

ring.bin holds the MAIN ring (stores/branches/channels/DMA/adoption/call-ret),
oldest-to-newest by ring position -- in v2 this is a genuine multi-writer
global order (every record's slot comes from one shared atomic fetch-add,
whether YZ_FLTREC_ALLCTX recorded one context or many). foreign.bin holds the
separate FOREIGN_WRITE ring, also oldest-to-newest but on ITS OWN sequence (no
shared ordering with ring.bin -- only relate the two via the nearest SYNC
record's host time, if that matters).

USAGE
    py -3 tools/fltrec_dump.py <dumpdir> --summary
    py -3 tools/fltrec_dump.py <dumpdir> --filter addr=0xBD70
    py -3 tools/fltrec_dump.py <dumpdir> --filter addr=0xBC90-0xBDB0
    py -3 tools/fltrec_dump.py <dumpdir> --window 1000 1100
    py -3 tools/fltrec_dump.py <dumpdir> --stores-in 0xBC90-0xBDB0
"""

import argparse
import json
import os
import struct
import sys

# v1 (phase 1, no format_version in meta.json): pc,kind,aux,len_or_ch,addr,value
REC_FMT_V1 = "<IBBHII"
REC_SIZE_V1 = struct.calcsize(REC_FMT_V1)
assert REC_SIZE_V1 == 16, REC_SIZE_V1

# v2 (s42, format_version==2): seq,pc,spu_id,kind,aux,len_or_ch,addr,value
REC_FMT_V2 = "<QIIBBHII"
REC_SIZE_V2 = struct.calcsize(REC_FMT_V2)
assert REC_SIZE_V2 == 28, REC_SIZE_V2

KIND_NAMES = {
    0: "SYNC",
    1: "STORE32",
    2: "STORE128",
    3: "BRANCH",
    4: "RDCH",
    5: "WRCH",
    6: "DMA_GET",
    7: "DMA_PUT",
    8: "DMA_META",
    9: "FOREIGN_WRITE",
    10: "XIMG",
    11: "CALL_RET",
}

FOREIGN_SITE_NAMES = {
    0: "ctxsave-hdr-restore",
    1: "ctxsave-region-restore",
    2: "shadow-hdr-restore",
    3: "shadow-region-restore",
    # sites 4-6 added 2026-07-16 (scratch/s41_upstream_audit.md Probe 1):
    # closed the recorder's blind spot at these three other host-side
    # foreign LS writers (runtime/spu/spu_channels.c).
    4: "ro-guard-heal",
    5: "bss-zero-fresh-entry",
    6: "mod-reload-guard",
}


class Rec:
    """seq = position in this FILE (0..N-1, i.e. ring write order on disk --
    always present, both versions). gseq/spu_id = the v2 in-record fields
    (this ring's own atomic global sequence, and the writer/subject SPU's
    identity); both None when reading a v1 dump."""
    __slots__ = ("seq", "gseq", "spu_id", "pc", "kind", "aux", "len_or_ch", "addr", "value")

    def __init__(self, seq, raw, version):
        self.seq = seq
        if version >= 2:
            (self.gseq, self.pc, self.spu_id, self.kind, self.aux,
             self.len_or_ch, self.addr, self.value) = struct.unpack(REC_FMT_V2, raw)
        else:
            self.gseq = None
            self.spu_id = None
            (self.pc, self.kind, self.aux, self.len_or_ch,
             self.addr, self.value) = struct.unpack(REC_FMT_V1, raw)

    def kind_name(self):
        return KIND_NAMES.get(self.kind, "UNK%d" % self.kind)

    def describe(self):
        k = self.kind
        if k == 0:
            qpc = (self.value << 32) | self.addr
            return "SYNC qpc=0x%016X" % qpc
        if k in (1, 2):
            lane = "" if k == 1 else (" lane%d" % self.aux)
            return "%s%s addr=0x%05X value=0x%08X" % (self.kind_name(), lane,
                                                        self.addr, self.value)
        if k == 3:
            return "BRANCH target=0x%05X" % self.addr
        if k in (4, 5):
            return "%s ch=%u value=0x%08X" % (self.kind_name(), self.len_or_ch, self.value)
        if k in (6, 7):
            return "%s lsa=0x%05X ea_lo=0x%08X" % (self.kind_name(), self.addr, self.value)
        if k == 8:
            return "DMA_META ea_hi=0x%08X cmd=0x%02X tag=%u size=%u" % (
                self.addr, self.value & 0xFF, self.aux, self.len_or_ch)
        if k == 9:
            site = FOREIGN_SITE_NAMES.get(self.aux, "site%d" % self.aux)
            return "FOREIGN_WRITE addr=0x%05X len=%u (%s) checksum=0x%08X" % (
                self.addr, self.len_or_ch, site, self.value)
        if k == 10:
            return "XIMG target=0x%05X image %d -> %d" % (self.addr, self.aux, self.len_or_ch)
        if k == 11:
            return "CALL_RET resumed_pc=0x%05X image %d -> %d" % (
                self.addr, self.aux, self.len_or_ch)
        return "UNKNOWN kind=%d aux=%d len_or_ch=%d addr=0x%X value=0x%X" % (
            k, self.aux, self.len_or_ch, self.addr, self.value)

    def line(self, t=None):
        tstr = ("t=%10.6fs " % t) if t is not None else ""
        idstr = ("spu=0x%X gseq=%d " % (self.spu_id, self.gseq)) if self.spu_id is not None else ""
        return "#%08d %s%spc=0x%05X %s" % (self.seq, tstr, idstr, self.pc, self.describe())


def load_records(path, version):
    """Read a ring/foreign .bin file into a list of Rec (seq 0..N-1, file order
    == the ring's own oldest-to-newest write order, per yz_fltrec_dump()).
    `version` selects the 16-byte v1 or 28-byte v2 on-disk record layout."""
    rec_size = REC_SIZE_V2 if version >= 2 else REC_SIZE_V1
    recs = []
    if not os.path.isfile(path):
        return recs
    with open(path, "rb") as f:
        data = f.read()
    n = len(data) // rec_size
    if len(data) % rec_size:
        sys.stderr.write("warning: %s size %d is not a multiple of %d (truncated record dropped)\n"
                          % (path, len(data), rec_size))
    for i in range(n):
        recs.append(Rec(i, data[i * rec_size:(i + 1) * rec_size], version))
    return recs


def load_meta(dump_dir):
    p = os.path.join(dump_dir, "meta.json")
    if not os.path.isfile(p):
        return {}
    with open(p, "r") as f:
        return json.load(f)


def build_time_index(recs, time_unit_hz):
    """Return a parallel list of seconds-since-first-SYNC for every record
    (None where no SYNC has been seen yet), by walking the SYNC records that
    are interleaved in the SAME ring. time_unit_hz comes from meta.json."""
    times = [None] * len(recs)
    if not time_unit_hz:
        return times
    base_ticks = None
    for r in recs:
        if r.kind == 0:
            ticks = (r.value << 32) | r.addr
            if base_ticks is None:
                base_ticks = ticks
            times[r.seq] = (ticks - base_ticks) / float(time_unit_hz)
        elif base_ticks is not None:
            # carry the last known SYNC time forward as an approximation
            times[r.seq] = times[r.seq - 1] if r.seq > 0 else 0.0
    return times


def parse_addr_range(spec):
    """'0xBD70' -> (0xBD70, 0xBD74); '0xBC90-0xBDB0' -> (0xBC90, 0xBDB0)."""
    spec = spec.strip()
    if "-" in spec:
        lo_s, hi_s = spec.split("-", 1)
        lo, hi = int(lo_s, 0), int(hi_s, 0)
    else:
        lo = int(spec, 0)
        hi = lo + 4
    return lo, hi


def touches(rec, lo, hi):
    """Does this record's addressed span overlap [lo, hi)?"""
    if rec.kind in (1,):
        return lo <= rec.addr < hi
    if rec.kind in (2,):
        return lo <= rec.addr < hi or lo <= rec.addr + 15 < hi
    if rec.kind == 9:
        return not (rec.addr + rec.len_or_ch <= lo or rec.addr >= hi)
    if rec.kind in (6, 7):
        return lo <= rec.addr < hi
    if rec.kind in (3, 10, 11):   # BRANCH / XIMG target / CALL_RET resumed pc
        return lo <= rec.addr < hi
    return False


def cmd_summary(dump_dir, recs, foreign_recs, meta):
    counts = {}
    for r in recs:
        counts[r.kind] = counts.get(r.kind, 0) + 1
    fcounts = {}
    for r in foreign_recs:
        fcounts[r.kind] = fcounts.get(r.kind, 0) + 1

    print("=== %s ===" % dump_dir)
    if meta:
        print("format_version: %s   reason: %s   dump_index: %s   build: %s" % (
            meta.get("format_version", 1), meta.get("reason"),
            meta.get("dump_index"), meta.get("build")))
        if meta.get("allctx") is not None:
            print("allctx: %s" % meta.get("allctx"))
        mr = meta.get("main_ring", {})
        fr = meta.get("foreign_ring", {})
        print("main_ring:    capacity=%s emitted=%s wraps=%s overflowed=%s" % (
            mr.get("capacity_records"), mr.get("emitted_records"),
            mr.get("wraps"), mr.get("overflowed")))
        print("foreign_ring: capacity=%s emitted=%s wraps=%s overflowed=%s" % (
            fr.get("capacity_records"), fr.get("emitted_records"),
            fr.get("wraps"), fr.get("overflowed")))
    print()
    print("main ring: %d records on disk" % len(recs))
    for k in sorted(counts):
        print("  %-14s %8d" % (KIND_NAMES.get(k, "UNK%d" % k), counts[k]))
    print()
    print("foreign ring: %d records on disk" % len(foreign_recs))
    for k in sorted(fcounts):
        print("  %-14s %8d" % (KIND_NAMES.get(k, "UNK%d" % k), fcounts[k]))

    time_unit_hz = meta.get("time_unit_hz") if meta else None
    syncs = [r for r in recs if r.kind == 0]
    print()
    if len(syncs) >= 2 and time_unit_hz:
        first_ticks = (syncs[0].value << 32) | syncs[0].addr
        last_ticks = (syncs[-1].value << 32) | syncs[-1].addr
        span = (last_ticks - first_ticks) / float(time_unit_hz)
        print("time span covered by ring.bin: %.3f s (%d SYNC markers, every %s records)"
              % (span, len(syncs), meta.get("sync_interval_records", "?")))
    else:
        print("time span: not computable (need >=2 SYNC records + time_unit_hz in meta.json)")

    mr = meta.get("main_ring", {}) if meta else {}
    if mr.get("overflowed"):
        print()
        print("NOTE: main ring OVERFLOWED (wraps=%s) -- ring.bin's oldest record is NOT "
              "the boot start; it is whatever was still live when the ring wrapped. "
              "Check --filter/--stores-in results against your window of interest before "
              "concluding an address was 'never touched'." % mr.get("wraps"))


def cmd_filter(recs, foreign_recs, meta, spec):
    lo, hi = parse_addr_range(spec)
    time_unit_hz = meta.get("time_unit_hz") if meta else None
    times = build_time_index(recs, time_unit_hz) if time_unit_hz else [None] * len(recs)
    print("=== events touching [0x%X, 0x%X) ===" % (lo, hi))
    n = 0
    for r in recs:
        if touches(r, lo, hi):
            print(r.line(times[r.seq]))
            n += 1
    for r in foreign_recs:
        if touches(r, lo, hi):
            print("[foreign] " + r.line())
            n += 1
    print("--- %d matching records ---" % n)


def cmd_window(recs, seq0, seq1):
    seq0 = max(0, seq0)
    seq1 = min(len(recs), seq1)
    print("=== raw window [%d, %d) of %d records ===" % (seq0, seq1, len(recs)))
    for r in recs[seq0:seq1]:
        print(r.line())


def cmd_stores_in(recs, spec):
    lo, hi = parse_addr_range(spec)
    print("=== STORE32/STORE128 history in [0x%X, 0x%X) ===" % (lo, hi))
    n = 0
    for r in recs:
        if r.kind in (1, 2) and touches(r, lo, hi):
            print(r.line())
            n += 1
    print("--- %d matching store records ---" % n)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dumpdir", help="dump directory (scratch/fltrec/<ts>_d<N>/)")
    ap.add_argument("--summary", action="store_true", help="event counts by kind, time span, wrap info")
    ap.add_argument("--filter", metavar="addr=0xADDR[-0xADDR]",
                     help="all events touching an address/range, in order")
    ap.add_argument("--window", nargs=2, type=int, metavar=("SEQ0", "SEQ1"),
                     help="raw listing of main-ring records [SEQ0, SEQ1)")
    ap.add_argument("--stores-in", metavar="0xLO-0xHI", help="STORE32/STORE128 history in a range")
    args = ap.parse_args()

    ring_path = os.path.join(args.dumpdir, "ring.bin")
    foreign_path = os.path.join(args.dumpdir, "foreign.bin")
    meta = load_meta(args.dumpdir)
    # format_version absent = a pre-s42 v1 dump (16-byte records, no seq/spu_id).
    version = int(meta.get("format_version", 1)) if meta else 1
    recs = load_records(ring_path, version)
    foreign_recs = load_records(foreign_path, version)

    if not recs and not foreign_recs:
        sys.stderr.write("no records found in %s (missing ring.bin/foreign.bin?)\n" % args.dumpdir)
        return 2

    did_something = False
    if args.summary:
        cmd_summary(args.dumpdir, recs, foreign_recs, meta)
        did_something = True
    if args.filter:
        spec = args.filter
        if spec.startswith("addr="):
            spec = spec[len("addr="):]
        cmd_filter(recs, foreign_recs, meta, spec)
        did_something = True
    if args.window:
        cmd_window(recs, args.window[0], args.window[1])
        did_something = True
    if args.stores_in:
        cmd_stores_in(recs, args.stores_in)
        did_something = True

    if not did_something:
        cmd_summary(args.dumpdir, recs, foreign_recs, meta)

    return 0


if __name__ == "__main__":
    sys.exit(main())
