#!/usr/bin/env python3
"""SPU fuzz reference plug-in -- intmisc family (clz, cntb, sumb, avgb, absdb).

Clean-room refs transcribed from the RTL blocks of
SPU_ISA_v1.2_27Jan2007_pub.pdf (NOT the prose):
    clz   -- Count Leading Zeros            (ISA p83, op11 0x2A5, RR 1-src)
    cntb  -- Count Ones in Bytes            (ISA p84, op11 0x2B4, RR 1-src)
    avgb  -- Average Bytes                  (ISA p91, op11 0x0D3, RR 2-src)
    absdb -- Absolute Differences of Bytes  (ISA p92, op11 0x053, RR 2-src)
    sumb  -- Sum Bytes into Halfwords       (ISA p93, op11 0x253, RR 2-src)

SUBTLE family: clz operates on 32-bit words; cntb/avgb/absdb per byte;
sumb interleaves RB-sum and RA-sum within each word. All the per-byte and
per-halfword placement goes through the big-endian to_b/from_b / to_h/from_h
helpers so byte order is correct by construction (byte 0 = quadword MSByte).
"""

from spu_refs_api import register, to_b, from_b, to_h, from_h, M32


# --- clz  (Count Leading Zeros; ISA p83) ------------------------------------
# RTL: for each of four 32-bit word slots, count zero bits to the left of the
# first '1'. If the word is zero the result is 32 (0 <= RT <= 32).
def ref_clz(a):
    out = []
    for w in a:
        w &= M32
        if w == 0:
            out.append(32)
            continue
        t = 0
        # scan from MSB (bit 0 in ISA numbering) down to first '1'
        for m in range(31, -1, -1):
            if (w >> m) & 1:
                break
            t += 1
        out.append(t)
    return out

register("clz", "rr1", ref_clz, op11=0x2A5, subtle=True)


# --- cntb  (Count Ones in Bytes; ISA p84) -----------------------------------
# RTL: for each of 16 byte slots, count the '1' bits in that byte (0 <= RT <= 8);
# place the count in the corresponding byte of RT.
def ref_cntb(a):
    out = []
    for byte in to_b(a):            # 16 big-endian bytes, byte 0 first
        c = 0
        for m in range(8):
            if (byte >> m) & 1:
                c += 1
        out.append(c)
    return from_b(out)

register("cntb", "rr1", ref_cntb, op11=0x2B4, subtle=True)


# --- avgb  (Average Bytes; ISA p91) -----------------------------------------
# RTL: for each of 16 byte slots, RTj <- ((0x00||RAj) + (0x00||RBj) + 1)[7:14].
# The zero-extended add is done without precision loss; bits 7:14 of the 16-bit
# result (bit 0 = MSB) are the 8 bits above the LSB, i.e. the rounded average
# (a + b + 1) >> 1, which stays within a byte (max (255+255+1)>>1 = 255).
def ref_avgb(a, b):
    ba, bb = to_b(a), to_b(b)
    out = []
    for j in range(16):
        s = (ba[j] + bb[j] + 1) & 0xFFFF   # 16-bit sum, no precision loss
        out.append((s >> 1) & 0xFF)        # bits 7:14 of the 16-bit field
    return from_b(out)

register("avgb", "rr", ref_avgb, op11=0x0D3, subtle=True)


# --- absdb  (Absolute Differences of Bytes; ISA p92) ------------------------
# RTL: for each of 16 byte slots (operands UNSIGNED):
#   if RBj >u RAj then RTj <- RBj - RAj  else RTj <- RAj - RBj
def ref_absdb(a, b):
    ba, bb = to_b(a), to_b(b)
    out = []
    for j in range(16):
        out.append((bb[j] - ba[j]) if bb[j] > ba[j] else (ba[j] - bb[j]))
    return from_b(out)

register("absdb", "rr", ref_absdb, op11=0x053, subtle=True)


# --- sumb  (Sum Bytes into Halfwords; ISA p93) ------------------------------
# RTL (per 4-byte word slot k = 0..3, unsigned):
#   RT bytes 0:1 (of word) <- sum of the 4 RB bytes of that word
#   RT bytes 2:3 (of word) <- sum of the 4 RA bytes of that word
# Placed as big-endian halfwords: halfword 0 of each word = RB sum, halfword 1
# = RA sum. Max byte sum = 4*255 = 1020, fits in 16 bits (no truncation).
def ref_sumb(a, b):
    ba, bb = to_b(a), to_b(b)          # 16 big-endian bytes each
    h = []                             # 8 halfwords, big-endian order
    for k in range(4):                 # word slot k
        base = k * 4
        rb_sum = sum(bb[base:base + 4]) & 0xFFFF
        ra_sum = sum(ba[base:base + 4]) & 0xFFFF
        h.append(rb_sum)               # halfword 0 of word k (bytes 0:1)
        h.append(ra_sum)               # halfword 1 of word k (bytes 2:3)
    return from_h(h)

register("sumb", "rr", ref_sumb, op11=0x253, subtle=True)
