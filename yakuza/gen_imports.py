#!/usr/bin/env python3
"""
Generate import_bridges_gen.cpp from the EBOOT's PRX import stubs.

The EBOOT imports firmware functions through sce stubs:

    li r12,0; oris r12,r12,HI; lwz r12,LO(r12)   ; r12 = [slot]
    std r2,0x28(r1)
    lwz r0,0(r12); lwz r2,4(r12)                 ; descriptor -> {code, toc}
    mtctr r0; bctr

On a real PS3 the loader patches each 32-bit slot to point at the exporting
PRX's OPD entry. Statically the slots are unresolved (self-referencing), so
the first import call lands in ps3_indirect_call with garbage.

This generator reads the PROC_PRX_PARAM segment (p_type 0x60000002), walks
the scelibstub records, resolves NIDs to names, and emits:
  - one host bridge per imported function (calls the HLE implementation in
    libs/ with args from gpr[3..10] when the name is implemented; otherwise
    a first-call-logging stub returning CELL_OK), and
  - yz_install_imports(): writes a synthetic OPD per import into guest
    scratch memory (YZ_IMPORT_OPD_BASE), points each slot at it, and the
    fake code addresses (YZ_IMPORT_FAKE_BASE + i*4) route ps3_indirect_call
    to the bridges.

Implemented-name detection scans libs/ + runtime/ sources for a function
definition `name(`. Bridges call through a generic 8 x u64 prototype; the
x64 ABI tolerates extra args, and returns are narrowed via (int32_t).
Float-arg HLE functions would need a hand-written bridge (none needed yet).

Usage:  py -3 gen_imports.py
"""

import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from nid_database import get_default_db, compute_nid

ELF = os.path.join(ROOT, "game", "EBOOT.elf")
OUT = os.path.join(HERE, "import_bridges_gen.cpp")

PT_PROC_PRX_PARAM = 0x60000002


def be16(b, o): return struct.unpack_from(">H", b, o)[0]
def be32(b, o): return struct.unpack_from(">I", b, o)[0]
def be64(b, o): return struct.unpack_from(">Q", b, o)[0]


def main():
    data = open(ELF, "rb").read()

    e_phoff = be64(data, 32)
    e_phentsize = be16(data, 54)
    e_phnum = be16(data, 56)

    segs = []          # (vaddr, offset, filesz)
    prx_param_off = None
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type = be32(data, o)
        p_offset = be64(data, o + 8)
        p_vaddr = be64(data, o + 16)
        p_filesz = be64(data, o + 32)
        if p_type == 1:
            segs.append((p_vaddr, p_offset, p_filesz))
        elif p_type == PT_PROC_PRX_PARAM:
            prx_param_off = p_offset
    if prx_param_off is None:
        sys.exit("no PROC_PRX_PARAM segment found")

    def v2o(vaddr):
        for v, o, sz in segs:
            if v <= vaddr < v + sz:
                return o + (vaddr - v)
        sys.exit(f"vaddr 0x{vaddr:X} not in any PT_LOAD")

    # sys_process_prx_param: size, magic 0x1B434CEC, version, pad,
    #                        libentstart, libentend, libstubstart, libstubend
    magic = be32(data, prx_param_off + 4)
    if magic != 0x1B434CEC:
        sys.exit(f"bad PROC_PRX_PARAM magic 0x{magic:08X}")
    libstubstart = be32(data, prx_param_off + 24)
    libstubend = be32(data, prx_param_off + 28)

    # ---- candidate names -> NID map ------------------------------------
    # Sources: builtin nid db, function definitions in libs/ + runtime/,
    # and REG_FUNC names from the RPCS3 reference tree (names only).
    db = get_default_db()
    candidates = {e[1] if isinstance(e, tuple) else e.name
                  for e in db.all_entries()} if hasattr(db, "all_entries") else set()

    src_blobs = []
    for top in ("libs", "runtime"):
        for dirpath, _dirs, files in os.walk(os.path.join(ROOT, top)):
            for fn in files:
                if fn.endswith((".c", ".cpp")):
                    src_blobs.append(open(os.path.join(dirpath, fn), encoding="utf-8",
                                          errors="replace").read())
    libs_blob = "\n".join(src_blobs)
    defined = set(re.findall(r"^(?:[A-Za-z_][\w]*[\s\*]+)+([A-Za-z_]\w+)\s*\([^;]*$",
                             libs_blob, re.M))
    candidates |= defined

    emu_modules = os.path.join(ROOT, "Emu", "Cell", "Modules")
    if os.path.isdir(emu_modules):
        for fn in os.listdir(emu_modules):
            if fn.endswith(".cpp"):
                text = open(os.path.join(emu_modules, fn), encoding="utf-8",
                            errors="replace").read()
                candidates |= set(re.findall(r"REG_FUNC\(\s*\w+\s*,\s*(\w+)\s*\)", text))

    nid2name = {}
    for n in candidates:
        nid2name[compute_nid(n)] = n

    imports = []   # (module, nid, name_or_None, slot_vaddr)
    nvar_total = 0
    off = v2o(libstubstart)
    end = v2o(libstubend)
    while off < end:
        structsize = data[off]
        nfunc = be16(data, off + 6)
        nvar = be16(data, off + 8)
        libname_ptr = be32(data, off + 16)
        func_nid_ptr = be32(data, off + 20)
        func_stub_ptr = be32(data, off + 24)
        name_off = v2o(libname_ptr)
        module = data[name_off:data.index(b"\0", name_off)].decode("ascii")
        nvar_total += nvar
        for j in range(nfunc):
            nid = be32(data, v2o(func_nid_ptr) + j * 4)
            slot = be32(data, v2o(func_stub_ptr) + j * 4)
            imports.append((module, nid, nid2name.get(nid), slot))
        off += structsize

    def implemented(name):
        return name in defined

    # ---- signature parsing (libs HLE convention) ------------------------
    # HLE functions take HOST pointers for memory args (the bridge must
    # translate guest->host), but callback function pointers and their
    # opaque userdata stay RAW guest values (see cellSysutil.c:82). Pointer
    # returns are host pointers that must be translated back to guest.
    def signature_of(name):
        m = re.search(rf"^([A-Za-z_][\w \t\*]*?)\b{re.escape(name)}\s*\(([^)]*)\)",
                      libs_blob, re.M | re.S)
        if not m:
            return None
        ret = m.group(1).strip()
        ptxt = m.group(2).strip()
        if not ptxt or ptxt == "void":
            params = []
        else:
            params = [p.strip() for p in ptxt.split(",")]
        return ret, params

    def param_marshal(p):
        """Return C expression template for one param ('{}' = gpr value)."""
        if p == "...":
            return "{}"
        name_tok = p.replace("*", " ").split()[-1] if p.split() else ""
        if "(" in p or "Callback" in p or "callback" in p.lower() \
           or name_tok.lower().startswith("userdata"):
            return "{}"            # raw guest value
        if "*" in p:
            return "yz_hp({})"     # guest addr -> host pointer
        return "{}"

    # Imports that need ppu_context access -> hand-written bridges in
    # import_overrides.cpp. These take precedence over libs/ implementations.
    OVERRIDES = {
        "sys_initialize_tls",
        "_sys_heap_create_heap",
        "_sys_heap_delete_heap",
        "_sys_heap_malloc",
        "_sys_heap_memalign",
        "_sys_heap_free",
        "sys_time_get_system_time",
        "sys_ppu_thread_get_id",
        "sys_ppu_thread_create",
        "sys_ppu_thread_exit",
        "sys_ppu_thread_once",
        "sys_lwmutex_destroy",
    }

    resolved, implemented_n = 0, 0
    lines = []
    lines.append("/* Auto-generated by gen_imports.py -- do not edit. */")
    lines.append('#include "ppu_recomp.h"')
    lines.append('#include "yakuza_runner.h"')
    lines.append("#include <cstdio>")
    lines.append("")
    lines.append("typedef uint64_t u64;")
    lines.append('extern "C" uint8_t* vm_base;')
    lines.append("/* guest addr -> host pointer (NULL-preserving) */")
    lines.append("static inline u64 yz_hp(u64 a) { return a ? (u64)(uintptr_t)(vm_base + (uint32_t)a) : 0; }")
    lines.append("/* host pointer -> guest addr (NULL-preserving) */")
    lines.append("static inline u64 yz_gp(u64 h) { return h ? (u64)(uint32_t)((uint8_t*)(uintptr_t)h - vm_base) : 0; }")
    lines.append('extern "C" {')
    seen_decl = set()
    for module, nid, name, slot in imports:
        if name:
            resolved += 1
            if name in OVERRIDES:
                if name not in seen_decl:
                    seen_decl.add(name)
                    lines.append(f"void yz_ovr_{name}(ppu_context*);")
            elif implemented(name) and name not in seen_decl:
                seen_decl.add(name)
                lines.append(f"int64_t {name}(u64,u64,u64,u64,u64,u64,u64,u64);")
    lines.append("}")
    lines.append("")

    bridges = []   # c identifiers, one per import, in order
    emitted = set()
    for module, nid, name, slot in imports:
        if name and name in OVERRIDES:
            implemented_n += 1
            ident = f"yz_ovr_{name}"
            emitted.add(ident)
        elif name and implemented(name):
            implemented_n += 1
            ident = f"yz_imp_{name}"
            if ident not in emitted:
                emitted.add(ident)
                sig = signature_of(name)
                gprs = [f"ctx->gpr[{3+i}]" for i in range(8)]
                if sig:
                    ret, params = sig
                    if params and params[-1] == "...":
                        # fixed params marshalled, varargs passed raw
                        fixed = params[:-1]
                        args = [param_marshal(p).format(g)
                                for p, g in zip(fixed, gprs)]
                        args += gprs[len(fixed):8]
                    else:
                        args = [param_marshal(p).format(g)
                                for p, g in zip(params, gprs)]
                    # extern decl takes 8 args; pad with raw gprs (x64 ABI
                    # ignores extras beyond what the callee reads)
                    args += gprs[len(args):8]
                    if "*" in ret:
                        retexpr = "yz_gp((u64)"
                        retclose = ")"
                    elif re.search(r"\b(u64|uint64_t|s64|int64_t)\b", ret):
                        retexpr = "(uint64_t)"
                        retclose = ""
                    else:
                        retexpr = "(uint64_t)(int64_t)(int32_t)"
                        retclose = ""
                else:
                    args = gprs
                    retexpr = "(uint64_t)(int64_t)(int32_t)"
                    retclose = ""
                lines.append(f"static void {ident}(ppu_context* ctx) {{")
                lines.append(f"    static int logged = 0;")
                lines.append(f'    if (!logged) {{ logged = 1; fprintf(stderr, "[import] call {name}\\n"); }}')
                lines.append(f"    ctx->gpr[3] = {retexpr}{name}(")
                lines.append("        " + ", ".join(args) + f"){retclose};")
                lines.append("}")
        else:
            label = name if name else f"0x{nid:08X}"
            ident = f"yz_imp_stub_{module}_{nid:08X}"
            if ident not in emitted:
                emitted.add(ident)
                lines.append(f"static void {ident}(ppu_context* ctx) {{")
                lines.append(f"    static int warned = 0;")
                lines.append(f'    if (!warned) {{ warned = 1; fprintf(stderr, "[import] unimplemented {module}::{label}\\n"); }}')
                lines.append(f"    ctx->gpr[3] = 0; /* CELL_OK */")
                lines.append("}")
        bridges.append(ident)
    lines.append("")

    lines.append("struct yz_import_entry { uint32_t slot; yz_ppu_fn fn; };")
    lines.append("static const yz_import_entry yz_imports[] = {")
    for (module, nid, name, slot), ident in zip(imports, bridges):
        comment = f"{module}::{name if name else f'0x{nid:08X}'}"
        lines.append(f"    {{ 0x{slot:08X}u, {ident} }}, /* {comment} */")
    lines.append("};")
    lines.append(f"const unsigned g_yz_import_count = {len(imports)}u;")
    lines.append("yz_ppu_fn g_yz_import_bridges[{0}];".format(len(imports)))
    lines.append("")
    lines.append("extern \"C\" void yz_install_imports(void) {")
    lines.append("    for (unsigned i = 0; i < g_yz_import_count; i++) {")
    lines.append("        uint32_t opd  = YZ_IMPORT_OPD_BASE + i * 8;")
    lines.append("        uint32_t stub = yz_imports[i].slot;  /* stub CODE address */")
    lines.append("        vm_write32(opd,     YZ_IMPORT_FAKE_BASE + i * 4);")
    lines.append("        vm_write32(opd + 4, 0);  /* TOC unused by host bridges */")
    lines.append("        /* The real data slot is encoded in the stub:")
    lines.append("         *   li r12,0; oris r12,r12,HI; lwz r12,LO(r12); ... */")
    lines.append("        uint32_t w1 = vm_read32(stub + 4), w2 = vm_read32(stub + 8);")
    lines.append("        if ((w1 >> 16) == 0x658Cu && (w2 >> 16) == 0x818Cu) {")
    lines.append("            uint32_t slot = ((w1 & 0xFFFFu) << 16) + (uint32_t)(int32_t)(int16_t)(w2 & 0xFFFFu);")
    lines.append("            vm_write32(slot, opd);")
    lines.append("        } else {")
    lines.append("            /* unexpected stub shape: plant the descriptor over the")
    lines.append("             * stub bytes (slots self-point at the stub) */")
    lines.append("            vm_write32(stub,     YZ_IMPORT_FAKE_BASE + i * 4);")
    lines.append("            vm_write32(stub + 4, 0);")
    lines.append("        }")
    lines.append("        g_yz_import_bridges[i] = yz_imports[i].fn;")
    lines.append("    }")
    lines.append("}")
    lines.append("")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    print(f"{len(imports)} imported funcs across modules; "
          f"{resolved} NIDs resolved to names; {implemented_n} have host impls; "
          f"{len(imports) - implemented_n} stubbed. {nvar_total} variable imports (NOT handled).")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
