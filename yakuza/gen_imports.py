#!/usr/bin/env python3
"""
Generate import_bridges_gen.cpp from the EBOOT's PRX import stubs.

The EBOOT imports system-library functions through sce stubs:

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
    a first-call-logging stub returning CELL_ENOSYS), and
  - yz_install_imports(): writes a synthetic OPD per import into guest
    scratch memory (YZ_IMPORT_OPD_BASE), points each slot at it, and the
    fake code addresses (YZ_IMPORT_FAKE_BASE + i*4) route ps3_indirect_call
    to the bridges.

Implemented-name detection scans libs/ + runtime/ sources for a function
definition `name(`. Bridges are signature-aware: float/double
params read ctx->fpr[1..13] (PPU ABI: fp args travel in FPRs; every param
still consumes a GPR slot) and are declared in-position so x64 passes them
in xmmN; float/double returns write ctx->fpr[1]; u32 returns zero-extend,
s32 returns sign-extend, 64-bit returns pass through. Signature-less names
fall back to the legacy 8 x u64 prototype with (int32_t) narrowing. No HLE
import returns a vector today; a vr[2] return lane is unimplemented.

Usage:  py -3 gen_imports.py [--elf EBOOT.elf] [--output import_bridges_gen.cpp]
"""

import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from nid_database import get_default_db, compute_nid
from source_parts import read_source

ELF = os.path.join(ROOT, "game", "EBOOT.elf")
OUT = os.path.join(HERE, "import_bridges_gen.cpp")

PT_PROC_PRX_PARAM = 0x60000002


def be16(b, o): return struct.unpack_from(">H", b, o)[0]
def be32(b, o): return struct.unpack_from(">I", b, o)[0]
def be64(b, o): return struct.unpack_from(">Q", b, o)[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", default=ELF)
    parser.add_argument("--output", default=OUT)
    opts = parser.parse_args()
    data = open(opts.elf, "rb").read()
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
    # Sources: the builtin NID database and the function definitions in
    # libs/ and runtime/.
    db = get_default_db()
    candidates = {e[1] if isinstance(e, tuple) else e.name
                  for e in db.all_entries()} if hasattr(db, "all_entries") else set()

    src_blobs = []
    for top in ("libs", "runtime"):
        for dirpath, _dirs, files in os.walk(os.path.join(ROOT, top)):
            for fn in files:
                if fn.endswith((".c", ".cpp")):
                    src_blobs.append(read_source(os.path.join(dirpath, fn)))
    libs_blob = "\n".join(src_blobs)
    defined = set(re.findall(r"^(?:[A-Za-z_][\w]*[\s\*]+)+([A-Za-z_]\w+)\s*\([^;]*$",
                             libs_blob, re.M))
    candidates |= defined

    nid2name = {}
    for n in candidates:
        nid2name[compute_nid(n)] = n
    # This cellGcmSys entry has no public name; bind its NID explicitly so the
    # context-aware override in import_overrides.cpp resolves.
    nid2name[0x3A33C1FD] = "_cellGcmFunc15"

    # sys_net exports its BSD socket entry points under the classic bare
    # names ("socket", "bind", "connect", ...), so the game's import NIDs hash
    # those literal strings. Our host implementation in
    # libs/network/sysNet.c instead uses a sys_net_bnet_* prefix (matching
    # LV2's internal syscall names) to avoid colliding with the platform
    # socket()/bind()/connect()/... functions it wraps. Without this alias
    # every one of these NIDs falls through as an unresolved 0x-named stub
    # (CELL_ENOSYS) even though the real implementation exists right below.
    SYS_NET_NAME_ALIASES = {
        "socket":       "sys_net_bnet_socket",
        "bind":         "sys_net_bnet_bind",
        "listen":       "sys_net_bnet_listen",
        "accept":       "sys_net_bnet_accept",
        "connect":      "sys_net_bnet_connect",
        "send":         "sys_net_bnet_send",
        "sendto":       "sys_net_bnet_sendto",
        "recv":         "sys_net_bnet_recv",
        "recvfrom":     "sys_net_bnet_recvfrom",
        "shutdown":     "sys_net_bnet_shutdown",
        "socketclose":  "sys_net_bnet_close",
        "setsockopt":   "sys_net_bnet_setsockopt",
        "getsockopt":   "sys_net_bnet_getsockopt",
        "getsockname":  "sys_net_bnet_getsockname",
        "socketpoll":   "sys_net_bnet_poll",
        "socketselect": "sys_net_bnet_select",
        "inet_aton":    "sys_net_bnet_inet_aton",
        "gethostbyname":"sys_net_bnet_gethostbyname",
    }
    for exported_name, impl_name in SYS_NET_NAME_ALIASES.items():
        nid2name[compute_nid(exported_name)] = impl_name

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
    # opaque userdata stay raw guest values (see cellSysutil.c). Pointer
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

    def param_kind(p, prev_kind=None):
        """Classify one parameter: 'float' | 'double' | 'ptr' | 'raw' | 'cb'.

        Order matters: the '*' test comes before the "Callback" type-name
        test, so a pointer to a callback-named struct
        (CellOskDialogCallbackReturnParam*) is translated to a host pointer
        rather than passed as a raw guest EA to an HLE function that
        dereferences it. A '*' means guest->host translation except for the
        opaque user-token idiom: a void* named like a user token, or a void*
        immediately following a callback-typedef param (the SDK's
        (cb, cbArg) pair convention), travels back to a guest callback
        verbatim and must stay raw.
        'cb' is a raw starless callback typedef (returned so the next
        param's classification can see it).
        """
        if p == "...":
            return "raw"
        name_tok = p.replace("*", " ").split()[-1] if p.split() else ""
        if "(" in p:
            return "cb"            # explicit function-pointer syntax
        if name_tok.lower().startswith(("userdata", "userarg", "callbackarg",
                                        "cbarg")):
            return "raw"           # opaque guest token (cellSysutil.c)
        if "*" in p:
            if prev_kind == "cb" and re.match(r"^\s*(const\s+)?void\s*\*", p):
                return "raw"       # the (callback, void* arg) pair idiom
            return "ptr"           # guest addr -> host pointer
        if "Callback" in p or "callback" in p.lower():
            return "cb"            # typedef'd function pointer, no '*'
        if re.search(r"\bfloat\b", p):
            return "float"         # PPU ABI: f1..f13, gpr slot skipped
        if re.search(r"\bdouble\b", p):
            return "double"
        return "raw"

    def ret_kind(ret):
        """Classify a return type for bridge marshalling: u32 returns must
        zero-extend, 64-bit returns must not be narrowed to int32_t, and float
        returns go to fpr[1]."""
        if "*" in ret:
            return "ptr"
        if re.match(r"^void\b", ret):
            return "void"
        if re.search(r"\bfloat\b", ret):
            return "float"
        if re.search(r"\bdouble\b", ret):
            return "double"
        if re.search(r"\b(u64|uint64_t|s64|int64_t|long long)\b", ret):
            return "i64"
        if re.search(r"\b(u32|uint32_t|unsigned)\b", ret):
            return "u32"
        return "s32"

    def bridge_plan(name):
        """Compute (decl_ret, decl_params[8], args[8], rkind) for an
        implemented HLE import, or None when no signature was found (the
        caller falls back to the legacy generic bridge). PPU ELF ABI: every
        parameter consumes one 64-bit GPR slot (r3..r10) whether or not it
        is floating point; float/double values travel in f1..f13 in order
        of appearance. On the host side the extern declaration must carry
        float/double in the right positions (x64 passes by position into
        xmmN) while everything else stays u64."""
        sig = signature_of(name)
        if not sig:
            return None
        ret, params = sig
        gprs = [f"ctx->gpr[{3+i}]" for i in range(8)]
        args = []
        decl_params = []
        nf = 0
        variadic = bool(params) and params[-1] == "..."
        fixed = params[:-1] if variadic else params
        prev_kind = None
        for i, p in enumerate(fixed[:8]):
            kind = param_kind(p, prev_kind)
            prev_kind = kind
            if kind == "float":
                nf += 1
                args.append(f"(float)ctx->fpr[{nf}]")
                decl_params.append("float")
            elif kind == "double":
                nf += 1
                args.append(f"ctx->fpr[{nf}]")
                decl_params.append("double")
            elif kind == "ptr":
                args.append(f"yz_hp({gprs[i]})")
                decl_params.append("u64")
            else:
                args.append(gprs[i])
                decl_params.append("u64")
        if variadic and nf:
            # PPU varargs copy fp args into GPRs, so the fixed-float read
            # above still holds; no HLE vararg import has float fixed params
            # today -- warn if one appears so the bridge gets hand-checked.
            print(f"WARNING: variadic import {name} has float fixed params; "
                  f"verify its bridge by hand")
        # pad to 8: raw gprs (x64 ABI ignores extras beyond what callee reads)
        for i in range(len(args), 8):
            args.append(gprs[i])
            decl_params.append("u64")
        rk = ret_kind(ret)
        decl_ret = {"float": "float", "double": "double"}.get(rk, "int64_t")
        return decl_ret, decl_params, args, rk

    # Imports that need ppu_context access -> hand-written bridges in
    # import_overrides.cpp. These take precedence over libs/ implementations.
    OVERRIDES = {
        "sys_lwcond_wait",  # forwards the wait with the lwcond pointer translated to a host pointer
        # runtime PRX loading of the game's own engine modules (pxd_shader):
        # load -> handle, start -> run the module's module_start (inits the
        # shader subsystem the render thread spin-waits on). See import_overrides.cpp.
        "sys_prx_load_module",
        "sys_prx_start_module",
        "sys_prx_stop_module",
        "sys_prx_unload_module",
        "sys_prx_register_library",
        # DRM availability: disc content -> report OK so the title sequence doesn't
        # take the DRM/trophy error path (cellMsgDialog).
        "sceNpDrmIsAvailable",
        # audio output: the engine's early init spin-polls GetState until ENABLED+OK
        # (the render path stays black until it succeeds). Report a ready
        # stereo/48kHz LPCM output + accept Configure. See import_overrides.cpp.
        "cellAudioOutGetState",
        "cellAudioOutConfigure",
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
        # libs HLE writes int out-params host-endian; ctx-aware override
        # marshals through vm_write32 (big-endian) instead.
        "cellSysutilGetSystemParamInt",
        # Nine-argument savedata API: the generic bridge only forwards r3-r10,
        # while userdata lives in the caller's stack argument area.  The
        # ctx-aware bridge also routes this legacy API through the implemented
        # FixedLoad2 callback flow instead of returning CELL_ENOSYS.
        "cellSaveDataListAutoLoad",
        # gcm init: must build guest-side context/control structures (the
        # game's SDK-inline gcm code walks them in guest memory directly).
        "_cellGcmInitBody",
        "cellGcmGetControlRegister",
        "cellGcmGetConfiguration",
        "cellGcmSetDisplayBuffer",
        # gcm BE out-params / guest-address returns (fifo path depends on
        # AddressToOffset: the inline flush stores its result to ctrl->put).
        "cellGcmAddressToOffset",
        "cellGcmGetTiledPitchSize",
        "cellGcmGetTimeStampLocation",
        "cellGcmGetLabelAddress",
        # io map: must populate the consumer's iomap (g_rsx_iomap_ea), not the
        # libs offset table.
        "cellGcmMapEaIoAddress",
        "cellGcmUnmapEaIoAddress",
        # SDK-internal flip entries take (ctx, id, ...); libs versions
        # lack the leading context arg.
        "_cellGcmSetFlipCommand",
        "_cellGcmSetFlipCommandWithWaitLabel",
        # SPU image loading: parses the SPU ELF in guest memory and writes
        # big-endian guest structs.
        "sys_spu_image_import",
        "sys_spu_image_close",
    }
    # Fourteen-argument ABI: arguments 9-14 live in the guest parameter save
    # area and cannot pass through the generic r3-r10 bridge.
    OVERRIDES.add("_cellSpursJobChainAttributeInitialize")
    OVERRIDES.add("_cellGcmFunc15")

    # Several context-aware overrides live in yakuza/*.cpp rather than the
    # libs/ and runtime/ trees scanned above.  Resolve their NIDs explicitly
    # after the override set exists; otherwise regenerating the bridge table
    # silently turns essential TLS/thread/time/SPU-image imports into ENOSYS
    # stubs even though their hand-written implementations are present.
    override_by_nid = {compute_nid(name): name for name in OVERRIDES}
    imports = [
        (module, nid, name or override_by_nid.get(nid), slot)
        for module, nid, name, slot in imports
    ]

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
                plan = bridge_plan(name)
                if plan:
                    decl_ret, decl_params, _args, _rk = plan
                    lines.append(f"{decl_ret} {name}({','.join(decl_params)});")
                else:
                    lines.append(f"int64_t {name}(u64,u64,u64,u64,u64,u64,u64,u64);")
    lines.append("}")
    lines.append("")

    bridges = []   # C identifiers, one per import, in order
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
                plan = bridge_plan(name)
                gprs = [f"ctx->gpr[{3+i}]" for i in range(8)]
                if plan:
                    _decl_ret, _decl_params, args, rk = plan
                else:
                    args = gprs
                    rk = "s32"
                lines.append(f"static void {ident}(ppu_context* ctx) {{")
                lines.append(f"    static int logged = 0;")
                lines.append(f'    if (!logged) {{ logged = 1; fprintf(stderr, "[import] call {name}\\n"); }}')
                call = f"{name}(\n        " + ", ".join(args) + ")"
                if rk == "void":
                    # void-returning guest function: no r3 value to marshal back;
                    # leave ctx->gpr[3] whatever it already was (CELL_OK convention
                    # doesn't apply: callers of a void SDK function don't read r3).
                    lines.append(f"    {call};")
                elif rk in ("float", "double"):
                    # PPU ABI: fp returns travel in f1 only; r3 is not written.
                    lines.append(f"    ctx->fpr[1] = (double){call};")
                elif rk == "ptr":
                    lines.append(f"    ctx->gpr[3] = yz_gp((u64){call});")
                elif rk == "i64":
                    lines.append(f"    ctx->gpr[3] = (uint64_t){call};")
                elif rk == "u32":
                    # zero-extend: an (int32_t) narrowing would sign-extend
                    # u32 returns >= 0x80000000
                    lines.append(f"    ctx->gpr[3] = (uint64_t)(uint32_t){call};")
                else:
                    lines.append(f"    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t){call};")
                lines.append("}")
        else:
            label = name if name else f"0x{nid:08X}"
            ident = f"yz_imp_stub_{module}_{nid:08X}"
            if ident not in emitted:
                emitted.add(ident)
                # Honest failure: returning CELL_OK from a stub that produced
                # no out-data poisons the game (it consumes garbage handles and
                # crashes far from the cause). CELL_ENOSYS makes the game take
                # its own error paths and the boot log name the real gap.
                lines.append(f"static void {ident}(ppu_context* ctx) {{")
                lines.append(f"    static int warned = 0;")
                lines.append(f'    if (!warned) {{ warned = 1; fprintf(stderr, "[import] unimplemented {module}::{label} -> CELL_ENOSYS\\n"); }}')
                lines.append(f"    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010003; /* CELL_ENOSYS */")
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
    lines.append("/* Import names, for diagnostics. */")
    lines.append("const char* const g_yz_import_names[] = {")
    for module, nid, name, slot in imports:
        label = name if name else f"0x{nid:08X}"
        lines.append(f'    "{module}::{label}",')
    lines.append("};")
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

    with open(opts.output, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    print(f"{len(imports)} imported funcs across modules; "
          f"{resolved} NIDs resolved to names; "
          f"{implemented_n} have host impls; "
          f"{len(imports) - implemented_n} stubbed. "
          f"{nvar_total} variable imports (NOT handled).")
    print(f"wrote {opts.output}")


if __name__ == "__main__":
    main()
