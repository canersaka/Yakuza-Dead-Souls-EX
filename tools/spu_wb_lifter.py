#!/usr/bin/env python3
"""Whole-block (WB) optimizing SPU lifter -- the third source lane beside the
per-instruction DIAG lift and the region FAST lift (both in spu_lifter.py).

Design: docs/SPU_WB_BACKEND.md. Summary:

  * The image is split into REGIONS exactly like the FAST lift (same
    form_regions + same region-start seeding), and each region becomes one C
    function `spu_wb_<ns><addr>` whose entry switch cases only BASIC-BLOCK
    LEADERS. Non-leader pcs (mid-block resumes) and fallback blocks stay on
    the proven FAST region twin, which is always linked alongside; the
    emitted registration table maps every pc of the image to the right
    function, so the enter-at-any-pc contract is preserved without any
    runtime change.
  * Inside a basic block, guest registers live in __m128i locals (host XMM
    registers): loaded from ctx->gpr at first use, stored back once per
    flush point (block exit, channel op, call, halt path). Instruction
    semantics come from runtime/spu/spu_wb_simd.h kernels (differentially
    proven against spu_helpers.h by tests/spu_wb_kernels).
  * Whole-block optimization: constant propagation/folding (integer
    whitelist), copy propagation, common-subexpression elimination,
    local-store load CSE + store-to-load forwarding under a conservative
    alias rule, dead-code elimination against flush-point liveness.
  * Every architectural boundary publishes state exactly like the FAST
    twin: ctx->pc is materialized before channel ops, stops/halts, calls
    and every trampoline set (the s41 [REV] invariant is asserted on the
    emitted text); dirty registers are flushed before anything that can
    block, longjmp, or run other lifted code.
  * Blocks the backend cannot prove are NOT claimed (fallback reasons in
    the metrics JSON); their pcs register to the FAST twin.

Usage (single image; the batch driver is tools/gen_spu_wb.py):
    py -3 tools/spu_wb_lifter.py --auto-functions IMG.elf \
        --func-prefix spu_jobAE4_ --register-name spu_recomp_register_x \
        --fast-source yakuza/generated/fast/<stem>_fast.c \
        --output DIR --source-name <stem>_wb.c --header-name <stem>_wb.h \
        --metrics-json M.json
"""

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from spu_disasm import disassemble_spu, CHANNEL_NAMES
from spu_lifter import (_ops, _reg, _disp_base, _imm, _chan, _bi_target_reg,
                        compute_bi_r0_jumps, form_regions, derive_region_prefix,
                        SPULifter, _RRR_DEST_OPS, _NO_RT_WRITE)

_CHANNEL_MACROS = set(CHANNEL_NAMES.values())

# ---------------------------------------------------------------------------
# Instruction classification tables (kernel names match spu_wb_simd.h; the
# source-of-truth mapping mnemonic->semantics is spu_lifter's tables, which
# the kernel tests pin against spu_helpers.h)
# ---------------------------------------------------------------------------

# rt = wbk(ra, rb)
VEC2 = {
    "a": "wbk_a", "sf": "wbk_sf", "ah": "wbk_ah", "sfh": "wbk_sfh",
    "and": "wbk_and", "or": "wbk_or", "xor": "wbk_xor",
    "nand": "wbk_nand", "nor": "wbk_nor", "andc": "wbk_andc", "orc": "wbk_orc",
    "ceq": "wbk_ceq", "ceqh": "wbk_ceqh", "ceqb": "wbk_ceqb",
    "cgt": "wbk_cgt", "cgth": "wbk_cgth", "cgtb": "wbk_cgtb",
    "clgt": "wbk_clgt", "clgth": "wbk_clgth", "clgtb": "wbk_clgtb",
    "mpy": "wbk_mpy", "mpyu": "wbk_mpyu",
    "mpyh": "wbk_mpyh", "mpyhh": "wbk_mpyhh", "mpys": "wbk_mpys",
    "fa": "wbk_fa", "fs": "wbk_fs", "fm": "wbk_fm", "fi": "wbk_fi",
    "fceq": "wbk_fceq", "fcgt": "wbk_fcgt",
    "fcmeq": "wbk_fcmeq", "fcmgt": "wbk_fcmgt",
    "dfa": "wbk_dfa", "dfs": "wbk_dfs", "dfm": "wbk_dfm",
    "dfceq": "wbk_dfceq", "dfcmeq": "wbk_dfcmeq",
    "dfcgt": "wbk_dfcgt", "dfcmgt": "wbk_dfcmgt",
    "mpyhhu": "wbk_mpyhhu",
    "eqv": "wbk_eqv", "absdb": "wbk_absdb", "avgb": "wbk_avgb",
    "cg": "wbk_cg", "bg": "wbk_bg", "sumb": "wbk_sumb",
    "shl": "wbk_shl", "shlh": "wbk_shlh",
    "rot": "wbk_rot", "roth": "wbk_roth",
    "shlqbi": "wbk_shlqbi", "rotqbi": "wbk_rotqbi",
    "shlqby": "wbk_shlqby", "rotqby": "wbk_rotqby",
    "shlqbybi": "wbk_shlqbybi", "rotqbybi": "wbk_rotqbybi",
    "rotm": "wbk_rotm", "rotma": "wbk_rotma",
    "rothm": "wbk_rothm", "rothma": "wbk_rothma",
    "rotmah": "wbk_rothma",          # assembler alias (spu_lifter rr_bin)
    "rotqmbi": "wbk_rotqmbi", "rotqmby": "wbk_rotqmby",
    "rotqmbybi": "wbk_rotqmbybi",
}

# rt = wbk(ra)
VEC1 = {
    "clz": "wbk_clz", "cntb": "wbk_cntb",
    "gb": "wbk_gb", "gbh": "wbk_gbh", "gbb": "wbk_gbb",
    "frsqest": "wbk_frsqest", "frest": "wbk_frest",
    "fesd": "wbk_fesd", "frds": "wbk_frds",
    "xsbh": "wbk_xsbh", "xshw": "wbk_xshw", "xswd": "wbk_xswd",
    "orx": "wbk_orx",
    "fsm": "wbk_fsm", "fsmh": "wbk_fsmh", "fsmb": "wbk_fsmb",
    "mfspr": "wbk_mfspr",
}

# rt = wbk(ra, imm)   (ri_imm + sh_imm from spu_lifter, incl. RI8 conversions)
VECI = {
    "ai": "wbk_ai", "ahi": "wbk_ahi", "sfi": "wbk_sfi", "sfhi": "wbk_sfhi",
    "andi": "wbk_andi", "ori": "wbk_ori", "xori": "wbk_xori",
    "ceqi": "wbk_ceqi", "cgti": "wbk_cgti", "clgti": "wbk_clgti",
    "ceqbi": "wbk_ceqbi", "ceqhi": "wbk_ceqhi",
    "clgtbi": "wbk_clgtbi", "clgthi": "wbk_clgthi",
    "cgthi": "wbk_cgthi", "cgtbi": "wbk_cgtbi",
    "mpyi": "wbk_mpyi", "mpyui": "wbk_mpyui",
    "andhi": "wbk_andhi", "andbi": "wbk_andbi",
    "orhi": "wbk_orhi", "orbi": "wbk_orbi",
    "xorhi": "wbk_xorhi", "xorbi": "wbk_xorbi",
    "dftsv": "wbk_dftsv",
    "cflts": "wbk_cflts", "cfltu": "wbk_cfltu",
    "csflt": "wbk_csflt", "cuflt": "wbk_cuflt",
    "iohl": "wbk_iohl",                      # rt also a source, handled below
    "shli": "wbk_shli", "shlhi": "wbk_shlhi",
    "roti": "wbk_roti", "rothi": "wbk_rothi",
    "rotmi": "wbk_rotmi", "rotmai": "wbk_rotmai", "rotmhi": "wbk_rotmhi",
    "rotmahi": "wbk_rotmahi", "rothmi": "wbk_rothmi",
    "shlqbyi": "wbk_shlqbyi", "rotqbyi": "wbk_rotqbyi",
    "shlqbii": "wbk_shlqbii", "rotqbii": "wbk_rotqbii",
    "rotqmbii": "wbk_rotqmbii", "rotqmbyi": "wbk_rotqmbyi",
}
# sh_imm mnemonics whose disasm operand may carry the RI7 field as "$rN"
_SHIMM_REGTOKEN = {"shli", "shlhi", "roti", "rothi", "rotmi", "rotmai",
                   "rotmhi", "rotmahi", "rothmi",
                   "shlqbyi", "rotqbyi", "shlqbii", "rotqbii",
                   "rotqmbii", "rotqmbyi", "cbd", "chd", "cwd", "cdd"}

# rt = wbk(ra, rb, rc)  (RRR: dest in bits 21-27)
VEC3_RRR = {"selb": "wbk_selb", "shufb": "wbk_shufb", "mpya": "wbk_mpya",
            "fma": "wbk_fma", "fms": "wbk_fms", "fnms": "wbk_fnms"}
# rt = wbk(ra, rb, rt)  (accumulator forms)
VEC3_ACC = {"addx": "wbk_addx", "sfx": "wbk_sfx", "cgx": "wbk_cgx",
            "bgx": "wbk_bgx",
            "dfma": "wbk_dfma", "dfms": "wbk_dfms",
            "dfnms": "wbk_dfnms", "dfnma": "wbk_dfnma",
            "mpyhha": "wbk_mpyhha", "mpyhhau": "wbk_mpyhhau"}

GEN_CTRL = {"cbd": "wbk_cbd_pos", "chd": "wbk_chd_pos",
            "cwd": "wbk_cwd_pos", "cdd": "wbk_cdd_pos",
            "cbx": "wbk_cbd_pos", "chx": "wbk_chd_pos",
            "cwx": "wbk_cwd_pos", "cdx": "wbk_cdd_pos"}

_TERMINATORS = {"br", "bra", "bi", "iret", "stop", "stopd"}
_COND_BR = {"brz", "brnz", "brhz", "brhnz"}
_BRANCHY = _TERMINATORS | _COND_BR | {"brsl", "brasl", "bisl", "bisled",
                                      "biz", "binz", "bihz", "bihnz"}

# Pure kernels eligible for CSE / DCE (no ctx, no side effects).
# Everything in VEC1/VEC2/VEC3_*/VECI/GEN_CTRL is pure.

# ---------------------------------------------------------------------------
# Constant folding (Python evaluation of a small integer whitelist; verified
# end-to-end by the whole-image differential sweeps)
# ---------------------------------------------------------------------------

M32 = 0xFFFFFFFF


def _splat(w):
    w &= M32
    return (w, w, w, w)


def fold_const(mn, args, imm):
    """args: tuple of 4-word tuples (or None). Returns 4-word tuple or None."""
    if mn == "il":
        return _splat(imm)
    if mn == "ila":
        return _splat(imm & 0x3FFFF)
    if mn == "ilh":
        h = imm & 0xFFFF
        return _splat((h << 16) | h)
    if mn == "ilhu":
        return _splat((imm & 0xFFFF) << 16)
    if mn == "fsmbi":
        # spu_fsmbi: SPU byte P gets 0xFF iff bit (15-P) of imm set; word i is
        # SPU bytes 4i..4i+3 big-endian -> byte 4i is the word's MSB.
        v = imm & 0xFFFF
        words = []
        for wi in range(4):
            w = 0
            for b in range(4):
                p = wi * 4 + b
                if (v >> (15 - p)) & 1:
                    w |= 0xFF << (24 - 8 * b)
            words.append(w)
        return tuple(words)
    if any(a is None for a in args):
        return None
    if mn == "iohl":
        return tuple((w | (imm & 0xFFFF)) & M32 for w in args[0])
    if mn == "ai":
        return tuple((w + imm) & M32 for w in args[0])
    if mn == "a":
        return tuple((x + y) & M32 for x, y in zip(args[0], args[1]))
    if mn == "sf":
        return tuple((y - x) & M32 for x, y in zip(args[0], args[1]))
    if mn == "andi":
        return tuple(w & (imm & M32) for w in args[0])
    if mn == "ori":
        return tuple((w | (imm & M32)) & M32 for w in args[0])
    if mn == "xori":
        return tuple((w ^ (imm & M32)) & M32 for w in args[0])
    if mn == "and":
        return tuple(x & y for x, y in zip(args[0], args[1]))
    if mn == "or":
        return tuple(x | y for x, y in zip(args[0], args[1]))
    if mn == "xor":
        return tuple(x ^ y for x, y in zip(args[0], args[1]))
    if mn == "shli":
        sh = imm & 0x3F
        return tuple(0 if sh > 31 else (w << sh) & M32 for w in args[0])
    if mn == "rotmi":
        sh = (0 - imm) & 0x3F
        return tuple(0 if sh > 31 else (w >> sh) for w in args[0])
    if mn == "roti":
        sh = imm & 31
        return tuple(((w << sh) | (w >> (32 - sh))) & M32 if sh else w
                     for w in args[0])
    return None


# ---------------------------------------------------------------------------
# IR
# ---------------------------------------------------------------------------

@dataclass
class IROp:
    kind: str            # 'kernel' | 'const' | 'gprload' | 'lsload' | 'lsstore'
                         # | 'rdch' | 'wrch' | 'rchcnt' | 'genctrl' | 'fscrrd'
                         # | 'fscrwr'
    vid: int = -1        # defined value id (-1: none)
    kernel: str = ""
    args: tuple = ()     # value ids (vectors)
    imm: int = None
    words: tuple = None  # const payload
    reg: int = -1        # gprload source register
    addr: object = None  # ('base+disp', base_vid, disp) | ('abs', addr) for LS
    ch: str = ""         # channel expression text
    pc: int = 0
    live: bool = False


@dataclass
class Block:
    leader: int
    end: int                      # exclusive
    insns: list = field(default_factory=list)
    fallback: str = None          # reason string or None
    ops: list = field(default_factory=list)
    tail_lines: list = field(default_factory=list)   # emitted terminator code
    kernel_native = 0


class WBFunc:
    def __init__(self, name, start, end):
        self.name = name
        self.start = start
        self.end = end
        self.blocks = []          # Block list in address order


# ---------------------------------------------------------------------------
# fast-twin parsing: pc -> region symbol (the fallback surface)
# ---------------------------------------------------------------------------

def parse_fast_table(fast_source_path):
    """Extract {pc: region_symbol} from the FAST twin's function table, plus
    the set of region function symbols and the register symbol."""
    text = open(fast_source_path, "r", errors="replace").read()
    table = {}
    for m in re.finditer(r'\{\s*0x([0-9A-Fa-f]{8})u,\s*(\w+),\s*"', text):
        table[int(m.group(1), 16)] = m.group(2)
    m = re.search(r"^void (\w+)\(void\) \{$", text, re.M)
    register = m.group(1) if m else None
    if not table or not register:
        raise SystemExit(f"cannot parse FAST table from {fast_source_path}")
    return table, register


# ---------------------------------------------------------------------------
# The lifter
# ---------------------------------------------------------------------------

class WBLifter:
    def __init__(self, insns, base, func_prefix, register_name,
                 fast_table, fast_register, region_cap=256):
        self.insns = sorted(insns, key=lambda i: i.addr)
        self.by_addr = {i.addr: i for i in self.insns}
        self.base = base
        self.func_prefix = func_prefix
        self.register_name = register_name
        self.fast_table = fast_table
        self.fast_register = fast_register
        self.region_cap = region_cap
        rp = derive_region_prefix(func_prefix)
        assert rp.startswith("spu_region")
        self.wb_prefix = "spu_wb" + rp[len("spu_region"):]
        self.image_span = (self.insns[0].addr, self.insns[-1].addr + 4) \
            if self.insns else (0, 0)
        self.bi_r0_jump = set()
        self.repair_pcs = set()   # forces fallback (gs_task tagread pcs)
        self.metrics = {
            "kernel_native": 0, "kernel_wrapped": 0, "const_folded": 0,
            "cse_hits": 0, "copy_prop": 0, "dce_dropped": 0,
            "ls_load_forwarded": 0, "gpr_loads": 0, "gpr_stores": 0,
            "ls_loads": 0, "ls_stores": 0, "channel_ops": 0, "calls": 0,
            "trampolines": 0,
        }
        self.fallback_blocks = []   # (leader, reason)
        self.unresolved_calls = []
        self.unsupported = {}
        self.n_blocks_helper_free = 0   # compiled blocks with zero wrapped kernels
        self.n_insns_compiled = 0       # insns inside compiled blocks

    # -- CFG ---------------------------------------------------------------
    def compute_leaders(self):
        leaders = set()
        addrs = [i.addr for i in self.insns]
        if not addrs:
            return leaders
        leaders.add(addrs[0])
        lifter = SPULifter()          # reuse branch-target decode
        for insn in self.insns:
            mn = insn.mnemonic
            tgt = lifter._branch_target(insn)
            nxt = insn.addr + 4
            if tgt is not None and self.image_span[0] <= tgt < self.image_span[1]:
                leaders.add(tgt)
            if mn in _BRANCHY or mn in ("stop", "stopd"):
                if nxt in self.by_addr:
                    leaders.add(nxt)
            if mn == ".word" or insn.mnemonic == "unknown":
                # data/undecodable: isolate into its own (fallback) block
                leaders.add(insn.addr)
                if nxt in self.by_addr:
                    leaders.add(nxt)
        return leaders

    def build(self):
        # region split identical to the FAST lift
        from find_spu_functions import collect_brsl_targets
        region_starts = set(collect_brsl_targets(self.insns))
        for ins in self.insns:
            if ins.mnemonic in ("bisl", "bisld", "bisle"):
                region_starts.add(ins.addr + 4)
        regions = form_regions(self.insns, region_starts, self.region_cap)
        leaders = self.compute_leaders()
        leaders |= {rs for rs, _re in regions}

        self.funcs = []
        self.block_owner = {}      # leader -> WBFunc
        self.pc_region = {}        # pc -> region start
        for (rs, re_) in regions:
            fn = WBFunc(f"{self.wb_prefix}{rs:08X}", rs, re_)
            self.funcs.append(fn)
            blk = None
            for insn in self.insns:
                if not (rs <= insn.addr < re_):
                    continue
                self.pc_region[insn.addr] = rs
                if insn.addr in leaders or blk is None:
                    blk = Block(leader=insn.addr, end=insn.addr + 4)
                    fn.blocks.append(blk)
                    self.block_owner[insn.addr] = fn
                blk.insns.append(insn)
                blk.end = insn.addr + 4
        self.leaders = leaders

        # classify fallback blocks
        for fn in self.funcs:
            for blk in fn.blocks:
                for insn in blk.insns:
                    mn = insn.mnemonic
                    if mn == ".word" or mn == "unknown":
                        blk.fallback = "data-word"
                        break
                    if insn.addr in self.repair_pcs:
                        blk.fallback = "repair-pc"
                        break
                    if not self.supported(insn):
                        blk.fallback = f"unsupported:{mn}"
                        self.unsupported[mn] = self.unsupported.get(mn, 0) + 1
                        break
                    if mn in ("brsl", "brasl"):
                        lifter = SPULifter()
                        tgt = lifter._branch_target(insn)
                        if tgt is None:
                            blk.fallback = "unresolved-call"
                            self.unresolved_calls.append(insn.addr)
                            break
                if blk.fallback:
                    self.fallback_blocks.append((blk.leader, blk.fallback))

    def supported(self, insn):
        mn = insn.mnemonic
        if mn in VEC1 or mn in VEC2 or mn in VECI or mn in VEC3_RRR \
                or mn in VEC3_ACC or mn in GEN_CTRL:
            return True
        return mn in ("nop", "lnop", "sync", "dsync", "syncc",
                      "hbr", "hbra", "hbrr", "mtspr",
                      "il", "ila", "ilh", "ilhu", "fsmbi",
                      "lqd", "lqa", "lqr", "lqx", "stqd", "stqa", "stqr", "stqx",
                      "rdch", "wrch", "rchcnt", "fscrrd", "fscrwr",
                      "stop", "stopd",
                      "hgt", "hlgt", "heq", "hgti", "hlgti", "heqi",
                      "br", "bra", "brsl", "brasl",
                      "brz", "brnz", "brhz", "brhnz",
                      "bi", "iret", "bisl", "bisled",
                      "biz", "binz", "bihz", "bihnz")

    # -- routing -----------------------------------------------------------
    def compiled_leader(self, pc):
        """pc is an in-image address: is it the leader of a WB-compiled block?"""
        fn = self.block_owner.get(pc)
        if fn is None:
            return False
        for blk in fn.blocks:
            if blk.leader == pc:
                return blk.fallback is None
        return False

    def target_symbol(self, tgt):
        """C function symbol that executes in-image pc `tgt` when entered with
        ctx->pc == tgt (a leader for WB functions; any pc for FAST)."""
        if self.compiled_leader(tgt):
            return f"{self.wb_prefix}{self.pc_region[tgt]:08X}"
        sym = self.fast_table.get(tgt)
        return sym   # None -> caller falls back to spu_indirect_branch


# ---------------------------------------------------------------------------
# Block translation + optimization + emission
# ---------------------------------------------------------------------------

class BlockEmitter:
    def __init__(self, wl: WBLifter, fn: WBFunc, blk: Block):
        self.wl = wl
        self.fn = fn
        self.blk = blk
        self.ops = []               # IROp list (linear)
        self.nextv = 0
        self.regmap = {}            # guest reg -> vid
        self.reg_dirty = {}         # guest reg -> bool
        self.const = {}             # vid -> 4-word tuple
        self.cse = {}               # (kernel,args,imm) -> vid
        self.lscache = {}           # (basekey, disp) -> vid
        self.lines = []             # emitted C
        self.vname = {}             # vid -> C expr (local name)
        self.sname = {}             # vid -> scalar local name (pref word)
        self.emitted = set()        # vids with a local emitted
        self.m = wl.metrics

    # ---- value plumbing --------------------------------------------------
    def newv(self):
        v = self.nextv
        self.nextv += 1
        return v

    def read_reg(self, r):
        r = int(r)
        if r in self.regmap:
            return self.regmap[r]
        v = self.newv()
        op = IROp(kind="gprload", vid=v, reg=r)
        self.ops.append(op)
        self.regmap[r] = v
        self.reg_dirty[r] = False
        return v

    def write_reg(self, r, vid):
        r = int(r)
        self.regmap[r] = vid
        self.reg_dirty[r] = True

    def add_const(self, words):
        key = ("const", words)
        if key in self.cse:
            return self.cse[key]
        v = self.newv()
        # no IROp: constants materialize on demand at first use (vexpr)
        self.const[v] = words
        self.cse[key] = v
        return v

    def add_kernel(self, mn, kernel, argv, imm=None, native=True):
        # constant folding
        cargs = tuple(self.const.get(a) for a in argv)
        folded = fold_const(mn, cargs, imm) if imm is not None or argv else None
        if folded is None and not argv and imm is not None:
            folded = fold_const(mn, (), imm)
        if folded is not None:
            self.m["const_folded"] += 1
            return self.add_const(folded)
        key = (kernel, tuple(argv), imm)
        if key in self.cse:
            self.m["cse_hits"] += 1
            return self.cse[key]
        v = self.newv()
        self.ops.append(IROp(kind="kernel", vid=v, kernel=kernel,
                             args=tuple(argv), imm=imm))
        self.cse[key] = v
        if native:
            self.m["kernel_native"] += 1
        else:
            self.m["kernel_wrapped"] += 1
        return v

    # ---- LS addressing ---------------------------------------------------
    def ls_key(self, addr):
        kind = addr[0]
        if kind == "abs":
            return ("abs", addr[1] & 0x3FFF0)
        if kind == "bb":
            return ("bb", addr[1], addr[2])
        return ("b", addr[1], addr[2])

    def ls_invalidate_for_store(self, addr):
        key = self.ls_key(addr)
        keep = {}
        for k, v in self.lscache.items():
            if k[0] == "b" and key[0] == "b" and k[1] == key[1]:
                # same base value id: aligned quads with |delta| >= 16 are
                # provably distinct ((x+16)>>4 == (x>>4)+1, wrap included)
                if abs(k[2] - key[2]) >= 16:
                    keep[k] = v
            elif k[0] == "abs" and key[0] == "abs":
                if k[1] != key[1]:
                    keep[k] = v
            # "bb" and cross-shape pairs: cannot prove disjoint -> drop
        self.lscache = keep

    def ls_load(self, addr):
        key = self.ls_key(addr)
        if key in self.lscache:
            self.m["ls_load_forwarded"] += 1
            return self.lscache[key]
        v = self.newv()
        self.ops.append(IROp(kind="lsload", vid=v, addr=addr))
        self.lscache[key] = v
        self.m["ls_loads"] += 1
        return v

    def ls_store(self, addr, vid):
        self.ls_invalidate_for_store(addr)
        self.ops.append(IROp(kind="lsstore", args=(vid,), addr=addr))
        self.lscache[self.ls_key(addr)] = vid
        self.m["ls_stores"] += 1

    def flush_marker(self):
        """Record which (reg, vid) pairs are dirty NOW; used for liveness roots
        and store emission."""
        dirty = {r: v for r, v in self.regmap.items() if self.reg_dirty.get(r)}
        op = IROp(kind="flush")
        op.dirty = dict(dirty)
        self.ops.append(op)
        for r in dirty:
            self.reg_dirty[r] = False
        return op

    def channel_barrier(self):
        # values stay valid across a channel op (docs/SPU_WB_BACKEND.md), but
        # DMA can rewrite LS: drop the LS cache.
        self.lscache = {}

    # ---- per-instruction translation ------------------------------------
    def translate(self, insn):
        mn = insn.mnemonic
        ops = _ops(insn.operands)
        addr = insn.addr

        def rt():
            return int(_reg(ops[0]))

        if mn in ("nop", "lnop"):
            return
        if mn in ("hbr", "hbra", "hbrr"):
            return
        if mn == "mtspr":
            return
        if mn in ("sync", "dsync", "syncc"):
            self.ops.append(IROp(kind="fence", pc=addr))
            return
        if mn == "fscrrd":
            v = self.newv()
            self.ops.append(IROp(kind="fscrrd", vid=v))
            self.write_reg(rt(), v)
            return
        if mn == "fscrwr":
            v = self.read_reg(_reg(ops[1] if len(ops) > 1 else ops[0]))
            self.ops.append(IROp(kind="fscrwr", args=(v,)))
            return

        # constants
        if mn in ("il", "ila", "ilh", "ilhu", "fsmbi"):
            imm = int(_imm(ops[1]), 0)
            words = fold_const(mn, (), imm)
            self.write_reg(rt(), self.add_const(words))
            self.m["const_folded"] += 1
            return
        if mn == "iohl":
            imm = int(_imm(ops[1]), 0)
            a = self.read_reg(rt())
            self.write_reg(rt(), self.add_kernel("iohl", "wbk_iohl", [a], imm))
            return

        # loads/stores
        if mn == "lqd":
            disp, base = _disp_base(ops[1])
            bv = self.read_reg(base)
            self.write_reg(rt(), self.ls_load(("b+d", bv, int(disp, 0))))
            return
        if mn == "stqd":
            disp, base = _disp_base(ops[1])
            bv = self.read_reg(base)
            sv = self.read_reg(rt())
            self.ls_store(("b+d", bv, int(disp, 0)), sv)
            return
        if mn in ("lqa", "lqr"):
            self.write_reg(rt(), self.ls_load(("abs", int(_imm(ops[1]), 0))))
            return
        if mn in ("stqa", "stqr"):
            sv = self.read_reg(rt())
            self.ls_store(("abs", int(_imm(ops[1]), 0)), sv)
            return
        if mn == "lqx":
            av = self.read_reg(_reg(ops[1]))
            bv = self.read_reg(_reg(ops[2]))
            self.write_reg(rt(), self.ls_load(("bb", av, bv)))
            return
        if mn == "stqx":
            av = self.read_reg(_reg(ops[1]))
            bv = self.read_reg(_reg(ops[2]))
            sv = self.read_reg(rt())
            self.ls_store(("bb", av, bv), sv)
            return

        # generate-controls: scalar position
        if mn in GEN_CTRL:
            if mn in ("cbd", "chd", "cwd", "cdd"):
                tok = ops[2]
                imm = int(tok[2:], 0) if tok.startswith("$r") else int(_imm(tok), 0)
                a = self.read_reg(_reg(ops[1]))
                v = self.newv()
                self.ops.append(IROp(kind="genctrl", vid=v, kernel=GEN_CTRL[mn],
                                     args=(a,), imm=imm))
            else:
                a = self.read_reg(_reg(ops[1]))
                b = self.read_reg(_reg(ops[2]))
                v = self.newv()
                self.ops.append(IROp(kind="genctrl", vid=v, kernel=GEN_CTRL[mn],
                                     args=(a, b), imm=None))
            self.m["kernel_native"] += 1
            self.write_reg(rt(), v)
            return

        # channels
        if mn == "rdch":
            self.flush_marker()
            v = self.newv()
            self.ops.append(IROp(kind="rdch", vid=v, ch=_chan(ops[1]), pc=addr))
            self.channel_barrier()
            self.write_reg(rt(), v)
            self.m["channel_ops"] += 1
            return
        if mn == "rchcnt":
            self.flush_marker()
            v = self.newv()
            self.ops.append(IROp(kind="rchcnt", vid=v, ch=_chan(ops[1]), pc=addr))
            self.channel_barrier()
            self.write_reg(rt(), v)
            self.m["channel_ops"] += 1
            return
        if mn == "wrch":
            sv = self.read_reg(_reg(ops[1]))
            self.flush_marker()
            self.ops.append(IROp(kind="wrch", args=(sv,), ch=_chan(ops[0]), pc=addr))
            self.channel_barrier()
            self.m["channel_ops"] += 1
            return

        # conditional halt (falls through when false)
        if mn in ("hgt", "hlgt", "heq", "hgti", "hlgti", "heqi"):
            av = self.read_reg(_reg(ops[1])) if len(ops) >= 2 else self.read_reg(ops[0])
            # operand layout: rt(false target), ra, rb/imm -- ra is ops[1]
            if mn in ("heqi", "hgti", "hlgti"):
                rhs = ("imm", int(_imm(ops[2]), 0))
            else:
                rhs = ("vec", self.read_reg(_reg(ops[2])))
            op = IROp(kind="halt_cond", args=(av,), pc=addr)
            op.halt_mn = mn
            op.rhs = rhs
            # snapshot of dirty (reg -> vid) at this point: the taken path
            # must publish them before spu_halt longjmps; the untaken path
            # keeps them cached (dirty flags unchanged).
            op.dirty_snapshot = {r: v for r, v in self.regmap.items()
                                 if self.reg_dirty.get(r)}
            self.ops.append(op)
            return

        # vector ALU
        if mn in VEC3_RRR:
            d = (insn.raw >> 21) & 0x7F
            a = self.read_reg(_reg(ops[1]))
            b = self.read_reg(_reg(ops[2]))
            c = self.read_reg(_reg(ops[3]))
            native = mn in ("selb", "shufb")
            self.write_reg(d, self.add_kernel(mn, VEC3_RRR[mn], [a, b, c],
                                              native=native))
            return
        if mn in VEC3_ACC:
            a = self.read_reg(_reg(ops[1]))
            b = self.read_reg(_reg(ops[2]))
            t = self.read_reg(rt())
            native = mn in ("addx", "sfx", "cgx", "bgx", "mpyhha", "mpyhhau")
            self.write_reg(rt(), self.add_kernel(mn, VEC3_ACC[mn], [a, b, t],
                                                 native=native))
            return
        if mn in VEC2:
            a = self.read_reg(_reg(ops[1]))
            b = self.read_reg(_reg(ops[2]))
            native = not VEC2[mn].startswith(("wbk_f", "wbk_df")) \
                and mn not in ("shlh", "roth", "rothm", "rothma", "rotmah")
            self.write_reg(rt(), self.add_kernel(mn, VEC2[mn], [a, b],
                                                 native=native))
            return
        if mn in VEC1:
            a = self.read_reg(_reg(ops[1]))
            native = mn not in ("clz", "gbh", "gbb", "frest", "frsqest",
                                "fesd", "frds", "mfspr")
            self.write_reg(rt(), self.add_kernel(mn, VEC1[mn], [a], native=native))
            return
        if mn in VECI:
            tok = ops[2]
            if tok.startswith("$r"):
                # RR-decode-priority RI7 forms (quad shifts/rotates, dftsv):
                # the disassembler prints the raw i7 field as a register
                # token; the digits ARE the unsigned field value. Consumers
                # mask (&0x3F/&0x1F/&7 -- unchanged by sign) or ignore it
                # (dftsv), so the raw value is exact. The DIAG lifter has the
                # same decode quirk but only strips it for the shift forms;
                # for dftsv it would emit the token verbatim (invalid C) --
                # a latent hole no shipped image reaches, found by the WB
                # fuzzer (randomized-block campaign, 2026-08-23).
                imm = int(tok[2:], 0)
            else:
                imm = int(_imm(tok), 0)
            a = self.read_reg(_reg(ops[1]))
            native = mn not in ("cflts", "cfltu", "csflt", "cuflt", "dftsv")
            self.write_reg(rt(), self.add_kernel(mn, VECI[mn], [a], imm,
                                                 native=native))
            return

        raise AssertionError(f"unclassified supported insn {mn} @0x{addr:X}")

    # ---- liveness / DCE --------------------------------------------------
    def _addr_bases(self, addr):
        if addr[0] == "b+d":
            return (addr[1],)
        if addr[0] == "bb":
            return (addr[1], addr[2])
        return ()

    def run_dce(self):
        live = set()
        # roots: flush dirties, LS stores, channel writes, halt conds, fscrwr
        for op in self.ops:
            if op.kind == "flush":
                live.update(op.dirty.values())
            elif op.kind in ("lsstore", "wrch", "fscrwr"):
                live.update(op.args)
                if op.kind == "lsstore":
                    live.update(self._addr_bases(op.addr))
            elif op.kind == "halt_cond":
                live.update(op.args)
                if op.rhs[0] == "vec":
                    live.add(op.rhs[1])
                live.update(op.dirty_snapshot.values())
        # tail (terminator) roots recorded separately
        live.update(getattr(self, "tail_roots", ()))
        # transitive closure (ops are topologically ordered)
        for op in reversed(self.ops):
            if op.vid >= 0 and op.vid in live:
                live.update(op.args)
                if op.kind == "lsload":
                    live.update(self._addr_bases(op.addr))
        for op in self.ops:
            if op.kind in ("kernel", "gprload", "genctrl", "lsload"):
                op.live = op.vid in live
                if not op.live:
                    self.m["dce_dropped"] += 1
            else:
                op.live = True
        self.live = live

    # ---- emission --------------------------------------------------------
    def vexpr(self, vid):
        """C expression for a vector value; constants materialize lazily at
        first use so unused constants never emit."""
        if vid not in self.vname:
            w = self.const[vid]     # KeyError here = internal ordering bug
            n = f"c{vid}"
            self.lines.append(
                f"    const __m128i {n} = _mm_setr_epi32("
                f"(int)0x{w[0]:X}, (int)0x{w[1]:X}, (int)0x{w[2]:X}, (int)0x{w[3]:X});")
            self.vname[vid] = n
        return self.vname[vid]

    def sexpr(self, vid):
        """Scalar (preferred word) expression for a value; constant-folded."""
        if vid in self.const:
            return f"0x{self.const[vid][0]:X}u"
        if vid not in self.sname:
            n = f"s{vid}"
            self.lines.append(f"    const uint32_t {n} = wb_pref({self.vexpr(vid)});")
            self.sname[vid] = n
        return self.sname[vid]

    def addr_expr(self, addr):
        if addr[0] == "abs":
            return f"0x{addr[1]:X}u"
        if addr[0] == "bb":
            return f"{self.sexpr(addr[1])} + {self.sexpr(addr[2])}"
        base = self.sexpr(addr[1])
        d = addr[2]
        if d == 0:
            return base
        return f"{base} + {d}" if d > 0 else f"{base} - {-d}"

    def emit_ops(self):
        for op in self.ops:
            if not op.live:
                continue
            if op.kind == "gprload":
                n = f"g{op.reg}_{op.vid}"
                self.lines.append(f"    const __m128i {n} = WB_GPR_LOAD(ctx, {op.reg});")
                self.vname[op.vid] = n
                self.m["gpr_loads"] += 1
            elif op.kind == "kernel":
                n = f"v{op.vid}"
                args = ", ".join(self.vexpr(a) for a in op.args)
                if op.imm is not None:
                    args = f"{args}, {op.imm}" if args else f"{op.imm}"
                self.lines.append(f"    const __m128i {n} = {op.kernel}({args});")
                self.vname[op.vid] = n
            elif op.kind == "genctrl":
                n = f"v{op.vid}"
                if op.imm is not None:
                    pos = f"{self.sexpr(op.args[0])} + {op.imm}" if op.imm \
                        else self.sexpr(op.args[0])
                else:
                    pos = f"{self.sexpr(op.args[0])} + {self.sexpr(op.args[1])}"
                self.lines.append(f"    const __m128i {n} = {op.kernel}((uint32_t)({pos}));")
                self.vname[op.vid] = n
            elif op.kind == "lsload":
                n = f"v{op.vid}"
                self.lines.append(
                    f"    const __m128i {n} = wbk_ls_read(ctx, (uint32_t)({self.addr_expr(op.addr)}));")
                self.vname[op.vid] = n
            elif op.kind == "lsstore":
                self.lines.append(
                    f"    wbk_ls_write(ctx, (uint32_t)({self.addr_expr(op.addr)}), {self.vexpr(op.args[0])});")
            elif op.kind == "flush":
                for r in sorted(op.dirty):
                    self.lines.append(
                        f"    WB_GPR_STORE(ctx, {r}, {self.vexpr(op.dirty[r])});")
                    self.m["gpr_stores"] += 1
            elif op.kind == "fence":
                self.lines.append("    spu_arch_fence();")
            elif op.kind == "fscrrd":
                n = f"v{op.vid}"
                self.lines.append(f"    const __m128i {n} = wb_from_u128(ctx->fpscr);")
                self.vname[op.vid] = n
            elif op.kind == "fscrwr":
                self.lines.append(f"    ctx->fpscr = wb_to_u128({self.vexpr(op.args[0])});")
            elif op.kind == "rdch":
                n = f"v{op.vid}"
                self.lines.append(f"    ctx->pc = 0x{op.pc:X}u;")
                self.lines.append(f"    const __m128i {n} = wb_from_u128(spu_rdch(ctx, {op.ch}));")
                self.vname[op.vid] = n
            elif op.kind == "rchcnt":
                n = f"v{op.vid}"
                self.lines.append(f"    ctx->pc = 0x{op.pc:X}u;")
                self.lines.append(
                    f"    const __m128i {n} = wb_pref_set(spu_rchcnt(ctx, {op.ch}));")
                self.vname[op.vid] = n
            elif op.kind == "wrch":
                self.lines.append(f"    ctx->pc = 0x{op.pc:X}u;")
                self.lines.append(
                    f"    spu_wrch(ctx, {op.ch}, wb_to_u128({self.vexpr(op.args[0])}));")
            elif op.kind == "halt_cond":
                a = f"(int32_t){self.sexpr(op.args[0])}"
                mn = op.halt_mn
                if op.rhs[0] == "imm":
                    b = f"(int32_t)({op.rhs[1]})"
                    bu = f"(uint32_t)({op.rhs[1]})"
                else:
                    b = f"(int32_t){self.sexpr(op.rhs[1])}"
                    bu = f"(uint32_t){self.sexpr(op.rhs[1])}"
                if mn in ("heq", "heqi"):
                    cond = f"{a} == {b}"
                elif mn in ("hgt", "hgti"):
                    cond = f"{a} > {b}"
                else:
                    cond = f"(uint32_t){self.sexpr(op.args[0])} > {bu}"
                flushes = "".join(
                    f"WB_GPR_STORE(ctx, {r}, {self.vexpr(v)}); "
                    for r, v in sorted(op.dirty_snapshot.items()))
                self.lines.append(
                    f"    if ({cond}) {{ {flushes}ctx->pc = 0x{op.pc:X}u; "
                    f"spu_halt(ctx, SPU_STATUS_STOPPED_BY_HALT); return; }}")


    # ---- terminators -----------------------------------------------------
    # Phase 1 (pre-DCE): IR side effects (link defs, target/cond reads, the
    # final flush marker) + a descriptor. Phase 2 (post-emit): C text --
    # vexpr/sexpr may append materialization lines, which land at the end of
    # the body, before the terminator statements.

    def prep_term(self, insn):
        self.tail_roots = set()
        self.term = insn
        self.term_reads = {}
        if insn is None:
            self.flush_marker()
            return
        mn = insn.mnemonic
        ops = _ops(insn.operands)
        if mn in ("brsl", "brasl", "bisl", "bisled"):
            link_rt = insn.raw & 0x7F
            self.write_reg(link_rt, self.add_const(((insn.addr + 4) & M32, 0, 0, 0)))
        if mn in _COND_BR:
            v = self.read_reg(_reg(ops[0]))
            self.term_reads["cond"] = v
            self.tail_roots.add(v)
        if mn in ("biz", "binz", "bihz", "bihnz"):
            v = self.read_reg(_reg(ops[0]))
            self.term_reads["cond"] = v
            self.tail_roots.add(v)
            tgt_reg = _reg(ops[1])
            if not (tgt_reg == "0" and insn.addr not in self.wl.bi_r0_jump):
                tv = self.read_reg(tgt_reg)
                self.term_reads["tgt"] = tv
                self.tail_roots.add(tv)
        if mn == "bi":
            tgt_reg = _reg(ops[0])
            if not (tgt_reg == "0" and insn.addr not in self.wl.bi_r0_jump):
                tv = self.read_reg(tgt_reg)
                self.term_reads["tgt"] = tv
                self.tail_roots.add(tv)
        if mn in ("bisl", "bisled"):
            tv = self.read_reg(_reg(ops[-1]))
            self.term_reads["tgt"] = tv
            self.tail_roots.add(tv)
        self.flush_marker()

    def _goto_or_hop(self, tgt, debug_pc=None):
        """Transfer to in-image pc `tgt` (a leader): goto inside this WB
        function, else a single-line pc-materialized trampoline."""
        wl = self.wl
        if wl.compiled_leader(tgt) and wl.pc_region.get(tgt) == self.fn.start:
            return f"goto b_{tgt:08X};"
        sym = wl.target_symbol(tgt) or "spu_indirect_branch"
        wl.metrics["trampolines"] += 1
        dbg = (f"ctx->debug_indirect_source_pc = 0x{debug_pc:X}u; "
               if debug_pc is not None else "")
        return (f"{{ ctx->pc = 0x{tgt:X}u; {dbg}"
                f"g_spu_trampoline_fn = {sym}; return; }}")

    def emit_term(self):
        insn = self.term
        L = self.lines
        blk = self.blk
        wl = self.wl
        in_image = lambda t: wl.image_span[0] <= t < wl.image_span[1]
        if insn is None:
            end = blk.end
            if end in wl.by_addr:      # fallthrough successor exists
                L.append(f"    {self._goto_or_hop(end)}")
            return
        mn = insn.mnemonic
        ops = _ops(insn.operands)
        addr = insn.addr
        helper = SPULifter()
        tgt = helper._branch_target(insn)

        if mn == "stop":
            L.append(f"    ctx->pc = 0x{addr:X}u; ctx->stop_code = {_imm(ops[0])}; "
                     f"ctx->status = SPU_STATUS_STOPPED_BY_STOP; return;")
            return
        if mn == "stopd":
            L.append(f"    ctx->pc = 0x{addr:X}u; ctx->stop_code = 0x3FFF; "
                     f"ctx->status = SPU_STATUS_STOPPED_BY_STOP; return;")
            return
        if mn in ("br", "bra"):
            if tgt == addr:   # self-loop trap
                L.append(f"    ctx->pc = 0x{addr:X}u; "
                         f"ctx->status = SPU_STATUS_STOPPED_BY_HALT; return;")
                return
            if tgt is not None and in_image(tgt):
                L.append(f"    {self._goto_or_hop(tgt, debug_pc=addr)}")
            else:
                t = tgt if tgt is not None else 0
                wl.metrics["trampolines"] += 1
                L.append(f"    {{ ctx->pc = 0x{t:X}u; "
                         f"ctx->debug_indirect_source_pc = 0x{addr:X}u; "
                         f"g_spu_trampoline_fn = spu_indirect_branch; return; }}")
            return
        if mn in _COND_BR:
            s = self.sexpr(self.term_reads["cond"])
            conds = {"brz": f"{s} == 0u", "brnz": f"{s} != 0u",
                     "brhz": f"(uint16_t){s} == 0", "brhnz": f"(uint16_t){s} != 0"}
            cond = conds[mn]
            if tgt is not None and in_image(tgt):
                body = self._goto_or_hop(tgt, debug_pc=addr)
            else:
                t = tgt if tgt is not None else 0
                wl.metrics["trampolines"] += 1
                body = (f"{{ ctx->pc = 0x{t:X}u; "
                        f"ctx->debug_indirect_source_pc = 0x{addr:X}u; "
                        f"g_spu_trampoline_fn = spu_indirect_branch; return; }}")
            L.append(f"    if ({cond}) {body}")
            if blk.end in wl.by_addr:
                L.append(f"    {self._goto_or_hop(blk.end)}")
            return
        if mn in ("brsl", "brasl"):
            if tgt == addr:   # self-loop trap (link already set + flushed)
                L.append(f"    ctx->pc = 0x{addr:X}u; "
                         f"ctx->status = SPU_STATUS_STOPPED_BY_HALT; return;")
                return
            if tgt == addr + 4:   # pc-getter idiom: no call
                L.append(f"    {self._goto_or_hop(addr + 4)}")
                return
            assert tgt is not None   # unresolved -> block is fallback
            if in_image(tgt):
                callee = wl.target_symbol(tgt) or "spu_indirect_branch"
            else:
                callee = "spu_indirect_branch"
            wl.metrics["calls"] += 1
            L.append(f"    ctx->pc = 0x{tgt:X}u; "
                     f"{{ int32_t _si = (int32_t)ctx->image_id; "
                     f"ctx->debug_indirect_source_pc = 0x{addr:X}u; "
                     f"ctx->host_depth++; {callee}(ctx); SPU_DRAIN(ctx); "
                     f"ctx->host_depth--; spu_img_restore(ctx, _si); }}")
            if addr + 4 in wl.by_addr:
                L.append(f"    {self._goto_or_hop(addr + 4)}")
            return
        if mn == "bi":
            if "tgt" not in self.term_reads:   # function return
                L.append(f"    ctx->debug_indirect_source_pc = 0x{addr:X}u; "
                         f"SPU_RET(ctx);")
                return
            s = self.sexpr(self.term_reads["tgt"])
            wl.metrics["trampolines"] += 1
            L.append(f"    ctx->debug_indirect_source_pc = 0x{addr:X}u; "
                     f"ctx->pc = {s}; "
                     f"g_spu_trampoline_fn = spu_indirect_branch; return;")
            return
        if mn == "iret":
            wl.metrics["trampolines"] += 1
            L.append(f"    ctx->debug_indirect_source_pc = 0x{addr:X}u; "
                     f"ctx->pc = ctx->srr0; "
                     f"g_spu_trampoline_fn = spu_indirect_branch; return;")
            return
        if mn == "bisl":
            s = self.sexpr(self.term_reads["tgt"])
            wl.metrics["calls"] += 1
            L.append(f"    {{ int32_t _si = (int32_t)ctx->image_id; "
                     f"ctx->debug_indirect_source_pc = 0x{addr:X}u; "
                     f"ctx->pc = {s}; ctx->host_depth++; "
                     f"spu_indirect_branch(ctx); SPU_DRAIN(ctx); "
                     f"ctx->host_depth--; spu_img_restore(ctx, _si); }}")
            if addr + 4 in wl.by_addr:
                L.append(f"    {self._goto_or_hop(addr + 4)}")
            return
        if mn == "bisled":
            s = self.sexpr(self.term_reads["tgt"])
            wl.metrics["trampolines"] += 1
            L.append(f"    if ((ctx->event_status & ctx->event_mask) != 0) {{ "
                     f"ctx->debug_indirect_source_pc = 0x{addr:X}u; "
                     f"ctx->pc = {s}; "
                     f"g_spu_trampoline_fn = spu_indirect_branch; return; }}")
            if addr + 4 in wl.by_addr:
                L.append(f"    {self._goto_or_hop(addr + 4)}")
            return
        if mn in ("biz", "binz", "bihz", "bihnz"):
            s = self.sexpr(self.term_reads["cond"])
            conds = {"biz": f"{s} == 0u", "binz": f"{s} != 0u",
                     "bihz": f"(uint16_t){s} == 0", "bihnz": f"(uint16_t){s} != 0"}
            cond = conds[mn]
            if "tgt" not in self.term_reads:   # conditional return
                L.append(f"    if ({cond}) {{ "
                         f"ctx->debug_indirect_source_pc = 0x{addr:X}u; "
                         f"SPU_RET(ctx); }}")
            else:
                t = self.sexpr(self.term_reads["tgt"])
                wl.metrics["trampolines"] += 1
                L.append(f"    if ({cond}) {{ "
                         f"ctx->debug_indirect_source_pc = 0x{addr:X}u; "
                         f"ctx->pc = {t}; "
                         f"g_spu_trampoline_fn = spu_indirect_branch; return; }}")
            if blk.end in wl.by_addr:
                L.append(f"    {self._goto_or_hop(blk.end)}")
            return
        raise AssertionError(f"unhandled terminator {mn} @0x{addr:X}")

    def translate_block(self):
        insns = self.blk.insns
        term = None
        for i, insn in enumerate(insns):
            if insn.mnemonic in _BRANCHY:
                assert i == len(insns) - 1, \
                    f"branch mid-block @0x{insn.addr:X}"
                term = insn
                break
            self.translate(insn)
        self.prep_term(term)
        self.run_dce()
        self.emit_ops()
        self.emit_term()
        return self.lines


# ---------------------------------------------------------------------------
# Source / header emission
# ---------------------------------------------------------------------------

WB_SOURCE_PREAMBLE = """\
/* Auto-generated by spu_wb_lifter.py (whole-block SPU backend) -- do not
 * edit by hand. Fallback surface: the FAST region twin (always linked
 * alongside; see docs/SPU_WB_BACKEND.md). */
#include "{header_name}"
#include "spu_wb_simd.h"
"""

WB_HEADER_PREAMBLE = """\
/* Auto-generated by spu_wb_lifter.py -- do not edit by hand. */
#pragma once
#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

void spu_indirect_branch(spu_context* ctx);
u128 spu_rdch(spu_context* ctx, uint32_t channel);
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel);
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
"""


def emit_wb(wl: WBLifter, header_name):
    src = [WB_SOURCE_PREAMBLE.replace("{header_name}", header_name), ""]
    hdr = [WB_HEADER_PREAMBLE]
    fast_syms = sorted(set(wl.fast_table.values()))
    hdr.append("/* FAST region twin fallback surface */")
    for s in fast_syms:
        hdr.append(f"void {s}(spu_context* ctx);")
    hdr.append("")

    emitted_funcs = []
    for fn in wl.funcs:
        compiled = [b for b in fn.blocks if b.fallback is None]
        if not compiled:
            continue
        emitted_funcs.append(fn.name)
        src.append(f"void {fn.name}(spu_context* ctx) {{")
        src.append("switch ((uint32_t)ctx->pc) {")
        for blk in compiled:
            src.append(f"case 0x{blk.leader:08X}u: goto b_{blk.leader:08X};")
        src.append("default:")
        src.append("    /* pc preserved: entry switch default -- a non-leader"
                    " resume; the registry maps it to the FAST twin */")
        src.append("    g_spu_trampoline_fn = spu_indirect_branch;")
        src.append("    return;")
        src.append("}")
        for blk in fn.blocks:
            if blk.fallback is not None:
                continue
            # block-scoped braces: value locals are per-block, and jumping to
            # the label enters the compound at its top (no VLAs, so legal C)
            src.append(f"b_{blk.leader:08X}: {{")
            wrapped_before = wl.metrics["kernel_wrapped"]
            em = BlockEmitter(wl, fn, blk)
            src.extend(em.translate_block())
            src.append("}")
            wl.n_insns_compiled += len(blk.insns)
            if wl.metrics["kernel_wrapped"] == wrapped_before:
                wl.n_blocks_helper_free += 1
        src.append("}")
        src.append("")

    for name in emitted_funcs:
        hdr.append(f"void {name}(spu_context* ctx);")
    hdr.append("")
    hdr.append("void spu_register_function(uint32_t addr, void (*fn)(spu_context*));")
    hdr.append(f"void {wl.register_name}(void);")
    hdr.append(f"void {wl.register_name}_fastbase(void); /* renamed FAST registration */")
    hdr.append("")
    hdr.append("#ifdef __cplusplus")
    hdr.append("}")
    hdr.append("#endif")

    # registration table: every pc -> WB super-function (compiled leaders) or
    # the FAST region twin (everything else)
    src.append("/* Registration: every pc of the image; compiled block leaders")
    src.append(" * enter the WB functions, everything else stays on the FAST twin. */")
    src.append("typedef struct { uint32_t addr; void (*func)(spu_context*); } spu_wb_entry_t;")
    src.append("static const spu_wb_entry_t spu_wb_table[] = {")
    n_wb = 0
    for insn in wl.insns:
        pc = insn.addr
        if wl.compiled_leader(pc):
            sym = f"{wl.wb_prefix}{wl.pc_region[pc]:08X}"
            n_wb += 1
        else:
            sym = wl.fast_table.get(pc)
            if sym is None:
                continue   # pc absent from the FAST table: nothing to register
        src.append(f"    {{ 0x{pc:08X}u, {sym} }},")
    src.append("    { 0, 0 }")
    src.append("};")
    src.append("")
    src.append(f"void {wl.register_name}(void) {{")
    src.append("    if (!spu_wb_runtime_ok()) {")
    src.append(f'        fprintf(stderr, "[spu-wb] AVX2/OS support missing; '
               f'{wl.register_name} registering FAST twin only\\n");')
    src.append(f"        {wl.register_name}_fastbase();")
    src.append("        return;")
    src.append("    }")
    src.append("    for (const spu_wb_entry_t* e = spu_wb_table; e->func; ++e)")
    src.append("        spu_register_function(e->addr, e->func);")
    src.append("}")
    src.append("")
    return "\n".join(src), "\n".join(hdr), n_wb


def assert_pc_invariant(source_text):
    """Every g_spu_trampoline_fn set must have ctx->pc materialized on the
    same line, except the documented entry-switch default (pc is the switch
    scrutinee there)."""
    bad = []
    lines = source_text.splitlines()
    for i, line in enumerate(lines):
        idx = line.find("g_spu_trampoline_fn =")
        if idx < 0:
            continue
        if "ctx->pc = " in line[:idx] or "ctx->pc=" in line[:idx]:
            continue
        window = "\n".join(lines[max(0, i - 3):i])
        if "entry switch default" in window:
            continue
        bad.append((i + 1, line.strip()))
    return bad


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description="whole-block SPU lifter (WB lane)")
    p.add_argument("input", nargs="?", default=None)
    p.add_argument("--auto-functions", metavar="ELF")
    p.add_argument("--base", type=lambda x: int(x, 0), default=0)
    p.add_argument("--func-prefix", required=True)
    p.add_argument("--register-name", required=True)
    p.add_argument("--fast-source", required=True,
                   help="the placement's FAST twin .c (fallback surface + "
                        "pc->region symbol table)")
    p.add_argument("--output", "-o", default=".")
    p.add_argument("--source-name", required=True)
    p.add_argument("--header-name", required=True)
    p.add_argument("--region-cap", type=int, default=256)
    p.add_argument("--repair-pcs", default="",
                   help="comma-separated pcs whose blocks must fall back "
                        "(e.g. gs_task tagread-repair reads)")
    p.add_argument("--metrics-json", default=None)
    args = p.parse_args()

    if args.auto_functions:
        from find_spu_functions import detect_functions
        with open(args.auto_functions, "rb") as f:
            elf_buf = f.read()
        funcs, (text_off, base, size) = detect_functions(elf_buf)
        data = elf_buf[text_off:text_off + size]
        if args.base != 0:
            base = args.base
        logical_bounds = funcs
    else:
        with open(args.input, "rb") as f:
            data = f.read()
        base = args.base
        logical_bounds = [(base, base + len(data))]

    insns = disassemble_spu(data, base)
    fast_table, fast_register = parse_fast_table(args.fast_source)

    wl = WBLifter(insns, base, args.func_prefix, args.register_name,
                  fast_table, fast_register, region_cap=args.region_cap)
    wl.bi_r0_jump = compute_bi_r0_jumps(insns, logical_bounds)
    wl.repair_pcs = {int(x, 0) for x in args.repair_pcs.split(",") if x}
    wl.build()

    src, hdr, n_wb = emit_wb(wl, args.header_name)
    bad = assert_pc_invariant(src)
    if bad:
        for ln, txt in bad[:10]:
            print(f"PC-INVARIANT FAIL line {ln}: {txt}", file=sys.stderr)
        sys.exit(1)

    os.makedirs(args.output, exist_ok=True)
    sp = os.path.join(args.output, args.source_name)
    hp = os.path.join(args.output, args.header_name)
    with open(sp, "w", newline="\n") as f:
        f.write(src)
    with open(hp, "w", newline="\n") as f:
        f.write(hdr)

    n_blocks = sum(len(f.blocks) for f in wl.funcs)
    n_fb = len(wl.fallback_blocks)
    n_insns = len([i for i in wl.insns])
    print(f"Wrote {sp}")
    print(f"Wrote {hp}")
    print(f"  WB LIFT: {len(wl.funcs)} region fn(s), {n_blocks} block(s), "
          f"{n_blocks - n_fb} compiled, {n_fb} fallback, "
          f"{n_wb} leader pc(s) registered WB, {n_insns} insn(s)")
    if wl.fallback_blocks:
        reasons = {}
        for _pc, r in wl.fallback_blocks:
            reasons[r] = reasons.get(r, 0) + 1
        for r, n in sorted(reasons.items(), key=lambda kv: -kv[1]):
            print(f"      fallback {r}: {n}")
    print(f"  PASS: pc-materialization invariant holds on every trampoline set")

    if args.metrics_json:
        m = dict(wl.metrics)
        m.update({
            "source": args.source_name,
            "register_name": args.register_name,
            "wb_prefix": wl.wb_prefix,
            "base": base,
            "image_span": list(wl.image_span),
            "n_insns": n_insns,
            "n_regions": len(wl.funcs),
            "n_blocks": n_blocks,
            "n_blocks_compiled": n_blocks - n_fb,
            "n_blocks_fallback": n_fb,
            "n_blocks_helper_free": wl.n_blocks_helper_free,
            "n_insns_compiled": wl.n_insns_compiled,
            "fallback_blocks": [[pc, r] for pc, r in wl.fallback_blocks],
            "wb_leader_pcs": n_wb,
            "unsupported": wl.unsupported,
            "unresolved_calls": wl.unresolved_calls,
            "source_bytes": len(src),
        })
        with open(args.metrics_json, "w") as f:
            json.dump(m, f, indent=1)
        print(f"  metrics -> {args.metrics_json}")


if __name__ == "__main__":
    main()
