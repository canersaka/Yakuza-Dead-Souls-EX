#!/usr/bin/env python3
"""Decrypt a PS3 retail SELF/SPRX (SCE) -> plaintext ELF.

Clean-room port of the public SELF format (mirrors RPCS3's Crypto/unself.cpp).
Uses the PUBLIC retail decryption keys (the same erk/riv RPCS3 hardcodes in
Crypto/key_vault.cpp) -- no Sony SDK, no firmware-derived secret. Disc content
is non-NPDRM (se_flags & 0x8000 == 0), so the metadata decrypts directly with the
per-revision APP key; no klicensee/RAP needed.

Validate it against a known-good file before trusting it: decrypting the dump's
EBOOT.BIN must reproduce game/EBOOT.elf byte-for-byte.

Usage: py -3 tools/decrypt_self.py <in.self/sprx> <out.elf>
"""
import sys, struct, zlib
from Crypto.Cipher import AES
from Crypto.Util import Counter

# Public retail SELF keys (program_type -> {revision: (erk, riv)}), from RPCS3
# Crypto/key_vault.cpp. KEY_APP=4. Yakuza disc content is revision 0x16.
APP_KEYS = {
    0x16: ("A106692224F1E91E1C4EBAD4A25FBFF66B4B13E88D878E8CD072F23CD1C5BF7C",
           "62773C70BD749269C0AFD1F12E73909E"),
}
KEY_APP = 4


def be(fmt, buf, off):
    return struct.unpack_from(">" + fmt, buf, off)


def aes_cbc_dec(key, iv, data):
    return AES.new(key, AES.MODE_CBC, iv).decrypt(data)


def aes_ctr_dec(key, iv, data):
    ctr = Counter.new(128, initial_value=int.from_bytes(iv, "big"))
    return AES.new(key, AES.MODE_CTR, counter=ctr).decrypt(data)


def decrypt_self(path):
    with open(path, "rb") as f:
        d = f.read()

    if d[:4] != b"SCE\x00":
        raise SystemExit("not an SCE file (bad magic)")

    # --- SCE header (0x20, big-endian) ---
    se_magic, se_hver = be("II", d, 0)
    se_flags, se_type = be("HH", d, 8)
    se_meta, = be("I", d, 0x0C)
    se_hsize, se_esize = be("QQ", d, 0x10)
    if se_type != 1:
        raise SystemExit(f"not a SELF (se_type={se_type})")
    if se_flags & 0x8000:
        raise SystemExit("NPDRM SELF (needs klicensee) -- not handled")

    # --- extended header (0x50 @ 0x20): 10 x u64 ---
    (ext_ver, progid_off, ehdr_off, phdr_off, shdr_off,
     seg_ext_off, ver_hdr_off, supp_off, supp_size, _pad) = be("Q" * 10, d, 0x20)

    # --- program identification header: program_type @ +0x0C, sceversion @ +0x10 ---
    program_type, = be("I", d, progid_off + 0x0C)
    program_sceversion, = be("Q", d, progid_off + 0x10)
    if program_type != KEY_APP:
        raise SystemExit(f"program_type={program_type} (only KEY_APP=4 keyed here); "
                         f"add its key array")
    rev = se_flags
    if rev not in APP_KEYS:
        raise SystemExit(f"no APP key for revision 0x{rev:X}; add it from key_vault.cpp")
    erk = bytes.fromhex(APP_KEYS[rev][0])
    riv = bytes.fromhex(APP_KEYS[rev][1])

    # --- plaintext ELF header (@ ehdr_off, big-endian ELF64) ---
    if d[ehdr_off + 4] != 2:
        raise SystemExit("only ELF64 handled")
    e_phoff, e_shoff = be("QQ", d, ehdr_off + 0x20)
    e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = be(
        "HHHHHH", d, ehdr_off + 0x34)

    # plaintext phdr array (@ phdr_off) -- we need p_offset/p_filesz per segment
    phdrs = []
    for i in range(e_phnum):
        o = phdr_off + i * 0x38
        p_type, p_flags = be("II", d, o)
        p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = be("QQQQQQ", d, o + 8)
        phdrs.append((p_offset, p_filesz))

    # --- metadata ---
    meta_base = se_meta + 0x20                       # sizeof(sce_hdr)=0x20
    meta_info = bytearray(d[meta_base:meta_base + 0x40])
    meta_info = aes_cbc_dec(erk, riv, bytes(meta_info))
    mkey, kpad, miv, ivpad = meta_info[0:16], meta_info[16:32], meta_info[32:48], meta_info[48:64]
    if kpad[0] != 0 or ivpad[0] != 0:
        raise SystemExit("metadata-info pad != 0 -> wrong key/decrypt")

    hdrs_off = meta_base + 0x40
    hdrs_size = se_hsize - (0x20 + se_meta + 0x40)
    hdrs = aes_ctr_dec(mkey, miv, d[hdrs_off:hdrs_off + hdrs_size])

    # metadata header (0x20): section_count @ +0x0C, key_count @ +0x10
    sig_len, unk1, section_count, key_count, opt_size, unk2, unk3 = be("QIIIIII", hdrs, 0)

    meta_shdr = []
    for i in range(section_count):
        o = 0x20 + i * 0x30
        data_offset, data_size = be("QQ", hdrs, o)
        s_type, program_idx, hashed, sha1_idx, encrypted, key_idx, iv_idx, compressed = be(
            "IIIIIIII", hdrs, o + 0x10)
        meta_shdr.append(dict(data_offset=data_offset, data_size=data_size, type=s_type,
                              program_idx=program_idx, encrypted=encrypted,
                              key_idx=key_idx, iv_idx=iv_idx, compressed=compressed))

    dk_off = 0x20 + section_count * 0x30
    data_keys = hdrs[dk_off:dk_off + key_count * 0x10]

    # --- reconstruct the ELF ---
    out_size = max(e_shoff + e_shnum * e_shentsize, e_phoff + e_phnum * e_phentsize, e_ehsize)
    for s in meta_shdr:
        if s["type"] == 2:
            po, pf = phdrs[s["program_idx"]]
            out_size = max(out_size, po + pf)
    out = bytearray(out_size)

    # headers (copied plaintext from the SELF)
    out[0:e_ehsize] = d[ehdr_off:ehdr_off + e_ehsize]
    out[e_phoff:e_phoff + e_phnum * e_phentsize] = d[phdr_off:phdr_off + e_phnum * e_phentsize]
    if e_shoff and e_shnum:
        out[e_shoff:e_shoff + e_shnum * e_shentsize] = d[shdr_off:shdr_off + e_shnum * e_shentsize]

    # program segments (type==2): decrypt (CTR) + optional zlib, place at p_offset
    for s in meta_shdr:
        if s["type"] != 2:
            continue
        raw = d[s["data_offset"]:s["data_offset"] + s["data_size"]]
        if s["encrypted"] == 3:
            dkey = data_keys[s["key_idx"] * 0x10:s["key_idx"] * 0x10 + 0x10]
            div = data_keys[s["iv_idx"] * 0x10:s["iv_idx"] * 0x10 + 0x10]
            raw = aes_ctr_dec(dkey, div, raw)
        if s["compressed"] == 2:
            raw = zlib.decompress(raw)
        po, pf = phdrs[s["program_idx"]]
        out[po:po + len(raw)] = raw

    return bytes(out)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    elf = decrypt_self(sys.argv[1])
    with open(sys.argv[2], "wb") as f:
        f.write(elf)
    print(f"wrote {len(elf)} bytes -> {sys.argv[2]}")
