#!/usr/bin/env python3
"""Side-by-side compare our consumer's executed-method stream against RPCS3's.

Inputs share the canonical 'M' format (method, value):
  - ours   : the YZ_FIFO_TRACE file (M lines + J/C/R/S flow lines)
  - rpcs3  : tools/rrc_methods.py output (M lines only)

The two streams have slightly different boot preambles (our FIFO-reset first
frame vs RPCS3's captured mid-movie frame) and ours covers several short frames
while RPCS3's .rrc is one complete frame -- so this uses difflib to ALIGN them
structurally (on method-id, ignoring run-specific EA values) and reports the
matching blocks, the draw/method profile each side, and -- crucially -- the
point where our executed stream STOPS relative to RPCS3's, with our consumer's
flow actions at that tail (the WHY: STOPPER PARK / CALL FOLLOW into stale, etc.).

ASCII-only output (Windows console safe).

Usage:
    py -3 tools/cmp_fifo.py <ours.txt> <rpcs3.txt>
"""
import sys, argparse, difflib, collections


def parse(path, keep_flow):
    records, methods = [], []
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            p = line.split("\t")
            if p[0] == "M":
                # ours:  M <idx> <io_get> <method> <value>   (5 fields)
                # rpcs3: M <idx> <method> <value>            (4 fields)
                try:
                    if len(p) >= 5:
                        m = int(p[3], 16); v = int(p[4], 16)
                    else:
                        m = int(p[2], 16); v = int(p[3], 16)
                except (IndexError, ValueError):
                    continue
                methods.append((m, v))
                records.append(("M", line))
            elif keep_flow and p[0] in ("J", "C", "R", "S"):
                records.append((p[0], line))
    return records, methods


DRAW = 0x1808  # NV4097_SET_BEGIN_END


def profile(methods, label):
    h = collections.Counter(m for m, _ in methods)
    print("%s: %d methods, %d distinct, %d draws (0x1808)"
          % (label, len(methods), len(h), h.get(DRAW, 0)))
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ours")
    ap.add_argument("rpcs3")
    ap.add_argument("-c", "--context", type=int, default=10)
    args = ap.parse_args()

    o_rec, o_m = parse(args.ours, keep_flow=True)
    _,     r_m = parse(args.rpcs3, keep_flow=False)

    print("=== PROFILE ===")
    o_h = profile(o_m, "ours ")
    r_h = profile(r_m, "rpcs3")
    print()

    # Histogram diff: method types RPCS3 executes that ours NEVER does (and v.v.).
    print("=== METHOD-TYPE DIFF ===")
    only_r = sorted((c, m) for m, c in r_h.items() if m not in o_h)
    print("methods RPCS3 runs but OURS never reaches (top 12 by count):")
    for c, m in sorted(only_r, reverse=True)[:12]:
        tag = "  <-- DRAW" if m == DRAW else ""
        print("  0x%05X  x%d%s" % (m, c, tag))
    only_o = sorted((c, m) for m, c in o_h.items() if m not in r_h)
    if only_o:
        print("methods OURS runs but RPCS3's frame does not (top 6):")
        for c, m in sorted(only_o, reverse=True)[:6]:
            print("  0x%05X  x%d" % (m, c))
    print()

    # Anchored side-by-side: align on the SET_OBJECT 0x31337000 marker (the
    # per-frame init start, present in both), then walk forward reporting the
    # first method-id divergence -- the clean "perfect side-by-side".
    def anchor(ms):
        for i, (m, v) in enumerate(ms):
            if m == 0x00000 and v == 0x31337000:
                return i
        return 0
    oa, ra = anchor(o_m), anchor(r_m)
    print("=== ANCHORED SIDE-BY-SIDE (from SET_OBJECT 0x31337000: ours[%d], rpcs3[%d]) ===" % (oa, ra))
    print("  %-26s | %-26s" % ("OURS", "RPCS3"))
    i = 0; div = -1
    while oa + i < len(o_m) and ra + i < len(r_m):
        om, ov = o_m[oa + i]; rm, rv = r_m[ra + i]
        if om != rm:
            div = i; break
        i += 1
    show_to = (div if div >= 0 else i)
    lo = max(0, show_to - args.context)
    for k in range(lo, min(show_to + 1, min(len(o_m) - oa, len(r_m) - ra))):
        om, ov = o_m[oa + k]; rm, rv = r_m[ra + k]
        mk = "==" if om == rm else "!!"
        vk = "" if ov == rv else "  (val differs)"
        print("  %3d %s 0x%05X=0x%08X | 0x%05X=0x%08X%s"
              % (k, mk, om, ov, rm, rv, vk))
    if div >= 0:
        print("=> first method-id DIVERGENCE at +%d after the init anchor "
              "(ours executes a different frame than RPCS3's captured one)" % div)
    else:
        print("=> matched to +%d (end of one stream) with no method-id divergence" % i)
    print()

    print("--- our consumer's LAST %d trace records (WHY we stopped) ---"
          % (args.context * 2))
    for kind, line in o_rec[-(args.context * 2):]:
        print("  " + line)


if __name__ == "__main__":
    main()
